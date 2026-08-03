#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Artifact,
    [Parameter(Mandatory = $true)][string]$Identity,
    [Parameter(Mandatory = $true)][string]$MapleReplay,
    [Parameter(Mandatory = $true)][string]$FlycastExecutable,
    [Parameter(Mandatory = $true)][string]$ArtifactValidator,
    [string]$PackageValidator = (Join-Path $PSScriptRoot `
        'flycast-validate-aica-package-v1.ps1'),
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateRange(1, 3600)][int]$ValidatorTimeoutSeconds = 300,
    [ValidateSet('', 'after-staging-v1', 'after-validation-v1')]
    [string]$IntegrationTestAbortToken = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Resolve-Regular([string]$Path, [string]$Field) {
    if ([string]::IsNullOrWhiteSpace($Path) -or
            -not [IO.Path]::IsPathFullyQualified($Path)) {
        throw "$Field must be an absolute path."
    }
    $resolved = (Resolve-Path -LiteralPath ([IO.Path]::GetFullPath($Path))).Path
    $item = Get-Item -LiteralPath $resolved -Force
    if ($item.PSIsContainer -or
            (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "$Field must be a non-linked regular file."
    }
    return $item.FullName
}

function Get-Blob([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    return [ordered]@{
        size = [int64]$item.Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Assert-Blob([object]$Expected, [string]$Path, [string]$Field) {
    if ($null -eq $Expected -or [int64]$Expected.size -le 0 -or
            [string]$Expected.sha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Field declaration is invalid."
    }
    $actual = Get-Blob $Path
    if ([int64]$actual.size -ne [int64]$Expected.size -or
            [string]$actual.sha256 -cne [string]$Expected.sha256) {
        throw "$Field bytes differ from their authority."
    }
}

function Copy-Verified([string]$Source, [string]$Destination,
        [object]$Expected, [string]$Field) {
    [IO.File]::Copy($Source, $Destination, $false)
    $copy = Resolve-Regular $Destination "$Field staged copy"
    Assert-Blob $Expected $copy "$Field staged copy"
}

function Invoke-Tool([string]$Executable, [string[]]$Arguments,
        [int]$TimeoutSeconds, [string]$Name) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw "$Name did not start." }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try { $process.Kill($true) } catch { }
            try { $process.WaitForExit() } catch { }
            throw "$Name timed out."
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult().Trim()
        $stderr = $stderrTask.GetAwaiter().GetResult().Trim()
        if ($process.ExitCode -ne 0) {
            $message = ($stdout + "`n" + $stderr).Trim()
            if ($message.Length -gt 4096) { $message = $message.Substring(0, 4096) }
            throw "$Name rejected the candidate: $message"
        }
        return $stdout
    }
    finally { $process.Dispose() }
}

function Write-Json([string]$Path, [object]$Value) {
    [IO.File]::WriteAllText($Path,
        (($Value | ConvertTo-Json -Depth 32) + "`n"), $utf8NoBom)
}

$artifactPath = Resolve-Regular $Artifact 'Artifact'
$identityPath = Resolve-Regular $Identity 'Identity'
$replayPath = Resolve-Regular $MapleReplay 'MapleReplay'
$flycastPath = Resolve-Regular $FlycastExecutable 'FlycastExecutable'
$artifactValidatorPath = Resolve-Regular $ArtifactValidator 'ArtifactValidator'
$packageValidatorPath = Resolve-Regular $PackageValidator 'PackageValidator'
$publisherPath = Resolve-Regular $PSCommandPath 'Publisher'
$identityObject = Get-Content -LiteralPath $identityPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 64
if ([string]$identityObject.schema -cne 'flycast-research-identity' -or
        [int]$identityObject.schema_version -ne 2) {
    throw 'AICA publication requires a Flycast research identity v2.'
}
Assert-Blob $identityObject.emulator.executable $flycastPath `
    'identity.emulator.executable'
if ($null -eq $identityObject.initial_state -or
        [string]$identityObject.initial_state.kind -cne 'savestate') {
    throw 'AICA package v1 requires an authenticated initial savestate.'
}
$statePath = Resolve-Regular ([string]$identityObject.initial_state.blob.path) `
    'identity.initial_state.blob'
Assert-Blob $identityObject.initial_state.blob $statePath `
    'identity.initial_state.blob'

$accepted = [IO.Path]::GetFullPath($OutputDirectory)
if (-not [IO.Path]::IsPathFullyQualified($accepted)) {
    throw 'OutputDirectory must be an absolute path.'
}
$parent = Split-Path -Parent $accepted
if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
    throw 'OutputDirectory parent must already exist.'
}
$parent = (Resolve-Path -LiteralPath $parent).Path
$accepted = Join-Path $parent (Split-Path -Leaf $accepted)
if (Test-Path -LiteralPath $accepted) { throw 'OutputDirectory already exists.' }
if ($IntegrationTestAbortToken -and
        (Split-Path -Leaf $parent) -cnotmatch
        '^flycast-research-aica-integration-[0-9a-f]+$') {
    throw 'Integration abort tokens require a dedicated integration-test parent.'
}

$packageId = [Guid]::NewGuid().ToString('D').ToLowerInvariant()
$candidate = Join-Path $parent ".flycast-research-aica-candidate-$packageId"
$quarantineRoot = Join-Path $parent '.flycast-research-aica-quarantine'
$quarantine = Join-Path $quarantineRoot $packageId
$sources = [ordered]@{
    artifact = Get-Blob $artifactPath
    identity = Get-Blob $identityPath
    initial_state = Get-Blob $statePath
    maple_replay = Get-Blob $replayPath
    flycast = Get-Blob $flycastPath
    artifact_validator = Get-Blob $artifactValidatorPath
    package_validator = Get-Blob $packageValidatorPath
    publisher = Get-Blob $publisherPath
}

try {
    [IO.Directory]::CreateDirectory($candidate) | Out-Null
    Copy-Verified $artifactPath (Join-Path $candidate 'aica.fcaica') `
        $sources.artifact 'Artifact'
    Copy-Verified $identityPath (Join-Path $candidate 'identity.json') `
        $sources.identity 'Identity'
    Copy-Verified $statePath (Join-Path $candidate 'initial-state.state') `
        $sources.initial_state 'InitialState'
    Copy-Verified $replayPath (Join-Path $candidate 'maple-replay.fcmt') `
        $sources.maple_replay 'MapleReplay'
    Copy-Verified $flycastPath (Join-Path $candidate 'flycast.exe') `
        $sources.flycast 'FlycastExecutable'
    Copy-Verified $artifactValidatorPath `
        (Join-Path $candidate 'artifact-validator.exe') `
        $sources.artifact_validator 'ArtifactValidator'
    Copy-Verified $packageValidatorPath `
        (Join-Path $candidate 'package-validator.ps1') `
        $sources.package_validator 'PackageValidator'
    Copy-Verified $publisherPath (Join-Path $candidate 'publisher.ps1') `
        $sources.publisher 'Publisher'
    Write-Json (Join-Path $candidate 'package.json') ([ordered]@{
        schema = 'flycast-research-aica-package'
        schema_version = 1
        package_id = $packageId
        accepted_directory = $accepted
        entries = $sources
    })
    if ($IntegrationTestAbortToken -ceq 'after-staging-v1') {
        throw 'Integration test forced abort after staging.'
    }
    $validatorOutput = Invoke-Tool (Join-Path $candidate 'artifact-validator.exe') @(
        (Join-Path $candidate 'aica.fcaica'),
        (Join-Path $candidate 'identity.json'),
        (Join-Path $candidate 'maple-replay.fcmt')
    ) $ValidatorTimeoutSeconds 'AICA artifact validator'
    $artifactValidation = $validatorOutput | ConvertFrom-Json -Depth 16
    if ([string]$artifactValidation.schema -cne 'flycast-aica-validation-v1' -or
            $artifactValidation.accepted -ne $true) {
        throw 'AICA validator output is not an accepted typed result.'
    }
    $receiptEntries = @()
    foreach ($name in @('aica.fcaica', 'identity.json', 'initial-state.state',
            'maple-replay.fcmt', 'flycast.exe', 'artifact-validator.exe',
            'package-validator.ps1', 'publisher.ps1', 'package.json')) {
        $blob = Get-Blob (Join-Path $candidate $name)
        $receiptEntries += [ordered]@{
            name = $name; size = $blob.size; sha256 = $blob.sha256
        }
    }
    Write-Json (Join-Path $candidate 'package-validation.json') ([ordered]@{
        schema = 'flycast-research-aica-package-validation'
        schema_version = 1
        package_id = $packageId
        status = 'accepted'
        artifact_validation = $artifactValidation
        entries = $receiptEntries
    })
    & (Join-Path $candidate 'package-validator.ps1') -Package $candidate `
        -ValidatorTimeoutSeconds $ValidatorTimeoutSeconds | Out-Null
    if ($IntegrationTestAbortToken -ceq 'after-validation-v1') {
        throw 'Integration test forced abort after validation.'
    }
    if (Test-Path -LiteralPath $accepted) {
        throw 'Accepted output appeared before atomic publication.'
    }
    [IO.Directory]::Move($candidate, $accepted)
    Write-Output "ACCEPTED flycast-research-aica-package-v1 $accepted"
}
catch {
    $failure = $_.Exception.Message
    if (Test-Path -LiteralPath $candidate -PathType Container) {
        [IO.Directory]::CreateDirectory($quarantineRoot) | Out-Null
        if (Test-Path -LiteralPath $quarantine) {
            throw 'Quarantine destination already exists.'
        }
        Write-Json (Join-Path $candidate 'rejection.json') ([ordered]@{
            schema = 'flycast-research-aica-package-quarantine'
            schema_version = 1
            package_id = $packageId
            accepted_directory = $accepted
            failure = $failure
        })
        [IO.Directory]::Move($candidate, $quarantine)
    }
    throw
}

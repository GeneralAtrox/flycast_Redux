#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Artifact,
    [Parameter(Mandatory = $true)][string]$Identity,
    [Parameter(Mandatory = $true)][string]$MapleReplay,
    [Parameter(Mandatory = $true)][string]$FlycastExecutable,
    [Parameter(Mandatory = $true)][string]$ArtifactValidator,
    [string]$PackageValidator = (Join-Path $PSScriptRoot `
        'flycast-validate-gdrom-package-v1.ps1'),
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateRange(1, 3600)][int]$ValidatorTimeoutSeconds = 300,
    [ValidateSet('', 'after-staging-v1', 'after-validation-v1')]
    [string]$IntegrationTestAbortToken = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Resolve-RegularFile([string]$Path, [string]$Field) {
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

function Assert-DeclaredBlob([object]$Declared, [string]$Actual,
        [string]$Field) {
    if ($null -eq $Declared -or [int64]$Declared.size -le 0 -or
            [string]$Declared.sha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Field declaration is invalid."
    }
    $observed = Get-Blob $Actual
    if ([int64]$observed.size -ne [int64]$Declared.size -or
            [string]$observed.sha256 -cne [string]$Declared.sha256) {
        throw "$Field bytes differ from the identity."
    }
}

function Copy-Verified([string]$Source, [string]$Destination, [object]$Expected,
        [string]$Field) {
    [IO.File]::Copy($Source, $Destination, $false)
    $copy = Resolve-RegularFile $Destination "$Field staged copy"
    $observed = Get-Blob $copy
    if ([int64]$observed.size -ne [int64]$Expected.size -or
            [string]$observed.sha256 -cne [string]$Expected.sha256) {
        throw "$Field changed while staging."
    }
}

function Invoke-Validator([string]$Executable, [string[]]$Arguments,
        [int]$TimeoutSeconds) {
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
        if (-not $process.Start()) { throw 'GD-ROM validator did not start.' }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try { $process.Kill($true) } catch { }
            try { $process.WaitForExit() } catch { }
            throw 'GD-ROM validator timed out.'
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            $message = ($stdout + "`n" + $stderr).Trim()
            if ($message.Length -gt 4096) { $message = $message.Substring(0, 4096) }
            throw "GD-ROM validator rejected the candidate: $message"
        }
        return $stdout.Trim()
    }
    finally { $process.Dispose() }
}

$artifactPath = Resolve-RegularFile $Artifact 'Artifact'
$identityPath = Resolve-RegularFile $Identity 'Identity'
$replayPath = Resolve-RegularFile $MapleReplay 'MapleReplay'
$flycastPath = Resolve-RegularFile $FlycastExecutable 'FlycastExecutable'
$validatorPath = Resolve-RegularFile $ArtifactValidator 'ArtifactValidator'
$packageValidatorPath = Resolve-RegularFile $PackageValidator 'PackageValidator'
$publisherPath = Resolve-RegularFile $PSCommandPath 'Publisher'
$identityObject = Get-Content -LiteralPath $identityPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 64
if ([string]$identityObject.schema -cne 'flycast-research-identity' -or
        [int]$identityObject.schema_version -ne 2) {
    throw 'GD-ROM publication requires a Flycast research identity v2.'
}
if ([string]$identityObject.firmware.mode -cne 'hle') {
    throw 'GD-ROM package v1 is restricted to HLE firmware; use the hardware package v2 publisher.'
}
Assert-DeclaredBlob $identityObject.emulator.executable $flycastPath `
    'Flycast executable'
Assert-DeclaredBlob $identityObject.media.source `
    (Resolve-RegularFile ([string]$identityObject.media.source.path) 'GDI descriptor') `
    'GDI descriptor'
foreach ($track in @($identityObject.media.tracks)) {
    Assert-DeclaredBlob $track `
        (Resolve-RegularFile ([string]$track.path) "GDI track $($track.track)") `
        "GDI track $($track.track)"
}

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
        (Split-Path -Leaf $parent) -cnotmatch '^flycast-research-gdrom-integration-[0-9a-f]+$') {
    throw 'Integration abort tokens require a dedicated integration-test parent.'
}

$packageId = [guid]::NewGuid().ToString('D').ToLowerInvariant()
$candidate = Join-Path $parent ".flycast-research-gdrom-candidate-$packageId"
$quarantineRoot = Join-Path $parent '.flycast-research-gdrom-quarantine'
$quarantine = Join-Path $quarantineRoot $packageId
$sources = [ordered]@{
    artifact = Get-Blob $artifactPath
    identity = Get-Blob $identityPath
    maple_replay = Get-Blob $replayPath
    flycast = Get-Blob $flycastPath
    artifact_validator = Get-Blob $validatorPath
    package_validator = Get-Blob $packageValidatorPath
    publisher = Get-Blob $publisherPath
}

try {
    [IO.Directory]::CreateDirectory($candidate) | Out-Null
    Copy-Verified $artifactPath (Join-Path $candidate 'gdrom.fcgd') `
        $sources.artifact 'Artifact'
    Copy-Verified $identityPath (Join-Path $candidate 'identity.json') `
        $sources.identity 'Identity'
    Copy-Verified $replayPath (Join-Path $candidate 'maple-replay.fcmt') `
        $sources.maple_replay 'MapleReplay'
    Copy-Verified $flycastPath (Join-Path $candidate 'flycast.exe') `
        $sources.flycast 'FlycastExecutable'
    Copy-Verified $validatorPath (Join-Path $candidate 'artifact-validator.exe') `
        $sources.artifact_validator 'ArtifactValidator'
    Copy-Verified $packageValidatorPath (Join-Path $candidate 'package-validator.ps1') `
        $sources.package_validator 'PackageValidator'
    Copy-Verified $publisherPath (Join-Path $candidate 'publisher.ps1') `
        $sources.publisher 'Publisher'
    $manifest = [ordered]@{
        schema = 'flycast-research-gdrom-package'
        schema_version = 1
        package_id = $packageId
        accepted_directory = $accepted
        entries = $sources
    }
    [IO.File]::WriteAllText((Join-Path $candidate 'package.json'),
        (($manifest | ConvertTo-Json -Depth 16) + "`n"), $utf8NoBom)

    if ($IntegrationTestAbortToken -ceq 'after-staging-v1') {
        throw 'Integration test forced abort after staging.'
    }
    $validatorOutput = Invoke-Validator (Join-Path $candidate 'artifact-validator.exe') @(
        '--artifact', (Join-Path $candidate 'gdrom.fcgd'),
        '--identity', (Join-Path $candidate 'identity.json'),
        '--replay', (Join-Path $candidate 'maple-replay.fcmt')
    ) $ValidatorTimeoutSeconds
    $receiptEntries = @()
    foreach ($name in @('gdrom.fcgd', 'identity.json', 'maple-replay.fcmt',
            'flycast.exe', 'artifact-validator.exe', 'publisher.ps1',
            'package-validator.ps1', 'package.json')) {
        $blob = Get-Blob (Join-Path $candidate $name)
        $receiptEntries += [ordered]@{ name = $name; size = $blob.size; sha256 = $blob.sha256 }
    }
    $receipt = [ordered]@{
        schema = 'flycast-research-gdrom-package-validation'
        schema_version = 1
        package_id = $packageId
        status = 'accepted'
        validator_output = $validatorOutput
        entries = $receiptEntries
    }
    [IO.File]::WriteAllText((Join-Path $candidate 'package-validation.json'),
        (($receipt | ConvertTo-Json -Depth 16) + "`n"), $utf8NoBom)
    & (Join-Path $candidate 'package-validator.ps1') -Package $candidate `
        -ValidatorTimeoutSeconds $ValidatorTimeoutSeconds | Out-Null
    if ($IntegrationTestAbortToken -ceq 'after-validation-v1') {
        throw 'Integration test forced abort after validation.'
    }
    [IO.Directory]::Move($candidate, $accepted)
    Write-Output "ACCEPTED flycast-research-gdrom-package-v1 $accepted"
}
catch {
    $failure = $_.Exception.Message
    if (Test-Path -LiteralPath $candidate) {
        [IO.Directory]::CreateDirectory($quarantineRoot) | Out-Null
        [IO.Directory]::Move($candidate, $quarantine)
        $rejection = [ordered]@{
            schema = 'flycast-research-gdrom-package-quarantine'
            schema_version = 1
            package_id = $packageId
            accepted_directory = $accepted
            failure = $failure
        }
        [IO.File]::WriteAllText((Join-Path $quarantine 'rejection.json'),
            (($rejection | ConvertTo-Json -Depth 8) + "`n"), $utf8NoBom)
    }
    throw
}

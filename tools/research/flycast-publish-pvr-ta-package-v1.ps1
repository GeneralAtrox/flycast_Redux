[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Job,

    [Parameter(Mandatory = $false)]
    [ValidateSet('', 'after-staging-v1', 'after-validation-v1')]
    [string]$IntegrationTestAbortToken = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'PowerVR TA package publication requires PowerShell 7 or newer.'
}

function Assert-ExactProperties([object]$Value, [string[]]$Names, [string]$Field) {
    if ($null -eq $Value) { throw "$Field must be an object." }
    $actual = @($Value.PSObject.Properties.Name)
    foreach ($name in $Names) {
        if ($actual -cnotcontains $name) { throw "$Field.$name is missing." }
    }
    foreach ($name in $actual) {
        if ($Names -cnotcontains $name) { throw "$Field has unknown property '$name'." }
    }
}

function Get-AbsolutePath([string]$Path, [string]$Field, [switch]$Existing) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not [IO.Path]::IsPathFullyQualified($Path)) {
        throw "$Field must be an absolute path."
    }
    $full = [IO.Path]::GetFullPath($Path)
    if ($Existing) { $full = (Resolve-Path -LiteralPath $full).Path }
    return $full
}

function Assert-RegularFile([string]$Path, [string]$Field) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or
            (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "$Field must be a non-linked regular file."
    }
    return $item
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-Blob([object]$Blob, [string]$Field) {
    Assert-ExactProperties $Blob @('path', 'size', 'sha256') $Field
    $path = Get-AbsolutePath ([string]$Blob.path) "$Field.path" -Existing
    $item = Assert-RegularFile $path $Field
    if ([int64]$Blob.size -le 0 -or [string]$Blob.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
            [int64]$item.Length -ne [int64]$Blob.size -or
            (Get-Sha256 $path) -cne [string]$Blob.sha256) {
        throw "$Field bytes do not match their declaration."
    }
    return $path
}

function Copy-Blob([object]$Blob, [string]$Destination, [string]$Field) {
    $source = Assert-Blob $Blob $Field
    [IO.File]::Copy($source, $Destination, $false)
    $copy = Assert-RegularFile $Destination "$Field staged copy"
    if ([int64]$copy.Length -ne [int64]$Blob.size -or
            (Get-Sha256 $Destination) -cne [string]$Blob.sha256) {
        throw "$Field changed while staging."
    }
}

function Invoke-OwnedProcess([string]$Executable, [string[]]$Arguments,
        [int]$TimeoutSeconds, [string]$Name) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
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
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            $combined = ($stdout + "`n" + $stderr)
            if ($combined.Length -gt 4096) { $combined = $combined.Substring(0, 4096) }
            throw "$Name rejected the candidate (exit $($process.ExitCode)): $combined"
        }
    }
    finally { $process.Dispose() }
}

$jobPath = Get-AbsolutePath $Job 'Job' -Existing
Assert-RegularFile $jobPath 'Job' | Out-Null
$jobObject = Get-Content -LiteralPath $jobPath -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 64
$jobProperties = @(
    'schema', 'schema_version', 'package_id', 'output', 'base_equivalence_package',
    'identity', 'maple_replay', 'pvr_manifest', 'artifact', 'static_analysis',
    'publisher', 'artifact_validator', 'package_validator', 'limits', 'metadata'
)
$hasInitialState = $jobObject.PSObject.Properties.Name -ccontains 'initial_state'
if ($hasInitialState) { $jobProperties += 'initial_state' }
Assert-ExactProperties $jobObject $jobProperties 'job'
if ([string]$jobObject.schema -cne 'flycast-research-pvr-ta-package-job' -or
        [int]$jobObject.schema_version -ne 1) { throw 'Unsupported package job schema.' }
$packageId = [string]$jobObject.package_id
if ($packageId -cnotmatch '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$') {
    throw 'job.package_id must be a lowercase UUID.'
}
Assert-ExactProperties $jobObject.output @('accepted_directory') 'job.output'
$accepted = Get-AbsolutePath ([string]$jobObject.output.accepted_directory) `
    'job.output.accepted_directory'
$parent = Split-Path -Parent $accepted
if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
    throw 'Accepted-directory parent must already exist.'
}
$parent = Get-AbsolutePath $parent 'accepted-directory parent' -Existing
$accepted = Join-Path $parent (Split-Path -Leaf $accepted)
if (Test-Path -LiteralPath $accepted) { throw 'Accepted directory already exists.' }
if ($IntegrationTestAbortToken -and
        (Split-Path -Leaf $parent) -cnotmatch '^flycast-research-pvr-ta-integration-[0-9a-f]+$') {
    throw 'Integration abort tokens require a dedicated integration-test parent.'
}

$candidate = Join-Path $parent ".flycast-research-pvr-ta-candidate-$packageId"
$quarantineRoot = Join-Path $parent '.flycast-research-pvr-ta-quarantine'
$quarantine = Join-Path $quarantineRoot $packageId
if (Test-Path -LiteralPath $candidate) { throw 'Private candidate already exists.' }
if (Test-Path -LiteralPath $quarantine) { throw 'Quarantine destination already exists.' }

Assert-ExactProperties $jobObject.base_equivalence_package @('accepted_directory', 'backend') `
    'job.base_equivalence_package'
Get-AbsolutePath ([string]$jobObject.base_equivalence_package.accepted_directory) `
    'job.base_equivalence_package.accepted_directory' -Existing | Out-Null
if ([string]$jobObject.base_equivalence_package.backend -cnotin @('interpreter', 'dynarec')) {
    throw 'Unsupported base backend.'
}
Assert-ExactProperties $jobObject.artifact @('candidate', 'maximum_bytes', 'maximum_events') `
    'job.artifact'
Assert-ExactProperties $jobObject.static_analysis @('export', 'program') `
    'job.static_analysis'
Assert-ExactProperties $jobObject.publisher @('script') 'job.publisher'
Assert-ExactProperties $jobObject.artifact_validator @('executable') 'job.artifact_validator'
Assert-ExactProperties $jobObject.package_validator @('executable') 'job.package_validator'
Assert-ExactProperties $jobObject.limits @('validator_timeout_seconds') 'job.limits'
$timeout = [int]$jobObject.limits.validator_timeout_seconds
if ($timeout -lt 1 -or $timeout -gt 3600) { throw 'Validator timeout is invalid.' }

$publisher = Assert-Blob $jobObject.publisher.script 'job.publisher.script'
if (-not [string]::Equals($publisher, (Resolve-Path -LiteralPath $PSCommandPath).Path,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Running publisher differs from job.publisher.script.'
}
$artifactValidator = Assert-Blob $jobObject.artifact_validator.executable `
    'job.artifact_validator.executable'
$packageValidator = Assert-Blob $jobObject.package_validator.executable `
    'job.package_validator.executable'
if ($hasInitialState) {
    Assert-Blob $jobObject.initial_state 'job.initial_state' | Out-Null
}

try {
    [IO.Directory]::CreateDirectory($candidate) | Out-Null
    [IO.File]::Copy($jobPath, (Join-Path $candidate 'job.json'), $false)
    Copy-Blob $jobObject.identity (Join-Path $candidate 'identity.json') 'job.identity'
    Copy-Blob $jobObject.maple_replay (Join-Path $candidate 'maple-replay.fcmt') `
        'job.maple_replay'
    Copy-Blob $jobObject.pvr_manifest (Join-Path $candidate 'pvr-ta-manifest.json') `
        'job.pvr_manifest'
    if ($hasInitialState) {
        Copy-Blob $jobObject.initial_state (Join-Path $candidate 'initial-state.state') `
            'job.initial_state'
    }
    Copy-Blob $jobObject.artifact.candidate (Join-Path $candidate 'pvr-ta.fcpvr') `
        'job.artifact.candidate'
    Copy-Blob $jobObject.static_analysis.export (Join-Path $candidate 'static-analysis.bin') `
        'job.static_analysis.export'
    Copy-Blob $jobObject.publisher.script (Join-Path $candidate 'publisher.ps1') `
        'job.publisher.script'
    Copy-Blob $jobObject.artifact_validator.executable `
        (Join-Path $candidate 'artifact-validator.exe') 'job.artifact_validator.executable'
    Copy-Blob $jobObject.package_validator.executable `
        (Join-Path $candidate 'package-validator.exe') 'job.package_validator.executable'

    if ($IntegrationTestAbortToken -ceq 'after-staging-v1') {
        throw 'Integration test forced abort after staging.'
    }

    Invoke-OwnedProcess (Join-Path $candidate 'artifact-validator.exe') @(
        '--artifact', (Join-Path $candidate 'pvr-ta.fcpvr'),
        '--identity', (Join-Path $candidate 'identity.json'),
        '--replay', (Join-Path $candidate 'maple-replay.fcmt'),
        '--manifest', (Join-Path $candidate 'pvr-ta-manifest.json')
    ) $timeout 'PowerVR TA artifact validator'

    Invoke-OwnedProcess (Join-Path $candidate 'package-validator.exe') @(
        '--package', $candidate,
        '--receipt', (Join-Path $candidate 'package-validation.json')
    ) $timeout 'PowerVR TA package validator'

    if ($IntegrationTestAbortToken -ceq 'after-validation-v1') {
        throw 'Integration test forced abort after validation.'
    }
    [IO.Directory]::Move($candidate, $accepted)
    Write-Output "ACCEPTED flycast-research-pvr-ta-package-v1 $accepted"
}
catch {
    $failure = $_.Exception.Message
    if (Test-Path -LiteralPath $candidate) {
        [IO.Directory]::CreateDirectory($quarantineRoot) | Out-Null
        [IO.Directory]::Move($candidate, $quarantine)
        $rejection = [ordered]@{
            schema = 'flycast-research-pvr-ta-package-quarantine'
            schema_version = 1
            package_id = $packageId
            accepted_directory = $accepted
            failure = $failure
        }
        $json = $rejection | ConvertTo-Json -Depth 8
        [IO.File]::WriteAllText((Join-Path $quarantine 'rejection.json'), $json + "`n",
            [Text.UTF8Encoding]::new($false))
    }
    throw
}

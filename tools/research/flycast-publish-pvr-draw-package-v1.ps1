#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Job,
    [ValidateSet('', 'after-staging-v1', 'after-validation-v1')]
    [string]$IntegrationTestAbortToken = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Assert-Exact([object]$Value, [string[]]$Names, [string]$Field) {
    if ($null -eq $Value) { throw "$Field must be an object." }
    $actual = @($Value.PSObject.Properties.Name)
    foreach ($name in $Names) {
        if ($actual -cnotcontains $name) { throw "$Field.$name is missing." }
    }
    foreach ($name in $actual) {
        if ($Names -cnotcontains $name) { throw "$Field has unknown property '$name'." }
    }
}

function Resolve-Absolute([string]$Path, [string]$Field, [switch]$Existing) {
    if ([string]::IsNullOrWhiteSpace($Path) -or
            -not [IO.Path]::IsPathFullyQualified($Path)) {
        throw "$Field must be an absolute path."
    }
    $result = [IO.Path]::GetFullPath($Path)
    if ($Existing) { $result = (Resolve-Path -LiteralPath $result).Path }
    return $result
}

function Get-Digest([string]$Path) {
    (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-Blob([object]$Blob, [string]$Field) {
    Assert-Exact $Blob @('path', 'size', 'sha256') $Field
    $path = Resolve-Absolute ([string]$Blob.path) "$Field.path" -Existing
    $item = Get-Item -LiteralPath $path -Force
    if ($item.PSIsContainer -or
            (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) -or
            [int64]$item.Length -ne [int64]$Blob.size -or
            (Get-Digest $path) -cne [string]$Blob.sha256) {
        throw "$Field bytes differ from their declaration."
    }
    return $path
}

function Copy-Blob([object]$Blob, [string]$Destination, [string]$Field) {
    $source = Assert-Blob $Blob $Field
    [IO.File]::Copy($source, $Destination, $false)
    if ((Get-Digest $Destination) -cne [string]$Blob.sha256) {
        throw "$Field changed during staging."
    }
}

function Invoke-Owned([string]$Executable, [string[]]$Arguments,
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
            $message = $stdout + "`n" + $stderr
            if ($message.Length -gt 4096) { $message = $message.Substring(0, 4096) }
            throw "$Name rejected the candidate (exit $($process.ExitCode)): $message"
        }
    }
    finally { $process.Dispose() }
}

$jobPath = Resolve-Absolute $Job 'Job' -Existing
$jobObject = Get-Content -LiteralPath $jobPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 64
$jobProperties = @(
    'schema', 'schema_version', 'package_id', 'output', 'base_pvr_ta_package',
    'identity', 'maple_replay', 'pvr_manifest', 'ta_artifact',
    'presentation_artifact', 'draw_artifact', 'publisher', 'draw_validator',
    'package_validator', 'limits', 'metadata')
$hasInitialState = $jobObject.PSObject.Properties.Name -ccontains 'initial_state'
if ($hasInitialState) { $jobProperties += 'initial_state' }
Assert-Exact $jobObject $jobProperties 'job'
if ([string]$jobObject.schema -cne 'flycast-research-pvr-draw-package-job' -or
        [int]$jobObject.schema_version -ne 1) { throw 'Unsupported job schema.' }
$packageId = [string]$jobObject.package_id
if ($packageId -cnotmatch '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$') {
    throw 'package_id must be a lowercase UUID.'
}
Assert-Exact $jobObject.output @('accepted_directory') 'job.output'
Assert-Exact $jobObject.base_pvr_ta_package @('accepted_directory') `
    'job.base_pvr_ta_package'
Assert-Exact $jobObject.presentation_artifact @('candidate', 'maximum_bytes') `
    'job.presentation_artifact'
Assert-Exact $jobObject.draw_artifact @('candidate', 'maximum_bytes', 'maximum_events') `
    'job.draw_artifact'
Assert-Exact $jobObject.publisher @('script') 'job.publisher'
Assert-Exact $jobObject.draw_validator @('executable') 'job.draw_validator'
Assert-Exact $jobObject.package_validator @('executable') 'job.package_validator'
Assert-Exact $jobObject.limits @('validator_timeout_seconds') 'job.limits'
$timeout = [int]$jobObject.limits.validator_timeout_seconds
if ($timeout -lt 1 -or $timeout -gt 3600) { throw 'Validator timeout is invalid.' }

$accepted = Resolve-Absolute ([string]$jobObject.output.accepted_directory) `
    'job.output.accepted_directory'
$parent = Split-Path -Parent $accepted
if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
    throw 'Accepted-directory parent must already exist.'
}
$parent = Resolve-Absolute $parent 'accepted parent' -Existing
$accepted = Join-Path $parent (Split-Path -Leaf $accepted)
if (Test-Path -LiteralPath $accepted) { throw 'Accepted directory already exists.' }
Resolve-Absolute ([string]$jobObject.base_pvr_ta_package.accepted_directory) `
    'job.base_pvr_ta_package.accepted_directory' -Existing | Out-Null
$candidate = Join-Path $parent ".flycast-research-pvr-draw-candidate-$packageId"
$quarantineRoot = Join-Path $parent '.flycast-research-pvr-draw-quarantine'
$quarantine = Join-Path $quarantineRoot $packageId
if (Test-Path -LiteralPath $candidate) { throw 'Private candidate already exists.' }
if (Test-Path -LiteralPath $quarantine) { throw 'Quarantine destination already exists.' }

$publisher = Assert-Blob $jobObject.publisher.script 'job.publisher.script'
if (-not [string]::Equals($publisher, (Resolve-Path -LiteralPath $PSCommandPath).Path,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Running publisher differs from job.publisher.script.'
}
$drawValidator = Assert-Blob $jobObject.draw_validator.executable `
    'job.draw_validator.executable'
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
    Copy-Blob $jobObject.ta_artifact (Join-Path $candidate 'pvr-ta.fcpvr') `
        'job.ta_artifact'
    Copy-Blob $jobObject.presentation_artifact.candidate `
        (Join-Path $candidate 'pvr-presentation.fcpvrp') `
        'job.presentation_artifact.candidate'
    Copy-Blob $jobObject.draw_artifact.candidate `
        (Join-Path $candidate 'pvr-draw.fcpvrd') 'job.draw_artifact.candidate'
    Copy-Blob $jobObject.publisher.script (Join-Path $candidate 'publisher.ps1') `
        'job.publisher.script'
    Copy-Blob $jobObject.draw_validator.executable `
        (Join-Path $candidate 'draw-validator.exe') 'job.draw_validator.executable'
    Copy-Blob $jobObject.package_validator.executable `
        (Join-Path $candidate 'package-validator.exe') `
        'job.package_validator.executable'
    if ($IntegrationTestAbortToken -ceq 'after-staging-v1') {
        throw 'Integration test forced abort after staging.'
    }
    Invoke-Owned (Join-Path $candidate 'draw-validator.exe') @(
        '--artifact', (Join-Path $candidate 'pvr-draw.fcpvrd'),
        '--ta-artifact', (Join-Path $candidate 'pvr-ta.fcpvr'),
        '--presentation-artifact', (Join-Path $candidate 'pvr-presentation.fcpvrp'),
        '--identity', (Join-Path $candidate 'identity.json'),
        '--replay', (Join-Path $candidate 'maple-replay.fcmt'),
        '--manifest', (Join-Path $candidate 'pvr-ta-manifest.json'),
        '--max-draw-bytes', [string]$jobObject.draw_artifact.maximum_bytes,
        '--max-draw-events', [string]$jobObject.draw_artifact.maximum_events,
        '--max-presentation-bytes', [string]$jobObject.presentation_artifact.maximum_bytes
    ) $timeout 'PowerVR draw validator'
    Invoke-Owned (Join-Path $candidate 'package-validator.exe') @(
        '--package', $candidate,
        '--receipt', (Join-Path $candidate 'package-validation.json')
    ) $timeout 'PowerVR draw package validator'
    if ($IntegrationTestAbortToken -ceq 'after-validation-v1') {
        throw 'Integration test forced abort after validation.'
    }
    [IO.Directory]::Move($candidate, $accepted)
    Write-Output "ACCEPTED flycast-research-pvr-draw-package-v1 $accepted"
}
catch {
    $failure = $_.Exception.Message
    if (Test-Path -LiteralPath $candidate) {
        [IO.Directory]::CreateDirectory($quarantineRoot) | Out-Null
        [IO.Directory]::Move($candidate, $quarantine)
        $rejection = [ordered]@{
            schema = 'flycast-research-pvr-draw-package-quarantine'
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

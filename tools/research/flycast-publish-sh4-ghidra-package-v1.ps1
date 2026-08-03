[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Job,
    [ValidateSet('', 'after-staging-v1', 'after-semantic-validation-v1',
        'after-validation-v1')]
    [string]$IntegrationTestAbortToken = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8NoBom = [Text.UTF8Encoding]::new($false)

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'SH-4/Ghidra package publication requires PowerShell 7 or newer.'
}

function Assert-Exact([object]$Value, [string[]]$Names, [string]$Field) {
    if ($null -eq $Value) { throw "$Field must be an object." }
    $actual = @($Value.PSObject.Properties.Name)
    foreach ($name in $Names) {
        if ($actual -cnotcontains $name) { throw "$Field.$name is missing." }
    }
    foreach ($name in $actual) {
        if ($Names -cnotcontains $name) {
            throw "$Field has unknown property '$name'."
        }
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

function Assert-NoReparse([string]$Path, [string]$Field) {
    $item = Get-Item -LiteralPath $Path -Force
    while ($null -ne $item) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Field contains a reparse point: $($item.FullName)"
        }
        $parent = Split-Path -Parent $item.FullName
        if ([string]::IsNullOrWhiteSpace($parent) -or
                [string]::Equals($parent, $item.FullName,
                    [StringComparison]::OrdinalIgnoreCase)) { break }
        $item = Get-Item -LiteralPath $parent -Force
    }
}

function Assert-Regular([string]$Path, [string]$Field) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or
            (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "$Field must be a non-linked regular file."
    }
    Assert-NoReparse $item.FullName $Field
    return $item
}

function Get-Digest([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-Blob([object]$Blob, [string]$Field) {
    Assert-Exact $Blob @('path', 'size', 'sha256') $Field
    if ([int64]$Blob.size -le 0 -or
            [string]$Blob.sha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Field size or SHA-256 is invalid."
    }
    $path = Resolve-Absolute ([string]$Blob.path) "$Field.path" -Existing
    $item = Assert-Regular $path $Field
    if ([int64]$item.Length -ne [int64]$Blob.size -or
            (Get-Digest $path) -cne [string]$Blob.sha256) {
        throw "$Field bytes differ from their declaration."
    }
    return $path
}

function Copy-Blob([object]$Blob, [string]$Destination, [string]$Field) {
    $source = Assert-Blob $Blob $Field
    [IO.File]::Copy($source, $Destination, $false)
    $copy = Assert-Regular $Destination "$Field staged copy"
    if ([int64]$copy.Length -ne [int64]$Blob.size -or
            (Get-Digest $Destination) -cne [string]$Blob.sha256) {
        throw "$Field changed during staging."
    }
}

function Get-StreamEvidence([string]$Value) {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Value)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = ([BitConverter]::ToString(
            $algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally { $algorithm.Dispose() }
    $preview = if ($Value.Length -gt 4096) { $Value.Substring(0, 4096) } else { $Value }
    return "size=$($bytes.Length), sha256=$digest, preview=$preview"
}

function Invoke-Owned([string]$Executable, [string[]]$Arguments,
        [int]$TimeoutSeconds, [string]$Name) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $start.StandardErrorEncoding = [Text.Encoding]::UTF8
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
            $stdout = $stdoutTask.GetAwaiter().GetResult()
            $stderr = $stderrTask.GetAwaiter().GetResult()
            throw "$Name timed out. stdout($(Get-StreamEvidence $stdout)) " +
                "stderr($(Get-StreamEvidence $stderr))"
        }
        $process.WaitForExit()
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            throw "$Name exited with $($process.ExitCode). " +
                "stdout($(Get-StreamEvidence $stdout)) " +
                "stderr($(Get-StreamEvidence $stderr))"
        }
    }
    finally { $process.Dispose() }
}

function Write-Json([string]$Path, [object]$Value) {
    [IO.File]::WriteAllText($Path,
        (($Value | ConvertTo-Json -Depth 64) + "`n"), $utf8NoBom)
}

$jobPath = Resolve-Absolute $Job 'Job' -Existing
Assert-Regular $jobPath 'Job' | Out-Null
$jobObject = Get-Content -LiteralPath $jobPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 64
$jobProperties = @(
    'schema', 'schema_version', 'package_id', 'output', 'identity', 'emulator',
    'maple_replay', 'profile', 'ghidra_export', 'program', 'exporter_script',
    'semantic_join', 'publisher', 'semantic_validator', 'package_validator',
    'limits', 'metadata')
$hasInitialState = $jobObject.PSObject.Properties.Name -ccontains 'initial_state'
if ($hasInitialState) { $jobProperties += 'initial_state' }
Assert-Exact $jobObject $jobProperties 'job'
if ([string]$jobObject.schema -cne 'flycast-research-sh4-ghidra-package-job' -or
        [int]$jobObject.schema_version -ne 1) { throw 'Unsupported job schema.' }
$packageId = [string]$jobObject.package_id
if ($packageId -cnotmatch
        '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$') {
    throw 'package_id must be a lowercase UUID.'
}
Assert-Exact $jobObject.output @('accepted_directory') 'job.output'
Assert-Exact $jobObject.profile @('candidate') 'job.profile'
Assert-Exact $jobObject.semantic_join @('candidate', 'maximum_bytes') `
    'job.semantic_join'
Assert-Exact $jobObject.publisher @('script') 'job.publisher'
Assert-Exact $jobObject.semantic_validator @('executable') 'job.semantic_validator'
Assert-Exact $jobObject.package_validator @('executable') 'job.package_validator'
Assert-Exact $jobObject.limits @('validator_timeout_seconds') 'job.limits'
if ($null -eq $jobObject.metadata -or @($jobObject.metadata.PSObject.Properties).Count -gt 64) {
    throw 'job.metadata must be a bounded object.'
}
$maximumJoinBytes = [int64]$jobObject.semantic_join.maximum_bytes
if ($maximumJoinBytes -lt 256 -or $maximumJoinBytes -gt 67108864) {
    throw 'job.semantic_join.maximum_bytes is outside [256, 67108864].'
}
$timeout = [int]$jobObject.limits.validator_timeout_seconds
if ($timeout -lt 1 -or $timeout -gt 3600) { throw 'Validator timeout is invalid.' }

$accepted = Resolve-Absolute ([string]$jobObject.output.accepted_directory) `
    'job.output.accepted_directory'
$parent = Split-Path -Parent $accepted
if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
    throw 'Accepted-directory parent must already exist.'
}
$parent = Resolve-Absolute $parent 'accepted parent' -Existing
Assert-NoReparse $parent 'accepted parent'
$accepted = Join-Path $parent (Split-Path -Leaf $accepted)
if (Test-Path -LiteralPath $accepted) { throw 'Accepted directory already exists.' }
if ($IntegrationTestAbortToken -and
        (Split-Path -Leaf $parent) -cnotmatch
        '^flycast-sh4-ghidra-package-integration-[0-9a-f]+$') {
    throw 'Integration abort tokens require a dedicated integration-test parent.'
}
$candidate = Join-Path $parent ".flycast-research-sh4-ghidra-candidate-$packageId"
$quarantineRoot = Join-Path $parent '.flycast-research-sh4-ghidra-quarantine'
$quarantine = Join-Path $quarantineRoot $packageId
if (Test-Path -LiteralPath $candidate) { throw 'Private candidate already exists.' }
if (Test-Path -LiteralPath $quarantine) { throw 'Quarantine destination already exists.' }
if (Test-Path -LiteralPath $quarantineRoot) {
    $quarantineItem = Get-Item -LiteralPath $quarantineRoot -Force
    if (-not $quarantineItem.PSIsContainer -or
            (($quarantineItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'Quarantine root must be a non-linked directory.'
    }
    Assert-NoReparse $quarantineRoot 'quarantine root'
}

$publisher = Assert-Blob $jobObject.publisher.script 'job.publisher.script'
if (-not [string]::Equals($publisher, (Resolve-Path -LiteralPath $PSCommandPath).Path,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Running publisher differs from job.publisher.script.'
}
Assert-Blob $jobObject.identity 'job.identity' | Out-Null
if ($hasInitialState) {
    Assert-Blob $jobObject.initial_state 'job.initial_state' | Out-Null
}
Assert-Blob $jobObject.emulator 'job.emulator' | Out-Null
Assert-Blob $jobObject.maple_replay 'job.maple_replay' | Out-Null
Assert-Blob $jobObject.profile.candidate 'job.profile.candidate' | Out-Null
Assert-Blob $jobObject.ghidra_export 'job.ghidra_export' | Out-Null
Assert-Blob $jobObject.program 'job.program' | Out-Null
Assert-Blob $jobObject.exporter_script 'job.exporter_script' | Out-Null
Assert-Blob $jobObject.semantic_join.candidate 'job.semantic_join.candidate' | Out-Null
Assert-Blob $jobObject.semantic_validator.executable `
    'job.semantic_validator.executable' | Out-Null
Assert-Blob $jobObject.package_validator.executable `
    'job.package_validator.executable' | Out-Null

$published = $false
try {
    [IO.Directory]::CreateDirectory($candidate) | Out-Null
    Assert-NoReparse $candidate 'private candidate'
    [IO.File]::Copy($jobPath, (Join-Path $candidate 'source-job.json'), $false)
    Copy-Blob $jobObject.identity (Join-Path $candidate 'identity.json') 'job.identity'
    if ($hasInitialState) {
        Copy-Blob $jobObject.initial_state (Join-Path $candidate 'initial-state.state') `
            'job.initial_state'
    }
    Copy-Blob $jobObject.emulator (Join-Path $candidate 'emulator.bin') 'job.emulator'
    Copy-Blob $jobObject.maple_replay (Join-Path $candidate 'maple-replay.fcmt') `
        'job.maple_replay'
    Copy-Blob $jobObject.profile.candidate `
        (Join-Path $candidate 'sh4-profile.fcsh4profile') 'job.profile.candidate'
    Copy-Blob $jobObject.ghidra_export (Join-Path $candidate 'ghidra-export.json') `
        'job.ghidra_export'
    Copy-Blob $jobObject.program (Join-Path $candidate 'program.bin') 'job.program'
    Copy-Blob $jobObject.exporter_script `
        (Join-Path $candidate 'exporter-script.java') 'job.exporter_script'
    Copy-Blob $jobObject.semantic_join.candidate `
        (Join-Path $candidate 'sh4-ghidra-join.json') 'job.semantic_join.candidate'
    Copy-Blob $jobObject.publisher.script (Join-Path $candidate 'publisher.ps1') `
        'job.publisher.script'
    Copy-Blob $jobObject.semantic_validator.executable `
        (Join-Path $candidate 'semantic-validator.exe') `
        'job.semantic_validator.executable'
    Copy-Blob $jobObject.package_validator.executable `
        (Join-Path $candidate 'package-validator.exe') `
        'job.package_validator.executable'

    $localized = $jobObject | ConvertTo-Json -Depth 64 | ConvertFrom-Json -Depth 64
    $localized.identity.path = 'identity.json'
    if ($hasInitialState) { $localized.initial_state.path = 'initial-state.state' }
    $localized.emulator.path = 'emulator.bin'
    $localized.maple_replay.path = 'maple-replay.fcmt'
    $localized.profile.candidate.path = 'sh4-profile.fcsh4profile'
    $localized.ghidra_export.path = 'ghidra-export.json'
    $localized.program.path = 'program.bin'
    $localized.exporter_script.path = 'exporter-script.java'
    $localized.semantic_join.candidate.path = 'sh4-ghidra-join.json'
    $localized.publisher.script.path = 'publisher.ps1'
    $localized.semantic_validator.executable.path = 'semantic-validator.exe'
    $localized.package_validator.executable.path = 'package-validator.exe'
    Write-Json (Join-Path $candidate 'job.json') $localized

    if ($IntegrationTestAbortToken -ceq 'after-staging-v1') {
        throw 'Integration-forced abort after staging.'
    }
    Invoke-Owned (Join-Path $candidate 'semantic-validator.exe') @(
        '--artifact', (Join-Path $candidate 'sh4-ghidra-join.json'),
        '--profile', (Join-Path $candidate 'sh4-profile.fcsh4profile'),
        '--identity', (Join-Path $candidate 'identity.json'),
        '--replay', (Join-Path $candidate 'maple-replay.fcmt'),
        '--ghidra-export', (Join-Path $candidate 'ghidra-export.json'),
        '--program', (Join-Path $candidate 'program.bin'),
        '--script', (Join-Path $candidate 'exporter-script.java'),
        '--max-bytes', [string]$maximumJoinBytes
    ) $timeout 'SH-4/Ghidra semantic validator'
    if ($IntegrationTestAbortToken -ceq 'after-semantic-validation-v1') {
        throw 'Integration-forced abort after semantic validation.'
    }

    $lockedEntries = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $candidate -File |
            Sort-Object Name)) {
        Assert-Regular $file.FullName 'staged package entry' | Out-Null
        $lockedEntries += [ordered]@{
            path = $file.Name
            size = [int64]$file.Length
            sha256 = Get-Digest $file.FullName
        }
    }
    Write-Json (Join-Path $candidate 'package.json') ([ordered]@{
        schema = 'flycast-research-sh4-ghidra-package'
        schema_version = 1
        package_id = $packageId
        locked_entries = $lockedEntries
    })
    Invoke-Owned (Join-Path $candidate 'package-validator.exe') @(
        '--package', $candidate,
        '--receipt', (Join-Path $candidate 'package-validation.json')
    ) $timeout 'SH-4/Ghidra package validator'
    if (-not (Test-Path -LiteralPath (Join-Path $candidate 'package-validation.json') `
            -PathType Leaf)) {
        throw 'Package validator did not issue its receipt.'
    }
    if ($IntegrationTestAbortToken -ceq 'after-validation-v1') {
        throw 'Integration-forced abort after package validation.'
    }
    if (Test-Path -LiteralPath $accepted) {
        throw 'Accepted output appeared before atomic publication.'
    }
    Assert-NoReparse $parent 'accepted parent'
    Assert-NoReparse $candidate 'private candidate'
    [IO.Directory]::Move($candidate, $accepted)
    $published = $true
    Write-Output 'ACCEPTED flycast-research-sh4-ghidra-package-v1'
    Write-Output "package_id=$packageId"
    Write-Output "accepted_directory=$accepted"
}
catch {
    $original = $_.Exception
    if (-not $published -and (Test-Path -LiteralPath $candidate -PathType Container)) {
        try {
            if (-not (Test-Path -LiteralPath $quarantineRoot)) {
                [IO.Directory]::CreateDirectory($quarantineRoot) | Out-Null
            }
            Assert-NoReparse $quarantineRoot 'quarantine root'
            if (Test-Path -LiteralPath $quarantine) {
                throw 'Quarantine destination appeared before failure publication.'
            }
            Write-Json (Join-Path $candidate 'rejection.json') ([ordered]@{
                schema = 'flycast-research-sh4-ghidra-package-quarantine'
                schema_version = 1
                package_id = $packageId
                accepted_directory = $accepted
                failure = $original.Message
            })
            [IO.Directory]::Move($candidate, $quarantine)
        }
        catch {
            throw [InvalidOperationException]::new(
                "$($original.Message) Quarantine failed: $($_.Exception.Message)",
                $original)
        }
    }
    throw $original
}

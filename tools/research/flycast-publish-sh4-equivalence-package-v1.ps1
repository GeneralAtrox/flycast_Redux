[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Job,
    [Parameter(Mandatory = $false)]
    [ValidateSet('', 'after-comparison-v1', 'after-validation-v1')]
    [string]$IntegrationTestAbortToken = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'SH-4 equivalence package publication requires PowerShell 7 or newer.'
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

function Get-CanonicalAbsolutePath([string]$Path, [string]$Field,
        [switch]$RequireExisting) {
    if ([string]::IsNullOrWhiteSpace($Path) -or -not [IO.Path]::IsPathFullyQualified($Path)) {
        throw "$Field must be an absolute path."
    }
    $full = [IO.Path]::GetFullPath($Path)
    if ($RequireExisting) { $full = (Resolve-Path -LiteralPath $full).Path }
    return $full
}

function Assert-NoReparseComponents([string]$Path, [string]$Field) {
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

function Assert-RegularFile([string]$Path, [string]$Field) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or
            (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "$Field must be a non-linked regular file."
    }
    Assert-NoReparseComponents $item.FullName $Field
    return $item
}

function Get-LowerSha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-Blob([object]$Blob, [string]$Field, [string]$BaseDirectory = '') {
    Assert-ExactProperties $Blob @('path', 'size', 'sha256') $Field
    if ([int64]$Blob.size -le 0) { throw "$Field.size must be positive." }
    if ([string]$Blob.sha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Field.sha256 must be lowercase SHA-256."
    }
    $declared = [string]$Blob.path
    if ([IO.Path]::IsPathFullyQualified($declared)) {
        $path = Get-CanonicalAbsolutePath $declared "$Field.path" -RequireExisting
    }
    else {
        if ([string]::IsNullOrWhiteSpace($BaseDirectory) -or
                $declared -match '(^|[\\/])\.\.([\\/]|$)' -or
                $declared -match '(^|[\\/])\.([\\/]|$)') {
            throw "$Field.path must be absolute or a safe relative path."
        }
        $path = (Resolve-Path -LiteralPath (Join-Path $BaseDirectory $declared)).Path
    }
    $item = Assert-RegularFile $path $Field
    if ([int64]$item.Length -ne [int64]$Blob.size -or
            (Get-LowerSha256 $path) -cne [string]$Blob.sha256) {
        throw "$Field bytes do not match their declaration."
    }
    return $path
}

function Copy-ExactBlob([object]$Blob, [string]$Destination, [string]$Field,
        [string]$BaseDirectory = '') {
    $source = Assert-Blob $Blob $Field $BaseDirectory
    [IO.File]::Copy($source, $Destination, $false)
    $copy = Assert-RegularFile $Destination "$Field staged copy"
    if ([int64]$copy.Length -ne [int64]$Blob.size -or
            (Get-LowerSha256 $Destination) -cne [string]$Blob.sha256) {
        throw "$Field changed while being staged."
    }
}

function Get-StreamEvidence([string]$Text) {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = ([BitConverter]::ToString(
            $algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally { $algorithm.Dispose() }
    $preview = if ($Text.Length -gt 4096) { $Text.Substring(0, 4096) } else { $Text }
    return "size=$($bytes.Length), sha256=$digest, preview=$preview"
}

function Invoke-Tool([string]$Executable, [string[]]$Arguments,
        [int]$TimeoutSeconds, [int[]]$AllowedExitCodes, [string]$Name) {
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
        if ($AllowedExitCodes -notcontains $process.ExitCode) {
            throw "$Name exited with $($process.ExitCode). " +
                "stdout($(Get-StreamEvidence $stdout)) stderr($(Get-StreamEvidence $stderr))"
        }
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = $stdout
            Stderr = $stderr
        }
    }
    finally { $process.Dispose() }
}

function Write-Utf8Json([string]$Path, [object]$Value) {
    $bytes = ($Value | ConvertTo-Json -Depth 64) + [Environment]::NewLine
    [IO.File]::WriteAllText($Path, $bytes, [Text.UTF8Encoding]::new($false))
}

$publicationJobPath = Get-CanonicalAbsolutePath $Job 'Job' -RequireExisting
Assert-RegularFile $publicationJobPath 'Job' | Out-Null
$publicationJob = Get-Content -LiteralPath $publicationJobPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 64
Assert-ExactProperties $publicationJob @(
    'schema', 'schema_version', 'package_id', 'output', 'equivalence_job',
    'publisher', 'validator', 'limits', 'metadata'
) 'job'
if ([string]$publicationJob.schema -cne 'flycast-research-sh4-equivalence-package-job' -or
        [int]$publicationJob.schema_version -ne 1) {
    throw 'Unsupported SH-4 equivalence package publication job schema.'
}
$packageId = [string]$publicationJob.package_id
if ($packageId -cnotmatch '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$') {
    throw 'job.package_id must be a lowercase UUID.'
}
Assert-ExactProperties $publicationJob.output @('accepted_directory') 'job.output'
$acceptedDirectory = Get-CanonicalAbsolutePath `
    ([string]$publicationJob.output.accepted_directory) 'job.output.accepted_directory'
$acceptedParent = Split-Path -Parent $acceptedDirectory
if (-not (Test-Path -LiteralPath $acceptedParent -PathType Container)) {
    throw 'The accepted-directory parent must already exist.'
}
$acceptedParent = Get-CanonicalAbsolutePath $acceptedParent 'accepted parent' -RequireExisting
Assert-NoReparseComponents $acceptedParent 'accepted parent'
$acceptedDirectory = Join-Path $acceptedParent (Split-Path -Leaf $acceptedDirectory)
if (Test-Path -LiteralPath $acceptedDirectory) { throw 'Accepted directory already exists.' }
if ($IntegrationTestAbortToken -and
        (Split-Path -Leaf $acceptedParent) -cnotmatch
            '^flycast-sh4-equivalence-package-integration-[0-9a-f]+$') {
    throw 'Integration abort tokens require a dedicated integration-test parent.'
}

Assert-ExactProperties $publicationJob.publisher @('script') 'job.publisher'
Assert-ExactProperties $publicationJob.validator @('executable') 'job.validator'
Assert-ExactProperties $publicationJob.limits @('validator_timeout_seconds') 'job.limits'
$timeout = [int]$publicationJob.limits.validator_timeout_seconds
if ($timeout -lt 1 -or $timeout -gt 3600) {
    throw 'job.limits.validator_timeout_seconds is outside [1, 3600].'
}
$publisherPath = Assert-Blob $publicationJob.publisher.script 'job.publisher.script'
if (-not [string]::Equals($publisherPath, (Resolve-Path -LiteralPath $PSCommandPath).Path,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The running publisher does not match job.publisher.script.'
}
$validatorPath = Assert-Blob $publicationJob.validator.executable `
    'job.validator.executable'
$sourceJobPath = Assert-Blob $publicationJob.equivalence_job 'job.equivalence_job'
$sourceJobDirectory = Split-Path -Parent $sourceJobPath
$sourceJob = Get-Content -LiteralPath $sourceJobPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 64
$sourceJobProperties = @(
    'schema', 'schema_version', 'job_id', 'interpreter', 'dynarec', 'emulator',
    'replay', 'manifest_set', 'manifests', 'comparator', 'limits', 'metadata'
)
$hasInitialState = $sourceJob.PSObject.Properties.Name -ccontains 'initial_state'
if ($hasInitialState) { $sourceJobProperties += 'initial_state' }
Assert-ExactProperties $sourceJob $sourceJobProperties 'equivalence job'
if ([string]$sourceJob.schema -cne 'flycast-research-sh4-equivalence-job' -or
        [int]$sourceJob.schema_version -ne 1) {
    throw 'Unsupported SH-4 equivalence job schema.'
}
Assert-ExactProperties $sourceJob.interpreter @('identity', 'trace') `
    'equivalence job.interpreter'
Assert-ExactProperties $sourceJob.dynarec @('identity', 'trace') 'equivalence job.dynarec'
Assert-ExactProperties $sourceJob.comparator @('executable') 'equivalence job.comparator'

$candidateDirectory = Join-Path $acceptedParent `
    ".flycast-research-sh4-equivalence-candidate-$packageId"
$quarantineRoot = Join-Path $acceptedParent '.flycast-research-sh4-equivalence-quarantine'
$quarantineDirectory = Join-Path $quarantineRoot $packageId
if (Test-Path -LiteralPath $candidateDirectory) { throw 'Private candidate already exists.' }
if (Test-Path -LiteralPath $quarantineDirectory) { throw 'Quarantine destination already exists.' }
if (Test-Path -LiteralPath $quarantineRoot) {
    $quarantineItem = Get-Item -LiteralPath $quarantineRoot -Force
    if (-not $quarantineItem.PSIsContainer -or
            (($quarantineItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'The quarantine root must be a non-linked directory.'
    }
    Assert-NoReparseComponents $quarantineRoot 'quarantine root'
}

$published = $false
try {
    [IO.Directory]::CreateDirectory($candidateDirectory) | Out-Null
    [IO.Directory]::CreateDirectory((Join-Path $candidateDirectory 'manifests')) | Out-Null
    Assert-NoReparseComponents $candidateDirectory 'private candidate'
    [IO.File]::Copy($publicationJobPath,
        (Join-Path $candidateDirectory 'publication-job.json'), $false)
    [IO.File]::Copy($sourceJobPath, (Join-Path $candidateDirectory 'source-job.json'), $false)
    Copy-ExactBlob $publicationJob.publisher.script `
        (Join-Path $candidateDirectory 'publisher.ps1') 'job.publisher.script'
    Copy-ExactBlob $publicationJob.validator.executable `
        (Join-Path $candidateDirectory 'package-validator.exe') 'job.validator.executable'

    Copy-ExactBlob $sourceJob.interpreter.identity `
        (Join-Path $candidateDirectory 'interpreter-identity.json') `
        'equivalence job.interpreter.identity' $sourceJobDirectory
    Copy-ExactBlob $sourceJob.interpreter.trace `
        (Join-Path $candidateDirectory 'interpreter.fcso') `
        'equivalence job.interpreter.trace' $sourceJobDirectory
    Copy-ExactBlob $sourceJob.dynarec.identity `
        (Join-Path $candidateDirectory 'dynarec-identity.json') `
        'equivalence job.dynarec.identity' $sourceJobDirectory
    Copy-ExactBlob $sourceJob.dynarec.trace `
        (Join-Path $candidateDirectory 'dynarec.fcso') `
        'equivalence job.dynarec.trace' $sourceJobDirectory
    Copy-ExactBlob $sourceJob.emulator (Join-Path $candidateDirectory 'emulator.bin') `
        'equivalence job.emulator' $sourceJobDirectory
    Copy-ExactBlob $sourceJob.replay (Join-Path $candidateDirectory 'maple-replay.fcmt') `
        'equivalence job.replay' $sourceJobDirectory
    if ($hasInitialState) {
        Copy-ExactBlob $sourceJob.initial_state `
            (Join-Path $candidateDirectory 'initial-state.state') `
            'equivalence job.initial_state' $sourceJobDirectory
    }
    Copy-ExactBlob $sourceJob.manifest_set `
        (Join-Path $candidateDirectory 'manifest-set.json') `
        'equivalence job.manifest_set' $sourceJobDirectory
    Copy-ExactBlob $sourceJob.comparator.executable `
        (Join-Path $candidateDirectory 'comparator.exe') `
        'equivalence job.comparator.executable' $sourceJobDirectory

    $previousManifestKey = ''
    $manifestNames = [Collections.Generic.HashSet[string]]::new(
        [StringComparer]::Ordinal)
    for ($index = 0; $index -lt @($sourceJob.manifests).Count; ++$index) {
        $manifest = @($sourceJob.manifests)[$index]
        Assert-ExactProperties $manifest @('kind', 'name', 'source') `
            "equivalence job.manifests[$index]"
        $kind = [string]$manifest.kind
        $name = [string]$manifest.name
        if ($kind -cnotmatch '^[a-z0-9-]{1,64}$' -or
                $name -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$') {
            throw "equivalence job.manifests[$index] has an invalid kind or name."
        }
        $key = "$kind`n$name"
        if ($previousManifestKey -and
                [string]::CompareOrdinal($key, $previousManifestKey) -le 0) {
            throw 'equivalence job manifests are not uniquely ordered.'
        }
        $previousManifestKey = $key
        if (-not $manifestNames.Add($name)) { throw 'Manifest package names are not unique.' }
        Copy-ExactBlob $manifest.source (Join-Path $candidateDirectory "manifests/$name") `
            "equivalence job.manifests[$index].source" $sourceJobDirectory
    }

    $localized = $sourceJob | ConvertTo-Json -Depth 64 | ConvertFrom-Json -Depth 64
    $localized.interpreter.identity.path = 'interpreter-identity.json'
    $localized.interpreter.trace.path = 'interpreter.fcso'
    $localized.dynarec.identity.path = 'dynarec-identity.json'
    $localized.dynarec.trace.path = 'dynarec.fcso'
    $localized.emulator.path = 'emulator.bin'
    $localized.replay.path = 'maple-replay.fcmt'
    if ($hasInitialState) { $localized.initial_state.path = 'initial-state.state' }
    $localized.manifest_set.path = 'manifest-set.json'
    $localized.comparator.executable.path = 'comparator.exe'
    for ($index = 0; $index -lt @($localized.manifests).Count; ++$index) {
        @($localized.manifests)[$index].source.path =
            "manifests/$([string]@($localized.manifests)[$index].name)"
    }
    $localizedJobPath = Join-Path $candidateDirectory 'job.json'
    Write-Utf8Json $localizedJobPath $localized

    $comparison = Invoke-Tool -Executable (Join-Path $candidateDirectory 'comparator.exe') `
        -Arguments @(
        '--job', $localizedJobPath, '--report',
        (Join-Path $candidateDirectory 'equivalence-report.json')
    ) -TimeoutSeconds $timeout -AllowedExitCodes @(0, 3) `
        -Name 'SH-4 equivalence comparator'
    if ($comparison.ExitCode -eq 3) {
        throw 'SH-4 interpreter and dynarec traces diverged; candidate is quarantined.'
    }
    if ($IntegrationTestAbortToken -ceq 'after-comparison-v1') {
        throw 'Integration-forced abort after equivalence comparison.'
    }
    if (Test-Path -LiteralPath $acceptedDirectory) {
        throw 'Accepted output appeared before package validation.'
    }

    $lockedEntries = @()
    foreach ($file in @(Get-ChildItem -LiteralPath $candidateDirectory -File -Recurse |
            Sort-Object { $_.FullName.Substring($candidateDirectory.Length + 1) })) {
        Assert-RegularFile $file.FullName 'staged package entry' | Out-Null
        $relative = $file.FullName.Substring($candidateDirectory.Length + 1).Replace('\', '/')
        $lockedEntries += [ordered]@{
            path = $relative
            size = [int64]$file.Length
            sha256 = Get-LowerSha256 $file.FullName
        }
    }
    $packageManifest = [ordered]@{
        schema = 'flycast-research-sh4-equivalence-package'
        schema_version = 1
        package_id = $packageId
        locked_entries = $lockedEntries
    }
    Write-Utf8Json (Join-Path $candidateDirectory 'package.json') $packageManifest

    Invoke-Tool -Executable (Join-Path $candidateDirectory 'package-validator.exe') `
        -Arguments @(
        '--package', $candidateDirectory, '--receipt',
        (Join-Path $candidateDirectory 'package-validation.json')
    ) -TimeoutSeconds $timeout -AllowedExitCodes @(0) `
        -Name 'SH-4 equivalence package validator' | Out-Null
    $receiptPath = Join-Path $candidateDirectory 'package-validation.json'
    if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
        throw 'The package validator did not issue its validation receipt.'
    }
    if ($IntegrationTestAbortToken -ceq 'after-validation-v1') {
        throw 'Integration-forced abort after equivalence package validation.'
    }
    if (Test-Path -LiteralPath $acceptedDirectory) {
        throw 'Accepted output appeared before atomic publication.'
    }
    Assert-NoReparseComponents $acceptedParent 'accepted parent'
    Assert-NoReparseComponents $candidateDirectory 'private candidate'
    [IO.Directory]::Move($candidateDirectory, $acceptedDirectory)
    $published = $true
    Write-Host 'ACCEPTED flycast-research-sh4-equivalence-package-v1'
    Write-Host "package_id=$packageId"
    Write-Host "accepted_directory=$acceptedDirectory"
}
catch {
    $original = $_.Exception
    if (-not $published -and (Test-Path -LiteralPath $candidateDirectory -PathType Container)) {
        try {
            if (-not (Test-Path -LiteralPath $quarantineRoot)) {
                [IO.Directory]::CreateDirectory($quarantineRoot) | Out-Null
            }
            $quarantineItem = Get-Item -LiteralPath $quarantineRoot -Force
            if (-not $quarantineItem.PSIsContainer -or
                    (($quarantineItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
                throw 'The quarantine root must be a non-linked directory.'
            }
            Assert-NoReparseComponents $quarantineRoot 'quarantine root'
            if (Test-Path -LiteralPath $quarantineDirectory) {
                throw 'Quarantine destination appeared before failure publication.'
            }
            Write-Utf8Json (Join-Path $candidateDirectory 'rejection.json') ([ordered]@{
                schema = 'flycast-research-sh4-equivalence-package-quarantine'
                schema_version = 1
                package_id = $packageId
                accepted_directory = $acceptedDirectory
                failure = $original.Message
            })
            [IO.Directory]::Move($candidateDirectory, $quarantineDirectory)
        }
        catch {
            throw [InvalidOperationException]::new(
                "$($original.Message) Quarantine failed: $($_.Exception.Message)", $original)
        }
    }
    throw $original
}

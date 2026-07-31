[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Job,

    [Parameter(Mandatory = $false)]
    [ValidateSet('', 'after-staging-v2', 'after-validation-v2')]
    [string]$IntegrationTestAbortToken = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'Capture-package v2 publication requires PowerShell 7 or newer.'
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
    if ($RequireExisting) {
        $full = (Resolve-Path -LiteralPath $full).Path
    }
    return $full
}

function Get-LowerSha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-StreamEvidence([string]$Text) {
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($Text)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $digest = ([BitConverter]::ToString(
            $algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally { $algorithm.Dispose() }
    $maximumPreviewCharacters = 4096
    $truncated = $Text.Length -gt $maximumPreviewCharacters
    $preview = if ($truncated) { $Text.Substring(0, $maximumPreviewCharacters) } else { $Text }
    return [pscustomobject]@{
        size = [int64]$bytes.Length
        sha256 = $digest
        truncated = $truncated
        preview = $preview
    }
}

function Format-StreamEvidence([string]$Name, [string]$Text) {
    $evidence = Get-StreamEvidence $Text
    return "$Name(size=$($evidence.size), sha256=$($evidence.sha256), " +
        "truncated=$($evidence.truncated)): $($evidence.preview)"
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

function Test-SameOrDescendant([string]$Parent, [string]$Value) {
    if ([string]::Equals($Parent, $Value, [StringComparison]::OrdinalIgnoreCase)) {
        return $true
    }
    $prefix = $Parent.TrimEnd([IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    return $Value.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)
}

function Assert-RegularFile([string]$Path, [string]$Field) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "$Field must be a non-linked regular file."
    }
    Assert-NoReparseComponents $item.FullName $Field
    return $item
}

function Assert-Blob([object]$Blob, [string]$Field, [switch]$Absolute) {
    Assert-ExactProperties $Blob @('path', 'size', 'sha256') $Field
    if ($Absolute -and -not [IO.Path]::IsPathFullyQualified([string]$Blob.path)) {
        throw "$Field.path must be absolute."
    }
    if ([int64]$Blob.size -le 0) { throw "$Field.size must be positive." }
    if ([string]$Blob.sha256 -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Field.sha256 must be lowercase SHA-256."
    }
    if ($Absolute) {
        $path = Get-CanonicalAbsolutePath ([string]$Blob.path) "$Field.path" -RequireExisting
        $item = Assert-RegularFile $path $Field
        if ([int64]$item.Length -ne [int64]$Blob.size -or
                (Get-LowerSha256 $path) -cne [string]$Blob.sha256) {
            throw "$Field bytes do not match their declaration."
        }
        return $path
    }
    return [string]$Blob.path
}

function Copy-ExactBlob([object]$Blob, [string]$Destination, [string]$Field) {
    $source = Assert-Blob $Blob $Field -Absolute
    [IO.File]::Copy($source, $Destination, $false)
    $copied = Assert-RegularFile $Destination "$Field staged copy"
    if ([int64]$copied.Length -ne [int64]$Blob.size -or
            (Get-LowerSha256 $Destination) -cne [string]$Blob.sha256) {
        throw "$Field staged bytes changed during copy."
    }
}

function Invoke-Validator([string]$Validator, [string]$Package, [int]$TimeoutSeconds) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Validator
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $start.StandardErrorEncoding = [Text.Encoding]::UTF8
    $start.ArgumentList.Add('--package')
    $start.ArgumentList.Add($Package)
    $start.ArgumentList.Add('--receipt')
    $start.ArgumentList.Add((Join-Path $Package 'capture-package-validation.json'))
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'Capture-package v2 validator did not start.' }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try { $process.Kill($true) } catch { }
            try { $process.WaitForExit() } catch { }
            $stdout = $stdoutTask.GetAwaiter().GetResult()
            $stderr = $stderrTask.GetAwaiter().GetResult()
            throw ('Capture-package v2 validator timed out. ' +
                (Format-StreamEvidence 'stdout' $stdout) + ' ' +
                (Format-StreamEvidence 'stderr' $stderr))
        }
        $process.WaitForExit()
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            throw ("Capture-package v2 validator rejected the candidate with exit code " +
                "$($process.ExitCode). " + (Format-StreamEvidence 'stdout' $stdout) + ' ' +
                (Format-StreamEvidence 'stderr' $stderr))
        }
    }
    finally {
        $process.Dispose()
    }
}

$jobPath = Get-CanonicalAbsolutePath $Job 'Job' -RequireExisting
Assert-RegularFile $jobPath 'Job' | Out-Null
$jobObject = Get-Content -LiteralPath $jobPath -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 64
Assert-ExactProperties $jobObject @(
    'schema', 'schema_version', 'package_id', 'output', 'base_capture_v1',
    'identity', 'static_analysis', 'artifacts', 'publisher', 'validator', 'limits', 'metadata'
) 'job'
if ([string]$jobObject.schema -cne 'flycast-research-capture-package-job' -or
        [int]$jobObject.schema_version -ne 2) {
    throw 'Unsupported capture-package job schema.'
}
$packageId = [string]$jobObject.package_id
if ($packageId -cnotmatch '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$') {
    throw 'job.package_id must be a lowercase UUID.'
}
Assert-ExactProperties $jobObject.output @('accepted_directory') 'job.output'
$acceptedDirectory = Get-CanonicalAbsolutePath ([string]$jobObject.output.accepted_directory) `
    'job.output.accepted_directory'
$acceptedParent = Split-Path -Parent $acceptedDirectory
if (-not (Test-Path -LiteralPath $acceptedParent -PathType Container)) {
    throw 'The accepted-directory parent must already exist.'
}
$acceptedParent = Get-CanonicalAbsolutePath $acceptedParent 'accepted-directory parent' -RequireExisting
$null = Assert-NoReparseComponents $acceptedParent 'accepted-directory parent'
$acceptedDirectory = Join-Path $acceptedParent (Split-Path -Leaf $acceptedDirectory)
if (Test-Path -LiteralPath $acceptedDirectory) {
    throw "Accepted directory already exists: $acceptedDirectory"
}
if ($IntegrationTestAbortToken -and
        (Split-Path -Leaf $acceptedParent) -cnotmatch '^flycast-research-package-v2-integration-[0-9a-f]+$') {
    throw 'Integration abort tokens require a dedicated integration-test parent.'
}

$candidateDirectory = Join-Path $acceptedParent ".flycast-research-package-v2-candidate-$packageId"
$quarantineRoot = Join-Path $acceptedParent '.flycast-research-package-v2-quarantine'
$quarantineDirectory = Join-Path $quarantineRoot $packageId
if (Test-Path -LiteralPath $candidateDirectory) { throw 'Private candidate already exists.' }
if (Test-Path -LiteralPath $quarantineRoot) {
    $quarantineRootItem = Get-Item -LiteralPath $quarantineRoot -Force
    if (-not $quarantineRootItem.PSIsContainer -or
            (($quarantineRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'The quarantine root must be a non-linked directory.'
    }
    Assert-NoReparseComponents $quarantineRoot 'quarantine root'
}
if (Test-Path -LiteralPath $quarantineDirectory) { throw 'Quarantine destination already exists.' }

Assert-ExactProperties $jobObject.base_capture_v1 @('accepted_directory', 'entries') `
    'job.base_capture_v1'
$baseDirectory = Get-CanonicalAbsolutePath ([string]$jobObject.base_capture_v1.accepted_directory) `
    'job.base_capture_v1.accepted_directory' -RequireExisting
if (Test-SameOrDescendant $baseDirectory $acceptedDirectory) {
    throw 'The v2 accepted directory may not be inside the immutable v1 base package.'
}
if (@($jobObject.base_capture_v1.entries).Count -ne 5) {
    throw 'job.base_capture_v1.entries must contain exactly five entries.'
}
foreach ($entry in @($jobObject.base_capture_v1.entries)) {
    Assert-Blob $entry 'job.base_capture_v1 entry' | Out-Null
    if ([string]$entry.path -notmatch '^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$') {
        throw 'A base-capture entry has an unsafe path.'
    }
    $entryPath = Join-Path $baseDirectory ([string]$entry.path)
    $item = Assert-RegularFile $entryPath 'base-capture entry'
    if ([int64]$item.Length -ne [int64]$entry.size -or
            (Get-LowerSha256 $entryPath) -cne [string]$entry.sha256) {
        throw 'A base-capture entry does not match its declaration.'
    }
}

$identityPath = Assert-Blob $jobObject.identity 'job.identity' -Absolute
Assert-ExactProperties $jobObject.static_analysis @('export', 'program', 'exporter_script') `
    'job.static_analysis'
$staticExportPath = Assert-Blob $jobObject.static_analysis.export `
    'job.static_analysis.export' -Absolute
Assert-Blob $jobObject.static_analysis.program 'job.static_analysis.program' -Absolute | Out-Null
Assert-Blob $jobObject.static_analysis.exporter_script `
    'job.static_analysis.exporter_script' -Absolute | Out-Null
Assert-ExactProperties $jobObject.publisher @('script') 'job.publisher'
$publisherPath = Assert-Blob $jobObject.publisher.script 'job.publisher.script' -Absolute
if (-not [string]::Equals($publisherPath, (Resolve-Path -LiteralPath $PSCommandPath).Path,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The running publisher does not match job.publisher.script.'
}
Assert-ExactProperties $jobObject.validator @('executable') 'job.validator'
$validatorPath = Assert-Blob $jobObject.validator.executable 'job.validator.executable' -Absolute
Assert-ExactProperties $jobObject.limits @('validator_timeout_seconds') 'job.limits'
$validatorTimeout = [int]$jobObject.limits.validator_timeout_seconds
if ($validatorTimeout -lt 1 -or $validatorTimeout -gt 3600) {
    throw 'job.limits.validator_timeout_seconds is outside [1, 3600].'
}

$artifacts = @($jobObject.artifacts)
if ($artifacts.Count -lt 1 -or $artifacts.Count -gt 2) {
    throw 'job.artifacts count is outside [1, 2].'
}
$artifactDestinations = [Collections.Generic.List[object]]::new()
$previousKind = ''
foreach ($artifact in $artifacts) {
    Assert-ExactProperties $artifact @('kind', 'artifact', 'manifest', 'maximum_bytes') `
        'job.artifacts entry'
    $kind = [string]$artifact.kind
    if ($previousKind -and [string]::CompareOrdinal($kind, $previousKind) -le 0) {
        throw 'job.artifacts must be uniquely ordered by kind.'
    }
    $previousKind = $kind
    if ($kind -ceq 'memory-ranges-v1') {
        $artifactName = 'memory-ranges.fcmr'
        $manifestName = 'memory-ranges-manifest.json'
        $maximum = 1074790400L
    }
    elseif ($kind -ceq 'sh4-events-v1') {
        $artifactName = 'sh4-events.fcsh4'
        $manifestName = 'sh4-events-manifest.json'
        $maximum = 268435456L
    }
    else { throw "Unsupported artifact kind: $kind" }
    if ([int64]$artifact.maximum_bytes -lt 1 -or [int64]$artifact.maximum_bytes -gt $maximum) {
        throw "$kind maximum_bytes exceeds its v1 implementation bound."
    }
    Assert-Blob $artifact.artifact "job artifact $kind" -Absolute | Out-Null
    Assert-Blob $artifact.manifest "job manifest $kind" -Absolute | Out-Null
    if ([int64]$artifact.artifact.size -gt [int64]$artifact.maximum_bytes) {
        throw "$kind artifact exceeds its declared maximum_bytes."
    }
    $artifactDestinations.Add([pscustomobject]@{
        value = $artifact
        artifact_name = $artifactName
        manifest_name = $manifestName
    })
}

$published = $false
try {
    [IO.Directory]::CreateDirectory($candidateDirectory) | Out-Null
    Assert-NoReparseComponents $candidateDirectory 'private candidate'
    [IO.File]::Copy($jobPath, (Join-Path $candidateDirectory 'job.json'), $false)
    Copy-ExactBlob $jobObject.identity (Join-Path $candidateDirectory 'identity.json') `
        'job.identity'
    Copy-ExactBlob $jobObject.static_analysis.export `
        (Join-Path $candidateDirectory 'static-analysis.json') 'job.static_analysis.export'
    foreach ($destination in $artifactDestinations) {
        Copy-ExactBlob $destination.value.artifact `
            (Join-Path $candidateDirectory $destination.artifact_name) `
            "job artifact $($destination.value.kind)"
        Copy-ExactBlob $destination.value.manifest `
            (Join-Path $candidateDirectory $destination.manifest_name) `
            "job manifest $($destination.value.kind)"
    }
    if ($IntegrationTestAbortToken -ceq 'after-staging-v2') {
        throw 'Integration-forced abort after v2 staging.'
    }
    if (Test-Path -LiteralPath $acceptedDirectory) {
        throw 'Accepted output appeared before validation.'
    }
    Invoke-Validator $validatorPath $candidateDirectory $validatorTimeout
    if (-not (Test-Path -LiteralPath (Join-Path $candidateDirectory `
            'capture-package-validation.json') -PathType Leaf)) {
        throw 'The independent validator did not issue the validation receipt.'
    }
    if ($IntegrationTestAbortToken -ceq 'after-validation-v2') {
        throw 'Integration-forced abort after v2 validation.'
    }
    if (Test-Path -LiteralPath $acceptedDirectory) {
        throw 'Accepted output appeared before atomic publication.'
    }
    Assert-NoReparseComponents $acceptedParent 'accepted-directory parent'
    Assert-NoReparseComponents $candidateDirectory 'private candidate'
    [IO.Directory]::Move($candidateDirectory, $acceptedDirectory)
    $published = $true
    Write-Host "ACCEPTED flycast-research-capture-package-v2"
    Write-Host "package_id=$packageId"
    Write-Host "accepted_directory=$acceptedDirectory"
}
catch {
    $originalException = $_.Exception
    if (-not $published -and (Test-Path -LiteralPath $candidateDirectory -PathType Container)) {
        try {
            if (-not (Test-Path -LiteralPath $quarantineRoot)) {
                [IO.Directory]::CreateDirectory($quarantineRoot) | Out-Null
            }
            $quarantineRootItem = Get-Item -LiteralPath $quarantineRoot -Force
            if (-not $quarantineRootItem.PSIsContainer -or
                    (($quarantineRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
                throw 'The quarantine root must be a non-linked directory.'
            }
            Assert-NoReparseComponents $quarantineRoot 'quarantine root'
            if (Test-Path -LiteralPath $quarantineDirectory) {
                throw 'The quarantine destination appeared before candidate quarantine.'
            }
            $failure = [ordered]@{
                schema = 'flycast-research-capture-package-quarantine'
                schema_version = 2
                package_id = $packageId
                accepted_directory = $acceptedDirectory
                failure = $originalException.Message
            }
            $failureBytes = ($failure | ConvertTo-Json -Depth 8) + [Environment]::NewLine
            [IO.File]::WriteAllText((Join-Path $candidateDirectory 'rejection.json'),
                $failureBytes, [Text.UTF8Encoding]::new($false))
            [IO.Directory]::Move($candidateDirectory, $quarantineDirectory)
        }
        catch {
            throw [InvalidOperationException]::new(
                "$($originalException.Message) Quarantine failed: $($_.Exception.Message)",
                $originalException)
        }
    }
    throw $originalException
}

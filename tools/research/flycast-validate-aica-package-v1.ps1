#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Package,
    [string]$TrustedArtifactValidator = '',
    [ValidateRange(1, 3600)][int]$ValidatorTimeoutSeconds = 300
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

function Get-Blob([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or
            (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "Package entry is not a non-linked regular file: $Path"
    }
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
        throw "$Field bytes differ from the package declaration."
    }
}

function Invoke-ArtifactValidator([string]$Executable, [string[]]$Arguments) {
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
        if (-not $process.Start()) { throw 'AICA artifact validator did not start.' }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($ValidatorTimeoutSeconds * 1000)) {
            try { $process.Kill($true) } catch { }
            try { $process.WaitForExit() } catch { }
            throw 'AICA artifact validator timed out.'
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult().Trim()
        $stderr = $stderrTask.GetAwaiter().GetResult().Trim()
        if ($process.ExitCode -ne 0) {
            throw "AICA artifact validator rejected the artifact: $stdout $stderr"
        }
        return $stdout | ConvertFrom-Json -Depth 16
    }
    finally { $process.Dispose() }
}

$root = (Resolve-Path -LiteralPath ([IO.Path]::GetFullPath($Package))).Path
$rootItem = Get-Item -LiteralPath $root -Force
if (-not $rootItem.PSIsContainer -or
        (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
    throw 'Package must be a non-linked directory.'
}
$artifactValidator = Join-Path $root 'artifact-validator.exe'
if (-not [string]::IsNullOrWhiteSpace($TrustedArtifactValidator)) {
    $artifactValidator = (Resolve-Path -LiteralPath `
        ([IO.Path]::GetFullPath($TrustedArtifactValidator))).Path
    $trustedItem = Get-Item -LiteralPath $artifactValidator -Force
    if ($trustedItem.PSIsContainer -or
            (($trustedItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw 'Trusted AICA artifact validator must be a non-linked regular file.'
    }
}
$expectedNames = @(
    'aica.fcaica', 'artifact-validator.exe', 'flycast.exe', 'identity.json',
    'initial-state.state', 'maple-replay.fcmt', 'package-validation.json',
    'package-validator.ps1', 'package.json', 'publisher.ps1'
)
$actualNames = @(Get-ChildItem -LiteralPath $root -Force | Sort-Object Name |
    ForEach-Object Name)
if ($actualNames.Count -ne $expectedNames.Count) {
    throw 'AICA package inventory count is incorrect.'
}
for ($index = 0; $index -lt $expectedNames.Count; ++$index) {
    if ($actualNames[$index] -cne $expectedNames[$index]) {
        throw 'AICA package inventory is not exact.'
    }
}

$manifest = Get-Content -LiteralPath (Join-Path $root 'package.json') -Raw `
    -Encoding UTF8 | ConvertFrom-Json -Depth 32
$receipt = Get-Content -LiteralPath (Join-Path $root 'package-validation.json') -Raw `
    -Encoding UTF8 | ConvertFrom-Json -Depth 32
Assert-Exact $manifest @('schema', 'schema_version', 'package_id',
    'accepted_directory', 'entries') 'package'
Assert-Exact $receipt @('schema', 'schema_version', 'package_id', 'status',
    'artifact_validation', 'entries') 'receipt'
if ([string]$manifest.schema -cne 'flycast-research-aica-package' -or
        [int]$manifest.schema_version -ne 1 -or
        [string]$receipt.schema -cne 'flycast-research-aica-package-validation' -or
        [int]$receipt.schema_version -ne 1 -or
        [string]$receipt.status -cne 'accepted' -or
        [string]$receipt.package_id -cne [string]$manifest.package_id) {
    throw 'AICA package manifest/receipt binding is invalid.'
}
$accepted = [IO.Path]::GetFullPath([string]$manifest.accepted_directory)
$candidate = Join-Path (Split-Path -Parent $accepted) `
    ".flycast-research-aica-candidate-$([string]$manifest.package_id)"
if (-not ([string]::Equals($root, $accepted, [StringComparison]::OrdinalIgnoreCase) -or
        [string]::Equals($root, $candidate, [StringComparison]::OrdinalIgnoreCase))) {
    throw 'Package is outside its declared candidate/accepted path.'
}
$mapping = [ordered]@{
    artifact = 'aica.fcaica'
    identity = 'identity.json'
    initial_state = 'initial-state.state'
    maple_replay = 'maple-replay.fcmt'
    flycast = 'flycast.exe'
    artifact_validator = 'artifact-validator.exe'
    package_validator = 'package-validator.ps1'
    publisher = 'publisher.ps1'
}
foreach ($key in $mapping.Keys) {
    $property = $manifest.entries.PSObject.Properties[$key]
    if ($null -eq $property) { throw "package.entries.$key is missing." }
    Assert-Blob $property.Value (Join-Path $root $mapping[$key]) `
        "package.entries.$key"
}
$receiptByName = @{}
foreach ($entry in @($receipt.entries)) {
    if ($null -eq $entry -or $receiptByName.ContainsKey([string]$entry.name)) {
        throw 'Validation receipt inventory is invalid or duplicated.'
    }
    $receiptByName[[string]$entry.name] = $entry
}
$receiptNames = @('aica.fcaica', 'identity.json', 'initial-state.state',
    'maple-replay.fcmt', 'flycast.exe', 'artifact-validator.exe',
    'package-validator.ps1', 'publisher.ps1', 'package.json')
if ($receiptByName.Count -ne $receiptNames.Count) {
    throw 'Validation receipt inventory is incomplete.'
}
foreach ($name in $receiptNames) {
    if (-not $receiptByName.ContainsKey($name)) {
        throw "Validation receipt is missing $name."
    }
    Assert-Blob $receiptByName[$name] (Join-Path $root $name) `
        "receipt.entries[$name]"
}
$identity = Get-Content -LiteralPath (Join-Path $root 'identity.json') -Raw `
    -Encoding UTF8 | ConvertFrom-Json -Depth 64
Assert-Blob $identity.emulator.executable (Join-Path $root 'flycast.exe') `
    'identity.emulator.executable'
Assert-Blob $identity.initial_state.blob (Join-Path $root 'initial-state.state') `
    'identity.initial_state.blob'
$validation = Invoke-ArtifactValidator $artifactValidator @(
    (Join-Path $root 'aica.fcaica'),
    (Join-Path $root 'identity.json'),
    (Join-Path $root 'maple-replay.fcmt')
)
if (($validation | ConvertTo-Json -Depth 16 -Compress) -cne
        ($receipt.artifact_validation | ConvertTo-Json -Depth 16 -Compress)) {
    throw 'AICA artifact validation differs from the deterministic receipt.'
}
Write-Output "ACCEPTED flycast-research-aica-package-v1 $root"

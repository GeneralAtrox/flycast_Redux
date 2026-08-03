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
        if ($Names -cnotcontains $name) { throw "$Field has unknown property '$name'." }
    }
}
function Get-Blob([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer -or
            (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "Package entry is not a non-linked regular file: $Path"
    }
    return [ordered]@{ size = [int64]$item.Length; sha256 =
        (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
}
function Assert-Blob([object]$Expected, [string]$Path, [string]$Field) {
    $actual = Get-Blob $Path
    if ($null -eq $Expected -or [int64]$Expected.size -ne [int64]$actual.size -or
            [string]$Expected.sha256 -cne [string]$actual.sha256) {
        throw "$Field bytes differ from their declaration."
    }
}
function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable; $start.UseShellExecute = $false
    $start.CreateNoWindow = $true; $start.WindowStyle = 'Hidden'
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'Artifact validator did not start.' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($ValidatorTimeoutSeconds * 1000)) {
            try { $process.Kill($true) } catch { }; throw 'Artifact validator timed out.'
        }
        $out = $stdout.GetAwaiter().GetResult().Trim()
        $err = $stderr.GetAwaiter().GetResult().Trim()
        if ($process.ExitCode -ne 0) { throw "Artifact rejected: $out $err" }
        return $out
    }
    finally { $process.Dispose() }
}

$root = (Resolve-Path -LiteralPath ([IO.Path]::GetFullPath($Package))).Path
$rootItem = Get-Item -LiteralPath $root -Force
if (-not $rootItem.PSIsContainer -or
        (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
    throw 'Package must be a non-linked directory.'
}
$identityPath = Join-Path $root 'identity.json'
if (-not (Test-Path -LiteralPath $identityPath -PathType Leaf)) {
    throw 'Package identity is missing.'
}
$identity = Get-Content -LiteralPath $identityPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 64
$deviceNames = @()
for ($deviceIndex = 0; $deviceIndex -lt @($identity.persistent_devices).Count;
        ++$deviceIndex) {
    $device = @($identity.persistent_devices)[$deviceIndex]
    $deviceNames += 'persistent-device-{0:D3}-vmu-{1}-{2}.bin' -f `
        $deviceIndex, [int]$device.bus, [int]$device.port
}
$names = @('artifact-validator.exe', 'dc_boot.bin', 'dc_nvmem.bin',
    'flycast.exe', 'gdrom-hardware.fcgd', 'identity.json',
    'maple-record-identity.json', 'maple-replay.fcmt', 'package-validation.json',
    'package-validator.ps1', 'package.json', 'publisher.ps1') + $deviceNames |
    Sort-Object
$actual = @(Get-ChildItem -LiteralPath $root -Force | Sort-Object Name |
    ForEach-Object Name)
if ($actual.Count -ne $names.Count) { throw 'Package inventory count is incorrect.' }
for ($index = 0; $index -lt $names.Count; ++$index) {
    if ($actual[$index] -cne $names[$index]) { throw 'Package inventory is not exact.' }
}
$manifest = Get-Content -LiteralPath (Join-Path $root 'package.json') -Raw `
    -Encoding UTF8 | ConvertFrom-Json -Depth 32
$receipt = Get-Content -LiteralPath (Join-Path $root 'package-validation.json') -Raw `
    -Encoding UTF8 | ConvertFrom-Json -Depth 32
Assert-Exact $manifest @('schema', 'schema_version', 'package_id',
    'accepted_directory', 'entries') 'package'
Assert-Exact $receipt @('schema', 'schema_version', 'package_id', 'status',
    'validator_output', 'entries') 'receipt'
if ([string]$manifest.schema -cne 'flycast-research-gdrom-hardware-package' -or
        [int]$manifest.schema_version -ne 2 -or
        [string]$receipt.schema -cne 'flycast-research-gdrom-hardware-package-validation' -or
        [int]$receipt.schema_version -ne 2 -or
        [string]$receipt.status -cne 'accepted' -or
        [string]$receipt.package_id -cne [string]$manifest.package_id) {
    throw 'Package manifest/receipt binding is invalid.'
}
$packageId = [string]$manifest.package_id
if ($packageId -cnotmatch
        '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$') {
    throw 'Package ID is invalid.'
}
$accepted = [IO.Path]::GetFullPath([string]$manifest.accepted_directory)
$candidate = Join-Path (Split-Path -Parent $accepted) `
    ".flycast-research-gdrom-hardware-candidate-$packageId"
if (-not ([string]::Equals($root, $accepted,
                [StringComparison]::OrdinalIgnoreCase) -or
        [string]::Equals($root, $candidate,
                [StringComparison]::OrdinalIgnoreCase))) {
    throw 'Package is outside its declared candidate/accepted path.'
}
$mapping = [ordered]@{
    artifact = 'gdrom-hardware.fcgd'; identity = 'identity.json'
    maple_replay = 'maple-replay.fcmt'
    maple_record_identity = 'maple-record-identity.json'
    flycast = 'flycast.exe'
    bios = 'dc_boot.bin'; initial_flash = 'dc_nvmem.bin'
    artifact_validator = 'artifact-validator.exe'
    package_validator = 'package-validator.ps1'; publisher = 'publisher.ps1'
}
$seenDeviceSlots = @{}
for ($deviceIndex = 0; $deviceIndex -lt @($identity.persistent_devices).Count;
        ++$deviceIndex) {
    $device = @($identity.persistent_devices)[$deviceIndex]
    if ([string]$device.kind -cne 'vmu') {
        throw "Unsupported persistent device kind at index ${deviceIndex}: $($device.kind)"
    }
    $bus = [int]$device.bus; $port = [int]$device.port
    if ($bus -lt 0 -or $bus -gt 3 -or $port -lt 0 -or $port -gt 5) {
        throw "Persistent device bus/port is out of range at index ${deviceIndex}."
    }
    $slotKey = "$bus/$port"
    if ($seenDeviceSlots.ContainsKey($slotKey)) {
        throw "Persistent device slot is duplicated: $slotKey"
    }
    $seenDeviceSlots[$slotKey] = $true
    if ([int64]$device.size -ne 131072) {
        throw "Persistent VMU at index $deviceIndex must be exactly 131072 bytes."
    }
    $key = 'persistent_device_{0:D3}' -f $deviceIndex
    $mapping[$key] = 'persistent-device-{0:D3}-vmu-{1}-{2}.bin' -f `
        $deviceIndex, $bus, $port
}
Assert-Exact $manifest.entries @($mapping.Keys) 'package.entries'
foreach ($key in $mapping.Keys) {
    $property = $manifest.entries.PSObject.Properties[$key]
    if ($null -eq $property) { throw "package.entries.$key is missing." }
    Assert-Exact $property.Value @('size', 'sha256') "package.entries.$key"
    Assert-Blob $property.Value (Join-Path $root $mapping[$key]) "package.entries.$key"
}
if ([string]$identity.firmware.mode -cne 'real' -or
		[int64]$identity.firmware.bios.size -ne 2097152 -or
		[int64]$identity.firmware.flash_initial.size -ne 131072) {
    throw 'Package identity is not a real Dreamcast BIOS authority.'
}
Assert-Blob $identity.firmware.bios (Join-Path $root 'dc_boot.bin') 'BIOS authority'
Assert-Blob $identity.firmware.flash_initial (Join-Path $root 'dc_nvmem.bin') `
    'initial flash authority'
Assert-Blob $identity.emulator.executable (Join-Path $root 'flycast.exe') `
    'Flycast authority'
for ($deviceIndex = 0; $deviceIndex -lt @($identity.persistent_devices).Count;
        ++$deviceIndex) {
    $key = 'persistent_device_{0:D3}' -f $deviceIndex
    Assert-Blob @($identity.persistent_devices)[$deviceIndex] `
        (Join-Path $root $mapping[$key]) `
        "persistent-device authority $deviceIndex"
}
$receiptByName = @{}
foreach ($entry in @($receipt.entries)) {
	Assert-Exact $entry @('name', 'size', 'sha256') 'receipt entry'
    $name = [string]$entry.name
    if ($name -notin @($mapping.Values) + @('package.json') -or
            $receiptByName.ContainsKey($name)) { throw 'Receipt inventory is invalid.' }
    $receiptByName[$name] = $entry
}
if ($receiptByName.Count -ne ($mapping.Count + 1)) {
    throw 'Receipt inventory is incomplete.'
}
foreach ($name in $receiptByName.Keys) {
    Assert-Blob $receiptByName[$name] (Join-Path $root $name) "receipt.entries[$name]"
}
$artifactValidator = Join-Path $root 'artifact-validator.exe'
if (-not [string]::IsNullOrWhiteSpace($TrustedArtifactValidator)) {
    $artifactValidator = (Resolve-Path -LiteralPath `
        ([IO.Path]::GetFullPath($TrustedArtifactValidator))).Path
    Get-Blob $artifactValidator | Out-Null
}
$validatorOutput = Invoke-Checked $artifactValidator @(
    '--artifact', (Join-Path $root 'gdrom-hardware.fcgd'),
    '--identity', (Join-Path $root 'identity.json'),
    '--replay', (Join-Path $root 'maple-replay.fcmt'),
    '--replay-identity', (Join-Path $root 'maple-record-identity.json'),
    '--bios', (Join-Path $root 'dc_boot.bin'),
    '--flash', (Join-Path $root 'dc_nvmem.bin'))
if ([string]$validatorOutput -cne [string]$receipt.validator_output) {
    throw 'Artifact validation differs from the deterministic receipt.'
}
Write-Output "ACCEPTED flycast-research-gdrom-hardware-package-v2 $root"

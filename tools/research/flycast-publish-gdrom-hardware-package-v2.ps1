#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Artifact,
    [Parameter(Mandatory = $true)][string]$Identity,
    [Parameter(Mandatory = $true)][string]$MapleReplay,
    [Parameter(Mandatory = $true)][string]$MapleReplayIdentity,
    [Parameter(Mandatory = $true)][string]$FlycastExecutable,
    [Parameter(Mandatory = $true)][string]$ArtifactValidator,
    [string]$PackageValidator = (Join-Path $PSScriptRoot `
        'flycast-validate-gdrom-hardware-package-v2.ps1'),
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateRange(1, 3600)][int]$ValidatorTimeoutSeconds = 300,
    [ValidateSet('', 'after-staging-v2', 'after-validation-v2')]
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
    return [ordered]@{ size = [int64]$item.Length; sha256 =
        (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
}
function Assert-Blob([object]$Expected, [string]$Path, [string]$Field) {
    $actual = Get-Blob $Path
    if ($null -eq $Expected -or [int64]$Expected.size -ne [int64]$actual.size -or
            [string]$Expected.sha256 -cne [string]$actual.sha256) {
        throw "$Field differs from its authority."
    }
}
function Copy-Verified([string]$Source, [string]$Destination,
        [object]$Expected, [string]$Field) {
    [IO.File]::Copy($Source, $Destination, $false)
    Assert-Blob $Expected $Destination "$Field staged copy"
}
function Invoke-Checked([string]$Executable, [string[]]$Arguments,
        [int]$TimeoutSeconds) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable; $start.UseShellExecute = $false
    $start.CreateNoWindow = $true; $start.WindowStyle = 'Hidden'
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new(); $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw 'Validator did not start.' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try { $process.Kill($true) } catch { }
            throw 'Validator timed out.'
        }
        $out = $stdout.GetAwaiter().GetResult().Trim()
        $err = $stderr.GetAwaiter().GetResult().Trim()
        if ($process.ExitCode -ne 0) { throw "Validator rejected candidate: $out $err" }
        return $out
    }
    finally { $process.Dispose() }
}

$artifactPath = Resolve-Regular $Artifact 'Artifact'
$identityPath = Resolve-Regular $Identity 'Identity'
$replayPath = Resolve-Regular $MapleReplay 'MapleReplay'
$replayIdentityPath = Resolve-Regular $MapleReplayIdentity 'MapleReplayIdentity'
$flycastPath = Resolve-Regular $FlycastExecutable 'FlycastExecutable'
$validatorPath = Resolve-Regular $ArtifactValidator 'ArtifactValidator'
$packageValidatorPath = Resolve-Regular $PackageValidator 'PackageValidator'
$publisherPath = Resolve-Regular $PSCommandPath 'Publisher'
$identityObject = Get-Content -LiteralPath $identityPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 64
if ([string]$identityObject.schema -cne 'flycast-research-identity' -or
        [int]$identityObject.schema_version -ne 2 -or
        [string]$identityObject.firmware.mode -cne 'real') {
    throw 'Hardware publication requires a real-firmware research identity v2.'
}
$biosPath = Resolve-Regular ([string]$identityObject.firmware.bios.path) `
    'identity.firmware.bios'
$flashPath = Resolve-Regular ([string]$identityObject.firmware.flash_initial.path) `
    'identity.firmware.flash_initial'
Assert-Blob $identityObject.firmware.bios $biosPath 'BIOS'
Assert-Blob $identityObject.firmware.flash_initial $flashPath 'initial flash'
Assert-Blob $identityObject.emulator.executable $flycastPath 'Flycast executable'
if ([int64]$identityObject.firmware.bios.size -ne 2097152) {
    throw 'Dreamcast BIOS authority must be exactly 2097152 bytes.'
}
if ([int64]$identityObject.firmware.flash_initial.size -ne 131072) {
    throw 'Dreamcast initial flash authority must be exactly 131072 bytes.'
}

$accepted = [IO.Path]::GetFullPath($OutputDirectory)
if (-not [IO.Path]::IsPathFullyQualified($accepted)) {
    throw 'OutputDirectory must be absolute.'
}
$parent = Split-Path -Parent $accepted
if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
    throw 'OutputDirectory parent must already exist.'
}
$parent = (Resolve-Path -LiteralPath $parent).Path
$accepted = Join-Path $parent (Split-Path -Leaf $accepted)
if (Test-Path -LiteralPath $accepted) { throw 'OutputDirectory already exists.' }
if ($IntegrationTestAbortToken -and
        (Split-Path -Leaf $parent) -cnotmatch '^flycast-research-gdrom-hardware-integration-[0-9a-f]+$') {
    throw 'Abort tokens require a dedicated integration-test parent.'
}
$packageId = [guid]::NewGuid().ToString('D').ToLowerInvariant()
$candidate = Join-Path $parent ".flycast-research-gdrom-hardware-candidate-$packageId"
$quarantineRoot = Join-Path $parent '.flycast-research-gdrom-hardware-quarantine'
$quarantine = Join-Path $quarantineRoot $packageId
$sources = [ordered]@{
    artifact = Get-Blob $artifactPath; identity = Get-Blob $identityPath
    maple_replay = Get-Blob $replayPath
    maple_record_identity = Get-Blob $replayIdentityPath
    flycast = Get-Blob $flycastPath
    bios = Get-Blob $biosPath; initial_flash = Get-Blob $flashPath
    artifact_validator = Get-Blob $validatorPath
    package_validator = Get-Blob $packageValidatorPath
    publisher = Get-Blob $publisherPath
}
$sourcePaths = [ordered]@{
    artifact = $artifactPath; identity = $identityPath
    maple_replay = $replayPath; maple_record_identity = $replayIdentityPath
    flycast = $flycastPath; bios = $biosPath; initial_flash = $flashPath
    artifact_validator = $validatorPath
    package_validator = $packageValidatorPath; publisher = $publisherPath
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
for ($deviceIndex = 0; $deviceIndex -lt @($identityObject.persistent_devices).Count;
        ++$deviceIndex) {
    $device = @($identityObject.persistent_devices)[$deviceIndex]
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
    $devicePath = Resolve-Regular ([string]$device.path) `
        "identity.persistent_devices[$deviceIndex]"
    Assert-Blob $device $devicePath "identity.persistent_devices[$deviceIndex]"
    if ([int64]$device.size -ne 131072) {
        throw "Persistent VMU at index $deviceIndex must be exactly 131072 bytes."
    }
    $key = 'persistent_device_{0:D3}' -f $deviceIndex
    $name = 'persistent-device-{0:D3}-vmu-{1}-{2}.bin' -f `
        $deviceIndex, $bus, $port
    $sources[$key] = Get-Blob $devicePath
    $sourcePaths[$key] = $devicePath
    $mapping[$key] = $name
}
try {
    [IO.Directory]::CreateDirectory($candidate) | Out-Null
    foreach ($key in $mapping.Keys) {
        $source = [string]$sourcePaths[$key]
        Copy-Verified $source (Join-Path $candidate $mapping[$key]) `
            $sources[$key] $key
    }
    $manifest = [ordered]@{
        schema = 'flycast-research-gdrom-hardware-package'; schema_version = 2
        package_id = $packageId; accepted_directory = $accepted; entries = $sources
    }
    [IO.File]::WriteAllText((Join-Path $candidate 'package.json'),
        (($manifest | ConvertTo-Json -Depth 16) + "`n"), $utf8NoBom)
    if ($IntegrationTestAbortToken -ceq 'after-staging-v2') {
        throw 'Integration test forced abort after staging.'
    }
    $validatorOutput = Invoke-Checked (Join-Path $candidate 'artifact-validator.exe') @(
        '--artifact', (Join-Path $candidate 'gdrom-hardware.fcgd'),
        '--identity', (Join-Path $candidate 'identity.json'),
        '--replay', (Join-Path $candidate 'maple-replay.fcmt'),
        '--replay-identity', (Join-Path $candidate 'maple-record-identity.json'),
        '--bios', (Join-Path $candidate 'dc_boot.bin'),
        '--flash', (Join-Path $candidate 'dc_nvmem.bin')) $ValidatorTimeoutSeconds
    $receiptEntries = @()
    foreach ($name in @($mapping.Values) + @('package.json')) {
        $blob = Get-Blob (Join-Path $candidate $name)
        $receiptEntries += [ordered]@{ name = $name; size = $blob.size; sha256 = $blob.sha256 }
    }
    $receipt = [ordered]@{
        schema = 'flycast-research-gdrom-hardware-package-validation'
        schema_version = 2; package_id = $packageId; status = 'accepted'
        validator_output = $validatorOutput; entries = $receiptEntries
    }
    [IO.File]::WriteAllText((Join-Path $candidate 'package-validation.json'),
        (($receipt | ConvertTo-Json -Depth 16) + "`n"), $utf8NoBom)
    & (Join-Path $candidate 'package-validator.ps1') -Package $candidate `
        -ValidatorTimeoutSeconds $ValidatorTimeoutSeconds | Out-Null
    if ($IntegrationTestAbortToken -ceq 'after-validation-v2') {
        throw 'Integration test forced abort after validation.'
    }
    [IO.Directory]::Move($candidate, $accepted)
    Write-Output "ACCEPTED flycast-research-gdrom-hardware-package-v2 $accepted"
}
catch {
    $failure = $_.Exception.Message
    if (Test-Path -LiteralPath $candidate) {
        [IO.Directory]::CreateDirectory($quarantineRoot) | Out-Null
        [IO.Directory]::Move($candidate, $quarantine)
        [IO.File]::WriteAllText((Join-Path $quarantine 'rejection.json'),
            (([ordered]@{ schema = 'flycast-research-gdrom-hardware-package-quarantine'
                schema_version = 2; package_id = $packageId
                accepted_directory = $accepted; failure = $failure } |
                ConvertTo-Json -Depth 8) + "`n"), $utf8NoBom)
    }
    throw
}

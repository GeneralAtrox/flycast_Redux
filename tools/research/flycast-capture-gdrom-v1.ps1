#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$FlycastExecutable,
    [Parameter(Mandatory = $true)][string]$Identity,
    [Parameter(Mandatory = $true)][string]$MapleReplay,
    [string]$MapleReplayIdentity = '',
    [Parameter(Mandatory = $true)][string]$ArtifactValidator,
    [Parameter(Mandatory = $true)][string]$Game,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateRange(256, 536870912)][int64]$MaximumArtifactBytes = 536870912,
    [ValidateRange(1, 3600)][int]$CaptureTimeoutSeconds = 300,
    [ValidateRange(1, 120)][int]$MinimumRunSeconds = 5,
    [ValidateRange(1, 120)][int]$ExitTimeoutSeconds = 30
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
        throw "$Field bytes differ from the identity."
    }
}

function Invoke-Validator([string]$Executable, [string[]]$Arguments,
        [int]$TimeoutSeconds) {
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
        if (-not $process.Start()) { throw 'GD-ROM validator did not start.' }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try { $process.Kill($true) } catch { }
            try { $process.WaitForExit() } catch { }
            throw 'GD-ROM validator timed out.'
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult().Trim()
        $stderr = $stderrTask.GetAwaiter().GetResult().Trim()
        if ($process.ExitCode -ne 0) {
            throw "GD-ROM validator rejected the capture: $stdout $stderr"
        }
        return $stdout
    }
    finally { $process.Dispose() }
}

$flycast = Resolve-Regular $FlycastExecutable 'FlycastExecutable'
$identityPath = Resolve-Regular $Identity 'Identity'
$replay = Resolve-Regular $MapleReplay 'MapleReplay'
$replayIdentity = $null
$validator = Resolve-Regular $ArtifactValidator 'ArtifactValidator'
$gamePath = Resolve-Regular $Game 'Game'
$identityObject = Get-Content -LiteralPath $identityPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 64
if ([string]$identityObject.schema -cne 'flycast-research-identity' -or
        [int]$identityObject.schema_version -ne 2) {
    throw 'GD-ROM capture requires a Flycast research identity v2.'
}
if ([string]$identityObject.firmware.mode -ceq 'real') {
    if ([string]::IsNullOrWhiteSpace($MapleReplayIdentity)) {
        throw 'Real-firmware GD-ROM capture requires MapleReplayIdentity.'
    }
    $replayIdentity = Resolve-Regular $MapleReplayIdentity 'MapleReplayIdentity'
}
elseif (-not [string]::IsNullOrWhiteSpace($MapleReplayIdentity)) {
    $replayIdentity = Resolve-Regular $MapleReplayIdentity 'MapleReplayIdentity'
}
Assert-Blob $identityObject.emulator.executable $flycast `
    'identity.emulator.executable'
Assert-Blob $identityObject.media.source $gamePath 'identity.media.source'
if ([string]$identityObject.media.kind -cne 'gdi' -or
        @($identityObject.media.tracks).Count -eq 0) {
    throw 'GD-ROM capture requires identity-bound GDI media.'
}
$checkpoint = [int64]$identityObject.configuration.values.maple_dma_checkpoint
if ($checkpoint -le 0) { throw 'Identity has no bounded Maple DMA checkpoint.' }
$rtcSeed = [uint64]$identityObject.configuration.values.dreamcast_rtc_seed
$pvrStartDma = [int64]$identityObject.configuration.values.pvr_ta_start_dma
$sh4StartDma = [int64]$identityObject.configuration.values.sh4_observation_start_dma

$output = [IO.Path]::GetFullPath($OutputDirectory)
if (-not [IO.Path]::IsPathFullyQualified($output)) {
    throw 'OutputDirectory must be an absolute path.'
}
if (Test-Path -LiteralPath $output) { throw 'OutputDirectory already exists.' }
$parent = Split-Path -Parent $output
if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
    throw 'OutputDirectory parent must already exist.'
}
$runtime = Join-Path $output 'runtime'
$data = Join-Path $runtime 'data'
$inputs = Join-Path $output 'inputs'
$candidateDirectory = Join-Path $output 'candidate'
$validation = Join-Path $output 'validation'
foreach ($directory in @($data, $inputs, $candidateDirectory, $validation)) {
    [IO.Directory]::CreateDirectory($directory) | Out-Null
}
$stagedFlycast = Join-Path $runtime 'flycast.exe'
$stagedIdentity = Join-Path $inputs 'identity.json'
$stagedReplay = Join-Path $inputs 'maple-replay.fcmt'
$stagedReplayIdentity = if ($null -ne $replayIdentity) {
    Join-Path $inputs 'maple-record-identity.json'
} else { $null }
$stagedValidator = Join-Path $validation 'artifact-validator.exe'
$candidate = Join-Path $candidateDirectory 'gdrom.candidate.fcgd'
[IO.File]::Copy($flycast, $stagedFlycast, $false)
[IO.File]::Copy($identityPath, $stagedIdentity, $false)
[IO.File]::Copy($replay, $stagedReplay, $false)
if ($null -ne $replayIdentity) {
    [IO.File]::Copy($replayIdentity, $stagedReplayIdentity, $false)
}
[IO.File]::Copy($validator, $stagedValidator, $false)
Assert-Blob $identityObject.emulator.executable $stagedFlycast 'staged Flycast'

$flash = Resolve-Regular ([string]$identityObject.firmware.flash_initial.path) `
    'identity.firmware.flash_initial'
Assert-Blob $identityObject.firmware.flash_initial $flash `
    'identity.firmware.flash_initial'
if ([string]$identityObject.firmware.mode -ceq 'real' -and
        [int64]$identityObject.firmware.flash_initial.size -ne 131072) {
    throw 'Real Dreamcast initial flash must be exactly 131072 bytes.'
}
[IO.File]::Copy($flash, (Join-Path $data 'dc_nvmem.bin'), $false)
if ([string]$identityObject.firmware.mode -ceq 'real') {
    $bios = Resolve-Regular ([string]$identityObject.firmware.bios.path) `
        'identity.firmware.bios'
    Assert-Blob $identityObject.firmware.bios $bios 'identity.firmware.bios'
    if ([int64]$identityObject.firmware.bios.size -ne 2097152) {
        throw 'Real Dreamcast BIOS must be exactly 2097152 bytes.'
    }
    [IO.File]::Copy($bios, (Join-Path $data 'dc_boot.bin'), $false)
}
$transientSettings = @($identityObject.configuration.values.flycast_transient)
$seenDeviceSlots = @{}
for ($deviceIndex = 0; $deviceIndex -lt @($identityObject.persistent_devices).Count;
        ++$deviceIndex) {
    $device = @($identityObject.persistent_devices)[$deviceIndex]
    if ([string]$device.kind -cne 'vmu') {
        throw "Unsupported persistent device kind at index ${deviceIndex}: $($device.kind)"
    }
    $bus = [int]$device.bus
    $port = [int]$device.port
    if ($bus -lt 0 -or $bus -gt 3 -or $port -lt 0 -or $port -gt 5) {
        throw "Persistent device bus/port is out of range at index ${deviceIndex}."
    }
    $slotKey = "$bus/$port"
    if ($seenDeviceSlots.ContainsKey($slotKey)) {
        throw "Persistent device slot is duplicated: $slotKey"
    }
    $seenDeviceSlots[$slotKey] = $true
    if ($bus -eq 0 -and $port -eq 0 -and
            $transientSettings -cnotcontains 'config:PerGameVmu=no') {
        throw 'An authenticated A1 VMU requires config:PerGameVmu=no so Flycast consumes the staged vmu_save_A1.bin.'
    }
    $devicePath = Resolve-Regular ([string]$device.path) `
        "identity.persistent_devices[$deviceIndex]"
    Assert-Blob $device $devicePath "identity.persistent_devices[$deviceIndex]"
    if ([int64]$device.size -ne 131072) {
        throw "Persistent VMU at index $deviceIndex must be exactly 131072 bytes."
    }
    $immutableName = 'persistent-device-{0:D3}-vmu-{1}-{2}.bin' -f `
        $deviceIndex, $bus, $port
    $immutablePath = Join-Path $inputs $immutableName
    [IO.File]::Copy($devicePath, $immutablePath, $false)
    Assert-Blob $device $immutablePath `
        "staged identity.persistent_devices[$deviceIndex]"
    $portName = if ($port -eq 5) { 'x' } else { [char]([int][char]'1' + $port) }
    $runtimeName = "vmu_save_$([char]([int][char]'A' + $bus))$portName.bin"
    [IO.File]::Copy($immutablePath, (Join-Path $data $runtimeName), $false)
}
$hasInitialState = $identityObject.PSObject.Properties.Name -ccontains 'initial_state'
if ($hasInitialState -and $null -ne $identityObject.initial_state) {
    if ([string]$identityObject.initial_state.kind -cne 'savestate') {
        throw 'Unsupported initial-state kind.'
    }
    $state = Resolve-Regular ([string]$identityObject.initial_state.blob.path) `
        'identity.initial_state.blob'
    Assert-Blob $identityObject.initial_state.blob $state `
        'identity.initial_state.blob'
    $slot = [int]$identityObject.initial_state.slot
    $suffix = if ($slot -eq 0) { '' } else { "_$slot" }
    $stateName = [IO.Path]::GetFileNameWithoutExtension($gamePath) + $suffix + '.state'
    [IO.File]::Copy($state, (Join-Path $data $stateName), $false)
}
[IO.File]::WriteAllText((Join-Path $runtime 'emu.cfg'),
    "[log]`nLogToFile = yes`nVerbosity = 6`n", $utf8NoBom)

$directives = [Collections.Generic.List[string]]::new()
foreach ($directive in $transientSettings) {
    if (-not [string]::IsNullOrWhiteSpace([string]$directive)) {
        $directives.Add([string]$directive)
    }
}
foreach ($directive in @(
    "research:IdentityManifest=$stagedIdentity",
    "research:MapleReplay=$stagedReplay",
    "research:GdromRecord=$candidate",
    "research:GdromMaxBytes=$MaximumArtifactBytes",
    "research:DreamcastRtcSeed=$rtcSeed",
    "research:MapleDmaCheckpoint=$checkpoint",
    "research:PvrTaStartDma=$pvrStartDma",
    "research:Sh4ObservationStartDma=$sh4StartDma")) {
    $key = $directive.Substring(0, $directive.IndexOf('=') + 1)
    for ($index = $directives.Count - 1; $index -ge 0; --$index) {
        if ($directives[$index].StartsWith($key, [StringComparison]::Ordinal)) {
            $directives.RemoveAt($index)
        }
    }
    $directives.Add($directive)
}
foreach ($directive in $directives) {
    if ($directive -notmatch '^[^=,\r\n''"]+=[^=,\r\n''"]+$') {
        throw "Configuration directive cannot be represented: $directive"
    }
}

$stdoutPath = Join-Path $validation 'flycast.stdout.txt'
$stderrPath = Join-Path $validation 'flycast.stderr.txt'
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $stagedFlycast
$start.WorkingDirectory = $runtime
$start.UseShellExecute = $false
$start.CreateNoWindow = $false
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.ArgumentList.Add('-config')
$start.ArgumentList.Add(($directives -join ','))
$start.ArgumentList.Add($gamePath)
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
try {
    if (-not $process.Start()) { throw 'Flycast did not start.' }
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $deadline = [DateTime]::UtcNow.AddSeconds($CaptureTimeoutSeconds)
    $notBefore = [DateTime]::UtcNow.AddSeconds($MinimumRunSeconds)
    while (-not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds 100
    }
    if (-not $process.HasExited) {
        throw 'Flycast did not exit at the authenticated Maple DMA checkpoint.'
    }
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText($stdoutPath, $stdout, $utf8NoBom)
    [IO.File]::WriteAllText($stderrPath, $stderr, $utf8NoBom)
    if ([DateTime]::UtcNow -lt $notBefore) {
        throw 'Flycast exited before the minimum bounded capture interval.'
    }
    if ($process.ExitCode -ne 0) { throw "Flycast exited with code $($process.ExitCode)." }
}
finally {
    if (-not $process.HasExited) {
        try { $process.CloseMainWindow() | Out-Null } catch { }
        try {
            if (-not $process.WaitForExit($ExitTimeoutSeconds * 1000)) {
                $process.Kill($true)
            }
        } catch { }
    }
    $process.Dispose()
}

$validatorArguments = @(
    '--artifact', $candidate,
    '--identity', $stagedIdentity,
    '--replay', $stagedReplay
)
if ($null -ne $stagedReplayIdentity) {
    $validatorArguments += @('--replay-identity', $stagedReplayIdentity)
}
$validatorOutput = Invoke-Validator $stagedValidator $validatorArguments `
    $CaptureTimeoutSeconds
[IO.File]::WriteAllText((Join-Path $validation 'artifact-validation.txt'),
    ($validatorOutput + "`n"), $utf8NoBom)
Write-Output "ACCEPTED flycast-research-gdrom-capture-v1 $output"
Write-Output $validatorOutput

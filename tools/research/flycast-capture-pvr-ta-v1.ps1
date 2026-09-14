#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$FlycastExecutable,
    [Parameter(Mandatory = $true)][string]$ValidatorExecutable,
    [Parameter(Mandatory = $true)][string]$PresentationValidatorExecutable,
    [Parameter(Mandatory = $true)][string]$Game,
    [Parameter(Mandatory = $true)][string]$Identity,
    [Parameter(Mandatory = $true)][string]$MapleReplay,
    [Parameter(Mandatory = $true)][string]$Manifest,
    [Parameter(Mandatory = $true)][string]$FlashSeed,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
	[string]$DrawValidatorExecutable = '',
	[ValidateRange(1, 600)][int]$MinimumRunSeconds = 10,
    [ValidateRange(10, 600)][int]$CaptureTimeoutSeconds = 120,
    [ValidateRange(5, 120)][int]$ExitTimeoutSeconds = 30,
    [ValidateRange(5, 300)][int]$ValidatorTimeoutSeconds = 60,
    [ValidateRange(160, 1073741824)][int64]$MaximumReplayBytes = 536870912,
    [ValidateRange(256, 1073741824)][int64]$MaximumPresentationBytes = 536870912,
	[ValidateRange(320, 2147483648)][int64]$MaximumDrawBytes = 536870912
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $IsWindows) { throw 'PowerVR TA capture v1 currently requires Windows.' }
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Assert-Condition([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Resolve-RegularFile([string]$Path, [string]$Field) {
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $item = Get-Item -LiteralPath $resolved -Force
    Assert-Condition (-not $item.PSIsContainer) "$Field is not a regular file."
    Assert-Condition (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) `
        "$Field must not be a link or reparse point."
    return $item.FullName
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-Blob([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    return [ordered]@{
        path = $item.FullName
        size = [int64]$item.Length
        sha256 = Get-Sha256 $item.FullName
    }
}

function Assert-Blob([object]$Blob, [string]$Path, [string]$Field) {
    $item = Get-Item -LiteralPath $Path -Force
    Assert-Condition (-not $item.PSIsContainer -and
        (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) -and
        [int64]$item.Length -eq [int64]$Blob.size -and
        (Get-Sha256 $item.FullName) -ceq [string]$Blob.sha256) `
        "$Field bytes changed."
}

function Assert-DeclaredBlob([object]$Declared, [string]$ActualPath,
        [string]$Field) {
    Assert-Condition ($null -ne $Declared -and [int64]$Declared.size -gt 0 -and
        [string]$Declared.sha256 -cmatch '^[0-9a-f]{64}$') `
        "$Field declaration is invalid."
    $actual = Get-Blob $ActualPath
    Assert-Condition ([int64]$actual.size -eq [int64]$Declared.size -and
        [string]$actual.sha256 -ceq [string]$Declared.sha256) `
        "$Field bytes differ from identity v2."
}

function Assert-CaptureIdentitySources([object]$IdentityObject,
        [string]$FlycastPath, [string]$GamePath, [string]$FlashPath) {
    Assert-DeclaredBlob $IdentityObject.emulator.executable $FlycastPath `
        'Flycast executable'
    Assert-DeclaredBlob $IdentityObject.media.source $GamePath 'game descriptor'
    Assert-DeclaredBlob $IdentityObject.firmware.flash_initial $FlashPath 'flash seed'
    if ($IdentityObject.media.PSObject.Properties.Name -ccontains 'tracks') {
        foreach ($track in @($IdentityObject.media.tracks)) {
            $trackPath = Resolve-RegularFile ([string]$track.path) 'identity media track'
            Assert-DeclaredBlob $track $trackPath 'identity media track'
        }
    }
    foreach ($entry in @(
        @($IdentityObject.media.ip_bin, 'identity IP.BIN'),
        @($IdentityObject.media.boot_executable, 'identity boot executable'))) {
        $entryPath = Resolve-RegularFile ([string]$entry[0].path) $entry[1]
        Assert-DeclaredBlob $entry[0] $entryPath $entry[1]
    }
    foreach ($device in @($IdentityObject.persistent_devices)) {
        Assert-Condition ([string]$device.kind -ceq 'vmu') `
            'Only VMU persistent devices are supported by PVR TA capture v1.'
        $bus = [int]$device.bus
        $port = [int]$device.port
        Assert-Condition ($bus -ge 0 -and $bus -le 3 -and
            $port -ge 0 -and $port -le 1) `
            'Identity VMU bus or port is outside supported bounds.'
        $devicePath = Resolve-RegularFile ([string]$device.path) `
            'identity persistent VMU'
        Assert-DeclaredBlob $device $devicePath 'identity persistent VMU'
    }
}

function Stop-OwnedProcess([Diagnostics.Process]$Process) {
    if ($null -ne $Process -and -not $Process.HasExited) {
        $Process.Kill($true)
        $Process.WaitForExit()
    }
}

function Invoke-OwnedProcess([string]$Executable, [string[]]$Arguments,
        [int]$TimeoutSeconds, [string]$StdoutPath, [string]$StderrPath,
        [string]$Name) {
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
        Assert-Condition $process.Start() "$Name did not start."
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            Stop-OwnedProcess $process
            throw "$Name timed out."
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($StdoutPath, $stdout, $utf8NoBom)
        [IO.File]::WriteAllText($StderrPath, $stderr, $utf8NoBom)
        Assert-Condition ($process.ExitCode -eq 0) `
            "$Name rejected the candidate (exit $($process.ExitCode)): $stderr"
    }
    finally { $process.Dispose() }
}

$flycast = Resolve-RegularFile $FlycastExecutable 'FlycastExecutable'
$validator = Resolve-RegularFile $ValidatorExecutable 'ValidatorExecutable'
$presentationValidator = Resolve-RegularFile $PresentationValidatorExecutable `
    'PresentationValidatorExecutable'
$gamePath = Resolve-RegularFile $Game 'Game'
$identityPath = Resolve-RegularFile $Identity 'Identity'
$replayPath = Resolve-RegularFile $MapleReplay 'MapleReplay'
$manifestPath = Resolve-RegularFile $Manifest 'Manifest'
$flashPath = Resolve-RegularFile $FlashSeed 'FlashSeed'
$sourceBlobs = [ordered]@{
    flycast = Get-Blob $flycast
    validator = Get-Blob $validator
    presentation_validator = Get-Blob $presentationValidator
    game = Get-Blob $gamePath
    identity = Get-Blob $identityPath
    replay = Get-Blob $replayPath
    manifest = Get-Blob $manifestPath
    flash = Get-Blob $flashPath
}

$identityObject = Get-Content -LiteralPath $identityPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 64
$manifestObject = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 64
Assert-Condition ([string]$identityObject.schema -ceq 'flycast-research-identity' -and
    [int]$identityObject.schema_version -eq 2) 'Identity must be identity v2.'
Assert-Condition ([string]$manifestObject.schema -ceq
    'flycast-research-pvr-ta-capture-manifest' -and
    [int]$manifestObject.schema_version -eq 1) 'Manifest must be PVR TA manifest v1.'
$hasInitialState = $identityObject.PSObject.Properties.Name -ccontains `
    'initial_state'
$initialState = if ($hasInitialState) {
    Assert-Condition ([string]$identityObject.initial_state.kind -ceq 'savestate') `
        'Identity initial_state kind must be savestate.'
    $stateSlot = [int]$identityObject.initial_state.slot
    Assert-Condition ($stateSlot -ge 0 -and $stateSlot -le 9) `
        'Identity initial_state slot is outside v2 bounds.'
    $statePath = Resolve-RegularFile `
        ([string]$identityObject.initial_state.blob.path) 'identity initial state'
    Assert-DeclaredBlob $identityObject.initial_state.blob $statePath `
        'identity initial state'
    [pscustomobject]@{ Path = $statePath; Slot = $stateSlot }
}
else { $null }
if ($hasInitialState) { $sourceBlobs['initial_state'] = Get-Blob $initialState.Path }
$configurationNames = @($identityObject.configuration.values.PSObject.Properties.Name)
$hasLuaInputDriver = $configurationNames -ccontains 'lua_input_driver'
$luaInputDriver = if ($hasLuaInputDriver) {
    $blob = $identityObject.configuration.values.lua_input_driver
    $path = Resolve-RegularFile ([string]$blob.path) 'identity Lua input driver'
    Assert-DeclaredBlob $blob $path 'identity Lua input driver'
    $luaDirectives = @($identityObject.configuration.values.flycast_transient |
        Where-Object { [string]$_ -cmatch '^config:LuaFileName=([^=,\\/]+)$' })
    Assert-Condition ($luaDirectives.Count -eq 1) `
        'Identity with a Lua input driver must bind one leaf LuaFileName directive.'
    [pscustomobject]@{
        Path = $path
        FileName = ([string]$luaDirectives[0]).Substring('config:LuaFileName='.Length)
    }
}
else { $null }
if ($hasLuaInputDriver) {
    $sourceBlobs['lua_input_driver'] = Get-Blob $luaInputDriver.Path
}
$persistentDevices = @($identityObject.persistent_devices | ForEach-Object {
    $devicePath = Resolve-RegularFile ([string]$_.path) 'identity persistent VMU'
    [pscustomobject]@{
        Blob = Get-Blob $devicePath
        Bus = [int]$_.bus
        Port = [int]$_.port
    }
})
$hasDraw = $identityObject.configuration.values.PSObject.Properties.Name `
    -ccontains 'pvr_draw_configuration'
$drawValidator = if ($hasDraw) {
    Assert-Condition (-not [string]::IsNullOrWhiteSpace($DrawValidatorExecutable)) `
        'DrawValidatorExecutable is required by the identity-bound draw configuration.'
    Resolve-RegularFile $DrawValidatorExecutable 'DrawValidatorExecutable'
}
else { $null }
if ($hasDraw) { $sourceBlobs['draw_validator'] = Get-Blob $drawValidator }
Assert-CaptureIdentitySources $identityObject $flycast $gamePath $flashPath
$maximumBytes = [int64]$manifestObject.limits.maximum_bytes
$maximumEvents = [int64]$manifestObject.limits.maximum_events
$startDma = [int64]$manifestObject.capture.start_dma
$checkpoint = if ($null -eq $identityObject.configuration.values.maple_dma_checkpoint) {
    0
}
else { [int64]$identityObject.configuration.values.maple_dma_checkpoint }
$rtcSeed = [uint64]$identityObject.configuration.values.dreamcast_rtc_seed
Assert-Condition ($MinimumRunSeconds -lt $CaptureTimeoutSeconds) `
    'MinimumRunSeconds must be less than CaptureTimeoutSeconds.'
Assert-Condition ($maximumBytes -ge 272 -and $maximumBytes -le 536870912 -and
    $maximumEvents -ge 1 -and $maximumEvents -le 10000000) `
    'Manifest artifact limits are outside v1 bounds.'
Assert-Condition ($startDma -ge 0 -and $startDma -le 10000000) `
    'Manifest start DMA is outside v1 bounds.'

$output = [IO.Path]::GetFullPath($OutputDirectory)
Assert-Condition (-not (Test-Path -LiteralPath $output)) `
    'OutputDirectory already exists.'
$outputParent = Split-Path -Parent $output
Assert-Condition (Test-Path -LiteralPath $outputParent -PathType Container) `
    'OutputDirectory parent must already exist.'
$runtime = Join-Path $output 'runtime'
$data = Join-Path $runtime 'data'
$inputs = Join-Path $output 'inputs'
$candidateDirectory = Join-Path $output 'candidate'
$validation = Join-Path $output 'validation'
foreach ($directory in @($data, $inputs, $candidateDirectory, $validation)) {
    [IO.Directory]::CreateDirectory($directory) | Out-Null
}
$stagedFlycast = Join-Path $runtime 'flycast.exe'
$stagedValidator = Join-Path $validation 'artifact-validator.exe'
$stagedPresentationValidator = Join-Path $validation 'presentation-validator.exe'
$stagedDrawValidator = Join-Path $validation 'draw-validator.exe'
$stagedIdentity = Join-Path $inputs 'identity.json'
$stagedReplay = Join-Path $inputs 'maple-replay.fcmt'
$stagedManifest = Join-Path $inputs 'pvr-ta-manifest.json'
$candidate = Join-Path $candidateDirectory 'pvr-ta.candidate.fcpvr'
$presentationCandidate = Join-Path $candidateDirectory `
    'pvr-presentation.candidate.fcpvrp'
$drawCandidate = Join-Path $candidateDirectory 'pvr-draw.candidate.fcpvrd'
[IO.File]::Copy($flycast, $stagedFlycast, $false)
[IO.File]::Copy($validator, $stagedValidator, $false)
[IO.File]::Copy($presentationValidator, $stagedPresentationValidator, $false)
if ($hasDraw) { [IO.File]::Copy($drawValidator, $stagedDrawValidator, $false) }
[IO.File]::Copy($identityPath, $stagedIdentity, $false)
[IO.File]::Copy($replayPath, $stagedReplay, $false)
[IO.File]::Copy($manifestPath, $stagedManifest, $false)
[IO.File]::Copy($flashPath, (Join-Path $data 'dc_nvmem.bin'), $false)
foreach ($device in $persistentDevices) {
    $portName = $device.Port + 1
    $vmuName = "vmu_save_$([char]([int][char]'A' + $device.Bus))$portName.bin"
    $stagedVmu = Join-Path $data $vmuName
    [IO.File]::Copy($device.Blob.path, $stagedVmu, $false)
}
if ($hasInitialState) {
    $stateSuffix = if ($initialState.Slot -eq 0) { '' } else { "_$($initialState.Slot)" }
    $stagedInitialState = Join-Path $data `
        "$([IO.Path]::GetFileNameWithoutExtension($gamePath))$stateSuffix.state"
    [IO.File]::Copy($initialState.Path, $stagedInitialState, $false)
}
if ($hasLuaInputDriver) {
    $stagedLuaInputDriver = Join-Path $runtime $luaInputDriver.FileName
    [IO.File]::Copy($luaInputDriver.Path, $stagedLuaInputDriver, $false)
}
[IO.File]::WriteAllText((Join-Path $runtime 'emu.cfg'),
    "[log]`nLogToFile = yes`nVerbosity = 6`n", $utf8NoBom)
foreach ($pair in @(
    @($sourceBlobs.flycast, $stagedFlycast, 'staged Flycast'),
    @($sourceBlobs.validator, $stagedValidator, 'staged validator'),
    @($sourceBlobs.presentation_validator, $stagedPresentationValidator,
        'staged presentation validator'),
    @($sourceBlobs.identity, $stagedIdentity, 'staged identity'),
    @($sourceBlobs.replay, $stagedReplay, 'staged replay'),
    @($sourceBlobs.manifest, $stagedManifest, 'staged manifest'),
    @($sourceBlobs.flash, (Join-Path $data 'dc_nvmem.bin'), 'staged flash'))) {
    Assert-Blob $pair[0] $pair[1] $pair[2]
}
if ($hasDraw) {
    Assert-Blob $sourceBlobs.draw_validator $stagedDrawValidator `
        'staged draw validator'
}
if ($hasInitialState) {
    Assert-Blob $sourceBlobs.initial_state $stagedInitialState `
        'staged initial state'
}
if ($hasLuaInputDriver) {
    Assert-Blob $sourceBlobs.lua_input_driver $stagedLuaInputDriver `
        'staged Lua input driver'
}
foreach ($device in $persistentDevices) {
    $portName = $device.Port + 1
    $vmuName = "vmu_save_$([char]([int][char]'A' + $device.Bus))$portName.bin"
    Assert-Blob $device.Blob (Join-Path $data $vmuName) 'staged persistent VMU'
}

$directives = [Collections.Generic.List[string]]::new()
foreach ($directive in @($identityObject.configuration.values.flycast_transient)) {
    if (-not [string]::IsNullOrWhiteSpace([string]$directive)) {
        $directives.Add([string]$directive)
    }
}
foreach ($directive in @(
    "research:IdentityManifest=$stagedIdentity",
    "research:MapleReplay=$stagedReplay",
    "research:PvrTaManifest=$stagedManifest",
    "research:PvrTaRecord=$candidate",
    "research:PvrTaMaxBytes=$maximumBytes",
    "research:PvrTaStartDma=$startDma",
    "research:PvrPresentationRecord=$presentationCandidate",
    "research:PvrPresentationMaxBytes=$MaximumPresentationBytes",
    "research:MapleTraceMaxBytes=$MaximumReplayBytes",
    "research:DreamcastRtcSeed=$rtcSeed",
    "research:MapleDmaCheckpoint=$checkpoint")) {
    $key = $directive.Substring(0, $directive.IndexOf('=') + 1)
    for ($index = $directives.Count - 1; $index -ge 0; --$index) {
        if ($directives[$index].StartsWith($key, [StringComparison]::Ordinal)) {
            $directives.RemoveAt($index)
        }
    }
    $directives.Add($directive)
}
if ($hasDraw) {
    foreach ($directive in @(
        "research:PvrDrawRecord=$drawCandidate",
        "research:PvrDrawMaxBytes=$MaximumDrawBytes")) {
        $key = $directive.Substring(0, $directive.IndexOf('=') + 1)
        for ($index = $directives.Count - 1; $index -ge 0; --$index) {
            if ($directives[$index].StartsWith($key, [StringComparison]::Ordinal)) {
                $directives.RemoveAt($index)
            }
        }
        $directives.Add($directive)
    }
}
foreach ($directive in $directives) {
    Assert-Condition ($directive -match '^[^=,\r\n''"]+=[^=,\r\n''"]+$') `
        "Configuration directive cannot be represented: $directive"
}

$flycastStdout = Join-Path $validation 'flycast.stdout.txt'
$flycastStderr = Join-Path $validation 'flycast.stderr.txt'
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $stagedFlycast
$start.WorkingDirectory = $runtime
$start.UseShellExecute = $false
$start.CreateNoWindow = $false
$start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Normal
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$start.ArgumentList.Add('-config')
$start.ArgumentList.Add(($directives -join ','))
$start.ArgumentList.Add($gamePath)
$process = [Diagnostics.Process]::new()
$process.StartInfo = $start
try {
    Assert-Condition $process.Start() 'Flycast did not start.'
    $stdoutTask = $process.StandardOutput.ReadToEndAsync()
    $stderrTask = $process.StandardError.ReadToEndAsync()
    $deadline = [DateTime]::UtcNow.AddSeconds($CaptureTimeoutSeconds)
	$notBefore = [DateTime]::UtcNow.AddSeconds($MinimumRunSeconds)
    $lastSize = -1L
    $stablePolls = 0
    $mainWindow = [IntPtr]::Zero
    while (-not $process.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        $process.Refresh()
        if ($process.MainWindowHandle -ne [IntPtr]::Zero) {
            $mainWindow = $process.MainWindowHandle
        }
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $size = [int64](Get-Item -LiteralPath $candidate).Length
            if ([DateTime]::UtcNow -ge $notBefore -and $size -gt 272 -and
                    $size -eq $lastSize) { ++$stablePolls }
            else { $stablePolls = 0 }
            $lastSize = $size
            if ($stablePolls -ge 20 -and $mainWindow -ne [IntPtr]::Zero) { break }
        }
        Start-Sleep -Milliseconds 100
    }
    if ($process.HasExited) {
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        [IO.File]::WriteAllText($flycastStdout, $stdout, $utf8NoBom)
        [IO.File]::WriteAllText($flycastStderr, $stderr, $utf8NoBom)
        throw "Flycast exited before a clean checkpoint close (exit $($process.ExitCode)): $stderr"
    }
    Assert-Condition ($stablePolls -ge 20 -and $mainWindow -ne [IntPtr]::Zero) `
        'Flycast did not reach a stable checkpoint with a closeable window.'
    Assert-Condition $process.CloseMainWindow() 'Flycast rejected the clean close request.'
    Assert-Condition ($process.WaitForExit($ExitTimeoutSeconds * 1000)) `
        'Flycast did not exit within the clean-close timeout.'
    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText($flycastStdout, $stdout, $utf8NoBom)
    [IO.File]::WriteAllText($flycastStderr, $stderr, $utf8NoBom)
    Assert-Condition ($process.ExitCode -eq 0) `
        "Flycast returned exit code $($process.ExitCode): $stderr"
}
catch {
    Stop-OwnedProcess $process
    throw
}
finally { $process.Dispose() }

Assert-Blob $sourceBlobs.flycast $flycast 'Flycast source'
Assert-Blob $sourceBlobs.validator $validator 'validator source'
Assert-Blob $sourceBlobs.presentation_validator $presentationValidator `
    'presentation validator source'
if ($hasDraw) {
    Assert-Blob $sourceBlobs.draw_validator $drawValidator 'draw validator source'
}
Assert-Blob $sourceBlobs.game $gamePath 'game source'
Assert-Blob $sourceBlobs.identity $identityPath 'identity source'
Assert-Blob $sourceBlobs.replay $replayPath 'replay source'
Assert-Blob $sourceBlobs.manifest $manifestPath 'manifest source'
Assert-Blob $sourceBlobs.flash $flashPath 'flash source'
if ($hasInitialState) {
    Assert-Blob $sourceBlobs.initial_state $initialState.Path `
        'initial state source'
    Assert-Blob $sourceBlobs.initial_state $stagedInitialState `
        'staged initial state'
}
if ($hasLuaInputDriver) {
    Assert-Blob $sourceBlobs.lua_input_driver $luaInputDriver.Path `
        'Lua input driver source'
    Assert-Blob $sourceBlobs.lua_input_driver $stagedLuaInputDriver `
        'staged Lua input driver'
}
Assert-CaptureIdentitySources $identityObject $flycast $gamePath $flashPath
$firstCandidate = Get-Blob $candidate
Start-Sleep -Milliseconds 250
$secondCandidate = Get-Blob $candidate
Assert-Condition ($firstCandidate.size -eq $secondCandidate.size -and
    $firstCandidate.sha256 -ceq $secondCandidate.sha256) `
    'Finalized candidate is not stable.'
$firstPresentationCandidate = Get-Blob $presentationCandidate
Start-Sleep -Milliseconds 250
$secondPresentationCandidate = Get-Blob $presentationCandidate
Assert-Condition ($firstPresentationCandidate.size -eq
        $secondPresentationCandidate.size -and
    $firstPresentationCandidate.sha256 -ceq
        $secondPresentationCandidate.sha256) `
    'Finalized presentation candidate is not stable.'
if ($hasDraw) {
    $firstDrawCandidate = Get-Blob $drawCandidate
    Start-Sleep -Milliseconds 250
    $secondDrawCandidate = Get-Blob $drawCandidate
    Assert-Condition ($firstDrawCandidate.size -eq $secondDrawCandidate.size -and
        $firstDrawCandidate.sha256 -ceq $secondDrawCandidate.sha256) `
        'Finalized draw candidate is not stable.'
}

$validatorStdout = Join-Path $validation 'artifact-validator.stdout.txt'
$validatorStderr = Join-Path $validation 'artifact-validator.stderr.txt'
Invoke-OwnedProcess $stagedValidator @(
    '--artifact', $candidate,
    '--identity', $stagedIdentity,
    '--replay', $stagedReplay,
    '--manifest', $stagedManifest,
    '--max-bytes', [string]$maximumBytes,
    '--max-events', [string]$maximumEvents
) $ValidatorTimeoutSeconds $validatorStdout $validatorStderr `
    'PowerVR TA artifact validator'

$presentationValidatorStdout = Join-Path $validation `
    'presentation-validator.stdout.txt'
$presentationValidatorStderr = Join-Path $validation `
    'presentation-validator.stderr.txt'
Invoke-OwnedProcess $stagedPresentationValidator @(
    '--artifact', $presentationCandidate,
    '--identity', $stagedIdentity,
    '--replay', $stagedReplay
) $ValidatorTimeoutSeconds $presentationValidatorStdout `
    $presentationValidatorStderr 'PowerVR presentation artifact validator'

if ($hasDraw) {
    $drawValidatorStdout = Join-Path $validation 'draw-validator.stdout.txt'
    $drawValidatorStderr = Join-Path $validation 'draw-validator.stderr.txt'
    Invoke-OwnedProcess $stagedDrawValidator @(
        '--artifact', $drawCandidate,
        '--ta-artifact', $candidate,
        '--presentation-artifact', $presentationCandidate,
        '--identity', $stagedIdentity,
        '--replay', $stagedReplay,
        '--manifest', $stagedManifest,
        '--max-draw-bytes', [string]$MaximumDrawBytes,
        '--max-draw-events', [string]$maximumEvents,
        '--max-presentation-bytes', [string]$MaximumPresentationBytes
    ) $ValidatorTimeoutSeconds $drawValidatorStdout $drawValidatorStderr `
        'PowerVR draw artifact validator'
}

$result = [ordered]@{
    schema = 'flycast-research-pvr-ta-capture-result'
    schema_version = 1
    status = 'validated-candidate'
    output_directory = $output
    candidate = Get-Blob $candidate
    presentation_candidate = Get-Blob $presentationCandidate
    identity = Get-Blob $stagedIdentity
    replay = Get-Blob $stagedReplay
    manifest = Get-Blob $stagedManifest
    validator = Get-Blob $stagedValidator
    presentation_validator = Get-Blob $stagedPresentationValidator
}
if ($hasInitialState) {
    $result['initial_state'] = Get-Blob $stagedInitialState
    $result['initial_state_slot'] = $initialState.Slot
}
if ($hasLuaInputDriver) {
    $result['lua_input_driver'] = Get-Blob $stagedLuaInputDriver
}
if ($hasDraw) {
    $result['draw_candidate'] = Get-Blob $drawCandidate
    $result['draw_validator'] = Get-Blob $stagedDrawValidator
}
$result | ConvertTo-Json -Depth 16

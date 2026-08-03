[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Emulator,
    [Parameter(Mandatory = $true)][string]$Game,
    [Parameter(Mandatory = $true)][string]$Session,
    [string]$AdditionalConfiguration = '',
    [ValidateRange(0, 100)][int]$StateSlot = 0,
    [ValidateSet('Default', 'Interpreter', 'Dynarec')]
    [string]$CpuBackend = 'Default',
    [ValidateRange(100, 60000)][int]$StartupTimeoutMilliseconds = 15000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$emulatorPath = (Resolve-Path -LiteralPath $Emulator).Path
$gamePath = (Resolve-Path -LiteralPath $Game).Path
$sessionPath = [IO.Path]::GetFullPath($Session)
if (Test-Path -LiteralPath $sessionPath) {
    throw "Control session already exists: $sessionPath"
}
if ([IO.Path]::GetExtension($emulatorPath) -cne '.exe') {
    throw 'Emulator must identify an executable.'
}
if ([IO.Path]::GetExtension($gamePath) -notin @('.gdi', '.cue', '.cdi', '.chd')) {
    throw 'Game must be a supported Dreamcast disc image.'
}
if ($AdditionalConfiguration.Contains("`r") -or
        $AdditionalConfiguration.Contains("`n")) {
    throw 'AdditionalConfiguration must be one bounded command-line value.'
}
if ($AdditionalConfiguration.Length -gt 16384) {
    throw 'AdditionalConfiguration exceeds the 16 KiB launcher limit.'
}
$launcherOwnedKeys = @(
    'research:ControlPipe',
    'research:ControlNonce',
    'research:ControlClientExecutable',
    'research:ControlClientSha256',
    'config:Dreamcast.AutoLoadState',
    'config:Dreamcast.SavestateSlot',
    'config:Dynarec.Enabled'
)
foreach ($key in $launcherOwnedKeys) {
    if ($AdditionalConfiguration.IndexOf($key + '=',
            [StringComparison]::OrdinalIgnoreCase) -ge 0) {
        throw "AdditionalConfiguration must not override launcher-owned key $key."
    }
}

$clientPath = [IO.Path]::GetFullPath((Get-Process -Id $PID).Path)
$clientHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $clientPath).Hash.ToLowerInvariant()
$emulatorHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $emulatorPath).Hash.ToLowerInvariant()
$pipeName = 'flycast-research-' + [Guid]::NewGuid().ToString('N')
$random = [byte[]]::new(32)
[Security.Cryptography.RandomNumberGenerator]::Fill($random)
$nonce = [Convert]::ToHexString($random).ToLowerInvariant()

$configuration = @(
    "research:ControlPipe=$pipeName"
    "research:ControlNonce=$nonce"
    "research:ControlClientExecutable=$clientPath"
    "research:ControlClientSha256=$clientHash"
    'config:Dreamcast.AutoLoadState=no'
)
if ($StateSlot -ne 0) {
    $configuration[$configuration.Count - 1] = 'config:Dreamcast.AutoLoadState=yes'
    $configuration += "config:Dreamcast.SavestateSlot=$($StateSlot - 1)"
}
if ($CpuBackend -ceq 'Interpreter') {
    $configuration += 'config:Dynarec.Enabled=no'
}
elseif ($CpuBackend -ceq 'Dynarec') {
    $configuration += 'config:Dynarec.Enabled=yes'
}
if (-not [string]::IsNullOrWhiteSpace($AdditionalConfiguration)) {
    $configuration += $AdditionalConfiguration.Trim(',')
}
$configurationText = [string]::Join(',', $configuration)

$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $emulatorPath
$start.WorkingDirectory = Split-Path -Parent $emulatorPath
$start.UseShellExecute = $false
$start.ArgumentList.Add('--config')
$start.ArgumentList.Add($configurationText)
$start.ArgumentList.Add($gamePath)
$process = [Diagnostics.Process]::Start($start)
if ($null -eq $process) { throw 'Failed to launch Flycast.' }

try {
    $deadline = [DateTime]::UtcNow.AddMilliseconds($StartupTimeoutMilliseconds)
    $ready = $false
    do {
        if ($process.HasExited) {
            throw "Flycast exited during control startup with status $($process.ExitCode)."
        }
        $probe = [IO.Pipes.NamedPipeClientStream]::new('.', $pipeName,
            [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::None)
        try {
            $probe.Connect(100)
            $ready = $true
        }
        catch [TimeoutException] { }
        finally { $probe.Dispose() }
        if (-not $ready) { Start-Sleep -Milliseconds 50 }
    } while (-not $ready -and [DateTime]::UtcNow -lt $deadline)
    if (-not $ready) { throw 'Timed out waiting for the private research control pipe.' }

    $contract = [ordered]@{
        schema = 'flycast-research-control-session'
        schema_version = 1
        pipe_name = $pipeName
        nonce = $nonce
        server_process_id = [uint32]$process.Id
        server_start_filetime_utc = [string]$process.StartTime.ToFileTimeUtc()
        server_executable = $emulatorPath
        server_executable_sha256 = $emulatorHash
        client_executable = $clientPath
        client_executable_sha256 = $clientHash
    }
    $parent = Split-Path -Parent $sessionPath
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        [IO.Directory]::CreateDirectory($parent) | Out-Null
    }
    $temporary = Join-Path $parent ('.' + [IO.Path]::GetFileName($sessionPath) +
        '.' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllText($temporary,
            (($contract | ConvertTo-Json -Depth 10) + "`n"),
            [Text.UTF8Encoding]::new($false))
        Move-Item -LiteralPath $temporary -Destination $sessionPath
    }
    finally {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
    $contract | ConvertTo-Json -Depth 10 -Compress
}
catch {
    if (-not $process.HasExited) {
        $process.CloseMainWindow() | Out-Null
    }
    throw
}

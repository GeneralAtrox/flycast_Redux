[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Emulator,
    [Parameter(Mandatory = $true)][string]$Game,
    [Parameter(Mandatory = $true)][string]$Output,
    [Parameter(Mandatory = $true)][string]$Events,
    [ValidateRange(0, 100)][int]$StateSlot = 0,
    [ValidateSet('Default', 'Interpreter', 'Dynarec')]
    [string]$CpuBackend = 'Default',
    [switch]$DynarecOwnership,
    [ValidateRange(1, 65536)][int]$QueueCapacity = 4096,
    [ValidateRange(1, 100000000)][int]$MaxEvents = 100000,
    [ValidateRange(0, 2147483647)][int]$AutoExitFrames = 0,
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$emulatorPath = (Resolve-Path -LiteralPath $Emulator).Path
$gamePath = (Resolve-Path -LiteralPath $Game).Path
$watcherPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot `
    'lua\hardware-watch.lua')).Path
$outputPath = [IO.Path]::GetFullPath($Output)
if ([IO.Path]::GetExtension($emulatorPath) -cne '.exe') {
    throw 'Emulator must identify a Flycast executable.'
}
if ([IO.Path]::GetExtension($gamePath) -notin @('.gdi', '.cue', '.cdi', '.chd')) {
    throw 'Game must be a supported Dreamcast disc image.'
}
if ([string]::IsNullOrWhiteSpace($Events)) {
    throw 'Events must contain at least one explicit hardware event.'
}
if ($DynarecOwnership -and $CpuBackend -cne 'Dynarec') {
    throw 'DynarecOwnership requires -CpuBackend Dynarec.'
}
if (Test-Path -LiteralPath $outputPath) {
    if (-not $Force) {
        throw "Output already exists; choose a new path or pass -Force: $outputPath"
    }
    Remove-Item -LiteralPath $outputPath -Force
}
$outputParent = Split-Path -Parent $outputPath
if ($outputParent -and -not (Test-Path -LiteralPath $outputParent)) {
    [IO.Directory]::CreateDirectory($outputParent) | Out-Null
}
$emulatorDirectory = Split-Path -Parent $emulatorPath
$relativeWatcher = [IO.Path]::GetRelativePath($emulatorDirectory, $watcherPath)
$configuration = "config:LuaFileName=$relativeWatcher,config:Dreamcast.AutoLoadState=no"
if ($StateSlot -ne 0) {
    $configuration = "config:LuaFileName=$relativeWatcher," +
        "config:Dreamcast.AutoLoadState=yes," +
        "config:Dreamcast.SavestateSlot=$($StateSlot - 1)"
}
if ($CpuBackend -ceq 'Interpreter') {
    $configuration += ',config:Dynarec.Enabled=no'
}
elseif ($CpuBackend -ceq 'Dynarec') {
    $configuration += ',config:Dynarec.Enabled=yes'
}
if ($DynarecOwnership) {
    $configuration += ',research:DynarecObservation=yes'
}
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $emulatorPath
$start.WorkingDirectory = $emulatorDirectory
$start.UseShellExecute = $false
$start.ArgumentList.Add('--config')
$start.ArgumentList.Add($configuration)
$start.ArgumentList.Add($gamePath)
$start.Environment['FLYCAST_HARDWARE_WATCH_OUTPUT'] = $outputPath
$start.Environment['FLYCAST_HARDWARE_WATCH_EVENTS'] = $Events
$start.Environment['FLYCAST_HARDWARE_WATCH_QUEUE_CAPACITY'] = [string]$QueueCapacity
$start.Environment['FLYCAST_HARDWARE_WATCH_MAX_EVENTS'] = [string]$MaxEvents
if ($AutoExitFrames -ne 0) {
    $start.Environment['FLYCAST_HARDWARE_WATCH_AUTO_EXIT_FRAMES'] = `
        [string]$AutoExitFrames
}
else {
    [void]$start.Environment.Remove('FLYCAST_HARDWARE_WATCH_AUTO_EXIT_FRAMES')
}
$process = [Diagnostics.Process]::Start($start)
if ($null -eq $process) { throw 'Failed to start Flycast.' }
$process.WaitForExit()
if ($process.ExitCode -ne 0) {
    throw "Flycast exited with status $($process.ExitCode)."
}
if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
    throw 'Flycast exited without creating hardware discovery output.'
}
Write-Output $outputPath

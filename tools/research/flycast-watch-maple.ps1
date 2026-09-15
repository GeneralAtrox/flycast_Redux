#Requires -Version 7.0
# Uses ProcessStartInfo.ArgumentList and [IO.Path]::GetRelativePath, which
# Windows PowerShell 5.1 lacks. Run with pwsh.

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Emulator,

    [Parameter(Mandatory = $true)]
    [string]$Game,

    [Parameter(Mandatory = $true)]
    [string]$Output,

    [ValidateRange(0, 100)]
    [int]$StateSlot = 0,

    [ValidateRange(1, 65536)]
    [int]$QueueCapacity = 4096,

    [ValidateRange(-1, 3)]
    [int]$Bus = -1,

    [ValidateRange(-1, 5)]
    [int]$Port = -1,

    [ValidateRange(-1, 255)]
    [int]$Command = -1,

    [ValidateRange(0, 2147483647)]
    [int]$AutoExitFrames = 0,

    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$emulatorPath = (Resolve-Path -LiteralPath $Emulator).Path
$gamePath = (Resolve-Path -LiteralPath $Game).Path
$watcherPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'lua\maple-watch.lua')).Path
$outputPath = [System.IO.Path]::GetFullPath($Output)
$outputDirectory = [System.IO.Path]::GetDirectoryName($outputPath)

if ([System.IO.Path]::GetExtension($emulatorPath) -ne '.exe') {
    throw "Emulator must identify a Flycast executable: $emulatorPath"
}
if ([System.IO.Path]::GetExtension($gamePath) -notin @('.gdi', '.cue', '.cdi', '.chd')) {
    throw "Game must be a supported Dreamcast disc image: $gamePath"
}
if (Test-Path -LiteralPath $outputPath) {
    if (-not $Force) {
        throw "Output already exists; choose a new path or pass -Force: $outputPath"
    }
    Remove-Item -LiteralPath $outputPath -Force
}
if ($outputDirectory) {
    New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
}

$emulatorDirectory = [System.IO.Path]::GetDirectoryName($emulatorPath)
$relativeWatcher = [System.IO.Path]::GetRelativePath($emulatorDirectory, $watcherPath)
$configuration = "config:LuaFileName=$relativeWatcher,config:Dreamcast.AutoLoadState=no"
if ($StateSlot -ne 0) {
    $slotIndex = $StateSlot - 1
    $configuration = "config:LuaFileName=$relativeWatcher,config:Dreamcast.AutoLoadState=yes,config:Dreamcast.SavestateSlot=$slotIndex"
}

Write-Host "Flycast Maple discovery output: $outputPath"
Write-Host "Lua watcher: $watcherPath"
if ($StateSlot -ne 0) {
    Write-Host "Auto-loading displayed save-state slot $StateSlot"
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $emulatorPath
$startInfo.WorkingDirectory = $emulatorDirectory
$startInfo.UseShellExecute = $false
$startInfo.ArgumentList.Add('--config')
$startInfo.ArgumentList.Add($configuration)
$startInfo.ArgumentList.Add($gamePath)
$startInfo.Environment['FLYCAST_MAPLE_WATCH_OUTPUT'] = $outputPath
$startInfo.Environment['FLYCAST_MAPLE_WATCH_QUEUE_CAPACITY'] = [string]$QueueCapacity
if ($Bus -ne -1) {
    $startInfo.Environment['FLYCAST_MAPLE_WATCH_BUS'] = [string]$Bus
} else {
    [void]$startInfo.Environment.Remove('FLYCAST_MAPLE_WATCH_BUS')
}
if ($Port -ne -1) {
    $startInfo.Environment['FLYCAST_MAPLE_WATCH_PORT'] = [string]$Port
} else {
    [void]$startInfo.Environment.Remove('FLYCAST_MAPLE_WATCH_PORT')
}
if ($Command -ne -1) {
    $startInfo.Environment['FLYCAST_MAPLE_WATCH_COMMAND'] = [string]$Command
} else {
    [void]$startInfo.Environment.Remove('FLYCAST_MAPLE_WATCH_COMMAND')
}
if ($AutoExitFrames -ne 0) {
    $startInfo.Environment['FLYCAST_MAPLE_WATCH_AUTO_EXIT_FRAMES'] = [string]$AutoExitFrames
} else {
    [void]$startInfo.Environment.Remove('FLYCAST_MAPLE_WATCH_AUTO_EXIT_FRAMES')
}

$process = [System.Diagnostics.Process]::Start($startInfo)
if ($null -eq $process) {
    throw 'Failed to start Flycast'
}
$process.WaitForExit()
$exitCode = $process.ExitCode
if ($exitCode -ne 0) {
    throw "Flycast exited with status $exitCode"
}
if (-not (Test-Path -LiteralPath $outputPath)) {
    throw "Flycast exited without creating the Maple discovery output"
}

Write-Output $outputPath

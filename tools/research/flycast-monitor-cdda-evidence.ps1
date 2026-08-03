[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Emulator,
    [Parameter(Mandatory = $true)][string]$Game,
    [Parameter(Mandatory = $true)][string]$Output,
    [ValidateRange(0, 100)][int]$StateSlot = 0,
    [ValidateSet('Interpreter', 'Dynarec')][string]$CpuBackend = 'Interpreter',
    [switch]$DynarecOwnership,
    [ValidateRange(1, 65536)][int]$QueueCapacity = 4096,
    [ValidateRange(0, 2147483647)][int]$AutoExitFrames = 0,
    [switch]$AllowNoHit,
    [switch]$Force
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$emulatorPath = (Resolve-Path -LiteralPath $Emulator).Path
$gamePath = (Resolve-Path -LiteralPath $Game).Path
$monitorPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot `
    'lua\cdda-evidence-monitor.lua')).Path
$outputPath = [IO.Path]::GetFullPath($Output)
if ([IO.Path]::GetExtension($emulatorPath) -cne '.exe') {
    throw 'Emulator must identify a Flycast executable.'
}
if ([IO.Path]::GetExtension($gamePath) -notin @('.gdi', '.cue', '.cdi', '.chd')) {
    throw 'Game must be a supported Dreamcast disc image.'
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
$relativeMonitor = [IO.Path]::GetRelativePath($emulatorDirectory, $monitorPath)
$configuration = @(
    "config:LuaFileName=$relativeMonitor",
    'config:Dreamcast.AutoLoadState=no',
    'config:Dreamcast.AutoSaveState=no',
    "config:Dynarec.Enabled=$(if ($CpuBackend -ceq 'Dynarec') { 'yes' } else { 'no' })"
)
if ($StateSlot -ne 0) {
    $configuration[1] = 'config:Dreamcast.AutoLoadState=yes'
    $configuration += "config:Dreamcast.SavestateSlot=$($StateSlot - 1)"
}
if ($DynarecOwnership) {
    $configuration += 'research:DynarecObservation=yes'
}
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $emulatorPath
$start.WorkingDirectory = $emulatorDirectory
$start.UseShellExecute = $false
$start.ArgumentList.Add('--config')
$start.ArgumentList.Add(($configuration -join ','))
$start.ArgumentList.Add($gamePath)
$start.Environment['FLYCAST_CDDA_MONITOR_OUTPUT'] = $outputPath
$start.Environment['FLYCAST_CDDA_MONITOR_QUEUE_CAPACITY'] = [string]$QueueCapacity
if ($AutoExitFrames -ne 0) {
    $start.Environment['FLYCAST_CDDA_MONITOR_AUTO_EXIT_FRAMES'] = [string]$AutoExitFrames
}
else {
    [void]$start.Environment.Remove('FLYCAST_CDDA_MONITOR_AUTO_EXIT_FRAMES')
}
$process = [Diagnostics.Process]::Start($start)
if ($null -eq $process) { throw 'Failed to start Flycast.' }
$process.WaitForExit()
if ($process.ExitCode -ne 0) {
    throw "Flycast exited with status $($process.ExitCode)."
}
if (-not (Test-Path -LiteralPath $outputPath -PathType Leaf)) {
    throw 'Flycast exited without creating CD-DA monitor output.'
}
$summary = Get-Content -LiteralPath $outputPath -Tail 1 | ConvertFrom-Json
if ($summary.record_type -cne 'summary' -or -not $summary.complete `
        -or (-not $AllowNoHit -and -not $summary.hit)) {
    throw "Flycast exited before a complete CD-DA evidence hit: $outputPath"
}
Write-Output $outputPath

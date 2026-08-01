[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Emulator,

    [Parameter(Mandatory = $true)]
    [string]$Game,

    [Parameter(Mandatory = $true)]
    [string]$Output,

    [Parameter(Mandatory = $true)]
    [string]$StartAddress,

    [Parameter(Mandatory = $true)]
    [ValidateRange(1, 1048576)]
    [uint64]$Length,

    [ValidateSet('read', 'write', 'both')]
    [string]$Access = 'write',

    [ValidateSet('interpreter', 'dynarec')]
    [string]$Backend = 'interpreter',

    [string]$StartPc = '',

    [string]$EndPc = '',

    [ValidateRange(0, 100)]
    [int]$StateSlot = 0,

    [ValidateRange(1, 65536)]
    [int]$QueueCapacity = 4096,

    [ValidateRange(1, 100000000)]
    [int]$MaxEvents = 100000,

    [ValidateRange(0, 2147483647)]
    [int]$AutoExitFrames = 0,

    [switch]$NoSnapshots,

    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function ConvertTo-GuestAddress {
    param([Parameter(Mandatory = $true)][string]$Value, [Parameter(Mandatory = $true)][string]$Name)
    $text = $Value.Trim()
    try {
        if ($text.StartsWith('0x', [System.StringComparison]::OrdinalIgnoreCase)) {
            $parsed = [Convert]::ToUInt64($text.Substring(2), 16)
        } else {
            $parsed = [uint64]::Parse($text, [Globalization.CultureInfo]::InvariantCulture)
        }
    } catch {
        throw "$Name must be a decimal or 0x-prefixed 32-bit guest address: $Value"
    }
    if ($parsed -gt 4294967295) {
        throw "$Name exceeds the 32-bit guest address space: $Value"
    }
    return [uint64]$parsed
}

$startAddressValue = ConvertTo-GuestAddress -Value $StartAddress -Name 'StartAddress'
$endAddressValue = $startAddressValue + $Length - 1
if ($endAddressValue -gt 4294967295) {
    throw 'StartAddress plus Length exceeds the 32-bit guest address space.'
}
if (($StartPc.Length -eq 0) -ne ($EndPc.Length -eq 0)) {
    throw 'StartPc and EndPc must be supplied together.'
}
$startPcValue = $null
$endPcValue = $null
if ($StartPc.Length -ne 0) {
    $startPcValue = ConvertTo-GuestAddress -Value $StartPc -Name 'StartPc'
    $endPcValue = ConvertTo-GuestAddress -Value $EndPc -Name 'EndPc'
    if ($startPcValue -gt $endPcValue) {
        throw 'StartPc must not exceed EndPc.'
    }
}
if (-not $NoSnapshots -and $Length -gt 65536) {
    throw 'Snapshot-enabled ranges are limited to 65536 bytes; pass -NoSnapshots for a larger watch.'
}
if (-not $NoSnapshots) {
    $aliasStart = [Math]::Floor($startAddressValue / 0x20000000)
    $aliasEnd = [Math]::Floor($endAddressValue / 0x20000000)
    $physicalStart = $startAddressValue % 0x20000000
    $physicalEnd = $endAddressValue % 0x20000000
    if ($aliasStart -ne $aliasEnd -or $physicalStart -lt 0x0c000000 -or
            $physicalEnd -gt 0x0dffffff) {
        throw 'Snapshots are restricted to one Dreamcast main-RAM alias; pass -NoSnapshots for other ranges.'
    }
}

$emulatorPath = (Resolve-Path -LiteralPath $Emulator).Path
$gamePath = (Resolve-Path -LiteralPath $Game).Path
$watcherPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'lua\memory-watch.lua')).Path
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
$configurationValues = @(
    "config:LuaFileName=$relativeWatcher",
    'config:Dreamcast.AutoLoadState=no'
)
if ($Backend -eq 'interpreter') {
    $configurationValues += 'config:Dynarec.Enabled=no'
    $configurationValues += 'research:DynarecObservation=no'
} else {
    $configurationValues += 'config:Dynarec.Enabled=yes'
    $configurationValues += 'research:DynarecObservation=yes'
}
if ($StateSlot -ne 0) {
    $configurationValues[1] = 'config:Dreamcast.AutoLoadState=yes'
    $configurationValues += "config:Dreamcast.SavestateSlot=$($StateSlot - 1)"
}
$configuration = $configurationValues -join ','

Write-Host "Flycast memory discovery output: $outputPath"
Write-Host "Range: 0x$($startAddressValue.ToString('x8'))-0x$($endAddressValue.ToString('x8'))"
Write-Host "Access: $Access; backend: $Backend; maximum events: $MaxEvents"
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
$startInfo.Environment['FLYCAST_MEMORY_WATCH_OUTPUT'] = $outputPath
$startInfo.Environment['FLYCAST_MEMORY_WATCH_START_ADDRESS'] = [string]$startAddressValue
$startInfo.Environment['FLYCAST_MEMORY_WATCH_LENGTH'] = [string]$Length
$startInfo.Environment['FLYCAST_MEMORY_WATCH_ACCESS'] = $Access
$startInfo.Environment['FLYCAST_MEMORY_WATCH_BACKEND'] = $Backend
$startInfo.Environment['FLYCAST_MEMORY_WATCH_QUEUE_CAPACITY'] = [string]$QueueCapacity
$startInfo.Environment['FLYCAST_MEMORY_WATCH_MAX_EVENTS'] = [string]$MaxEvents
$startInfo.Environment['FLYCAST_MEMORY_WATCH_SNAPSHOTS'] = [string](-not $NoSnapshots)
if ($null -ne $startPcValue) {
    $startInfo.Environment['FLYCAST_MEMORY_WATCH_START_PC'] = [string]$startPcValue
    $startInfo.Environment['FLYCAST_MEMORY_WATCH_END_PC'] = [string]$endPcValue
} else {
    [void]$startInfo.Environment.Remove('FLYCAST_MEMORY_WATCH_START_PC')
    [void]$startInfo.Environment.Remove('FLYCAST_MEMORY_WATCH_END_PC')
}
if ($AutoExitFrames -ne 0) {
    $startInfo.Environment['FLYCAST_MEMORY_WATCH_AUTO_EXIT_FRAMES'] = [string]$AutoExitFrames
} else {
    [void]$startInfo.Environment.Remove('FLYCAST_MEMORY_WATCH_AUTO_EXIT_FRAMES')
}

$process = [System.Diagnostics.Process]::Start($startInfo)
if ($null -eq $process) { throw 'Failed to start Flycast' }
$process.WaitForExit()
if ($process.ExitCode -ne 0) { throw "Flycast exited with status $($process.ExitCode)" }
if (-not (Test-Path -LiteralPath $outputPath)) {
    throw 'Flycast exited without creating the memory discovery output.'
}

Write-Output $outputPath

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

    [Parameter(Mandatory = $true)]
    [string]$ProducerStartPc,

    [Parameter(Mandatory = $true)]
    [string]$ProducerEndPc,

    [Parameter(Mandatory = $true)]
    [string]$CallSiteStartPc,

    [Parameter(Mandatory = $true)]
    [string]$CallSiteEndPc,

    [Parameter(Mandatory = $true)]
    [string]$ReturnStartPc,

    [Parameter(Mandatory = $true)]
    [string]$ReturnEndPc,

    [ValidateSet('read', 'write', 'both')]
    [string]$Access = 'write',

    [ValidateSet('interpreter', 'dynarec')]
    [string]$Backend = 'interpreter',

    [ValidateRange(0, 100)]
    [int]$StateSlot = 0,

    [ValidateRange(1, 65536)]
    [int]$QueueCapacity = 8192,

    [ValidateRange(1, 1000000)]
    [int]$MaxSlices = 100,

    [ValidateRange(1, 1024)]
    [int]$MaxCallDepth = 64,

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
$producerStartPcValue = ConvertTo-GuestAddress -Value $ProducerStartPc -Name 'ProducerStartPc'
$producerEndPcValue = ConvertTo-GuestAddress -Value $ProducerEndPc -Name 'ProducerEndPc'
if ($producerStartPcValue -gt $producerEndPcValue) {
    throw 'ProducerStartPc must not exceed ProducerEndPc.'
}
$callSiteStartPcValue = ConvertTo-GuestAddress -Value $CallSiteStartPc -Name 'CallSiteStartPc'
$callSiteEndPcValue = ConvertTo-GuestAddress -Value $CallSiteEndPc -Name 'CallSiteEndPc'
if ($callSiteStartPcValue -gt $callSiteEndPcValue) {
    throw 'CallSiteStartPc must not exceed CallSiteEndPc.'
}
$returnStartPcValue = ConvertTo-GuestAddress -Value $ReturnStartPc -Name 'ReturnStartPc'
$returnEndPcValue = ConvertTo-GuestAddress -Value $ReturnEndPc -Name 'ReturnEndPc'
if ($returnStartPcValue -gt $returnEndPcValue) {
    throw 'ReturnStartPc must not exceed ReturnEndPc.'
}

$emulatorPath = (Resolve-Path -LiteralPath $Emulator).Path
$gamePath = (Resolve-Path -LiteralPath $Game).Path
$watcherPath = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot 'lua\causal-slice-watch.lua')).Path
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

Write-Host "Flycast causal-slice discovery output: $outputPath"
Write-Host "Memory: 0x$($startAddressValue.ToString('x8'))-0x$($endAddressValue.ToString('x8'))"
Write-Host "Producer: 0x$($producerStartPcValue.ToString('x8'))-0x$($producerEndPcValue.ToString('x8'))"
Write-Host "Call sites: 0x$($callSiteStartPcValue.ToString('x8'))-0x$($callSiteEndPcValue.ToString('x8'))"
Write-Host "Returns: 0x$($returnStartPcValue.ToString('x8'))-0x$($returnEndPcValue.ToString('x8'))"
Write-Host "Access: $Access; backend: $Backend; maximum slices: $MaxSlices"
if ($StateSlot -ne 0) {
    Write-Host "Auto-loading displayed save-state slot $StateSlot before arming"
}

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $emulatorPath
$startInfo.WorkingDirectory = $emulatorDirectory
$startInfo.UseShellExecute = $false
$startInfo.ArgumentList.Add('--config')
$startInfo.ArgumentList.Add($configuration)
$startInfo.ArgumentList.Add($gamePath)
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_OUTPUT'] = $outputPath
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_START_ADDRESS'] = [string]$startAddressValue
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_LENGTH'] = [string]$Length
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_PRODUCER_START_PC'] = [string]$producerStartPcValue
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_PRODUCER_END_PC'] = [string]$producerEndPcValue
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_CALL_SITE_START_PC'] = [string]$callSiteStartPcValue
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_CALL_SITE_END_PC'] = [string]$callSiteEndPcValue
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_RETURN_START_PC'] = [string]$returnStartPcValue
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_RETURN_END_PC'] = [string]$returnEndPcValue
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_ACCESS'] = $Access
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_BACKEND'] = $Backend
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_QUEUE_CAPACITY'] = [string]$QueueCapacity
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_MAX_SLICES'] = [string]$MaxSlices
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_MAX_CALL_DEPTH'] = [string]$MaxCallDepth
$startInfo.Environment['FLYCAST_CAUSAL_SLICE_WAIT_FOR_LOAD_STATE'] = [string]($StateSlot -ne 0)

$process = [System.Diagnostics.Process]::Start($startInfo)
if ($null -eq $process) { throw 'Failed to start Flycast' }
$process.WaitForExit()
if ($process.ExitCode -ne 0) { throw "Flycast exited with status $($process.ExitCode)" }
if (-not (Test-Path -LiteralPath $outputPath)) {
    throw 'Flycast exited without creating the causal-slice discovery output.'
}

Write-Output $outputPath

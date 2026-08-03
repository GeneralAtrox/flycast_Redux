#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$FlycastExecutable,
    [Parameter(Mandatory = $true)][string]$Identity,
    [Parameter(Mandatory = $true)][string]$MapleReplay,
    [Parameter(Mandatory = $true)][string]$MapleReplayIdentity,
    [Parameter(Mandatory = $true)][string]$ArtifactValidator,
    [Parameter(Mandatory = $true)][string]$Game,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [ValidateRange(384, 536870912)][int64]$MaximumArtifactBytes = 536870912,
    [ValidateRange(1, 3600)][int]$CaptureTimeoutSeconds = 300,
    [ValidateRange(1, 120)][int]$MinimumRunSeconds = 5,
    [ValidateRange(1, 120)][int]$ExitTimeoutSeconds = 30
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$identityPath = (Resolve-Path -LiteralPath ([IO.Path]::GetFullPath($Identity))).Path
$identityObject = Get-Content -LiteralPath $identityPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 64
if ([string]$identityObject.schema -cne 'flycast-research-identity' -or
        [int]$identityObject.schema_version -ne 2 -or
        [string]$identityObject.firmware.mode -cne 'real') {
    throw 'Hardware capture v2 requires a real-firmware research identity v2.'
}
$capture = Join-Path $PSScriptRoot 'flycast-capture-gdrom-v1.ps1'
& $capture @PSBoundParameters

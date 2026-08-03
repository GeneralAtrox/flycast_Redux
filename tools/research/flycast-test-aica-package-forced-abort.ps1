#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Artifact,
    [Parameter(Mandatory = $true)][string]$Identity,
    [Parameter(Mandatory = $true)][string]$MapleReplay,
    [Parameter(Mandatory = $true)][string]$FlycastExecutable,
    [Parameter(Mandatory = $true)][string]$ArtifactValidator,
    [Parameter(Mandatory = $true)][string]$Publisher
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) `
    ('flycast-research-aica-integration-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    foreach ($token in @('after-staging-v1', 'after-validation-v1')) {
        $accepted = Join-Path $root "accepted-$token"
        $failed = $false
        try {
            & $Publisher -Artifact $Artifact -Identity $Identity `
                -MapleReplay $MapleReplay -FlycastExecutable $FlycastExecutable `
                -ArtifactValidator $ArtifactValidator -OutputDirectory $accepted `
                -IntegrationTestAbortToken $token | Out-Null
        }
        catch { $failed = $true }
        if (-not $failed) { throw "$token unexpectedly succeeded." }
        if (Test-Path -LiteralPath $accepted) {
            throw "$token exposed an accepted directory."
        }
    }
    Write-Output 'PASS flycast-research-aica-package-forced-abort-v1'
}
finally {
    $resolvedRoot = [IO.Path]::GetFullPath($root)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($resolvedRoot.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedRoot) -match
            '^flycast-research-aica-integration-[0-9a-f]+$' -and
            (Test-Path -LiteralPath $resolvedRoot)) {
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
}

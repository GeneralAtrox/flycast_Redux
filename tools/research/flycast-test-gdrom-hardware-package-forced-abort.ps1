#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Artifact,
    [Parameter(Mandatory = $true)][string]$Identity,
    [Parameter(Mandatory = $true)][string]$MapleReplay,
    [Parameter(Mandatory = $true)][string]$MapleReplayIdentity,
    [Parameter(Mandatory = $true)][string]$FlycastExecutable,
    [Parameter(Mandatory = $true)][string]$ArtifactValidator,
    [Parameter(Mandatory = $true)][string]$Publisher
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) `
    ('flycast-research-gdrom-hardware-integration-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    foreach ($token in @('after-staging-v2', 'after-validation-v2')) {
        $accepted = Join-Path $root "accepted-$token"
        $failed = $false
		$failure = ''
        try {
            & $Publisher -Artifact $Artifact -Identity $Identity `
                -MapleReplay $MapleReplay `
                -MapleReplayIdentity $MapleReplayIdentity `
                -FlycastExecutable $FlycastExecutable `
                -ArtifactValidator $ArtifactValidator -OutputDirectory $accepted `
                -IntegrationTestAbortToken $token | Out-Null
        }
		catch { $failed = $true; $failure = $_.Exception.Message }
        if (-not $failed) { throw "$token unexpectedly succeeded." }
		$expectedFailure = if ($token -ceq 'after-staging-v2') {
			'Integration test forced abort after staging.'
		} else { 'Integration test forced abort after validation.' }
		if ($failure -cne $expectedFailure) {
			throw "$token failed at the wrong boundary: $failure"
		}
        if (Test-Path -LiteralPath $accepted) {
            throw "$token exposed an accepted directory."
        }
		$quarantine = Join-Path $root '.flycast-research-gdrom-hardware-quarantine'
		$matching = @(Get-ChildItem -LiteralPath $quarantine -Directory -Force |
			Where-Object {
				$receipt = Join-Path $_.FullName 'rejection.json'
				if (-not (Test-Path -LiteralPath $receipt -PathType Leaf)) { return $false }
				$record = Get-Content -LiteralPath $receipt -Raw -Encoding UTF8 |
					ConvertFrom-Json -Depth 8
				return [string]$record.accepted_directory -ceq $accepted -and
					[string]$record.failure -ceq $expectedFailure
			})
		if ($matching.Count -ne 1) {
			throw "$token did not produce exactly one recoverable quarantine record."
		}
    }
    Write-Output 'PASS flycast-research-gdrom-hardware-package-forced-abort-v2'
}
finally {
    $resolvedRoot = [IO.Path]::GetFullPath($root)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($resolvedRoot.StartsWith($tempRoot,
                [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedRoot) -match
                '^flycast-research-gdrom-hardware-integration-[0-9a-f]+$' -and
            (Test-Path -LiteralPath $resolvedRoot)) {
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
}

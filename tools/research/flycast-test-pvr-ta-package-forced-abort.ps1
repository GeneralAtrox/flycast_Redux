#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Job,
    [Parameter(Mandatory = $true)][string]$Publisher
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8NoBom = [Text.UTF8Encoding]::new($false)
$jobPath = (Resolve-Path -LiteralPath $Job).Path
$publisherPath = (Resolve-Path -LiteralPath $Publisher).Path
$root = Join-Path ([IO.Path]::GetTempPath()) `
    ("flycast-research-pvr-ta-integration-" + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    foreach ($token in @('after-staging-v1', 'after-validation-v1')) {
        $case = Join-Path $root $token
        [IO.Directory]::CreateDirectory($case) | Out-Null
        $jobObject = Get-Content -LiteralPath $jobPath -Raw -Encoding UTF8 |
            ConvertFrom-Json -Depth 64
        $packageId = [Guid]::NewGuid().ToString('D').ToLowerInvariant()
        $accepted = Join-Path $root "accepted-$token"
        $jobObject.package_id = $packageId
        $jobObject.output.accepted_directory = $accepted
        $caseJob = Join-Path $case 'job.json'
        [IO.File]::WriteAllText($caseJob,
            (($jobObject | ConvertTo-Json -Depth 64) + "`n"), $utf8NoBom)
        $failed = $false
        try {
            & $publisherPath -Job $caseJob -IntegrationTestAbortToken $token `
                2>&1 | Out-Null
        }
        catch { $failed = $true }
        if (-not $failed) { throw "$token unexpectedly succeeded." }
        if (Test-Path -LiteralPath $accepted) {
            throw "$token exposed an accepted directory."
        }
        $quarantine = Join-Path (Join-Path $root `
            '.flycast-research-pvr-ta-quarantine') $packageId
        if (-not (Test-Path -LiteralPath $quarantine -PathType Container) -or
                -not (Test-Path -LiteralPath (Join-Path $quarantine `
                    'rejection.json') -PathType Leaf)) {
            throw "$token did not quarantine its private candidate."
        }
    }
    Write-Output 'PASS flycast-research-pvr-ta-package-forced-abort-v1'
}
finally {
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $resolvedRoot = [IO.Path]::GetFullPath($root)
    if ($resolvedRoot.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) `
            -and (Split-Path -Leaf $resolvedRoot) -cmatch `
                '^flycast-research-pvr-ta-integration-[0-9a-f]{32}$' `
            -and (Test-Path -LiteralPath $resolvedRoot)) {
        [IO.Directory]::Delete($resolvedRoot, $true)
    }
}

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
    ('flycast-sh4-ghidra-package-integration-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    foreach ($token in @(
            'after-staging-v1',
            'after-semantic-validation-v1',
            'after-validation-v1')) {
        $jobObject = Get-Content -LiteralPath $jobPath -Raw -Encoding UTF8 |
            ConvertFrom-Json -Depth 64
        $packageId = [Guid]::NewGuid().ToString('D').ToLowerInvariant()
        $accepted = Join-Path $root "accepted-$token"
        $jobObject.package_id = $packageId
        $jobObject.output.accepted_directory = $accepted
        $caseJob = Join-Path $root "job-$token.json"
        [IO.File]::WriteAllText($caseJob,
            (($jobObject | ConvertTo-Json -Depth 64) + "`n"), $utf8NoBom)
        $failure = ''
        try {
            & $publisherPath -Job $caseJob -IntegrationTestAbortToken $token |
                Out-Null
        }
        catch { $failure = $_.Exception.Message }
        if ([string]::IsNullOrWhiteSpace($failure)) {
            throw "$token unexpectedly succeeded."
        }
        if (Test-Path -LiteralPath $accepted) {
            throw "$token exposed an accepted directory."
        }
        $quarantine = Join-Path (Join-Path $root `
            '.flycast-research-sh4-ghidra-quarantine') $packageId
        if (-not (Test-Path -LiteralPath $quarantine -PathType Container) -or
                -not (Test-Path -LiteralPath (Join-Path $quarantine 'rejection.json') `
                    -PathType Leaf)) {
            throw "$token did not quarantine its private candidate."
        }
        if ($token -ceq 'after-validation-v1' -and
                -not (Test-Path -LiteralPath `
                    (Join-Path $quarantine 'package-validation.json') -PathType Leaf)) {
            throw 'The post-validation abort did not retain its receipt.'
        }
    }
    Write-Output 'PASS flycast-research-sh4-ghidra-package-forced-abort-v1'
}
finally {
    $resolvedRoot = [IO.Path]::GetFullPath($root)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if ($resolvedRoot.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -and
            (Split-Path -Leaf $resolvedRoot) -match
            '^flycast-sh4-ghidra-package-integration-[0-9a-f]+$' -and
            (Test-Path -LiteralPath $resolvedRoot)) {
        Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
    }
}

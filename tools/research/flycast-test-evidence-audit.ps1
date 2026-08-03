#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$KnownGoodPackage,
    [string]$ValidatorDirectory = '',
    [string]$WorkDirectory = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8 = [Text.UTF8Encoding]::new($false)

function Invoke-Audit([string]$Root, [string]$Output) {
    $arguments = @(
        '-NoLogo', '-NoProfile', '-NonInteractive', '-File',
        (Join-Path $PSScriptRoot 'flycast-audit-evidence.ps1'),
        '-Root', $Root,
        '-Output', $Output
    )
    if (-not [string]::IsNullOrWhiteSpace($ValidatorDirectory)) {
        $arguments += @('-ValidatorDirectory', $ValidatorDirectory)
    }
    & (Join-Path $PSHOME 'pwsh.exe') @arguments
    return $LASTEXITCODE
}

function Write-Json([string]$Path, [object]$Value) {
    [IO.File]::WriteAllText($Path,
        (($Value | ConvertTo-Json -Depth 16) + "`n"), $utf8)
}

function New-PresentationReceipt([string]$PackageId, [string]$PayloadHash) {
    return [ordered]@{
        schema = 'flycast-research-pvr-presentation-package-validation'
        schema_version = 1
        package_id = $PackageId
        status = 'accepted'
        entries = @([ordered]@{
            path = 'data.bin'
            size = 3
            sha256 = $PayloadHash
        })
    }
}

function New-Sh4Receipt([string]$PackageId, [string]$SourceJobHash,
        [int64]$SourceJobSize) {
    return [ordered]@{
        schema = 'flycast-research-sh4-equivalence-package-validation'
        schema_version = 1
        package_id = $PackageId
        status = 'accepted'
        entries = @([ordered]@{
            path = 'source-job.json'
            size = $SourceJobSize
            sha256 = $SourceJobHash
        })
    }
}

$good = (Resolve-Path -LiteralPath ([IO.Path]::GetFullPath($KnownGoodPackage))).Path
if ([string]::IsNullOrWhiteSpace($WorkDirectory)) {
    $WorkDirectory = [IO.Path]::GetTempPath()
}
$work = (Resolve-Path -LiteralPath ([IO.Path]::GetFullPath($WorkDirectory))).Path
$workItem = Get-Item -LiteralPath $work -Force
if (-not $workItem.PSIsContainer -or
        (($workItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
    throw 'WorkDirectory must be a non-linked directory.'
}
$testRoot = Join-Path $work "flycast-evidence-audit-test-$([Guid]::NewGuid().ToString('D'))"
[IO.Directory]::CreateDirectory($testRoot) | Out-Null

$successReportPath = Join-Path $testRoot 'known-good-report.json'
$successCode = Invoke-Audit $good $successReportPath
$success = Get-Content -LiteralPath $successReportPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 32
if ($successCode -ne 0 -or [string]$success.status -cne 'accepted' -or
        [int]$success.package_count -ne 1 -or [int]$success.accepted_count -ne 1) {
    throw 'Known-good package did not pass the evidence auditor.'
}

$negativeRoot = Join-Path $testRoot 'negative'
$unknown = Join-Path $negativeRoot 'accepted-unknown'
$mutated = Join-Path $negativeRoot 'accepted-mutated'
$duplicateA = Join-Path $negativeRoot 'accepted-duplicate-a'
$duplicateB = Join-Path $negativeRoot 'accepted-duplicate-b'
$collisionA = Join-Path $negativeRoot 'accepted-collision-a'
$collisionB = Join-Path $negativeRoot 'accepted-collision-b'
$jobCollisionA = Join-Path $negativeRoot 'accepted-job-collision-a'
$jobCollisionB = Join-Path $negativeRoot 'accepted-job-collision-b'
foreach ($directory in @(
        $unknown, $mutated, $duplicateA, $duplicateB, $collisionA,
        $collisionB, $jobCollisionA, $jobCollisionB)) {
    [IO.Directory]::CreateDirectory($directory) | Out-Null
}

$bytes = [byte[]](0x61, 0x62, 0x63)
[IO.File]::WriteAllBytes((Join-Path $mutated 'data.bin'), $bytes)
Write-Json (Join-Path $mutated 'package-validation.json') `
    (New-PresentationReceipt '12345678-1234-1234-1234-123456789abc' ('0' * 64))

$payloadPath = Join-Path $duplicateA 'data.bin'
[IO.File]::WriteAllBytes($payloadPath, $bytes)
$payloadHash = (Get-FileHash -LiteralPath $payloadPath -Algorithm SHA256).Hash.ToLowerInvariant()
$duplicateId = 'abcdefab-cdef-abcd-efab-cdefabcdefab'
Write-Json (Join-Path $duplicateA 'package-validation.json') `
    (New-PresentationReceipt $duplicateId $payloadHash)
[IO.File]::WriteAllBytes((Join-Path $duplicateB 'data.bin'), $bytes)
Write-Json (Join-Path $duplicateB 'package-validation.json') `
    (New-PresentationReceipt $duplicateId $payloadHash)

$collisionId = 'bcdefabc-defa-bcde-fabc-defabcdefabc'
[IO.File]::WriteAllBytes((Join-Path $collisionA 'data.bin'), $bytes)
Write-Json (Join-Path $collisionA 'package-validation.json') `
    (New-PresentationReceipt $collisionId $payloadHash)
$differentBytes = [byte[]](0x61, 0x62, 0x64)
$differentPayloadPath = Join-Path $collisionB 'data.bin'
[IO.File]::WriteAllBytes($differentPayloadPath, $differentBytes)
$differentPayloadHash = (Get-FileHash -LiteralPath $differentPayloadPath `
    -Algorithm SHA256).Hash.ToLowerInvariant()
Write-Json (Join-Path $collisionB 'package-validation.json') `
    (New-PresentationReceipt $collisionId $differentPayloadHash)

$jobId = 'cdefabcd-efab-cdef-abcd-efabcdefabcd'
$jobPackageIds = @(
    'defabcde-fabc-defa-bcde-fabcdefabcde',
    'efabcdef-abcd-efab-cdef-abcdefabcdef'
)
$jobDirectories = @($jobCollisionA, $jobCollisionB)
for ($index = 0; $index -lt $jobDirectories.Count; ++$index) {
    $sourceJobPath = Join-Path $jobDirectories[$index] 'source-job.json'
    Write-Json $sourceJobPath ([ordered]@{
        schema = 'flycast-research-sh4-equivalence-job'
        schema_version = 1
        job_id = $jobId
        fixture_variant = $index
    })
    $sourceJob = Get-Item -LiteralPath $sourceJobPath
    $sourceJobHash = (Get-FileHash -LiteralPath $sourceJobPath `
        -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Json (Join-Path $jobDirectories[$index] 'package-validation.json') `
        (New-Sh4Receipt $jobPackageIds[$index] $sourceJobHash $sourceJob.Length)
}

$negativeReportPath = Join-Path $testRoot 'negative-report.json'
$negativeCode = Invoke-Audit $negativeRoot $negativeReportPath
$negative = Get-Content -LiteralPath $negativeReportPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 32
if ($negativeCode -ne 1 -or [string]$negative.status -cne 'rejected' -or
        [int]$negative.package_count -ne 8 -or [int]$negative.rejected_count -ne 8) {
    throw 'Negative evidence-audit summary is incorrect.'
}
$byPath = @{}
foreach ($package in @($negative.packages)) { $byPath[[string]$package.path] = $package }
if ([string]$byPath['accepted-unknown'].diagnostic -cnotmatch
        'exactly one recognized receipt') {
    throw 'Unknown accepted-directory rejection was not observed.'
}
if ([string]$byPath['accepted-mutated'].diagnostic -cnotmatch
        'SHA-256 differs') {
    throw 'Mutated receipt-entry rejection was not observed.'
}
foreach ($path in @('accepted-duplicate-a', 'accepted-duplicate-b')) {
    if ([string]$byPath[$path].diagnostic -cnotmatch
            'identical locked package content') {
        throw "Mirrored package-ID rejection was not observed for $path."
    }
}
foreach ($path in @('accepted-collision-a', 'accepted-collision-b')) {
    if ([string]$byPath[$path].diagnostic -cnotmatch
            'Package identity collision') {
        throw "Conflicting package-ID rejection was not observed for $path."
    }
}
foreach ($path in @('accepted-job-collision-a', 'accepted-job-collision-b')) {
    if ([string]$byPath[$path].diagnostic -cnotmatch
            'SH-4 job identity collision') {
        throw "Conflicting SH-4 job-ID rejection was not observed for $path."
    }
}

Write-Output "ACCEPTED flycast-research-evidence-audit-tests $testRoot"
exit 0

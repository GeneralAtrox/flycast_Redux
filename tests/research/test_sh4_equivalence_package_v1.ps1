[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Fixture,
    [Parameter(Mandatory = $true)][string]$Comparator,
    [Parameter(Mandatory = $true)][string]$Publisher,
    [Parameter(Mandatory = $true)][string]$Validator,
    [Parameter(Mandatory = $true)][string]$JobSchema,
    [Parameter(Mandatory = $true)][string]$PackageSchema,
    [Parameter(Mandatory = $true)][string]$ValidationSchema,
    [Parameter(Mandatory = $true)][string]$QuarantineSchema
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Invoke-Fixture([string]$Root, [string]$Accepted, [string]$Job,
        [string]$PackageId, [switch]$Divergent) {
    $arguments = @(
        '--root', $Root,
        '--comparator', $Comparator,
        '--publisher', $Publisher,
        '--validator', $Validator,
        '--accepted', $Accepted,
        '--publication-job', $Job,
        '--package-id', $PackageId
    )
    if ($Divergent) { $arguments += '--divergent' }
    & $Fixture @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Equivalence process fixture failed.' }
}

function Assert-PublisherFailure([string]$Job, [string]$Token = '') {
    $message = ''
    try {
        if ($Token) { & $Publisher -Job $Job -IntegrationTestAbortToken $Token | Out-Null }
        else { & $Publisher -Job $Job | Out-Null }
    }
    catch { $message = $_.Exception.Message }
    Assert-True (-not [string]::IsNullOrWhiteSpace($message)) `
        'A rejected publication unexpectedly succeeded.'
    return $message
}

foreach ($schema in @($JobSchema, $PackageSchema, $ValidationSchema, $QuarantineSchema)) {
    $parsed = Get-Content -LiteralPath $schema -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 64
    Assert-True ($null -ne $parsed.'$schema') "Schema is not a typed JSON schema: $schema"
}

$root = Join-Path ([IO.Path]::GetTempPath()) `
    ('flycast-sh4-equivalence-package-integration-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    $successSources = Join-Path $root 'success-sources'
    $successAccepted = Join-Path $root 'success-accepted'
    $successJob = Join-Path $successSources 'publication-job.json'
    Invoke-Fixture $successSources $successAccepted $successJob `
        '11111111-2222-3333-4444-555555555555'
    & $Publisher -Job $successJob | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'Equivalent publication failed.'
    Assert-True (Test-Path -LiteralPath $successAccepted -PathType Container) `
        'Equivalent package was not atomically published.'
    & $Validator --package $successAccepted | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'Published package failed read-only validation.'
    $receipt = Get-Content -LiteralPath (Join-Path $successAccepted `
        'package-validation.json') -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 64
    Assert-True ([string]$receipt.status -ceq 'accepted') `
        'Published package receipt is not accepted.'

    [IO.File]::AppendAllText((Join-Path $successAccepted 'interpreter.fcso'), 'x')
    & $Validator --package $successAccepted 2>$null | Out-Null
    Assert-True ($LASTEXITCODE -ne 0) `
        'Mutation of a locked observation trace was not rejected.'

    $comparisonSources = Join-Path $root 'comparison-abort-sources'
    $comparisonAccepted = Join-Path $root 'comparison-abort-accepted'
    $comparisonJob = Join-Path $comparisonSources 'publication-job.json'
    $comparisonId = '22222222-3333-4444-5555-666666666666'
    Invoke-Fixture $comparisonSources $comparisonAccepted $comparisonJob $comparisonId
    $message = Assert-PublisherFailure $comparisonJob 'after-comparison-v1'
    Assert-True ($message -match 'forced abort after equivalence comparison') `
        'Comparison abort did not report the intended failpoint.'
    Assert-True (-not (Test-Path -LiteralPath $comparisonAccepted)) `
        'Comparison abort exposed an accepted directory.'
    Assert-True (Test-Path -LiteralPath (Join-Path $root `
        ".flycast-research-sh4-equivalence-quarantine/$comparisonId/rejection.json") `
        -PathType Leaf) 'Comparison abort was not quarantined.'

    $validationSources = Join-Path $root 'validation-abort-sources'
    $validationAccepted = Join-Path $root 'validation-abort-accepted'
    $validationJob = Join-Path $validationSources 'publication-job.json'
    $validationId = '33333333-4444-5555-6666-777777777777'
    Invoke-Fixture $validationSources $validationAccepted $validationJob $validationId
    $message = Assert-PublisherFailure $validationJob 'after-validation-v1'
    Assert-True ($message -match 'forced abort after equivalence package validation') `
        'Validation abort did not report the intended failpoint.'
    Assert-True (-not (Test-Path -LiteralPath $validationAccepted)) `
        'Validation abort exposed an accepted directory.'
    Assert-True (Test-Path -LiteralPath (Join-Path $root `
        ".flycast-research-sh4-equivalence-quarantine/$validationId/package-validation.json") `
        -PathType Leaf) 'Validated abort did not retain its receipt in quarantine.'

    $divergentSources = Join-Path $root 'divergent-sources'
    $divergentAccepted = Join-Path $root 'divergent-accepted'
    $divergentJob = Join-Path $divergentSources 'publication-job.json'
    $divergentId = '44444444-5555-6666-7777-888888888888'
    Invoke-Fixture $divergentSources $divergentAccepted $divergentJob $divergentId -Divergent
    $message = Assert-PublisherFailure $divergentJob
    Assert-True ($message -match 'traces diverged') `
        'Divergent traces did not report semantic divergence.'
    Assert-True (-not (Test-Path -LiteralPath $divergentAccepted)) `
        'Divergent traces exposed an accepted directory.'
    $divergentReport = Join-Path $root `
        ".flycast-research-sh4-equivalence-quarantine/$divergentId/equivalence-report.json"
    Assert-True (Test-Path -LiteralPath $divergentReport -PathType Leaf) `
        'Divergent candidate did not retain its typed report.'
    $report = Get-Content -LiteralPath $divergentReport -Raw -Encoding UTF8 |
        ConvertFrom-Json -Depth 64
    Assert-True ([string]$report.status -ceq 'divergent') `
        'Quarantined divergence report has the wrong status.'

    Write-Host 'Flycast SH-4 equivalence package v1 integration tests passed.'
}
finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}

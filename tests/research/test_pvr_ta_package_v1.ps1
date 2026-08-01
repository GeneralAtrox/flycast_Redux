[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Sh4Fixture,
    [Parameter(Mandatory = $true)][string]$Sh4Comparator,
    [Parameter(Mandatory = $true)][string]$Sh4Publisher,
    [Parameter(Mandatory = $true)][string]$Sh4Validator,
    [Parameter(Mandatory = $true)][string]$PvrFixture,
    [Parameter(Mandatory = $true)][string]$PvrPublisher,
    [Parameter(Mandatory = $true)][string]$ArtifactValidator,
    [Parameter(Mandatory = $true)][string]$PackageValidator,
    [Parameter(Mandatory = $true)][string]$JobSchema,
    [Parameter(Mandatory = $true)][string]$ValidationSchema
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Invoke-PvrFixture([string]$Root, [string]$Base, [string]$Accepted,
        [string]$Job, [string]$PackageId, [switch]$Incomplete) {
    $arguments = @(
        '--root', $Root,
        '--base', $Base,
        '--backend', 'interpreter',
        '--static-analysis', (Join-Path (Split-Path -Parent $Base) 'sources/static-analysis.bin'),
        '--program', (Join-Path (Split-Path -Parent $Base) 'sources/emulator.bin'),
        '--publisher', $PvrPublisher,
        '--artifact-validator', $ArtifactValidator,
        '--package-validator', $PackageValidator,
        '--accepted', $Accepted,
        '--publication-job', $Job,
        '--package-id', $PackageId
    )
    if ($Incomplete) { $arguments += '--incomplete' }
    & $PvrFixture @arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'PowerVR TA process fixture failed.' }
}

function Assert-PublisherFailure([string]$Job, [string]$Token = '') {
    $message = ''
    try {
        if ($Token) {
            & $PvrPublisher -Job $Job -IntegrationTestAbortToken $Token | Out-Null
        }
        else { & $PvrPublisher -Job $Job | Out-Null }
    }
    catch { $message = $_.Exception.Message }
    Assert-True (-not [string]::IsNullOrWhiteSpace($message)) `
        'A rejected PowerVR TA publication unexpectedly succeeded.'
    return $message
}

foreach ($schema in @($JobSchema, $ValidationSchema)) {
    $parsed = Get-Content -LiteralPath $schema -Raw -Encoding UTF8 |
        ConvertFrom-Json -Depth 64
    Assert-True ($null -ne $parsed.'$schema') "Schema is not a typed JSON schema: $schema"
}

$root = Join-Path ([IO.Path]::GetTempPath()) `
    ('flycast-research-pvr-ta-integration-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    $baseParent = Join-Path $root 'base'
    $baseSources = Join-Path $baseParent 'sources'
    $baseAccepted = Join-Path $baseParent 'accepted'
    $baseJob = Join-Path $baseSources 'publication-job.json'
    & $Sh4Fixture --root $baseSources --comparator $Sh4Comparator `
        --publisher $Sh4Publisher --validator $Sh4Validator --accepted $baseAccepted `
        --publication-job $baseJob --package-id '11111111-2222-3333-4444-555555555555' |
        Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'SH-4 equivalence fixture failed.'
    & $Sh4Publisher -Job $baseJob | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'Base SH-4 equivalence publication failed.'

    $successSources = Join-Path $root 'success-sources'
    $successAccepted = Join-Path $root 'success-accepted'
    $successJob = Join-Path $successSources 'publication-job.json'
    Invoke-PvrFixture $successSources $baseAccepted $successAccepted $successJob `
        '22222222-3333-4444-5555-666666666666'
    & $PvrPublisher -Job $successJob | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'PowerVR TA publication failed.'
    Assert-True (Test-Path -LiteralPath $successAccepted -PathType Container) `
        'PowerVR TA package was not atomically published.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $root `
        '.flycast-research-pvr-ta-candidate-22222222-3333-4444-5555-666666666666'))) `
        'Private candidate remained after publication.'
    & $PackageValidator --package $successAccepted | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'Published package failed read-only validation.'
    $receipt = Get-Content -LiteralPath (Join-Path $successAccepted `
        'package-validation.json') -Raw -Encoding UTF8 | ConvertFrom-Json -Depth 64
    Assert-True ([string]$receipt.status -ceq 'accepted') `
        'Published package receipt is not accepted.'
    Assert-True (@($receipt.entries).Count -eq 9) `
        'Published package receipt does not cover the fixed inventory.'

    [IO.File]::AppendAllText((Join-Path $successAccepted 'pvr-ta.fcpvr'), 'x')
    & $PackageValidator --package $successAccepted 2>$null | Out-Null
    Assert-True ($LASTEXITCODE -ne 0) `
        'Mutation of the accepted PowerVR artifact was not rejected.'

    $stagingSources = Join-Path $root 'staging-abort-sources'
    $stagingAccepted = Join-Path $root 'staging-abort-accepted'
    $stagingJob = Join-Path $stagingSources 'publication-job.json'
    $stagingId = '33333333-4444-5555-6666-777777777777'
    Invoke-PvrFixture $stagingSources $baseAccepted $stagingAccepted $stagingJob $stagingId
    $message = Assert-PublisherFailure $stagingJob 'after-staging-v1'
    Assert-True ($message -match 'forced abort after staging') `
        'Staging abort did not report its failpoint.'
    Assert-True (-not (Test-Path -LiteralPath $stagingAccepted)) `
        'Staging abort exposed an accepted directory.'
    Assert-True (Test-Path -LiteralPath (Join-Path $root `
        ".flycast-research-pvr-ta-quarantine/$stagingId/rejection.json") -PathType Leaf) `
        'Staging abort was not quarantined.'

    $validationSources = Join-Path $root 'validation-abort-sources'
    $validationAccepted = Join-Path $root 'validation-abort-accepted'
    $validationJob = Join-Path $validationSources 'publication-job.json'
    $validationId = '44444444-5555-6666-7777-888888888888'
    Invoke-PvrFixture $validationSources $baseAccepted $validationAccepted `
        $validationJob $validationId
    $message = Assert-PublisherFailure $validationJob 'after-validation-v1'
    Assert-True ($message -match 'forced abort after validation') `
        'Validation abort did not report its failpoint.'
    Assert-True (-not (Test-Path -LiteralPath $validationAccepted)) `
        'Validation abort exposed an accepted directory.'
    Assert-True (Test-Path -LiteralPath (Join-Path $root `
        ".flycast-research-pvr-ta-quarantine/$validationId/package-validation.json") `
        -PathType Leaf) 'Validated abort did not retain its receipt in quarantine.'

    $incompleteSources = Join-Path $root 'incomplete-sources'
    $incompleteAccepted = Join-Path $root 'incomplete-accepted'
    $incompleteJob = Join-Path $incompleteSources 'publication-job.json'
    $incompleteId = '55555555-6666-7777-8888-999999999999'
    Invoke-PvrFixture $incompleteSources $baseAccepted $incompleteAccepted `
        $incompleteJob $incompleteId -Incomplete
    $message = Assert-PublisherFailure $incompleteJob
    Assert-True ($message -match 'artifact validator') `
        'Incomplete artifact rejection did not come from the independent validator.'
    Assert-True (-not (Test-Path -LiteralPath $incompleteAccepted)) `
        'Incomplete artifact exposed an accepted directory.'

    Write-Host 'Flycast PowerVR TA package v1 integration tests passed.'
}
finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}

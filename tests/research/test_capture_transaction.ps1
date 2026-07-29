[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Driver,
    [Parameter(Mandatory = $true)][string]$Fixture,
    [Parameter(Mandatory = $true)][string]$MapleValidator,
    [Parameter(Mandatory = $true)][string]$CaptureValidator,
    [Parameter(Mandatory = $true)][string]$TranscriptSchema
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$utf8 = [Text.UTF8Encoding]::new($false)
$temporaryRoot = [IO.Path]::Combine([IO.Path]::GetTempPath(),
    'flycast-research-capture-tests-' + [Guid]::NewGuid().ToString('N'))
$sentinel = $null
$sentinelIdentity = $null

function Get-Hash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-TextHash([string]$Text) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString(
            $algorithm.ComputeHash($utf8.GetBytes($Text)))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $algorithm.Dispose()
    }
}

function Get-ByteHash([byte[]]$Bytes) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString(
            $algorithm.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $algorithm.Dispose()
    }
}

function Get-Blob([string]$Path) {
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $item = Get-Item -LiteralPath $resolved
    return [ordered]@{
        path = $resolved
        size = [int64]$item.Length
        sha256 = Get-Hash $resolved
    }
}

function Write-Json([string]$Path, [object]$Value) {
    [IO.File]::WriteAllText($Path,
        ($Value | ConvertTo-Json -Depth 32) + [Environment]::NewLine, $utf8)
}

function New-Identity([string]$Path, [string]$FixturePath, [string]$Media,
        [string]$Track, [string]$IpBin, [string]$Boot, [string]$Flash,
        [switch]$OmitBios) {
    $canonicalConfiguration = '{"autoload_state":false,"autosave_state":false,' +
        '"cpu_backend":"interpreter","ggpo":false,"threaded_rendering":false}'
    $configurationBytes = $utf8.GetBytes($canonicalConfiguration)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $configurationHash = ([BitConverter]::ToString(
            $algorithm.ComputeHash($configurationBytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $algorithm.Dispose()
    }
    $firmware = [ordered]@{
        mode = 'hle'
        hle_identity = 'capture-process-fixture-v1'
        flash_initial = Get-Blob $Flash
    }
    if (-not $OmitBios) { $firmware['bios'] = $null }
    $identity = [ordered]@{
        schema = 'flycast-research-identity'
        schema_version = 1
        media = [ordered]@{
            kind = 'gdi'
            source = Get-Blob $Media
            tracks = @(
                [ordered]@{
                    path = (Get-Blob $Track).path
                    size = (Get-Blob $Track).size
                    sha256 = (Get-Blob $Track).sha256
                    track = 1
                    start_fad = 150
                    sector_size = 2352
                    offset = 0
                }
            )
            ip_bin = Get-Blob $IpBin
            boot_executable = [ordered]@{
                path = (Get-Blob $Boot).path
                size = (Get-Blob $Boot).size
                sha256 = (Get-Blob $Boot).sha256
                name = 'FIXTURE.BIN'
                load_address = '0x8c010000'
            }
        }
        firmware = $firmware
        persistent_devices = @()
        emulator = [ordered]@{
            git_commit = '0000000000000000000000000000000000000000'
            executable = Get-Blob $FixturePath
        }
        configuration = [ordered]@{
            values = [ordered]@{
                autoload_state = $false
                autosave_state = $false
                cpu_backend = 'interpreter'
                ggpo = $false
                threaded_rendering = $false
            }
            sha256 = $configurationHash
        }
    }
    Write-Json -Path $Path -Value $identity
}

function New-Job([string]$Path, [string]$Identity, [string]$Output,
        [string]$Mode, [int]$TimeoutSeconds = 20,
        [string]$ArtifactName = 'maple.fcmr',
        [string]$ValidatorPath = $MapleValidator,
        [string[]]$ValidatorPrefix = @(),
        [int]$ValidatorTimeoutSeconds = 20,
        [string]$CaptureValidatorPath = $CaptureValidator) {
    $job = [ordered]@{
        schema = 'flycast-research-capture-job'
        schema_version = 1
        driver = Get-Blob $Driver
        identity_manifest = Get-Blob $Identity
        emulator = [ordered]@{
            stage_executable = $true
            interactive = $false
            arguments_prefix = @($Mode)
            authenticated_inputs = @()
            runtime_files = @()
        }
        validator = [ordered]@{
            executable = Get-Blob $ValidatorPath
            arguments_prefix = @($ValidatorPrefix)
            authenticated_inputs = @()
        }
        capture_validator = [ordered]@{
            executable = Get-Blob $CaptureValidatorPath
        }
        output = [ordered]@{
            accepted_directory = [IO.Path]::GetFullPath($Output)
            artifact_name = $ArtifactName
        }
        limits = [ordered]@{
            trace_max_bytes = 1048576
            emulator_timeout_seconds = $TimeoutSeconds
            validator_timeout_seconds = $ValidatorTimeoutSeconds
            stability_interval_ms = 100
        }
        metadata = [ordered]@{
            purpose = 'capture-transaction-integration-test'
        }
    }
    Write-Json -Path $Path -Value $job
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-V1ContractSchemas {
    $schemaRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\docs\research'))
    $expected = [ordered]@{
        'flycast-research-capture-job-v1.schema.json' =
            'c045487db37d60df9ed409429d6ca70da5704ddc7c3277506ddc8d0b724e5314'
        'flycast-research-capture-transcript-v1.schema.json' =
            'b11b5ab950d89308d560595b2512b8708ac3c84c244f44b1b7882c4551dff3ea'
        'flycast-research-capture-validation-v1.schema.json' =
            '740f49022cd3d0f55c86cd997a8d8e11fc0032625ddf1daaff055a451211d805'
        'flycast-research-identity-v1.schema.json' =
            '07cbd4a368806dff4fff28b1c1bc1f519539b1b2abf63afbe65fa6d3072b8b58'
        'flycast-research-quarantine-v1.schema.json' =
            '254f4f648a943f3952d98fb4c64c7d89394824595d7ca6e16a816866d8f2c51f'
    }
    foreach ($entry in $expected.GetEnumerator()) {
        $path = Join-Path $schemaRoot $entry.Key
        Assert-True (Test-Path -LiteralPath $path -PathType Leaf) `
            "Frozen v1 schema is missing: $($entry.Key)"
        Assert-True ((Get-Hash $path) -ceq $entry.Value) `
            "Frozen v1 schema changed without a version increment: $($entry.Key)"
    }
}

function Assert-TranscriptSchema([string]$Path) {
    $json = Get-Content -LiteralPath $Path -Raw -Encoding UTF8
    Assert-True (Test-Json -Json $json -SchemaFile $TranscriptSchema -ErrorAction Stop) `
        "Transcript does not conform to the v1 schema: $Path"
}

function Get-OnlyQuarantine([string]$Root, [string]$Label) {
    $quarantineRoot = Join-Path $Root '.flycast-research-quarantine'
    $items = @(Get-ChildItem -LiteralPath $quarantineRoot -Directory -Force)
    Assert-True ($items.Count -eq 1) "$Label did not create exactly one quarantine package."
    return $items[0].FullName
}

function Stop-ExactProcessFromTranscript([object]$ProcessRecord, [string]$Label) {
    $candidate = Get-Process -Id ([int]$ProcessRecord.pid) -ErrorAction Stop
    try {
        Assert-True ([string]::Equals($candidate.Path, [string]$ProcessRecord.staged_executable.path,
            [StringComparison]::OrdinalIgnoreCase)) "$Label executable identity changed."
        Assert-True ($candidate.StartTime.ToUniversalTime().Ticks -eq
            [int64]$ProcessRecord.start_utc_ticks) "$Label start identity changed."
        $candidate.Kill($true)
        Assert-True ($candidate.WaitForExit(10000)) "$Label did not exit after exact test cleanup."
    }
    finally {
        $candidate.Dispose()
    }
}

function Assert-ToolProcessExited([string]$Marker, [string]$Label) {
    Assert-True (Test-Path -LiteralPath $Marker -PathType Leaf) "$Label did not write its PID marker."
    $toolPid = [int](Get-Content -LiteralPath $Marker -Raw)
    $candidate = Get-Process -Id $toolPid -ErrorAction SilentlyContinue
    if ($null -ne $candidate) {
        try { throw "$Label process $toolPid is still running." }
        finally { $candidate.Dispose() }
    }
}

function Assert-RetainedProcess([object]$Record, [string]$ExpectedRole,
        [string]$ExpectedPath, [string]$Label) {
    Assert-True ($Record.role -ceq $ExpectedRole) "$Label role is not exact."
    Assert-True ([int64]$Record.pid -gt 0) "$Label PID is missing."
    Assert-True ([int64]$Record.start_utc_ticks -gt 0) "$Label start identity is missing."
    Assert-True ([IO.Path]::IsPathFullyQualified([string]$Record.executable_path)) `
        "$Label executable path is not absolute."
    if (-not [string]::IsNullOrWhiteSpace($ExpectedPath)) {
        Assert-True ([string]::Equals([IO.Path]::GetFullPath([string]$Record.executable_path),
            [IO.Path]::GetFullPath($ExpectedPath), [StringComparison]::OrdinalIgnoreCase)) `
            "$Label executable path is unexpected."
    }
    Assert-True (@($Record.arguments).Count -gt 0) "$Label argument vector is missing."
    Assert-True (-not [string]::IsNullOrWhiteSpace([string]$Record.reason)) `
        "$Label refusal reason is missing."
    Assert-True ($Record.required_action -ceq
        'verify_exact_tuple_and_stop_process_before_removing_candidate') `
        "$Label operator action is not explicit."
}

function Stop-ExactRetainedProcess([object]$Record, [string]$ExpectedPath, [string]$Label) {
    Assert-RetainedProcess -Record $Record -ExpectedRole ([string]$Record.role) `
        -ExpectedPath $ExpectedPath -Label $Label
    $candidate = Get-Process -Id ([int]$Record.pid) -ErrorAction Stop
    try {
        Assert-True ([string]::Equals([IO.Path]::GetFullPath($candidate.Path),
            [IO.Path]::GetFullPath([string]$Record.executable_path),
            [StringComparison]::OrdinalIgnoreCase)) "$Label executable identity changed."
        Assert-True ($candidate.StartTime.ToUniversalTime().Ticks -eq
            [int64]$Record.start_utc_ticks) "$Label start identity changed."
        $candidate.Kill($true)
        Assert-True ($candidate.WaitForExit(10000)) "$Label did not exit after exact test cleanup."
    }
    finally {
        $candidate.Dispose()
    }
}

function Assert-CandidateMutationRejected([string]$AcceptedDirectory,
        [scriptblock]$Mutation, [string]$ExpectedMessage, [string]$Label) {
    $acceptedTranscript = Get-Content -LiteralPath `
        (Join-Path $AcceptedDirectory 'transcript.json') -Raw | ConvertFrom-Json
    $candidate = Join-Path (Split-Path -Parent $AcceptedDirectory) `
        ".flycast-research-candidate-$($acceptedTranscript.capture_id)"
    Assert-True (-not (Test-Path -LiteralPath $candidate)) "$Label candidate already exists."
    try {
        Copy-Item -LiteralPath $AcceptedDirectory -Destination $candidate -Recurse
        Remove-Item -LiteralPath (Join-Path $candidate 'capture-validation.json') -Force
        $transcriptPath = Join-Path $candidate 'transcript.json'
        $transcript = Get-Content -LiteralPath $transcriptPath -Raw | ConvertFrom-Json
        & $Mutation $transcript $candidate
        Write-Json -Path $transcriptPath -Value $transcript
        $output = & $CaptureValidator --package $candidate `
            --receipt (Join-Path $candidate 'capture-validation.json') 2>&1 | Out-String
        $status = $LASTEXITCODE
        Assert-True ($status -eq 1) "$Label mutation was accepted."
        Assert-True ($output -like "*$ExpectedMessage*") `
            "$Label was rejected for an unexpected reason: $output"
    }
    finally {
        if (Test-Path -LiteralPath $candidate -PathType Container) {
            Remove-Item -LiteralPath $candidate -Force -Recurse
        }
    }
}

function Update-PackagedIdentityBindings([object]$Transcript, [string]$Candidate,
        [scriptblock]$Mutation) {
    $identityPath = Join-Path $Candidate 'identity.json'
    $identity = Get-Content -LiteralPath $identityPath -Raw | ConvertFrom-Json
    & $Mutation $identity
    Write-Json -Path $identityPath -Value $identity
    $identityItem = Get-Item -LiteralPath $identityPath
    $identityHash = Get-Hash $identityPath

    $jobPath = Join-Path $Candidate 'job.json'
    $job = Get-Content -LiteralPath $jobPath -Raw | ConvertFrom-Json
    $job.identity_manifest.size = [int64]$identityItem.Length
    $job.identity_manifest.sha256 = $identityHash
    Write-Json -Path $jobPath -Value $job
    $jobItem = Get-Item -LiteralPath $jobPath

    $Transcript.identity_manifest.size = [int64]$identityItem.Length
    $Transcript.identity_manifest.sha256 = $identityHash
    $Transcript.job.size = [int64]$jobItem.Length
    $Transcript.job.sha256 = Get-Hash $jobPath
}

function Stop-ExactSentinel {
    if ($null -eq $script:sentinel) { return }
    try {
        $script:sentinel.Refresh()
        if ($script:sentinel.HasExited) { return }
        $candidate = Get-Process -Id $script:sentinelIdentity.pid -ErrorAction Stop
        try {
            $same = $candidate.Id -eq $script:sentinelIdentity.pid -and
                [string]::Equals($candidate.Path, $script:sentinelIdentity.path,
                    [StringComparison]::OrdinalIgnoreCase) -and
                $candidate.StartTime.ToUniversalTime().Ticks -eq $script:sentinelIdentity.start_ticks
            if (-not $same) { throw 'Sentinel identity changed; refusing cleanup.' }
            $candidate.Kill($true)
            $candidate.WaitForExit(10000) | Out-Null
        }
        finally {
            $candidate.Dispose()
        }
    }
    finally {
        $script:sentinel.Dispose()
        $script:sentinel = $null
    }
}

try {
    Assert-V1ContractSchemas
    [IO.Directory]::CreateDirectory($temporaryRoot) | Out-Null
    $inputRoot = Join-Path $temporaryRoot 'inputs'
    $preflightRoot = Join-Path $temporaryRoot 'preflight-tests'
    $stagingRoot = Join-Path $temporaryRoot 'staging-failure-tests'
    $stagingFaultRoot = Join-Path $temporaryRoot 'staging-fault-tests'
    $successRoot = Join-Path $temporaryRoot "success-'quoted'-tests"
    $timeoutRoot = Join-Path $temporaryRoot 'emulator-timeout-tests'
    $nonzeroRoot = Join-Path $temporaryRoot 'nonzero-exit-tests'
    $mapleRejectionRoot = Join-Path $temporaryRoot 'maple-rejection-tests'
    $fastExitRoot = Join-Path $temporaryRoot 'fast-exit-tests'
    $toolTimeoutRoot = Join-Path $temporaryRoot 'tool-timeout-tests'
    $cleanupRefusalRoot = Join-Path $temporaryRoot 'cleanup-refusal-tests'
    $toolCleanupRefusalRoot = Join-Path $temporaryRoot 'tool-cleanup-refusal-tests'
    $captureValidatorCleanupRefusalRoot = Join-Path $temporaryRoot `
        'capture-validator-cleanup-refusal-tests'
    $toolOutputRoot = Join-Path $temporaryRoot 'tool-output-Ä-tests'
    $abortRoot = Join-Path $temporaryRoot 'forced-abort-tests'
    foreach ($directory in @($inputRoot, $preflightRoot, $stagingRoot, $stagingFaultRoot,
            $successRoot, $timeoutRoot, $nonzeroRoot, $mapleRejectionRoot, $fastExitRoot,
            $toolTimeoutRoot, $cleanupRefusalRoot, $toolCleanupRefusalRoot,
            $captureValidatorCleanupRefusalRoot, $toolOutputRoot, $abortRoot)) {
        [IO.Directory]::CreateDirectory($directory) | Out-Null
    }

    $media = Join-Path $inputRoot 'fixture.gdi'
    $track = Join-Path $inputRoot 'track01.bin'
    $ipBin = Join-Path $inputRoot 'ip.bin'
    $boot = Join-Path $inputRoot 'FIXTURE.BIN'
    $flash = Join-Path $inputRoot 'dc_nvmem.bin'
    [IO.File]::WriteAllText($media, "1`n1 0 4 2352 track01.bin 0`n", $utf8)
    [IO.File]::WriteAllBytes($track, [byte[]]::new(2352))
    [IO.File]::WriteAllBytes($ipBin, [byte[]]::new(32768))
    [IO.File]::WriteAllBytes($boot, [byte[]](1..64))
    [IO.File]::WriteAllBytes($flash, [byte[]]::new(131072))

    $identityPath = Join-Path $inputRoot 'identity.json'
    New-Identity -Path $identityPath -FixturePath $Fixture -Media $media -Track $track `
        -IpBin $ipBin -Boot $boot -Flash $flash -OmitBios

    # Raw relative output paths, config-injecting artifact names, and reparse
    # components are rejected before a private candidate can appear.
    $relativeJob = Join-Path $inputRoot 'relative-output-job.json'
    New-Job -Path $relativeJob -Identity $identityPath -Output (Join-Path $preflightRoot 'relative') `
        -Mode 'record'
    $relativeObject = Get-Content -LiteralPath $relativeJob -Raw | ConvertFrom-Json
    $relativeObject.output.accepted_directory = 'relative-output'
    Write-Json -Path $relativeJob -Value $relativeObject
    $relativeFailed = $false
    try { & $Driver -Job $relativeJob | Out-Null }
    catch { $relativeFailed = $_.Exception.Message -like '*must be absolute*' }
    Assert-True $relativeFailed 'A relative accepted output passed preflight.'

    $injectionJob = Join-Path $inputRoot 'artifact-injection-job.json'
    New-Job -Path $injectionJob -Identity $identityPath -Output (Join-Path $preflightRoot 'injection') `
        -Mode 'record' -ArtifactName "a',research:Injected='yes',tail.fcmr"
    $injectionFailed = $false
    try { & $Driver -Job $injectionJob | Out-Null }
    catch { $injectionFailed = $_.Exception.Message -like '*safe ASCII .fcmr*' }
    Assert-True $injectionFailed 'A config-injecting artifact name passed preflight.'

    $prefixInjectionJob = Join-Path $inputRoot 'prefix-injection-job.json'
    New-Job -Path $prefixInjectionJob -Identity $identityPath `
        -Output (Join-Path $preflightRoot 'prefix-injection') -Mode 'record'
    $prefixInjectionObject = Get-Content -LiteralPath $prefixInjectionJob -Raw | ConvertFrom-Json
    $prefixInjectionObject.emulator.arguments_prefix = @(
        'record', '-config', "config:Safe='ok'r e s e a r c h:MapleReplay='injected.fcmr'"
    )
    Write-Json -Path $prefixInjectionJob -Value $prefixInjectionObject
    $prefixInjectionFailed = $false
    try { & $Driver -Job $prefixInjectionJob | Out-Null }
    catch {
        $prefixInjectionFailed = $_.Exception.Message -like `
            '*arguments_prefix must not supply research settings*'
    }
    Assert-True $prefixInjectionFailed `
        'A parser-equivalent research setting in emulator.arguments_prefix passed preflight.'

    $danglingConfigJob = Join-Path $inputRoot 'dangling-config-job.json'
    New-Job -Path $danglingConfigJob -Identity $identityPath `
        -Output (Join-Path $preflightRoot 'dangling-config') -Mode 'record'
    $danglingConfigObject = Get-Content -LiteralPath $danglingConfigJob -Raw | ConvertFrom-Json
    $danglingConfigObject.emulator.arguments_prefix = @('record', '-config')
    Write-Json -Path $danglingConfigJob -Value $danglingConfigObject
    $danglingConfigFailed = $false
    try { & $Driver -Job $danglingConfigJob | Out-Null }
    catch {
        $danglingConfigFailed = $_.Exception.Message -like `
            '*must not end with an incomplete -config/--config option*'
    }
    Assert-True $danglingConfigFailed `
        'A dangling config option in emulator.arguments_prefix passed preflight.'

    $junction = Join-Path $preflightRoot 'input-junction'
    New-Item -ItemType Junction -Path $junction -Target $inputRoot | Out-Null
    $reparseFailed = $false
    try { & $Driver -Job (Join-Path $junction 'relative-output-job.json') | Out-Null }
    catch { $reparseFailed = $_.Exception.Message -like '*contains a reparse point*' }
    Assert-True $reparseFailed 'A capture job reached through a junction passed preflight.'
    Remove-Item -LiteralPath $junction -Force
    Assert-True (@(Get-ChildItem -LiteralPath $preflightRoot -Directory -Force |
        Where-Object { $_.Name -like '.flycast-research-candidate-*' }).Count -eq 0) `
        'Preflight rejection created a private candidate.'

    # Once a candidate exists, even a pre-launch staging error must quarantine it.
    $wrongFlash = Join-Path $inputRoot 'wrong-flash-name.bin'
    [IO.File]::WriteAllBytes($wrongFlash, [byte[]]::new(131072))
    $stagingIdentity = Join-Path $inputRoot 'staging-failure-identity.json'
    New-Identity -Path $stagingIdentity -FixturePath $Fixture -Media $media -Track $track `
        -IpBin $ipBin -Boot $boot -Flash $wrongFlash
    $stagingAccepted = Join-Path $stagingRoot 'must-not-exist'
    $stagingJob = Join-Path $inputRoot 'staging-failure-job.json'
    New-Job -Path $stagingJob -Identity $stagingIdentity -Output $stagingAccepted -Mode 'record'
    $stagingFailed = $false
    try {
        & $Driver -Job $stagingJob | Out-Null
    }
    catch {
        $stagingFailed = $_.Exception.Message -like '*canonical file name dc_nvmem.bin*'
    }
    Assert-True $stagingFailed 'Pre-launch staging failure was not reported.'
    Assert-True (-not (Test-Path -LiteralPath $stagingAccepted)) `
        'Pre-launch staging failure leaked an accepted output.'
    Assert-True (@(Get-ChildItem -LiteralPath $stagingRoot -Directory -Force |
        Where-Object { $_.Name -like '.flycast-research-candidate-*' }).Count -eq 0) `
        'Pre-launch staging failure left a private candidate directory.'
    $stagingQuarantineRoot = Join-Path $stagingRoot '.flycast-research-quarantine'
    $stagingQuarantines = @(Get-ChildItem -LiteralPath $stagingQuarantineRoot -Directory -Force)
    Assert-True ($stagingQuarantines.Count -eq 1) `
        'Pre-launch staging failure did not create exactly one quarantine package.'
    $stagingTranscript = Get-Content -LiteralPath `
        (Join-Path $stagingQuarantines[0].FullName 'transcript.json') -Raw | ConvertFrom-Json
    Assert-True (-not [bool]$stagingTranscript.accepted) `
        'Pre-launch quarantine transcript claims acceptance.'
    Assert-True ($null -eq $stagingTranscript.process) `
        'Pre-launch quarantine transcript unexpectedly identifies a launched process.'
    Assert-TranscriptSchema -Path (Join-Path $stagingQuarantines[0].FullName 'transcript.json')

    # A failure immediately after candidate creation has no packaged job or
    # identity yet, but its diagnostic transcript must still be schema-valid.
    $stagingFaultAccepted = Join-Path $stagingFaultRoot 'must-not-exist'
    $stagingFaultJob = Join-Path $inputRoot 'staging-fault-job.json'
    New-Job -Path $stagingFaultJob -Identity $identityPath -Output $stagingFaultAccepted -Mode 'record'
    $stagingFaultFailed = $false
    try {
        & $Driver -Job $stagingFaultJob `
            -IntegrationTestFaultToken 'FLYCAST_RESEARCH_CAPTURE_FORCE_STAGING_FAILURE_V1' | Out-Null
    }
    catch {
        $stagingFaultFailed = $_.Exception.Message -like `
            '*FLYCAST_RESEARCH_CAPTURE_FORCED_STAGING_FAILURE_TEST*'
    }
    Assert-True $stagingFaultFailed 'Injected earliest staging failure was not reported.'
    Assert-True (-not (Test-Path -LiteralPath $stagingFaultAccepted)) `
        'Earliest staging failure leaked an accepted output.'
    $stagingFaultQuarantine = Get-OnlyQuarantine -Root $stagingFaultRoot -Label 'Earliest staging failure'
    $stagingFaultTranscriptPath = Join-Path $stagingFaultQuarantine 'transcript.json'
    $stagingFaultTranscript = Get-Content -LiteralPath $stagingFaultTranscriptPath -Raw | ConvertFrom-Json
    Assert-True ($null -eq $stagingFaultTranscript.job) `
        'Earliest staging transcript unexpectedly identifies a packaged job.'
    Assert-True ($null -eq $stagingFaultTranscript.identity_manifest) `
        'Earliest staging transcript unexpectedly identifies a packaged manifest.'
    Assert-TranscriptSchema -Path $stagingFaultTranscriptPath

    # Successful publication: one private package becomes one accepted directory.
    $acceptedDirectory = Join-Path $successRoot 'accepted'
    $successJob = Join-Path $inputRoot 'success-job.json'
    New-Job -Path $successJob -Identity $identityPath -Output $acceptedDirectory -Mode 'record'
    $successResult = & $Driver -Job $successJob
    Assert-True ($successResult.Accepted -eq $true) 'Capture driver did not report acceptance.'
    Assert-True (Test-Path -LiteralPath $acceptedDirectory -PathType Container) `
        'Accepted directory was not published.'
    foreach ($name in @('job.json', 'identity.json', 'maple.fcmr', 'transcript.json',
            'capture-validation.json')) {
        Assert-True (Test-Path -LiteralPath (Join-Path $acceptedDirectory $name) -PathType Leaf) `
            "Accepted package is missing $name."
    }
    Assert-True (@(Get-ChildItem -LiteralPath $successRoot -Directory -Force |
        Where-Object { $_.Name -like '.flycast-research-candidate-*' }).Count -eq 0) `
        'Successful capture left a private candidate directory.'
    & $CaptureValidator --package $acceptedDirectory | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'Independent read-only package validation failed.'
    $successTranscriptPath = Join-Path $acceptedDirectory 'transcript.json'
    $successTranscript = Get-Content -LiteralPath $successTranscriptPath -Raw | ConvertFrom-Json
    Assert-TranscriptSchema -Path $successTranscriptPath
    Assert-True ($successTranscript.process.stdio_policy -ceq 'inherited_not_evidence') `
        'Accepted transcript does not declare the bounded-memory stdio policy.'
    Assert-True (@($successTranscript.process.arguments).Count -ge 4) `
        'Accepted transcript did not retain the exact emulator argument vector.'
    Assert-True ([bool]$successTranscript.publication.same_volume) `
        'Sibling candidate publication did not record same-volume construction.'
    Assert-True ([int64]$successTranscript.validator.stdout_size -le 1048576 -and
        [int64]$successTranscript.validator.stderr_size -le 1048576) `
        'Validator output exceeded its transcript memory bound.'
    Assert-True (@($successTranscript.retained_processes).Count -eq 0) `
        'Accepted transcript unexpectedly retained a process.'

    # Non-canonical absolute output text is normalized once, and a validator
    # flooding both pipes proves concurrent draining plus exact raw-byte bounds.
    $toolOutputAccepted = Join-Path $toolOutputRoot 'accepted'
    $toolOutputRaw = $toolOutputRoot.Replace('Ä', 'ä') + '\discarded\..\accepted\'
    $toolOutputJob = Join-Path $inputRoot 'tool-output-job.json'
    New-Job -Path $toolOutputJob -Identity $identityPath -Output $toolOutputAccepted `
        -Mode 'record' -ValidatorPath $Fixture -ValidatorPrefix @('tool-output') `
        -ValidatorTimeoutSeconds 5
    $toolOutputObject = Get-Content -LiteralPath $toolOutputJob -Raw | ConvertFrom-Json
    $toolOutputObject.output.accepted_directory = $toolOutputRaw
    Write-Json -Path $toolOutputJob -Value $toolOutputObject
    $toolOutputResult = & $Driver -Job $toolOutputJob
    Assert-True ([bool]$toolOutputResult.Accepted) 'Bounded tool-output capture was not accepted.'
    Assert-True (Test-Path -LiteralPath $toolOutputAccepted -PathType Container) `
        'Canonical accepted directory was not published.'
    & $CaptureValidator --package $toolOutputAccepted | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) `
        'Canonical tool-output package failed independent validation.'
    $toolOutputTranscriptPath = Join-Path $toolOutputAccepted 'transcript.json'
    $toolOutputTranscript = Get-Content -LiteralPath $toolOutputTranscriptPath -Raw | ConvertFrom-Json
    Assert-TranscriptSchema -Path $toolOutputTranscriptPath
    $expectedPrefix = [byte[]]::new(1048576)
    [Array]::Fill[byte]($expectedPrefix, [byte][char]'x')
    $expectedPrefix[8191] = 0xf0
    $expectedPrefix[8192] = 0x9f
    $expectedPrefix[8193] = 0x98
    $expectedPrefix[8194] = 0x80
    $expectedPrefixHash = Get-ByteHash $expectedPrefix
    foreach ($stream in @('stdout', 'stderr')) {
        Assert-True ([int64]$toolOutputTranscript.validator."${stream}_size" -eq 1048576) `
            "$stream capture prefix size is not exact."
        Assert-True ([int64]$toolOutputTranscript.validator."${stream}_total_size" -eq 1056769) `
            "$stream total byte count is not exact."
        Assert-True ([bool]$toolOutputTranscript.validator."${stream}_truncated") `
            "$stream truncation was not recorded."
        Assert-True ($toolOutputTranscript.validator."${stream}_sha256" -ceq $expectedPrefixHash) `
            "$stream captured-byte hash is not exact."
    }
    Assert-True ($toolOutputTranscript.publication.accepted_directory -ceq
        [IO.Path]::GetFullPath($toolOutputAccepted)) `
        'Transcript retained a non-canonical accepted path.'

    # Read-only validation requires the exact published location and the receipt.
    $relocatedDirectory = Join-Path $successRoot 'relocated-copy'
    Copy-Item -LiteralPath $acceptedDirectory -Destination $relocatedDirectory -Recurse
    & $CaptureValidator --package $relocatedDirectory 2>$null | Out-Null
    Assert-True ($LASTEXITCODE -eq 1) 'Relocated package was accepted outside its declared path.'
    $receiptPath = Join-Path $acceptedDirectory 'capture-validation.json'
    $heldReceiptPath = Join-Path $successRoot 'capture-validation.held.json'
    [IO.File]::Move($receiptPath, $heldReceiptPath)
    try {
        & $CaptureValidator --package $acceptedDirectory 2>$null | Out-Null
        Assert-True ($LASTEXITCODE -eq 1) 'Package without its validation receipt was accepted.'
    }
    finally {
        [IO.File]::Move($heldReceiptPath, $receiptPath)
    }
    $receiptBytes = [IO.File]::ReadAllBytes($receiptPath)
    try {
        $mutatedReceipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json
        $mutatedReceipt.validator.sha256 = '0000000000000000000000000000000000000000000000000000000000000000'
        Write-Json -Path $receiptPath -Value $mutatedReceipt
        & $CaptureValidator --package $acceptedDirectory 2>$null | Out-Null
        Assert-True ($LASTEXITCODE -eq 1) 'Receipt with a false validator identity was accepted.'
    }
    finally {
        [IO.File]::WriteAllBytes($receiptPath, $receiptBytes)
    }

    # A byte mutation after publication must be rejected independently.
    $tamperedDirectory = Join-Path $successRoot 'tampered-copy'
    Copy-Item -LiteralPath $acceptedDirectory -Destination $tamperedDirectory -Recurse
    $tamperedTrace = Join-Path $tamperedDirectory 'maple.fcmr'
    $tamperedBytes = [IO.File]::ReadAllBytes($tamperedTrace)
    $tamperedBytes[$tamperedBytes.Length - 1] = $tamperedBytes[$tamperedBytes.Length - 1] -bxor 1
    [IO.File]::WriteAllBytes($tamperedTrace, $tamperedBytes)
    & $CaptureValidator --package $tamperedDirectory 2>$null | Out-Null
    Assert-True ($LASTEXITCODE -eq 1) 'Mutated accepted artifact was not rejected.'

    # Re-issue mode validates the transcript before the existing accepted-path
    # check, allowing focused tests of self-consistent but false process claims.
    Assert-CandidateMutationRejected -AcceptedDirectory $acceptedDirectory `
        -Label 'Emulator prefix config-token completeness' `
        -ExpectedMessage `
            'job.emulator.arguments_prefix must not end with an incomplete -config/--config option' `
        -Mutation {
            param($Transcript, $Candidate)
            $jobPath = Join-Path $Candidate 'job.json'
            $job = Get-Content -LiteralPath $jobPath -Raw | ConvertFrom-Json
            $job.emulator.arguments_prefix = @('record', '-config')
            Write-Json -Path $jobPath -Value $job
            $jobItem = Get-Item -LiteralPath $jobPath
            $Transcript.job.size = [int64]$jobItem.Length
            $Transcript.job.sha256 = Get-Hash $jobPath
        }
    Assert-CandidateMutationRejected -AcceptedDirectory $acceptedDirectory `
        -Label 'Emulator prefix research-setting exclusion' `
        -ExpectedMessage 'job.emulator.arguments_prefix must not supply research settings' `
        -Mutation {
            param($Transcript, $Candidate)
            $jobPath = Join-Path $Candidate 'job.json'
            $job = Get-Content -LiteralPath $jobPath -Raw | ConvertFrom-Json
            $job.emulator.arguments_prefix = @(
                'record', '-config',
                "config:Safe='ok'r e s e a r c h:MapleReplay='injected.fcmr'"
            )
            Write-Json -Path $jobPath -Value $job
            $jobItem = Get-Item -LiteralPath $jobPath
            $Transcript.job.size = [int64]$jobItem.Length
            $Transcript.job.sha256 = Get-Hash $jobPath
        }
    Assert-CandidateMutationRejected -AcceptedDirectory $acceptedDirectory `
        -Label 'Whitespace-only transient exclusion' `
        -ExpectedMessage `
            'identity.configuration.values.flycast_transient contains an empty or whitespace-only setting' `
        -Mutation {
            param($Transcript, $Candidate)
            Update-PackagedIdentityBindings -Transcript $Transcript -Candidate $Candidate `
                -Mutation {
                    param($Identity)
                    $Identity.configuration.values | Add-Member -MemberType NoteProperty `
                        -Name flycast_transient -Value @(' ')
                    $sortedValues = [ordered]@{}
                    foreach ($property in @($Identity.configuration.values.PSObject.Properties |
                            Sort-Object Name)) {
                        $sortedValues[$property.Name] = $property.Value
                    }
                    $canonicalValues = $sortedValues | ConvertTo-Json -Compress -Depth 32
                    $Identity.configuration.sha256 = Get-TextHash $canonicalValues
                }
        }
    Assert-CandidateMutationRejected -AcceptedDirectory $acceptedDirectory `
        -Label 'Unicode-whitespace-only transient exclusion' `
        -ExpectedMessage `
            'identity.configuration.values.flycast_transient contains an empty or whitespace-only setting' `
        -Mutation {
            param($Transcript, $Candidate)
            Update-PackagedIdentityBindings -Transcript $Transcript -Candidate $Candidate `
                -Mutation {
                    param($Identity)
                    $noBreakSpace = ([char]0x00a0).ToString()
                    $Identity.configuration.values | Add-Member -MemberType NoteProperty `
                        -Name flycast_transient -Value @($noBreakSpace)
                    $sortedValues = [ordered]@{}
                    foreach ($property in @($Identity.configuration.values.PSObject.Properties |
                            Sort-Object Name)) {
                        $sortedValues[$property.Name] = $property.Value
                    }
                    $canonicalValues = $sortedValues | ConvertTo-Json -Compress -Depth 32
                    $Identity.configuration.sha256 = Get-TextHash $canonicalValues
                }
        }
    Assert-CandidateMutationRejected -AcceptedDirectory $acceptedDirectory `
        -Label 'Transient research-setting exclusion' `
        -ExpectedMessage `
            'identity.configuration.values.flycast_transient must not supply research settings' `
        -Mutation {
            param($Transcript, $Candidate)
            Update-PackagedIdentityBindings -Transcript $Transcript -Candidate $Candidate `
                -Mutation {
                    param($Identity)
                    $setting = "config:Safe='ok'r e s e a r c h:MapleReplay='injected.fcmr'"
                    $Identity.configuration.values | Add-Member -MemberType NoteProperty `
                        -Name flycast_transient -Value @($setting)
                    $sortedValues = [ordered]@{}
                    foreach ($property in @($Identity.configuration.values.PSObject.Properties |
                            Sort-Object Name)) {
                        $sortedValues[$property.Name] = $property.Value
                    }
                    $canonicalValues = $sortedValues | ConvertTo-Json -Compress -Depth 32
                    $Identity.configuration.sha256 = Get-TextHash $canonicalValues
                }
        }
    Assert-CandidateMutationRejected -AcceptedDirectory $acceptedDirectory `
        -Label 'Recorded argument vector binding' -ExpectedMessage 'process argument mismatch' `
        -Mutation {
            param($Transcript, $Candidate)
            $arguments = @($Transcript.process.arguments)
            $arguments[$arguments.Count - 1] = Join-Path $inputRoot 'wrong-media.gdi'
            $Transcript.process.arguments = $arguments
        }
    Assert-CandidateMutationRejected -AcceptedDirectory $acceptedDirectory `
        -Label 'Operating-system command-line binding' `
        -ExpectedMessage 'operating-system command-line argument count mismatch' `
        -Mutation {
            param($Transcript, $Candidate)
            $Transcript.process.command_line += ' extra-argument'
            $Transcript.process.command_line_sha256 = Get-TextHash $Transcript.process.command_line
        }
    Assert-CandidateMutationRejected -AcceptedDirectory $acceptedDirectory `
        -Label 'Maple validator absolute candidate binding' `
        -ExpectedMessage 'Maple validator arguments do not bind the candidate' `
        -Mutation {
            param($Transcript, $Candidate)
            $arguments = @($Transcript.validator.arguments)
            $traceIndex = [Array]::IndexOf($arguments, '--trace')
            Assert-True ($traceIndex -ge 0) 'Validator mutation could not locate --trace.'
            $arguments[$traceIndex + 1] = Join-Path $inputRoot 'maple.fcmr'
            $Transcript.validator.arguments = $arguments
        }
    Assert-CandidateMutationRejected -AcceptedDirectory $acceptedDirectory `
        -Label 'Capture-v1 non-GDI scope' `
        -ExpectedMessage "capture-v1 requires media.kind 'gdi'" `
        -Mutation {
            param($Transcript, $Candidate)
            Update-PackagedIdentityBindings -Transcript $Transcript -Candidate $Candidate `
                -Mutation { param($Identity) $Identity.media.kind = 'cue' }
        }
    Assert-CandidateMutationRejected -AcceptedDirectory $acceptedDirectory `
        -Label 'Capture-v1 empty track scope' `
        -ExpectedMessage 'capture-v1 requires at least one media track' `
        -Mutation {
            param($Transcript, $Candidate)
            Update-PackagedIdentityBindings -Transcript $Transcript -Candidate $Candidate `
                -Mutation { param($Identity) $Identity.media.tracks = @() }
        }
    Assert-CandidateMutationRejected -AcceptedDirectory $acceptedDirectory `
        -Label 'Accepted retained-process exclusion' `
        -ExpectedMessage 'accepted transcript retains a live process' `
        -Mutation {
            param($Transcript, $Candidate)
            $Transcript.retained_processes = @(
                [pscustomobject][ordered]@{
                    role = 'maple_validator'
                    label = 'Injected retained process'
                    pid = 1
                    executable_path = $Fixture
                    start_utc_ticks = 1
                    arguments = @('injected')
                    command_line_sha256 = $null
                    reason = 'mutation test'
                    required_action =
                        'verify_exact_tuple_and_stop_process_before_removing_candidate'
                }
            )
            $schemaAccepted = Test-Json -Json ($Transcript | ConvertTo-Json -Depth 64) `
                -SchemaFile $TranscriptSchema -ErrorAction SilentlyContinue
            Assert-True (-not $schemaAccepted) `
                'Transcript schema accepted retained_processes on accepted evidence.'
        }

    # Emulator terminal-state failures must quarantine and never publish.
    $timeoutAccepted = Join-Path $timeoutRoot 'must-not-exist'
    $timeoutJob = Join-Path $inputRoot 'emulator-timeout-job.json'
    New-Job -Path $timeoutJob -Identity $identityPath -Output $timeoutAccepted -Mode 'hold' `
        -TimeoutSeconds 1
    $timeoutFailed = $false
    try { & $Driver -Job $timeoutJob | Out-Null }
    catch { $timeoutFailed = $_.Exception.Message -like '*Emulator timed out*' }
    Assert-True $timeoutFailed 'Emulator timeout was not reported.'
    Assert-True (-not (Test-Path -LiteralPath $timeoutAccepted)) 'Emulator timeout published output.'
    $timeoutQuarantine = Get-OnlyQuarantine -Root $timeoutRoot -Label 'Emulator timeout'
    $timeoutTranscript = Get-Content -LiteralPath (Join-Path $timeoutQuarantine 'transcript.json') `
        -Raw | ConvertFrom-Json
    Assert-True ([bool]$timeoutTranscript.process.termination_requested -and
        [bool]$timeoutTranscript.process.termination_verified) `
        'Emulator timeout did not prove exact-owned cleanup.'
    Assert-True ($null -eq $timeoutTranscript.cleanup_failure) `
        'Emulator timeout recorded an unexpected cleanup failure.'
    Assert-True (@($timeoutTranscript.retained_processes).Count -eq 0) `
        'Verified emulator cleanup retained a process claim.'

    $nonzeroAccepted = Join-Path $nonzeroRoot 'must-not-exist'
    $nonzeroJob = Join-Path $inputRoot 'nonzero-job.json'
    New-Job -Path $nonzeroJob -Identity $identityPath -Output $nonzeroAccepted -Mode 'nonzero'
    $nonzeroFailed = $false
    try { & $Driver -Job $nonzeroJob | Out-Null }
    catch { $nonzeroFailed = $_.Exception.Message -like '*status zero: 7*' }
    Assert-True $nonzeroFailed 'Natural nonzero emulator exit was not reported.'
    $nonzeroQuarantine = Get-OnlyQuarantine -Root $nonzeroRoot -Label 'Natural nonzero exit'
    $nonzeroTranscript = Get-Content -LiteralPath (Join-Path $nonzeroQuarantine 'transcript.json') `
        -Raw | ConvertFrom-Json
    Assert-True ([bool]$nonzeroTranscript.process.natural_exit -and
        [int]$nonzeroTranscript.process.exit_code -eq 7) `
        'Natural nonzero exit was not recorded exactly.'

    $mapleRejectedAccepted = Join-Path $mapleRejectionRoot 'must-not-exist'
    $mapleRejectedJob = Join-Path $inputRoot 'maple-rejected-job.json'
    New-Job -Path $mapleRejectedJob -Identity $identityPath -Output $mapleRejectedAccepted `
        -Mode 'invalid'
    $mapleRejected = $false
    try { & $Driver -Job $mapleRejectedJob | Out-Null }
    catch { $mapleRejected = $_.Exception.Message -like '*Maple validator rejected the candidate*' }
    Assert-True $mapleRejected 'Independent Maple rejection was not reported.'
    $mapleRejectedQuarantine = Get-OnlyQuarantine -Root $mapleRejectionRoot `
        -Label 'Independent Maple rejection'
    $mapleRejectedTranscript = Get-Content -LiteralPath `
        (Join-Path $mapleRejectedQuarantine 'transcript.json') -Raw | ConvertFrom-Json
    Assert-True ([int]$mapleRejectedTranscript.validator.exit_code -ne 0) `
        'Independent Maple rejection did not retain the validator result.'

    $fastAccepted = Join-Path $fastExitRoot 'must-not-exist'
    $fastJob = Join-Path $inputRoot 'fast-exit-job.json'
    New-Job -Path $fastJob -Identity $identityPath -Output $fastAccepted -Mode 'instant'
    $fastRejected = $false
    try { & $Driver -Job $fastJob | Out-Null }
    catch { $fastRejected = $true }
    Assert-True $fastRejected 'An immediate child exit was accepted.'
    Assert-True (-not (Test-Path -LiteralPath $fastAccepted)) `
        'An immediate child exit published output.'
    Get-OnlyQuarantine -Root $fastExitRoot -Label 'Immediate child exit' | Out-Null

    # A timed-out independent tool is killed only after exact path/start
    # revalidation. A forced refusal leaves the candidate and orphan explicit.
    $toolTimeoutMarker = Join-Path $inputRoot 'tool-timeout.pid'
    $toolTimeoutAccepted = Join-Path $toolTimeoutRoot 'must-not-exist'
    $toolTimeoutJob = Join-Path $inputRoot 'tool-timeout-job.json'
    New-Job -Path $toolTimeoutJob -Identity $identityPath -Output $toolTimeoutAccepted `
        -Mode 'record' -ValidatorPath $Fixture -ValidatorPrefix @('tool-hold', $toolTimeoutMarker) `
        -ValidatorTimeoutSeconds 1
    $toolTimeoutFailed = $false
    try { & $Driver -Job $toolTimeoutJob | Out-Null }
    catch { $toolTimeoutFailed = $_.Exception.Message -like '*Maple validator timed out*' }
    Assert-True $toolTimeoutFailed 'Independent tool timeout was not reported.'
    Assert-ToolProcessExited -Marker $toolTimeoutMarker -Label 'Timed-out Maple validator'
    $toolTimeoutQuarantine = Get-OnlyQuarantine -Root $toolTimeoutRoot `
        -Label 'Timed-out Maple validator'
    $toolTimeoutTranscript = Get-Content -LiteralPath `
        (Join-Path $toolTimeoutQuarantine 'transcript.json') -Raw | ConvertFrom-Json
    Assert-True ($null -eq $toolTimeoutTranscript.cleanup_failure) `
        'Exact-owned tool timeout recorded an unexpected cleanup failure.'
    Assert-True (@($toolTimeoutTranscript.retained_processes).Count -eq 0) `
        'Verified tool cleanup retained a process claim.'

    $cleanupAccepted = Join-Path $cleanupRefusalRoot 'must-not-exist'
    $cleanupJob = Join-Path $inputRoot 'cleanup-refusal-job.json'
    New-Job -Path $cleanupJob -Identity $identityPath -Output $cleanupAccepted -Mode 'hold'
    $cleanupRefused = $false
    try {
        & $Driver -Job $cleanupJob `
            -IntegrationTestFaultToken 'FLYCAST_RESEARCH_CAPTURE_FORCE_CLEANUP_REFUSAL_V1' | Out-Null
    }
    catch { $cleanupRefused = $_.Exception.Message -like '*FORCED_CLEANUP_REFUSAL_TEST*' }
    Assert-True $cleanupRefused 'Forced emulator cleanup refusal was not reported.'
    Assert-True (-not (Test-Path -LiteralPath $cleanupAccepted)) `
        'Forced emulator cleanup refusal published output.'
    $retainedCandidates = @(Get-ChildItem -LiteralPath $cleanupRefusalRoot -Directory -Force |
        Where-Object { $_.Name -like '.flycast-research-candidate-*' })
    Assert-True ($retainedCandidates.Count -eq 1) `
        'Forced emulator cleanup refusal did not retain exactly one candidate.'
    $cleanupTranscriptPath = Join-Path $retainedCandidates[0].FullName 'transcript.json'
    $cleanupTranscript = Get-Content -LiteralPath $cleanupTranscriptPath -Raw | ConvertFrom-Json
    Assert-True ($cleanupTranscript.publication.state -ceq 'candidate_retained' -and
        -not [string]::IsNullOrWhiteSpace([string]$cleanupTranscript.cleanup_failure)) `
        'Forced emulator cleanup refusal is not explicit in the retained transcript.'
    $retainedEmulators = @($cleanupTranscript.retained_processes)
    Assert-True ($retainedEmulators.Count -eq 1) `
        'Forced emulator cleanup refusal lacks one structured retained-process tuple.'
    Assert-RetainedProcess -Record $retainedEmulators[0] -ExpectedRole 'emulator' `
        -ExpectedPath ([string]$cleanupTranscript.process.staged_executable.path) `
        -Label 'Retained emulator child'
    Assert-TranscriptSchema -Path $cleanupTranscriptPath
    Stop-ExactRetainedProcess -Record $retainedEmulators[0] `
        -ExpectedPath ([string]$cleanupTranscript.process.staged_executable.path) `
        -Label 'Retained emulator child'

    $toolCleanupMarker = Join-Path $inputRoot 'tool-cleanup-refusal.pid'
    $toolCleanupAccepted = Join-Path $toolCleanupRefusalRoot 'must-not-exist'
    $toolCleanupJob = Join-Path $inputRoot 'tool-cleanup-refusal-job.json'
    New-Job -Path $toolCleanupJob -Identity $identityPath -Output $toolCleanupAccepted `
        -Mode 'record' -ValidatorPath $Fixture `
        -ValidatorPrefix @('tool-hold', $toolCleanupMarker) -ValidatorTimeoutSeconds 1
    $toolCleanupRefused = $false
    try {
        & $Driver -Job $toolCleanupJob `
            -IntegrationTestFaultToken 'FLYCAST_RESEARCH_CAPTURE_FORCE_TOOL_CLEANUP_REFUSAL_V1' | Out-Null
    }
    catch { $toolCleanupRefused = $_.Exception.Message -like '*Maple validator timed out*' }
    Assert-True $toolCleanupRefused 'Forced tool cleanup refusal was not reported.'
    $toolRetainedCandidates = @(Get-ChildItem -LiteralPath $toolCleanupRefusalRoot -Directory -Force |
        Where-Object { $_.Name -like '.flycast-research-candidate-*' })
    Assert-True ($toolRetainedCandidates.Count -eq 1) `
        'Forced tool cleanup refusal did not retain exactly one candidate.'
    $toolCleanupTranscriptPath = Join-Path $toolRetainedCandidates[0].FullName 'transcript.json'
    $toolCleanupTranscript = Get-Content -LiteralPath $toolCleanupTranscriptPath -Raw | ConvertFrom-Json
    Assert-True ($toolCleanupTranscript.publication.state -ceq 'candidate_retained' -and
        [string]$toolCleanupTranscript.cleanup_failure -like '*cleanup refusal*') `
        'Forced tool cleanup refusal is not explicit in the retained transcript.'
    $retainedTools = @($toolCleanupTranscript.retained_processes)
    Assert-True ($retainedTools.Count -eq 1) `
        'Forced tool cleanup refusal lacks one structured retained-process tuple.'
    Assert-RetainedProcess -Record $retainedTools[0] -ExpectedRole 'maple_validator' `
        -ExpectedPath $Fixture -Label 'Retained independent tool'
    Assert-True (@($retainedTools[0].arguments)[0] -ceq 'tool-hold') `
        'Retained tool argument vector is not exact.'
    Assert-True ([int](Get-Content -LiteralPath $toolCleanupMarker -Raw) -eq
        [int]$retainedTools[0].pid) 'Tool PID marker and retained tuple disagree.'
    Assert-TranscriptSchema -Path $toolCleanupTranscriptPath
    Stop-ExactRetainedProcess -Record $retainedTools[0] -ExpectedPath $Fixture `
        -Label 'Retained independent tool'

    $captureValidatorCleanupAccepted = Join-Path $captureValidatorCleanupRefusalRoot `
        'must-not-exist'
    $captureValidatorCleanupJob = Join-Path $inputRoot `
        'capture-validator-cleanup-refusal-job.json'
    New-Job -Path $captureValidatorCleanupJob -Identity $identityPath `
        -Output $captureValidatorCleanupAccepted -Mode 'record' `
        -CaptureValidatorPath $Fixture -ValidatorTimeoutSeconds 1
    $captureValidatorCleanupRefused = $false
    try {
        & $Driver -Job $captureValidatorCleanupJob `
            -IntegrationTestFaultToken `
                'FLYCAST_RESEARCH_CAPTURE_FORCE_TOOL_CLEANUP_REFUSAL_V1' | Out-Null
    }
    catch {
        $captureValidatorCleanupRefused =
            $_.Exception.Message -like '*Capture transaction validator timed out*'
    }
    Assert-True $captureValidatorCleanupRefused `
        'Forced capture-validator cleanup refusal was not reported.'
    $captureValidatorCandidates = @(Get-ChildItem `
        -LiteralPath $captureValidatorCleanupRefusalRoot -Directory -Force |
        Where-Object { $_.Name -like '.flycast-research-candidate-*' })
    Assert-True ($captureValidatorCandidates.Count -eq 1) `
        'Capture-validator cleanup refusal did not retain exactly one candidate.'
    $captureValidatorTranscriptPath = Join-Path $captureValidatorCandidates[0].FullName `
        'transcript.json'
    $captureValidatorTranscript = Get-Content -LiteralPath $captureValidatorTranscriptPath `
        -Raw | ConvertFrom-Json
    $retainedCaptureValidators = @($captureValidatorTranscript.retained_processes)
    Assert-True ($retainedCaptureValidators.Count -eq 1) `
        'Capture-validator refusal lacks one structured retained-process tuple.'
    Assert-RetainedProcess -Record $retainedCaptureValidators[0] `
        -ExpectedRole 'capture_validator' -ExpectedPath $Fixture `
        -Label 'Retained capture validator'
    Assert-True (@($retainedCaptureValidators[0].arguments)[0] -ceq '--package') `
        'Retained capture-validator argument vector is not exact.'
    $captureValidatorMarker = Join-Path $captureValidatorCandidates[0].FullName `
        'capture-validator.pid'
    Assert-True (Test-Path -LiteralPath $captureValidatorMarker -PathType Leaf) `
        'Capture-validator fixture did not write its PID marker.'
    Assert-True ([int](Get-Content -LiteralPath $captureValidatorMarker -Raw) -eq
        [int]$retainedCaptureValidators[0].pid) `
        'Capture-validator PID marker and retained tuple disagree.'
    Assert-TranscriptSchema -Path $captureValidatorTranscriptPath
    Stop-ExactRetainedProcess -Record $retainedCaptureValidators[0] -ExpectedPath $Fixture `
        -Label 'Retained capture validator'

    # The unrelated sentinel shares the fixture image name with the owned child.
    # Exact path/start/command-line ownership must keep it alive during forced abort.
    $sentinelStart = [Diagnostics.ProcessStartInfo]::new()
    $sentinelStart.FileName = (Resolve-Path -LiteralPath $Fixture).Path
    $sentinelStart.UseShellExecute = $false
    $sentinelStart.CreateNoWindow = $true
    $sentinelStart.ArgumentList.Add('sentinel')
    $sentinelStart.ArgumentList.Add('60000')
    $sentinel = [Diagnostics.Process]::new()
    $sentinel.StartInfo = $sentinelStart
    Assert-True ($sentinel.Start()) 'Could not start unrelated sentinel.'
    $sentinelIdentity = [pscustomobject]@{
        pid = $sentinel.Id
        path = $sentinel.Path
        start_ticks = $sentinel.StartTime.ToUniversalTime().Ticks
    }
    $marker = Join-Path $abortRoot 'unrelated-marker.txt'
    [IO.File]::WriteAllText($marker, 'must remain exact', $utf8)
    $markerHash = Get-Hash $marker

    $abortAccepted = Join-Path $abortRoot 'must-not-exist'
    $abortJob = Join-Path $inputRoot 'abort-job.json'
    New-Job -Path $abortJob -Identity $identityPath -Output $abortAccepted -Mode 'hold'
    $abortFailed = $false
    try {
        & $Driver -Job $abortJob `
            -IntegrationTestAbortToken 'FLYCAST_RESEARCH_CAPTURE_FORCE_ABORT_AFTER_LAUNCH_V1' | Out-Null
    }
    catch {
        $abortFailed = $_.Exception.Message -like '*FLYCAST_RESEARCH_CAPTURE_FORCED_ABORT_TEST*'
    }
    Assert-True $abortFailed 'Forced-abort hook did not produce the guarded failure.'
    Assert-True (-not (Test-Path -LiteralPath $abortAccepted)) `
        'Forced abort leaked an accepted output.'
    $quarantineRoot = Join-Path $abortRoot '.flycast-research-quarantine'
    $quarantines = @(Get-ChildItem -LiteralPath $quarantineRoot -Directory -Force)
    Assert-True ($quarantines.Count -eq 1) 'Forced abort did not create exactly one quarantine package.'
    $rejectedTranscript = Get-Content -LiteralPath `
        (Join-Path $quarantines[0].FullName 'transcript.json') -Raw | ConvertFrom-Json
    Assert-True (-not [bool]$rejectedTranscript.accepted) 'Quarantine transcript claims acceptance.'
    Assert-True ([bool]$rejectedTranscript.process.termination_requested) `
        'Forced abort did not record an owned termination request.'
    Assert-True ([bool]$rejectedTranscript.process.termination_verified) `
        'Forced abort did not verify owned process termination.'

    $sentinel.Refresh()
    Assert-True (-not $sentinel.HasExited) 'Capture cleanup stopped the unrelated sentinel.'
    $sentinelCandidate = Get-Process -Id $sentinelIdentity.pid -ErrorAction Stop
    try {
        Assert-True ([string]::Equals($sentinelCandidate.Path, $sentinelIdentity.path,
            [StringComparison]::OrdinalIgnoreCase)) 'Sentinel executable identity changed.'
        Assert-True ($sentinelCandidate.StartTime.ToUniversalTime().Ticks -eq $sentinelIdentity.start_ticks) `
            'Sentinel process-start identity changed.'
    }
    finally {
        $sentinelCandidate.Dispose()
    }
    Assert-True ((Get-Hash $marker) -ceq $markerHash) 'Forced abort altered the unrelated marker.'

    'Flycast research capture transaction tests passed.'
}
finally {
    Stop-ExactSentinel
    if (Test-Path -LiteralPath $temporaryRoot -PathType Container) {
        $resolvedTemporary = (Resolve-Path -LiteralPath $temporaryRoot).Path
        $systemTemporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if (-not ($resolvedTemporary + '\').StartsWith($systemTemporary,
                [StringComparison]::OrdinalIgnoreCase) -or
            (Split-Path -Leaf $resolvedTemporary) -notlike 'flycast-research-capture-tests-*') {
            throw "Refusing to clean unexpected test directory: $resolvedTemporary"
        }
        # A killed fixture can remain transiently locked by host-side scanners
        # after its exact process identity has disappeared. Keep teardown
        # bounded, but allow that lock to drain before declaring a leak.
        for ($cleanupAttempt = 0; $cleanupAttempt -lt 150; $cleanupAttempt++) {
            try {
                Remove-Item -LiteralPath $resolvedTemporary -Force -Recurse -ErrorAction Stop
                break
            }
            catch {
                if ($cleanupAttempt -eq 149) { throw }
                Start-Sleep -Milliseconds 100
            }
        }
    }
}

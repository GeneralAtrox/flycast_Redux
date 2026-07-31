[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$CaptureDriver,
    [Parameter(Mandatory = $true)][string]$PackagePublisher,
    [Parameter(Mandatory = $true)][string]$Fixture,
    [Parameter(Mandatory = $true)][string]$MapleValidator,
    [Parameter(Mandatory = $true)][string]$CaptureValidator,
    [Parameter(Mandatory = $true)][string]$PackageValidator,
    [Parameter(Mandatory = $true)][string]$JobSchema,
    [Parameter(Mandatory = $true)][string]$ValidationSchema,
    [Parameter(Mandatory = $true)][string]$QuarantineSchema
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
$utf8 = [Text.UTF8Encoding]::new($false)

function Get-Hash([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-TextHash([string]$Text) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $utf8.GetBytes($Text)
        return ([BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    }
    finally { $algorithm.Dispose() }
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
        ($Value | ConvertTo-Json -Depth 64) + [Environment]::NewLine, $utf8)
}

function Write-Bytes([string]$Path, [byte[]]$Bytes) {
    [IO.File]::WriteAllBytes($Path, $Bytes)
}

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

function Assert-SchemaLocks {
    $expected = [ordered]@{
        $JobSchema = '8ba9a017b9aa074489089b602d08a9636ffe7d57f6ccf51444c1f712ae2419e5'
        $ValidationSchema = '3d722fbfae10949fc14b0fa59fcfc897f49cd06b04ae99f218c74b968ebd2bad'
        $QuarantineSchema = 'c751dba2f4e9d963f45e4f4f477d85820f7c4cdee7e55e37560dc16b1f4146db'
    }
    foreach ($entry in $expected.GetEnumerator()) {
        Assert-True ((Get-Hash $entry.Key) -ceq $entry.Value) `
            "Capture-package v2 schema contract changed: $($entry.Key)"
    }
}

function New-BaseJob([string]$Path, [string]$Identity, [string]$Output) {
    Write-Json $Path ([ordered]@{
        schema = 'flycast-research-capture-job'
        schema_version = 1
        driver = Get-Blob $CaptureDriver
        identity_manifest = Get-Blob $Identity
        emulator = [ordered]@{
            stage_executable = $true
            interactive = $false
            arguments_prefix = @('record')
            authenticated_inputs = @()
            runtime_files = @()
        }
        validator = [ordered]@{
            executable = Get-Blob $MapleValidator
            arguments_prefix = @()
            authenticated_inputs = @()
        }
        capture_validator = [ordered]@{ executable = Get-Blob $CaptureValidator }
        output = [ordered]@{
            accepted_directory = [IO.Path]::GetFullPath($Output)
            artifact_name = 'maple.fcmr'
        }
        limits = [ordered]@{
            trace_max_bytes = 1048576
            emulator_timeout_seconds = 20
            validator_timeout_seconds = 20
            stability_interval_ms = 100
        }
        metadata = [ordered]@{ purpose = 'capture-package-v2-integration-base' }
    })
}

function Get-BaseEntries([string]$Base) {
    $entries = [Collections.Generic.List[object]]::new()
    foreach ($name in @('capture-validation.json', 'identity.json', 'job.json',
            'maple.fcmr', 'transcript.json')) {
        $blob = Get-Blob (Join-Path $Base $name)
        $blob.path = $name
        $entries.Add($blob)
    }
    return @($entries)
}

function New-PackageJob([string]$Path, [string]$Id, [string]$Output,
        [string]$Base, [string]$Identity, [string]$StaticExport,
        [string]$Program, [string]$Exporter, [string]$MemoryManifest,
        [string]$MemoryArtifact, [string]$Sh4Manifest, [string]$Sh4Artifact,
        [bool]$IncludeSh4 = $true) {
    $artifacts = @([ordered]@{
        kind = 'memory-ranges-v1'
        artifact = Get-Blob $MemoryArtifact
        manifest = Get-Blob $MemoryManifest
        maximum_bytes = 1048576
    })
    if ($IncludeSh4) {
        $artifacts += [ordered]@{
            kind = 'sh4-events-v1'
            artifact = Get-Blob $Sh4Artifact
            manifest = Get-Blob $Sh4Manifest
            maximum_bytes = 1048576
        }
    }
    Write-Json $Path ([ordered]@{
        schema = 'flycast-research-capture-package-job'
        schema_version = 2
        package_id = $Id
        output = [ordered]@{ accepted_directory = [IO.Path]::GetFullPath($Output) }
        base_capture_v1 = [ordered]@{
            accepted_directory = [IO.Path]::GetFullPath($Base)
            entries = @(Get-BaseEntries $Base)
        }
        identity = Get-Blob $Identity
        static_analysis = [ordered]@{
            export = Get-Blob $StaticExport
            program = Get-Blob $Program
            exporter_script = Get-Blob $Exporter
        }
        artifacts = $artifacts
        publisher = [ordered]@{ script = Get-Blob $PackagePublisher }
        validator = [ordered]@{ executable = Get-Blob $PackageValidator }
        limits = [ordered]@{ validator_timeout_seconds = 30 }
        metadata = [ordered]@{ purpose = 'capture-package-v2-integration' }
    })
}

$root = Join-Path ([IO.Path]::GetTempPath()) `
    ('flycast-research-package-v2-integration-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null
try {
    Assert-SchemaLocks
    $sources = Join-Path $root 'sources'
    [IO.Directory]::CreateDirectory($sources) | Out-Null
    $media = Join-Path $sources 'disc.gdi'
    $track = Join-Path $sources 'track01.bin'
    $ipBin = Join-Path $sources 'ip.bin'
    $boot = Join-Path $sources '1ST_READ.BIN'
    $flash = Join-Path $sources 'dc_nvmem.bin'
    $exporter = Join-Path $sources 'fixture-exporter.java'
    Write-Bytes $media ([byte[]](0x31, 0x0d, 0x0a))
    Write-Bytes $track ([byte[]](0x10, 0x20, 0x30, 0x40))
    Write-Bytes $ipBin ([byte[]](0x49, 0x50, 0x2e, 0x42, 0x49, 0x4e))
    Write-Bytes $boot ([byte[]](0x46, 0x49, 0x58, 0x54, 0x55, 0x52, 0x45))
    Write-Bytes $flash ([byte[]](0x00, 0xff, 0x55, 0xaa))
    [IO.File]::WriteAllText($exporter, '// fixture exporter bytes' + [Environment]::NewLine, $utf8)

    $bootBlob = Get-Blob $boot
    $exporterBlob = Get-Blob $exporter
    $staticExport = Join-Path $sources 'static-analysis.json'
    $staticObject = [ordered]@{
        schema = 'flycast-research-ghidra-export'
        schema_version = 1
        export_id = 'STATIC-FIXTURE-V2'
        evidence_class = 'static-analysis'
        producer = [ordered]@{
            name = 'Ghidra'
            version = 'fixture-12.1.2'
            exporter_id = 'flycast-ghidra-export-v1'
            exporter_sha256 = $exporterBlob.sha256
        }
        program = [ordered]@{
            name = '1ST_READ.BIN'
            executable_sha256 = $bootBlob.sha256
            executable_size = $bootBlob.size
            language_id = 'SuperH4:LE:32:default'
            compiler_spec_id = 'default'
            endian = 'little'
            address_size = 32
            address_space = 'ram'
            ghidra_image_base = '0x8c010000'
            image_base = '0x8c010000'
            image_base_source = 'ghidra-program'
            minimum_address = '0x8c010000'
            maximum_address = '0x8c010fff'
        }
        memory_blocks = @([ordered]@{
            name = 'main'; start_address = '0x8c010000'; length = 4096
            read = $true; write = $true; execute = $true; initialized = $true
        })
        functions = @([ordered]@{
            entry_address = '0x8c010100'; name = 'fixture_function'; namespace = 'Global'
            calling_convention = 'default'; return_type = '/void'; parameter_types = @()
            body_ranges = @([ordered]@{ start_address = '0x8c010100'; length = 32 })
            thunk = $false; no_return = $false
        })
        symbols = @([ordered]@{
            address = '0x8c010100'; name = 'fixture_function'; namespace = 'Global'
            kind = 'function'; source = 'user_defined'; primary = $true
        })
        data_types = @([ordered]@{
            path = '/void'; kind = 'other'; length = 0; dynamic = $false; definition = 'void'
        })
        counts = [ordered]@{ memory_blocks = 1; functions = 1; symbols = 1; data_types = 1 }
    }
    Write-Json $staticExport $staticObject
    $staticHash = Get-Hash $staticExport
    $hookHash = Get-TextHash 'fixture hook manifest v2'

    $canonicalConfiguration = '{"autoload_state":false,"autosave_state":false,' +
        '"cpu_backend":"interpreter","ggpo":false,"threaded_rendering":false}'
    $identity = Join-Path $sources 'identity.json'
    Write-Json $identity ([ordered]@{
        schema = 'flycast-research-identity'
        schema_version = 1
        media = [ordered]@{
            kind = 'gdi'
            source = Get-Blob $media
            tracks = @([ordered]@{
                path = (Get-Blob $track).path; size = (Get-Blob $track).size
                sha256 = (Get-Blob $track).sha256; track = 1; start_fad = 150
                sector_size = 2352; offset = 0
            })
            ip_bin = Get-Blob $ipBin
            boot_executable = [ordered]@{
                path = $bootBlob.path; size = $bootBlob.size; sha256 = $bootBlob.sha256
                name = '1ST_READ.BIN'; load_address = '0x8c010000'
            }
        }
        firmware = [ordered]@{
            mode = 'hle'; bios = $null; hle_identity = 'capture-package-v2-fixture'
            flash_initial = Get-Blob $flash
        }
        persistent_devices = @()
        emulator = [ordered]@{
            git_commit = '0000000000000000000000000000000000000000'
            executable = Get-Blob $Fixture
        }
        configuration = [ordered]@{
            values = [ordered]@{
                autoload_state = $false; autosave_state = $false; cpu_backend = 'interpreter'
                ggpo = $false; threaded_rendering = $false
            }
            sha256 = Get-TextHash $canonicalConfiguration
        }
        static_analysis = [ordered]@{
            program_sha256 = $bootBlob.sha256
            image_base = '0x8c010000'
            export_sha256 = $staticHash
            hook_manifest_sha256 = $hookHash
        }
    })

    $memoryBytes = [byte[]](0x01, 0x23, 0x45, 0x67)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $memoryHash = ([BitConverter]::ToString(
            $algorithm.ComputeHash($memoryBytes))).Replace('-', '').ToLowerInvariant()
    }
    finally { $algorithm.Dispose() }
    $memoryManifest = Join-Path $sources 'memory-ranges-manifest.json'
    Write-Json $memoryManifest ([ordered]@{
        schema = 'flycast-research-memory-ranges-manifest'
        schema_version = 1
        manifest_id = 'FIXTURE-MEMORY-RANGES-V2'
        address_space = 'flycast-sh4-virtual'
        bindings = [ordered]@{
            executable_sha256 = $bootBlob.sha256
            static_analysis_id = 'STATIC-FIXTURE-V2'
            static_analysis_sha256 = $staticHash
            hook_manifest_id = 'HOOKS-FIXTURE-V2'
            hook_manifest_sha256 = $hookHash
        }
        trigger = [ordered]@{
            kind = 'guest-pc-enters-range'; start_address = '0x8c010000'
            end_address_exclusive = '0x8c020000'
        }
        ranges = @([ordered]@{
            range_id = 'RANGE-FIRST'; address = '0x8c001000'; length = 4
            expected_sha256 = $memoryHash
        })
        limits = [ordered]@{ maximum_total_bytes = 4 }
        acceptance = [ordered]@{
            snapshot_consistency = 'single-interpreter-boundary'
            expected_trigger_count = 1; expected_event_count = 1
            zero_dropped_events = $true; natural_exit = $true
        }
    })
    $memoryArtifact = Join-Path $sources 'memory-ranges.fcmr'
    & $Fixture memory-ranges-artifact $identity $memoryManifest $memoryArtifact
    Assert-True ($LASTEXITCODE -eq 0) 'The memory-ranges fixture failed.'

    $sh4Manifest = Join-Path $sources 'sh4-events-manifest.json'
    Write-Json $sh4Manifest ([ordered]@{
        schema = 'flycast-research-sh4-events-manifest'
        schema_version = 1
        manifest_id = 'FIXTURE-SH4-EVENTS-V2'
        address_space = 'flycast-sh4-virtual'
        bindings = [ordered]@{
            executable_sha256 = $bootBlob.sha256
            static_analysis_id = 'STATIC-FIXTURE-V2'
            static_analysis_sha256 = $staticHash
            hook_manifest_id = 'HOOKS-FIXTURE-V2'
            hook_manifest_sha256 = $hookHash
        }
        hooks = @()
        watch_ranges = @([ordered]@{
            watch_id = 'WATCH-FIXTURE'; address = '0x8c002000'; length = 16
            access = @('read')
        })
        limits = [ordered]@{
            maximum_events = 1; maximum_snapshot_bytes_per_event = 0
            maximum_total_snapshot_bytes = 0; maximum_open_invocations = 1
        }
        acceptance = [ordered]@{
            backend = 'interpreter'; minimum_call_events = 0; minimum_watch_events = 1
            require_balanced_calls = $true; zero_dropped_events = $true; natural_exit = $true
        }
    })
    $sh4Artifact = Join-Path $sources 'sh4-events.fcsh4'
    & $Fixture sh4-events-artifact $identity $sh4Manifest $sh4Artifact
    Assert-True ($LASTEXITCODE -eq 0) 'The SH-4 events fixture failed.'

    $baseAccepted = Join-Path $root 'base-maple-v1'
    $baseJob = Join-Path $sources 'base-job.json'
    New-BaseJob $baseJob $identity $baseAccepted
    & $CaptureDriver -Job $baseJob | Out-Null
    Assert-True (Test-Path -LiteralPath $baseAccepted -PathType Container) `
        'The base capture was not published.'
    & $CaptureValidator --package $baseAccepted | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'The base capture did not revalidate.'

    $duplicateIdentity = Join-Path $sources 'identity-duplicate-key.json'
    $identityText = [IO.File]::ReadAllText($identity, $utf8)
    $duplicateIdentityText = $identityText.Replace(
        '"schema_version": 1,', '"schema_version": 1,' + [Environment]::NewLine +
        '  "schema_version": 1,')
    Assert-True ($duplicateIdentityText -cne $identityText) `
        'Could not construct the duplicate-key identity fixture.'
    [IO.File]::WriteAllText($duplicateIdentity, $duplicateIdentityText, $utf8)
    $duplicateBaseAccepted = Join-Path $root 'base-maple-v1-duplicate-identity'
    $duplicateBaseJob = Join-Path $sources 'base-duplicate-identity-job.json'
    New-BaseJob $duplicateBaseJob $duplicateIdentity $duplicateBaseAccepted
    & $CaptureDriver -Job $duplicateBaseJob | Out-Null
    Assert-True (Test-Path -LiteralPath $duplicateBaseAccepted -PathType Container) `
        'The frozen v1 validator did not create the duplicate-identity proof fixture.'
    & $CaptureValidator --package $duplicateBaseAccepted | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) `
        'The duplicate-identity proof fixture is not a valid frozen v1 base.'
    $duplicateIdentityId = '10101010-2020-3030-4040-505050505050'
    $duplicateIdentityAccepted = Join-Path $root 'duplicate-identity-v2'
    $duplicateIdentityJob = Join-Path $sources 'duplicate-identity-v2-job.json'
    New-PackageJob $duplicateIdentityJob $duplicateIdentityId $duplicateIdentityAccepted `
        $duplicateBaseAccepted $duplicateIdentity $staticExport $boot $exporter `
        $memoryManifest $memoryArtifact $sh4Manifest $sh4Artifact
    $duplicateIdentityRejected = $false
    try { & $PackagePublisher -Job $duplicateIdentityJob | Out-Null }
    catch { $duplicateIdentityRejected = $true }
    Assert-True $duplicateIdentityRejected 'A packaged identity with duplicate keys was accepted.'
    $duplicateIdentityQuarantine = Join-Path `
        (Join-Path $root '.flycast-research-package-v2-quarantine') $duplicateIdentityId
    $duplicateIdentityRejection = Get-Content -Raw `
        (Join-Path $duplicateIdentityQuarantine 'rejection.json') | ConvertFrom-Json -Depth 8
    Assert-True ($duplicateIdentityRejection.failure -match 'duplicate key') `
        'The duplicate identity fixture did not reach the v2 duplicate-key gate.'

    $accepted = Join-Path $root 'accepted-v2'
    $job = Join-Path $sources 'package-job.json'
    New-PackageJob $job '11111111-2222-3333-4444-555555555555' $accepted `
        $baseAccepted $identity $staticExport $boot $exporter $memoryManifest $memoryArtifact `
        $sh4Manifest $sh4Artifact
    Assert-True ((Get-Content -Raw $job | Test-Json -SchemaFile $JobSchema)) `
        'The fixture v2 job does not match its schema.'
    $schemaLimitProbe = Get-Content -Raw $job | ConvertFrom-Json -Depth 64
    $schemaLimitProbe.artifacts[1].maximum_bytes = 268435457
    Assert-True (-not (($schemaLimitProbe | ConvertTo-Json -Depth 64) |
        Test-Json -SchemaFile $JobSchema -ErrorAction SilentlyContinue)) `
        'The job schema accepted an SH-4 maximum above its implementation bound.'
    & $PackagePublisher -Job $job | Out-Null
    Assert-True (Test-Path -LiteralPath $accepted -PathType Container) `
        'The v2 package was not atomically published.'
    & $PackageValidator --package $accepted | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'The published v2 package did not revalidate.'
    $receipt = Join-Path $accepted 'capture-package-validation.json'
    Assert-True ((Get-Content -Raw $receipt | Test-Json -SchemaFile $ValidationSchema)) `
        'The v2 validation receipt does not match its schema.'

    $receiptBytes = [IO.File]::ReadAllBytes($receipt)
    $tamperedReceipt = Get-Content -Raw $receipt | ConvertFrom-Json -Depth 32
    $tamperedReceipt.package_id = '00000000-0000-0000-0000-000000000000'
    Write-Json $receipt $tamperedReceipt
    & $PackageValidator --package $accepted 2>$null | Out-Null
    Assert-True ($LASTEXITCODE -eq 1) 'A package with a tampered receipt was accepted.'
    [IO.File]::WriteAllBytes($receipt, $receiptBytes)

    $extraEntry = Join-Path $accepted 'unexpected.txt'
    [IO.File]::WriteAllText($extraEntry, 'unexpected', $utf8)
    & $PackageValidator --package $accepted 2>$null | Out-Null
    Assert-True ($LASTEXITCODE -eq 1) 'A package with an extra entry was accepted.'
    Remove-Item -LiteralPath $extraEntry -Force

    $packagedJob = Join-Path $accepted 'job.json'
    $packagedJobBytes = [IO.File]::ReadAllBytes($packagedJob)
    $packagedJobText = $utf8.GetString($packagedJobBytes)
    $duplicateJobText = $packagedJobText.Replace(
        '"schema_version": 2,', '"schema_version": 2,' + [Environment]::NewLine +
        '  "schema_version": 2,')
    Assert-True ($duplicateJobText -cne $packagedJobText) `
        'Could not construct the duplicate-key package job fixture.'
    [IO.File]::WriteAllText($packagedJob, $duplicateJobText, $utf8)
    & $PackageValidator --package $accepted 2>$null | Out-Null
    Assert-True ($LASTEXITCODE -eq 1) 'A package job with duplicate keys was accepted.'
    [IO.File]::WriteAllBytes($packagedJob, $packagedJobBytes)

    $deepValue = '"leaf"'
    for ($depth = 0; $depth -lt 70; ++$depth) {
        $deepValue = '{"next":' + $deepValue + '}'
    }
    $deepJobText = [regex]::Replace($packagedJobText,
        '"metadata"\s*:\s*\{[^{}]*\}', '"metadata": ' + $deepValue)
    Assert-True ($deepJobText -cne $packagedJobText) `
        'Could not construct the deeply nested package job fixture.'
    [IO.File]::WriteAllText($packagedJob, $deepJobText, $utf8)
    $depthRejection = (& $PackageValidator --package $accepted 2>&1 | Out-String)
    Assert-True ($LASTEXITCODE -eq 1) 'A package job deeper than 64 levels was accepted.'
    Assert-True ($depthRejection -match 'JSON depth exceeds 64') `
        'The deeply nested package job did not reach the explicit depth gate.'
    [IO.File]::WriteAllBytes($packagedJob, $packagedJobBytes)

    $packagedArtifact = Join-Path $accepted 'memory-ranges.fcmr'
    $mutated = [IO.File]::ReadAllBytes($packagedArtifact)
    $mutated[$mutated.Length - 1] = $mutated[$mutated.Length - 1] -bxor 0x01
    [IO.File]::WriteAllBytes($packagedArtifact, $mutated)
    & $PackageValidator --package $accepted 2>$null | Out-Null
    Assert-True ($LASTEXITCODE -eq 1) 'A mutated packaged artifact was accepted.'
    [IO.File]::Copy($memoryArtifact, $packagedArtifact, $true)
    & $PackageValidator --package $accepted | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'The restored v2 package did not revalidate.'

    if ($IsWindows) {
        $packagedSh4Artifact = Join-Path $accepted 'sh4-events.fcsh4'
        Remove-Item -LiteralPath $packagedSh4Artifact -Force
        try {
            New-Item -ItemType Junction -Path $packagedSh4Artifact -Target $sources | Out-Null
            & $PackageValidator --package $accepted 2>$null | Out-Null
            Assert-True ($LASTEXITCODE -eq 1) 'A package with a linked entry was accepted.'
        }
        finally {
            if (Test-Path -LiteralPath $packagedSh4Artifact) {
                [IO.Directory]::Delete($packagedSh4Artifact)
            }
            [IO.File]::Copy($sh4Artifact, $packagedSh4Artifact, $false)
        }
        & $PackageValidator --package $accepted | Out-Null
        Assert-True ($LASTEXITCODE -eq 0) 'The package did not revalidate after link restoration.'
    }

    $singleAccepted = Join-Path $root 'accepted-v2-évidence'
    $singleJob = Join-Path $sources 'single-artifact-job.json'
    New-PackageJob $singleJob '22222222-3333-4444-5555-666666666666' $singleAccepted `
        $baseAccepted $identity $staticExport $boot $exporter $memoryManifest $memoryArtifact `
        $sh4Manifest $sh4Artifact $false
    & $PackagePublisher -Job $singleJob | Out-Null
    Assert-True (Test-Path -LiteralPath $singleAccepted -PathType Container) `
        'The single-artifact package was not published on a Unicode path.'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $singleAccepted 'sh4-events.fcsh4'))) `
        'The single-artifact package unexpectedly contains SH-4 evidence.'
    & $PackageValidator --package $singleAccepted | Out-Null
    Assert-True ($LASTEXITCODE -eq 0) 'The single-artifact package did not revalidate.'

    $reserializedIdentity = Join-Path $sources 'identity-reserialized.json'
    $identityObject = Get-Content -Raw $identity | ConvertFrom-Json -Depth 64
    [IO.File]::WriteAllText($reserializedIdentity,
        ($identityObject | ConvertTo-Json -Depth 64 -Compress) + [Environment]::NewLine, $utf8)
    Assert-True ((Get-Hash $reserializedIdentity) -cne (Get-Hash $identity)) `
        'The reserialized identity fixture is not byte-distinct.'
    $identityMismatchAccepted = Join-Path $root 'identity-byte-mismatch'
    $identityMismatchJob = Join-Path $sources 'identity-byte-mismatch-job.json'
    New-PackageJob $identityMismatchJob '33333333-4444-5555-6666-777777777777' `
        $identityMismatchAccepted $baseAccepted $reserializedIdentity $staticExport $boot `
        $exporter $memoryManifest $memoryArtifact $sh4Manifest $sh4Artifact
    $identityMismatchRejected = $false
    try { & $PackagePublisher -Job $identityMismatchJob | Out-Null }
    catch { $identityMismatchRejected = $true }
    Assert-True $identityMismatchRejected 'A byte-distinct v2 identity was accepted.'
    Assert-True (-not (Test-Path -LiteralPath $identityMismatchAccepted)) `
        'The byte-distinct identity package was published.'

    $wrongJoinManifest = Join-Path $sources 'memory-ranges-wrong-static-id.json'
    $wrongJoinObject = Get-Content -Raw $memoryManifest | ConvertFrom-Json -Depth 32
    $wrongJoinObject.bindings.static_analysis_id = 'OTHER-STATIC-EXPORT'
    Write-Json $wrongJoinManifest $wrongJoinObject
    $wrongJoinArtifact = Join-Path $sources 'memory-ranges-wrong-static-id.fcmr'
    & $Fixture memory-ranges-artifact $identity $wrongJoinManifest $wrongJoinArtifact
    Assert-True ($LASTEXITCODE -eq 0) 'The wrong-static-id fixture artifact failed.'
    $wrongJoinId = '99999999-8888-7777-6666-555555555555'
    $wrongJoinAccepted = Join-Path $root 'wrong-static-id'
    $wrongJoinJob = Join-Path $sources 'wrong-static-id-job.json'
    New-PackageJob $wrongJoinJob $wrongJoinId $wrongJoinAccepted $baseAccepted $identity `
        $staticExport $boot $exporter $wrongJoinManifest $wrongJoinArtifact `
        $sh4Manifest $sh4Artifact
    $wrongJoinRejected = $false
    try { & $PackagePublisher -Job $wrongJoinJob | Out-Null }
    catch { $wrongJoinRejected = $true }
    Assert-True $wrongJoinRejected 'A memory-range manifest joined to the wrong static id was accepted.'
    Assert-True (-not (Test-Path -LiteralPath $wrongJoinAccepted)) `
        'The wrong-static-id package was published.'
    $wrongJoinQuarantine = Join-Path `
        (Join-Path $root '.flycast-research-package-v2-quarantine') $wrongJoinId
    $wrongJoinRejection = Get-Content -Raw (Join-Path $wrongJoinQuarantine 'rejection.json') |
        ConvertFrom-Json -Depth 8
    Assert-True ($wrongJoinRejection.failure -match 'static_analysis_id') `
        'The quarantined validator rejection did not preserve its diagnostic.'

    foreach ($case in @(
        [pscustomobject]@{ id = 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeee1'; token = 'after-staging-v2'; leaf = 'abort-staging' },
        [pscustomobject]@{ id = 'aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeee2'; token = 'after-validation-v2'; leaf = 'abort-validation' }
    )) {
        $abortAccepted = Join-Path $root $case.leaf
        $abortJob = Join-Path $sources ($case.leaf + '.json')
        New-PackageJob $abortJob $case.id $abortAccepted $baseAccepted $identity `
            $staticExport $boot $exporter $memoryManifest $memoryArtifact `
            $sh4Manifest $sh4Artifact
        $rejected = $false
        try { & $PackagePublisher -Job $abortJob -IntegrationTestAbortToken $case.token | Out-Null }
        catch { $rejected = $true }
        Assert-True $rejected "Forced abort $($case.token) did not reject."
        Assert-True (-not (Test-Path -LiteralPath $abortAccepted)) `
            "Forced abort $($case.token) published an accepted directory."
        $quarantine = Join-Path (Join-Path $root '.flycast-research-package-v2-quarantine') $case.id
        Assert-True (Test-Path -LiteralPath $quarantine -PathType Container) `
            "Forced abort $($case.token) did not quarantine its candidate."
        $rejection = Join-Path $quarantine 'rejection.json'
        Assert-True ((Get-Content -Raw $rejection | Test-Json -SchemaFile $QuarantineSchema)) `
            "Forced abort $($case.token) rejection metadata does not match its schema."
    }

    $quarantineRoot = Join-Path $root '.flycast-research-package-v2-quarantine'
    $quarantineBackup = Join-Path $root '.flycast-research-package-v2-quarantine-backup'
    [IO.Directory]::Move($quarantineRoot, $quarantineBackup)
    [IO.File]::WriteAllText($quarantineRoot, 'not a directory', $utf8)
    try {
        $invalidQuarantineAccepted = Join-Path $root 'invalid-quarantine-root'
        $invalidQuarantineJob = Join-Path $sources 'invalid-quarantine-root-job.json'
        $invalidQuarantineId = 'bbbbbbbb-cccc-dddd-eeee-ffffffffffff'
        New-PackageJob $invalidQuarantineJob $invalidQuarantineId $invalidQuarantineAccepted `
            $baseAccepted $identity $staticExport $boot $exporter $memoryManifest `
            $memoryArtifact $sh4Manifest $sh4Artifact
        $invalidQuarantineMessage = ''
        try { & $PackagePublisher -Job $invalidQuarantineJob | Out-Null }
        catch { $invalidQuarantineMessage = $_.Exception.Message }
        Assert-True ($invalidQuarantineMessage -match 'quarantine root must be a non-linked directory') `
            'An invalid quarantine root was not rejected explicitly.'
        Assert-True (-not (Test-Path -LiteralPath $invalidQuarantineAccepted)) `
            'An invalid quarantine root allowed publication.'
        Assert-True (-not (Test-Path -LiteralPath `
            (Join-Path $root ".flycast-research-package-v2-candidate-$invalidQuarantineId"))) `
            'An invalid quarantine root left a private candidate.'
    }
    finally {
        Remove-Item -LiteralPath $quarantineRoot -Force
        [IO.Directory]::Move($quarantineBackup, $quarantineRoot)
    }

    Write-Host 'Flycast research capture-package v2 tests passed.'
}
finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}

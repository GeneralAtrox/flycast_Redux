[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Job,

    [Parameter(Mandatory = $false)]
    [string]$IntegrationTestAbortToken,

    [Parameter(Mandatory = $false)]
    [string]$IntegrationTestFaultToken
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0

$script:ForcedAbortToken = 'FLYCAST_RESEARCH_CAPTURE_FORCE_ABORT_AFTER_LAUNCH_V1'
$script:ForcedAbortMarker = '[FLYCAST_RESEARCH_CAPTURE_FORCED_ABORT_TEST]'
$script:ForcedStagingFailureToken = 'FLYCAST_RESEARCH_CAPTURE_FORCE_STAGING_FAILURE_V1'
$script:ForcedStagingFailureMarker = '[FLYCAST_RESEARCH_CAPTURE_FORCED_STAGING_FAILURE_TEST]'
$script:ForcedCleanupRefusalToken = 'FLYCAST_RESEARCH_CAPTURE_FORCE_CLEANUP_REFUSAL_V1'
$script:ForcedCleanupRefusalMarker = '[FLYCAST_RESEARCH_CAPTURE_FORCED_CLEANUP_REFUSAL_TEST]'
$script:ForcedToolCleanupRefusalToken = 'FLYCAST_RESEARCH_CAPTURE_FORCE_TOOL_CLEANUP_REFUSAL_V1'
$script:ForcedToolCleanupRefusalMarker = '[FLYCAST_RESEARCH_CAPTURE_FORCED_TOOL_CLEANUP_REFUSAL_TEST]'
$script:Utf8 = [Text.UTF8Encoding]::new($false)
$script:MaximumToolOutputBytes = 1048576
$script:Records = [Collections.Generic.List[object]]::new()
$script:RetainedProcesses = [Collections.Generic.List[object]]::new()
$script:StartedUtc = [DateTime]::UtcNow
$script:Watch = [Diagnostics.Stopwatch]::StartNew()
$script:Process = $null
$script:Ownership = $null
$script:EmulatorExecutablePath = $null
$script:EmulatorStartTicks = [int64]0
$script:TerminationRequested = $false
$script:TerminationVerified = $false
$script:NaturalExit = $false
$script:ProcessExitCode = $null
$script:CleanupFailure = $null
$script:ForceCleanupRefusal = $false
$script:ForceToolCleanupRefusal = $false
$script:EmulatorArguments = @()

if (-not $IsWindows) {
    throw 'flycast-capture-maple.ps1 is the Windows Phase 2 driver.'
}
if ($PSVersionTable.PSVersion.Major -lt 7) {
    throw 'flycast-capture-maple.ps1 requires PowerShell 7 or newer.'
}

function Assert-ObjectProperties {
    param(
        [Parameter(Mandatory = $true)][object]$Value,
        [Parameter(Mandatory = $true)][string[]]$Allowed,
        [Parameter(Mandatory = $true)][string[]]$Required,
        [Parameter(Mandatory = $true)][string]$Label
    )
    if ($null -eq $Value -or $Value -isnot [psobject]) {
        throw "$Label must be an object."
    }
    $names = @($Value.PSObject.Properties.Name)
    foreach ($name in $Required) {
        if ($names -cnotcontains $name) { throw "$Label is missing '$name'." }
    }
    foreach ($name in $names) {
        if ($Allowed -cnotcontains $name) { throw "$Label has unknown field '$name'." }
    }
}

function Assert-ArrayOfStrings {
    param([object]$Value, [string]$Label, [int]$Maximum = 64)
    $items = @($Value)
    if ($items.Count -gt $Maximum) { throw "$Label has too many entries." }
    foreach ($item in $items) {
        if ($item -isnot [string]) { throw "$Label must contain only strings." }
    }
    return $items
}

function Test-ContainsResearchConfigSection {
    param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)

    # Keep the parser state aligned with core/cfg/cl.cpp::parseConfigOption.
    # Section comparison is deliberately case-insensitive and therefore more
    # conservative than Flycast's case-sensitive configuration map.
    [int]$step = 0 # section, key, value
    [char]$inQuote = [char]0
    [string]$section = ''
    [bool]$keyEmpty = $true
    foreach ($character in $Value.ToCharArray()) {
        if ($inQuote -ne [char]0 -and $character -eq $inQuote) {
            $inQuote = [char]0
            $step = 0
            $section = ''
            $keyEmpty = $true
            continue
        }
        if ($character -eq ':') {
            if ($step -eq 0) {
                if ($section.Length -eq 0) { return $false }
                if ([string]::Equals($section, 'research',
                        [StringComparison]::OrdinalIgnoreCase)) {
                    return $true
                }
                $step = 1
                $keyEmpty = $true
            }
            elseif ($step -eq 1) {
                $keyEmpty = $false
            }
            continue
        }
        if ($character -eq '=') {
            if ($step -eq 0) { return $false }
            if ($step -eq 1) {
                if ($keyEmpty) { return $false }
                $step = 2
            }
            continue
        }
        if ($character -eq [char]39 -or $character -eq [char]34) {
            if ($step -eq 0) {
                $section += $character
            }
            elseif ($step -eq 1) {
                $keyEmpty = $false
            }
            elseif ($step -eq 2 -and $inQuote -eq [char]0) {
                $inQuote = $character
            }
            continue
        }
        if ($character -eq ',') {
            if ($step -eq 2 -and $inQuote -eq [char]0) {
                $step = 0
                $section = ''
                $keyEmpty = $true
            }
            elseif ($step -eq 1) {
                $keyEmpty = $false
            }
            continue
        }
        if ($character -eq ' ') {
            continue
        }
        if ($step -eq 0) {
            $section += $character
        }
        elseif ($step -eq 1) {
            $keyEmpty = $false
        }
    }
    return $false
}

function Test-HasProperty {
    param([Parameter(Mandatory = $true)][object]$Value, [Parameter(Mandatory = $true)][string]$Name)
    return @($Value.PSObject.Properties.Name) -ccontains $Name
}

function Test-SafeArtifactName {
    param([Parameter(Mandatory = $true)][string]$Name)
    if ($Name.Length -gt 128 -or $Name -cnotmatch '^[A-Za-z0-9][A-Za-z0-9._-]*\.fcmr$') {
        return
    }
    $stem = [IO.Path]::GetFileNameWithoutExtension($Name)
    return $stem -cnotmatch '^(?i:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\.|$)'
}

function Format-FlycastConfigValue {
    param([Parameter(Mandatory = $true)][string]$Value)
    if ($Value.IndexOf([char]0) -ge 0) {
        throw 'Flycast configuration values must not contain NUL.'
    }
    if ($Value.IndexOf("'") -lt 0) { return "'$Value'" }
    if ($Value.IndexOf('"') -lt 0) { return '"' + $Value + '"' }
    throw 'A Flycast configuration path contains both quote characters and cannot be encoded safely.'
}

function Get-CanonicalAbsolutePath {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$Label)
    if ([string]::IsNullOrWhiteSpace($Path) -or -not [IO.Path]::IsPathFullyQualified($Path)) {
        throw "$Label must be absolute."
    }
    $full = [IO.Path]::GetFullPath($Path)
    $root = [IO.Path]::GetPathRoot($full)
    if ($full.Length -gt $root.Length) { $full = $full.TrimEnd('\', '/') }
    $parent = [IO.Path]::GetDirectoryName($full)
    if (-not [string]::IsNullOrWhiteSpace($parent) -and
        (Test-Path -LiteralPath $parent -PathType Container)) {
        $canonicalParent = (Get-Item -LiteralPath $parent -Force).FullName
        $full = Join-Path $canonicalParent ([IO.Path]::GetFileName($full))
    }
    return $full
}

function Get-LowerSha256 {
    param([Parameter(Mandatory = $true)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-TextIdentity {
    param([AllowEmptyString()][string]$Text)
    $bytes = $script:Utf8.GetBytes($Text)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return [ordered]@{
            size = [int64]$bytes.LongLength
            sha256 = ([BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
        }
    }
    finally {
        $algorithm.Dispose()
    }
}

function Get-ByteIdentity {
    param([Parameter(Mandatory = $true)][AllowEmptyCollection()][byte[]]$Bytes)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return [ordered]@{
            size = [int64]$Bytes.LongLength
            sha256 = ([BitConverter]::ToString($algorithm.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant()
        }
    }
    finally {
        $algorithm.Dispose()
    }
}

function Add-CleanupFailure {
    param([Parameter(Mandatory = $true)][string]$Message)
    if ([string]::IsNullOrWhiteSpace($script:CleanupFailure)) {
        $script:CleanupFailure = $Message
    }
    else {
        $script:CleanupFailure += "; $Message"
    }
}

function Add-RetainedProcess {
    param(
        [Parameter(Mandatory = $true)][string]$Role,
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][int64]$ProcessId,
        [Parameter(Mandatory = $true)][string]$ExecutablePath,
        [Parameter(Mandatory = $true)][int64]$StartUtcTicks,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [AllowNull()][object]$CommandLineSha256,
        [Parameter(Mandatory = $true)][string]$Reason
    )
    if ($Role -cnotin @('emulator', 'maple_validator', 'capture_validator') -or
        $ProcessId -le 0 -or $StartUtcTicks -le 0 -or
        [string]::IsNullOrWhiteSpace($ExecutablePath) -or
        -not [IO.Path]::IsPathFullyQualified($ExecutablePath)) {
        Add-CleanupFailure -Message (
            "Could not record an actionable retained-process PID/path/start tuple for $Label.")
        Add-Record -Kind 'process_retention_identity_failure' -Details @{
            role = $Role
            label = $Label
            pid = $ProcessId
            executable_path = $ExecutablePath
            start_utc_ticks = $StartUtcTicks
            reason = $Reason
        }
        return
    }
    foreach ($existing in $script:RetainedProcesses) {
        if ($existing.pid -eq $ProcessId -and $existing.start_utc_ticks -eq $StartUtcTicks -and
            (Test-SamePath -Left $existing.executable_path -Right $ExecutablePath)) {
            return
        }
    }
    $entry = [pscustomobject][ordered]@{
        role = $Role
        label = $Label
        pid = $ProcessId
        executable_path = [IO.Path]::GetFullPath($ExecutablePath)
        start_utc_ticks = $StartUtcTicks
        arguments = @($Arguments)
        command_line_sha256 = if ($null -eq $CommandLineSha256 -or
            [string]::IsNullOrWhiteSpace([string]$CommandLineSha256)) {
            $null
        } else {
            [string]$CommandLineSha256
        }
        reason = $Reason
        required_action = 'verify_exact_tuple_and_stop_process_before_removing_candidate'
    }
    $script:RetainedProcesses.Add($entry) | Out-Null
    Add-Record -Kind 'process_retained' -Details @{
        role = $Role
        label = $Label
        pid = $ProcessId
        executable_path = $entry.executable_path
        start_utc_ticks = $StartUtcTicks
        reason = $Reason
    }
}

function New-BoundedByteDrain {
    param(
        [Parameter(Mandatory = $true)][IO.Stream]$Stream,
        [Parameter(Mandatory = $true)][string]$Label
    )
    return [pscustomobject][ordered]@{
        stream = $Stream
        buffer = [byte[]]::new(65536)
        task = $null
        cancellation = [Threading.CancellationTokenSource]::new()
        captured = [IO.MemoryStream]::new($script:MaximumToolOutputBytes)
        captured_bytes = [int64]0
        total_bytes = [int64]0
        truncated = $false
        complete = $false
        label = $Label
    }
}

function Start-BoundedByteRead {
    param([Parameter(Mandatory = $true)][object]$Drain)
    if (-not $Drain.complete -and $null -eq $Drain.task) {
        $Drain.task = $Drain.stream.ReadAsync($Drain.buffer, 0, $Drain.buffer.Length,
            $Drain.cancellation.Token)
    }
}

function Cancel-BoundedByteDrain {
    param([AllowNull()][object]$Drain)
    if ($null -eq $Drain) { return }
    try { $Drain.cancellation.Cancel() } catch { }
    try { $Drain.stream.Dispose() } catch { }
}

function Update-BoundedByteDrain {
    param([Parameter(Mandatory = $true)][object]$Drain)
    Start-BoundedByteRead -Drain $Drain
    if ($Drain.complete -or -not $Drain.task.IsCompleted) { return $false }
    $count = $Drain.task.GetAwaiter().GetResult()
    $Drain.task = $null
    if ($count -eq 0) {
        $Drain.complete = $true
        return $true
    }

    $Drain.total_bytes += [int64]$count
    $remaining = [int64]$script:MaximumToolOutputBytes - $Drain.captured_bytes
    if ($remaining -gt 0) {
        $take = [int][Math]::Min([int64]$count, $remaining)
        if ($take -gt 0) {
            $Drain.captured.Write($Drain.buffer, 0, $take)
            $Drain.captured_bytes += [int64]$take
        }
    }
    if ($Drain.captured_bytes -lt $Drain.total_bytes) { $Drain.truncated = $true }
    Start-BoundedByteRead -Drain $Drain
    return $true
}

function Get-BoundedByteResult {
    param([Parameter(Mandatory = $true)][object]$Drain)
    if (-not $Drain.complete) { throw "$($Drain.label) output drain did not reach EOF." }
    $bytes = $Drain.captured.ToArray()
    $identity = Get-ByteIdentity -Bytes $bytes
    return [pscustomobject][ordered]@{
        text = $script:Utf8.GetString($bytes)
        captured_size = $identity.size
        captured_sha256 = $identity.sha256
        total_size = $Drain.total_bytes
        truncated = [bool]$Drain.truncated
    }
}

function Assert-NoReparseComponents {
    param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$Label)
    $item = Get-Item -LiteralPath $Path -Force
    while ($null -ne $item) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "$Label contains a reparse point: $($item.FullName)"
        }
        $parentPath = Split-Path -Parent $item.FullName
        if ([string]::IsNullOrWhiteSpace($parentPath) -or
            [string]::Equals($parentPath, $item.FullName, [StringComparison]::OrdinalIgnoreCase)) {
            break
        }
        $item = Get-Item -LiteralPath $parentPath -Force
    }
}

function Get-FileIdentity {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $false)][string]$RecordedPath
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file is missing: $Path"
    }
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    Assert-NoReparseComponents -Path $resolved -Label 'Authenticated file'
    $item = Get-Item -LiteralPath $resolved -Force
    if ($item -isnot [IO.FileInfo]) { throw "Not a regular file: $resolved" }
    return [ordered]@{
        path = if ([string]::IsNullOrWhiteSpace($RecordedPath)) { $resolved } else { $RecordedPath }
        size = [int64]$item.Length
        sha256 = Get-LowerSha256 -Path $resolved
    }
}

function Resolve-AuthenticatedBlob {
    param([Parameter(Mandatory = $true)][object]$Blob, [Parameter(Mandatory = $true)][string]$Label)
    $names = @($Blob.PSObject.Properties.Name)
    foreach ($name in @('path', 'size', 'sha256')) {
        if ($names -cnotcontains $name) { throw "$Label is missing '$name'." }
    }
    $path = [string]$Blob.path
    if ([string]::IsNullOrWhiteSpace($path) -or -not [IO.Path]::IsPathFullyQualified($path)) {
        throw "$Label.path must be an absolute path."
    }
    $declaredHash = [string]$Blob.sha256
    if ($declaredHash -cnotmatch '^[0-9a-f]{64}$') {
        throw "$Label.sha256 must be lowercase SHA-256."
    }
    $declaredSize = [int64]$Blob.size
    if ($declaredSize -lt 0) { throw "$Label.size must not be negative." }
    $actual = Get-FileIdentity -Path $path
    if ($actual.size -ne $declaredSize -or $actual.sha256 -cne $declaredHash) {
        throw "$Label does not match its declared byte identity: $path"
    }
    return [pscustomobject]$actual
}

function Test-SamePath {
    param([Parameter(Mandatory = $true)][string]$Left, [Parameter(Mandatory = $true)][string]$Right)
    return [string]::Equals([IO.Path]::GetFullPath($Left), [IO.Path]::GetFullPath($Right),
        [StringComparison]::OrdinalIgnoreCase)
}

function Add-Record {
    param([Parameter(Mandatory = $true)][string]$Kind, [hashtable]$Details = @{})
    $record = [ordered]@{
        sequence = $script:Records.Count
        elapsed_ms = [int64]$script:Watch.ElapsedMilliseconds
        kind = $Kind
    }
    foreach ($key in $Details.Keys) { $record[$key] = $Details[$key] }
    $script:Records.Add([pscustomobject]$record) | Out-Null
}

function Write-AtomicJson {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][object]$Value,
        [switch]$ReplaceOwned
    )
    $parent = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        throw "Atomic JSON parent does not exist: $parent"
    }
    $temporary = "$Path.tmp.$PID.$([Guid]::NewGuid().ToString('N'))"
    [IO.File]::WriteAllText($temporary,
        ($Value | ConvertTo-Json -Depth 64) + [Environment]::NewLine, $script:Utf8)
    try {
        if (Test-Path -LiteralPath $Path) {
            if (-not $ReplaceOwned) { throw "Atomic JSON destination exists: $Path" }
            [IO.File]::Move($temporary, $Path, $true)
        }
        else {
            [IO.File]::Move($temporary, $Path)
        }
    }
    finally {
        if (Test-Path -LiteralPath $temporary -PathType Leaf) {
            Remove-Item -LiteralPath $temporary -Force
        }
    }
}

function Copy-AuthenticatedFile {
    param(
        [Parameter(Mandatory = $true)][object]$SourceIdentity,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$RuntimeRoot
    )
    $fullRoot = [IO.Path]::GetFullPath($RuntimeRoot).TrimEnd('\') + '\'
    $fullDestination = [IO.Path]::GetFullPath($Destination)
    if (-not $fullDestination.StartsWith($fullRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Runtime destination escapes the private runtime: $Destination"
    }
    if (Test-Path -LiteralPath $fullDestination) {
        throw "Runtime destination already exists: $fullDestination"
    }
    $parent = Split-Path -Parent $fullDestination
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    [IO.File]::Copy([string]$SourceIdentity.path, $fullDestination, $false)
    $copied = Get-FileIdentity -Path $fullDestination
    if ($copied.size -ne $SourceIdentity.size -or $copied.sha256 -cne $SourceIdentity.sha256) {
        throw "Staged file changed while copying: $fullDestination"
    }
    return [pscustomobject]$copied
}

function Remove-OwnedRuntime {
    param([Parameter(Mandatory = $true)][string]$Runtime, [Parameter(Mandatory = $true)][string]$Candidate)
    $fullCandidate = [IO.Path]::GetFullPath($Candidate).TrimEnd('\') + '\'
    $fullRuntime = [IO.Path]::GetFullPath($Runtime).TrimEnd('\')
    if ((Split-Path -Leaf $fullRuntime) -cne '.runtime' -or
        -not ($fullRuntime + '\').StartsWith($fullCandidate, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to remove an unowned runtime directory: $fullRuntime"
    }
    if (-not (Test-Path -LiteralPath $fullRuntime -PathType Container)) { return }
    Assert-NoReparseComponents -Path $fullRuntime -Label 'Private runtime'
    $reparseChildren = @(Get-ChildItem -LiteralPath $fullRuntime -Force -Recurse |
        Where-Object { ($_.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 })
    if ($reparseChildren.Count -ne 0) {
        throw "Refusing to remove a private runtime containing reparse points: $($reparseChildren[0].FullName)"
    }
    Remove-Item -LiteralPath $fullRuntime -Force -Recurse
    if (Test-Path -LiteralPath $fullRuntime) { throw "Private runtime removal was not verified: $fullRuntime" }
}

function Get-OwnershipSnapshot {
    param(
        [Parameter(Mandatory = $true)][Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][string]$ExpectedExecutable
    )
    $lastFailure = $null
    for ($attempt = 0; $attempt -lt 50; $attempt++) {
        try {
            $Process.Refresh()
            if ($Process.HasExited) { throw 'Process exited before ownership could be established.' }
            $path = $Process.Path
            $startTicks = $Process.StartTime.ToUniversalTime().Ticks
            $cim = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $($Process.Id)" -ErrorAction Stop
            if ($null -eq $cim -or [string]::IsNullOrWhiteSpace([string]$cim.CommandLine)) {
                throw 'The operating system did not return a command line.'
            }
            if (-not (Test-SamePath -Left $path -Right $ExpectedExecutable) -or
                -not (Test-SamePath -Left ([string]$cim.ExecutablePath) -Right $ExpectedExecutable)) {
                throw 'The launched executable path does not match the staged executable.'
            }
            $commandLine = [string]$cim.CommandLine
            $commandIdentity = Get-TextIdentity -Text $commandLine
            return [pscustomobject][ordered]@{
                pid = [int64]$Process.Id
                path = (Resolve-Path -LiteralPath $path).Path
                start_utc_ticks = [int64]$startTicks
                command_line = $commandLine
                command_line_sha256 = $commandIdentity.sha256
            }
        }
        catch {
            $lastFailure = $_.Exception.Message
            Start-Sleep -Milliseconds 100
        }
    }
    throw "Could not establish exact process ownership: $lastFailure"
}

function Test-OwnershipTuple {
    param([Parameter(Mandatory = $true)][object]$Ownership)
    $candidate = $null
    try {
        $candidate = Get-Process -Id ([int]$Ownership.pid) -ErrorAction Stop
        $candidatePath = $candidate.Path
        $candidateTicks = $candidate.StartTime.ToUniversalTime().Ticks
        $cim = Get-CimInstance -ClassName Win32_Process -Filter "ProcessId = $($Ownership.pid)" -ErrorAction Stop
        if ($null -eq $cim) { return $false }
        return $candidate.Id -eq $Ownership.pid -and
            (Test-SamePath -Left $candidatePath -Right ([string]$Ownership.path)) -and
            $candidateTicks -eq $Ownership.start_utc_ticks -and
            (Test-SamePath -Left ([string]$cim.ExecutablePath) -Right ([string]$Ownership.path)) -and
            [string]::Equals([string]$cim.CommandLine, [string]$Ownership.command_line,
                [StringComparison]::Ordinal)
    }
    catch {
        return $false
    }
    finally {
        if ($null -ne $candidate) { $candidate.Dispose() }
    }
}

function Add-RetainedEmulator {
    param([Parameter(Mandatory = $true)][string]$Reason)
    if ($null -eq $script:Process) { return }
    $processId = if ($null -ne $script:Ownership) {
        [int64]$script:Ownership.pid
    } else {
        [int64]$script:Process.Id
    }
    $executablePath = if ($null -ne $script:Ownership) {
        [string]$script:Ownership.path
    } else {
        [string]$script:EmulatorExecutablePath
    }
    $startTicks = if ($null -ne $script:Ownership) {
        [int64]$script:Ownership.start_utc_ticks
    } else {
        [int64]$script:EmulatorStartTicks
    }
    $commandLineSha256 = if ($null -ne $script:Ownership) {
        [string]$script:Ownership.command_line_sha256
    } else {
        $null
    }
    if ([string]::IsNullOrWhiteSpace($executablePath) -or $startTicks -le 0 -or $processId -le 0) {
        Add-CleanupFailure -Message 'Could not form the retained emulator PID/path/start tuple.'
        return
    }
    Add-RetainedProcess -Role 'emulator' -Label 'Flycast emulator' `
        -ProcessId $processId -ExecutablePath $executablePath -StartUtcTicks $startTicks `
        -Arguments @($script:EmulatorArguments) `
        -CommandLineSha256 $commandLineSha256 -Reason $Reason
}

function Get-RequiredProcessStartUtcTicks {
    param(
        [Parameter(Mandatory = $true)][Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][string]$Label
    )
    $lastFailure = $null
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        try {
            $Process.Refresh()
            $ticks = [int64]$Process.StartTime.ToUniversalTime().Ticks
            if ($ticks -le 0) { throw 'The operating system returned a non-positive start time.' }
            return $ticks
        }
        catch {
            $lastFailure = $_.Exception.Message
            try {
                $Process.Refresh()
                if ($Process.HasExited) { break }
            }
            catch { }
            Start-Sleep -Milliseconds 25
        }
    }
    throw "Could not acquire $Label process-start identity: $lastFailure"
}

function Stop-OwnedProcess {
    param([Parameter(Mandatory = $true)][string]$Reason)
    if ($null -eq $script:Process) { return $true }
    try {
        $script:Process.Refresh()
        if ($script:Process.HasExited) {
            $script:TerminationVerified = $true
            return $true
        }
    }
    catch {
        $reason = "Could not inspect the owned process: $($_.Exception.Message)"
        Add-CleanupFailure -Message $reason
        Add-RetainedEmulator -Reason $reason
        return $false
    }
    if ($null -eq $script:Ownership) {
        $reason = 'Owned process identity is incomplete; refusing termination.'
        Add-CleanupFailure -Message $reason
        Add-RetainedEmulator -Reason $reason
        return $false
    }
    if ($script:ForceCleanupRefusal) {
        $reason = 'Integration test forced exact-ownership cleanup refusal.'
        Add-CleanupFailure -Message $reason
        Add-RetainedEmulator -Reason $reason
        return $false
    }
    if (-not (Test-OwnershipTuple -Ownership $script:Ownership)) {
        $reason = 'Owned process tuple changed; refusing PID-only termination.'
        Add-CleanupFailure -Message $reason
        Add-RetainedEmulator -Reason $reason
        return $false
    }
    try {
        $script:TerminationRequested = $true
        $script:Process.Kill($true)
        if (-not $script:Process.WaitForExit(10000)) {
            $reason = 'Owned process did not exit within ten seconds.'
            Add-CleanupFailure -Message $reason
            Add-RetainedEmulator -Reason $reason
            return $false
        }
        $script:Process.Refresh()
        if (-not $script:Process.HasExited) {
            $reason = 'Owned process did not report a verified exit.'
            Add-CleanupFailure -Message $reason
            Add-RetainedEmulator -Reason $reason
            return $false
        }
        $script:TerminationVerified = $true
        $script:ProcessExitCode = [int]$script:Process.ExitCode
        Add-Record -Kind 'owned_process_stopped' -Details @{
            reason = $Reason
            pid = $script:Ownership.pid
            executable_path = $script:Ownership.path
            start_utc_ticks = $script:Ownership.start_utc_ticks
            command_line_sha256 = $script:Ownership.command_line_sha256
        }
        return $true
    }
    catch {
        $reason = "Owned process termination failed: $($_.Exception.Message)"
        Add-CleanupFailure -Message $reason
        Add-RetainedEmulator -Reason $reason
        return $false
    }
}

function Stop-ExactToolProcess {
    param(
        [Parameter(Mandatory = $true)][Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][object]$ExecutableIdentity,
        [Parameter(Mandatory = $true)][int64]$ExpectedStartTicks,
        [Parameter(Mandatory = $true)][string]$Role,
        [Parameter(Mandatory = $true)][string]$Label,
        [Parameter(Mandatory = $true)][string[]]$Arguments
    )
    $retain = {
        param([string]$Reason)
        Add-RetainedProcess -Role $Role -Label $Label -ProcessId ([int64]$Process.Id) `
            -ExecutablePath ([string]$ExecutableIdentity.path) -StartUtcTicks $ExpectedStartTicks `
            -Arguments @($Arguments) -CommandLineSha256 $null -Reason $Reason
    }
    try {
        $Process.Refresh()
        if ($Process.HasExited) { return $true }
        if ($ExpectedStartTicks -le 0) {
            $ExpectedStartTicks = Get-RequiredProcessStartUtcTicks -Process $Process -Label $Label
        }
        if (-not (Test-SamePath -Left $Process.Path -Right ([string]$ExecutableIdentity.path)) -or
            $Process.StartTime.ToUniversalTime().Ticks -ne $ExpectedStartTicks) {
            $reason = "$Label ownership tuple changed; refusing PID-only termination."
            Add-CleanupFailure -Message $reason
            & $retain $reason
            return $false
        }
    }
    catch {
        $reason = "Could not inspect timed-out $Label ownership; refusing termination: $($_.Exception.Message)"
        Add-CleanupFailure -Message $reason
        & $retain $reason
        return $false
    }
    if ($script:ForceToolCleanupRefusal) {
        $reason = "Integration test forced exact-ownership cleanup refusal for $Label."
        Add-CleanupFailure -Message $reason
        & $retain $reason
        return $false
    }
    try {
        $Process.Kill($true)
        if (-not $Process.WaitForExit(10000)) {
            $reason = "$Label did not exit within ten seconds after exact-owned termination."
            Add-CleanupFailure -Message $reason
            & $retain $reason
            return $false
        }
        $Process.Refresh()
        if (-not $Process.HasExited) {
            $reason = "$Label termination could not be verified."
            Add-CleanupFailure -Message $reason
            & $retain $reason
            return $false
        }
        return $true
    }
    catch {
        $reason = "$Label exact-owned termination failed: $($_.Exception.Message)"
        Add-CleanupFailure -Message $reason
        & $retain $reason
        return $false
    }
}

function Get-ArtifactSample {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$RecordedPath,
        [Parameter(Mandatory = $true)][Diagnostics.Process]$Process
    )
    $Process.Refresh()
    $exited = $Process.HasExited
    $exitCode = if ($exited) { [int]$Process.ExitCode } else { $null }
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return [pscustomobject][ordered]@{
            path = $RecordedPath; exists = $false; size = $null; sha256 = $null
            last_write_utc_ticks = $null; process_exited = $exited; exit_code = $exitCode
        }
    }
    $identity = Get-FileIdentity -Path $Path -RecordedPath $RecordedPath
    $item = Get-Item -LiteralPath $Path
    return [pscustomobject][ordered]@{
        path = $RecordedPath
        exists = $true
        size = $identity.size
        sha256 = $identity.sha256
        last_write_utc_ticks = [int64]$item.LastWriteTimeUtc.Ticks
        process_exited = $exited
        exit_code = $exitCode
    }
}

function Test-ArtifactSamplesMatch {
    param([Parameter(Mandatory = $true)][object]$First, [Parameter(Mandatory = $true)][object]$Second)
    foreach ($field in @('path', 'exists', 'size', 'sha256', 'last_write_utc_ticks',
            'process_exited', 'exit_code')) {
        if ($First.$field -cne $Second.$field) { return $false }
    }
    return $true
}

function Invoke-OwnedTool {
    param(
        [Parameter(Mandatory = $true)][object]$ExecutableIdentity,
        [Parameter(Mandatory = $true)][string[]]$Arguments,
        [Parameter(Mandatory = $true)][int]$TimeoutSeconds,
        [Parameter(Mandatory = $true)][string]$Role,
        [Parameter(Mandatory = $true)][string]$Label
    )
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = [string]$ExecutableIdentity.path
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    $toolProcessId = [int64]0
    $startTicks = [int64]0
    $stdoutDrain = $null
    $stderrDrain = $null
    $stopAttempted = $false
    $retainLiveResources = $false
    try {
        if (-not $process.Start()) { throw "Could not start $Label." }
        $toolProcessId = [int64]$process.Id
        $startTicks = Get-RequiredProcessStartUtcTicks -Process $process -Label $Label
        $stdoutDrain = New-BoundedByteDrain -Stream $process.StandardOutput.BaseStream -Label "$Label stdout"
        $stderrDrain = New-BoundedByteDrain -Stream $process.StandardError.BaseStream -Label "$Label stderr"
        Start-BoundedByteRead -Drain $stdoutDrain
        Start-BoundedByteRead -Drain $stderrDrain
        $watch = [Diagnostics.Stopwatch]::StartNew()
        while ($true) {
            for ($drainRound = 0; $drainRound -lt 64; $drainRound++) {
                $stdoutProgress = Update-BoundedByteDrain -Drain $stdoutDrain
                $stderrProgress = Update-BoundedByteDrain -Drain $stderrDrain
                if (-not $stdoutProgress -and -not $stderrProgress) { break }
            }
            $exited = $process.WaitForExit(10)
            if ($exited -and $stdoutDrain.complete -and $stderrDrain.complete) { break }
            if ($watch.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
                if (-not $exited) {
                    $stopAttempted = $true
                    Stop-ExactToolProcess -Process $process -ExecutableIdentity $ExecutableIdentity `
                        -ExpectedStartTicks $startTicks -Role $Role -Label $Label `
                        -Arguments @($Arguments) | Out-Null
                }
                throw "$Label timed out after $TimeoutSeconds seconds."
            }
        }
        $stdoutResult = Get-BoundedByteResult -Drain $stdoutDrain
        $stderrResult = Get-BoundedByteResult -Drain $stderrDrain
        return [pscustomobject][ordered]@{
            executable = [pscustomobject][ordered]@{
                path = $ExecutableIdentity.path
                size = $ExecutableIdentity.size
                sha256 = $ExecutableIdentity.sha256
            }
            arguments = @($Arguments)
            pid = $toolProcessId
            start_utc_ticks = $startTicks
            exit_code = [int]$process.ExitCode
            stdout_size = $stdoutResult.captured_size
            stdout_sha256 = $stdoutResult.captured_sha256
            stdout_total_size = $stdoutResult.total_size
            stdout_truncated = $stdoutResult.truncated
            stderr_size = $stderrResult.captured_size
            stderr_sha256 = $stderrResult.captured_sha256
            stderr_total_size = $stderrResult.total_size
            stderr_truncated = $stderrResult.truncated
            stdout = $stdoutResult.text
            stderr = $stderrResult.text
        }
    }
    catch {
        if ($toolProcessId -ne 0) {
            try {
                $process.Refresh()
                if (-not $process.HasExited -and -not $stopAttempted) {
                    Stop-ExactToolProcess -Process $process `
                        -ExecutableIdentity $ExecutableIdentity -ExpectedStartTicks $startTicks `
                        -Role $Role -Label $Label -Arguments @($Arguments) | Out-Null
                }
            }
            catch {
                Add-CleanupFailure -Message "Could not verify $Label terminal state after failure: $($_.Exception.Message)"
            }
            try {
                $process.Refresh()
                $retainLiveResources = -not $process.HasExited
            }
            catch {
                $retainLiveResources = $true
            }
        }
        throw
    }
    finally {
        if (-not $retainLiveResources) {
            Cancel-BoundedByteDrain -Drain $stdoutDrain
            Cancel-BoundedByteDrain -Drain $stderrDrain
            if ($null -ne $stdoutDrain) {
                $stdoutDrain.captured.Dispose()
                $stdoutDrain.cancellation.Dispose()
            }
            if ($null -ne $stderrDrain) {
                $stderrDrain.captured.Dispose()
                $stderrDrain.cancellation.Dispose()
            }
            $process.Dispose()
        }
    }
}

function Add-SourceSpec {
    param(
        [Parameter(Mandatory = $true)][Collections.Generic.List[object]]$List,
        [Parameter(Mandatory = $true)][hashtable]$ByRole,
        [Parameter(Mandatory = $true)][string]$Role,
        [Parameter(Mandatory = $true)][object]$Blob,
        [Parameter(Mandatory = $true)][string]$Label
    )
    if ($ByRole.ContainsKey($Role)) { throw "Duplicate source role: $Role" }
    $identity = Resolve-AuthenticatedBlob -Blob $Blob -Label $Label
    $entry = [pscustomobject][ordered]@{
        role = $Role
        path = $identity.path
        size = $identity.size
        sha256 = $identity.sha256
    }
    $List.Add($entry) | Out-Null
    $ByRole[$Role] = $entry
}

function Get-CurrentSourceSet {
    param([Parameter(Mandatory = $true)][object[]]$Sources)
    $result = [Collections.Generic.List[object]]::new()
    foreach ($source in $Sources) {
        $identity = Get-FileIdentity -Path ([string]$source.path)
        $result.Add([pscustomobject][ordered]@{
            role = [string]$source.role
            path = $identity.path
            size = $identity.size
            sha256 = $identity.sha256
        }) | Out-Null
    }
    return @($result)
}

function Assert-SourceSetsMatch {
    param([Parameter(Mandatory = $true)][object[]]$Before,
          [Parameter(Mandatory = $true)][object[]]$After,
          [Parameter(Mandatory = $true)][string]$Label)
    if ($Before.Count -ne $After.Count) { throw "$Label source count changed." }
    for ($index = 0; $index -lt $Before.Count; $index++) {
        if ($Before[$index].role -cne $After[$index].role -or
            -not (Test-SamePath -Left $Before[$index].path -Right $After[$index].path) -or
            $Before[$index].size -ne $After[$index].size -or
            $Before[$index].sha256 -cne $After[$index].sha256) {
            throw "$Label source identity changed: $($Before[$index].role)"
        }
    }
}

function Get-RuntimeState {
    param([Parameter(Mandatory = $true)][object[]]$RuntimeFiles)
    $result = [Collections.Generic.List[object]]::new()
    foreach ($runtimeFile in $RuntimeFiles) {
        $identity = Get-FileIdentity -Path ([string]$runtimeFile.path)
        $result.Add([pscustomobject][ordered]@{
            role = [string]$runtimeFile.role
            path = $identity.path
            size = $identity.size
            sha256 = $identity.sha256
        }) | Out-Null
    }
    return @($result)
}

# Immutable job and schema preflight.
$resolvedJob = (Resolve-Path -LiteralPath $Job).Path
Assert-NoReparseComponents -Path $resolvedJob -Label 'Capture job'
$jobObject = Get-Content -LiteralPath $resolvedJob -Raw -Encoding UTF8 | ConvertFrom-Json
Assert-ObjectProperties -Value $jobObject `
    -Allowed @('schema', 'schema_version', 'driver', 'identity_manifest', 'emulator',
        'validator', 'capture_validator', 'output', 'limits', 'metadata') `
    -Required @('schema', 'schema_version', 'driver', 'identity_manifest', 'emulator',
        'validator', 'capture_validator', 'output', 'limits') -Label 'Capture job'
if ([string]$jobObject.schema -cne 'flycast-research-capture-job' -or
    [int64]$jobObject.schema_version -ne 1) {
    throw 'Unsupported capture job schema.'
}
Assert-ObjectProperties -Value $jobObject.emulator `
    -Allowed @('stage_executable', 'interactive', 'arguments_prefix', 'authenticated_inputs', 'runtime_files') `
    -Required @('stage_executable', 'interactive', 'arguments_prefix', 'authenticated_inputs', 'runtime_files') `
    -Label 'Capture job emulator'
if (-not [bool]$jobObject.emulator.stage_executable) {
    throw 'Capture job emulator.stage_executable must be true.'
}
Assert-ObjectProperties -Value $jobObject.validator `
    -Allowed @('executable', 'arguments_prefix', 'authenticated_inputs') `
    -Required @('executable', 'arguments_prefix', 'authenticated_inputs') -Label 'Capture job validator'
Assert-ObjectProperties -Value $jobObject.capture_validator -Allowed @('executable') `
    -Required @('executable') -Label 'Capture job capture_validator'
Assert-ObjectProperties -Value $jobObject.output -Allowed @('accepted_directory', 'artifact_name') `
    -Required @('accepted_directory', 'artifact_name') -Label 'Capture job output'
Assert-ObjectProperties -Value $jobObject.limits `
    -Allowed @('trace_max_bytes', 'emulator_timeout_seconds', 'validator_timeout_seconds',
        'stability_interval_ms') `
    -Required @('trace_max_bytes', 'emulator_timeout_seconds', 'validator_timeout_seconds',
        'stability_interval_ms') -Label 'Capture job limits'

$emulatorPrefix = @(Assert-ArrayOfStrings -Value $jobObject.emulator.arguments_prefix `
    -Label 'Capture job emulator.arguments_prefix')
$validatorPrefix = @(Assert-ArrayOfStrings -Value $jobObject.validator.arguments_prefix `
    -Label 'Capture job validator.arguments_prefix')
foreach ($argument in $emulatorPrefix) {
    if (Test-ContainsResearchConfigSection -Value $argument) {
        throw 'job.emulator.arguments_prefix must not supply research settings.'
    }
}
if ($emulatorPrefix.Count -gt 0 -and
    $emulatorPrefix[$emulatorPrefix.Count - 1] -cin @('-config', '--config')) {
    throw 'job.emulator.arguments_prefix must not end with an incomplete -config/--config option.'
}
$traceMaxBytes = [int64]$jobObject.limits.trace_max_bytes
$emulatorTimeoutSeconds = [int]$jobObject.limits.emulator_timeout_seconds
$validatorTimeoutSeconds = [int]$jobObject.limits.validator_timeout_seconds
$stabilityIntervalMs = [int]$jobObject.limits.stability_interval_ms
if ($traceMaxBytes -lt 160 -or $traceMaxBytes -gt 1099511627776) { throw 'trace_max_bytes is out of range.' }
if ($emulatorTimeoutSeconds -lt 1 -or $emulatorTimeoutSeconds -gt 86400) { throw 'emulator_timeout_seconds is out of range.' }
if ($validatorTimeoutSeconds -lt 1 -or $validatorTimeoutSeconds -gt 3600) { throw 'validator_timeout_seconds is out of range.' }
if ($stabilityIntervalMs -lt 50 -or $stabilityIntervalMs -gt 5000) { throw 'stability_interval_ms is out of range.' }

$acceptedDirectoryText = [string]$jobObject.output.accepted_directory
$acceptedDirectory = Get-CanonicalAbsolutePath -Path $acceptedDirectoryText `
    -Label 'output.accepted_directory'
$acceptedParent = Split-Path -Parent $acceptedDirectory
if (-not (Test-Path -LiteralPath $acceptedParent -PathType Container)) {
    throw "Accepted output parent does not exist: $acceptedParent"
}
Assert-NoReparseComponents -Path $acceptedParent -Label 'Accepted output parent'
if (Test-Path -LiteralPath $acceptedDirectory) {
    throw "Accepted output already exists: $acceptedDirectory"
}
$artifactName = [string]$jobObject.output.artifact_name
if ([string]::IsNullOrWhiteSpace($artifactName) -or -not (Test-SafeArtifactName -Name $artifactName)) {
    throw 'output.artifact_name must be a safe ASCII .fcmr file name (letters, digits, dot, underscore, or hyphen).'
}
if ($IntegrationTestAbortToken) {
    if ($IntegrationTestAbortToken -cne $script:ForcedAbortToken) {
        throw 'The forced-abort integration token is invalid.'
    }
    if ((Split-Path -Leaf $acceptedParent) -cne 'forced-abort-tests') {
        throw 'Forced-abort integration is restricted to an output parent named forced-abort-tests.'
    }
}
if ($IntegrationTestFaultToken) {
    switch ($IntegrationTestFaultToken) {
        $script:ForcedStagingFailureToken {
            if ((Split-Path -Leaf $acceptedParent) -cne 'staging-fault-tests') {
                throw 'Forced staging-failure integration is restricted to an output parent named staging-fault-tests.'
            }
        }
        $script:ForcedCleanupRefusalToken {
            if ((Split-Path -Leaf $acceptedParent) -cne 'cleanup-refusal-tests') {
                throw 'Forced cleanup-refusal integration is restricted to an output parent named cleanup-refusal-tests.'
            }
        }
        $script:ForcedToolCleanupRefusalToken {
            if ((Split-Path -Leaf $acceptedParent) -cnotin
                @('tool-cleanup-refusal-tests', 'capture-validator-cleanup-refusal-tests')) {
                throw ('Forced tool-cleanup-refusal integration is restricted to a dedicated ' +
                    'tool-cleanup-refusal test output parent.')
            }
        }
        default { throw 'The integration fault token is invalid.' }
    }
}
$script:ForceToolCleanupRefusal =
    $IntegrationTestFaultToken -ceq $script:ForcedToolCleanupRefusalToken

# Flycast's comma-separated configuration grammar has quotes but no escaping.
# The candidate suffix and safe artifact name cannot introduce quotes, so the
# accepted parent is the only variable path component that needs this preflight.
Format-FlycastConfigValue -Value $acceptedParent | Out-Null

$driverIdentity = Resolve-AuthenticatedBlob -Blob $jobObject.driver -Label 'job.driver'
if (-not (Test-SamePath -Left $driverIdentity.path -Right $PSCommandPath)) {
    throw 'job.driver does not identify the running driver script.'
}
$identitySource = Resolve-AuthenticatedBlob -Blob $jobObject.identity_manifest -Label 'job.identity_manifest'
$identityObject = Get-Content -LiteralPath $identitySource.path -Raw -Encoding UTF8 | ConvertFrom-Json

$sourceSpecs = [Collections.Generic.List[object]]::new()
$sourceByRole = @{}
$jobSourceIdentity = Get-FileIdentity -Path $resolvedJob
$captureJobEntry = [pscustomobject][ordered]@{
    role = 'capture_job'; path = $jobSourceIdentity.path
    size = $jobSourceIdentity.size; sha256 = $jobSourceIdentity.sha256
}
$sourceSpecs.Add($captureJobEntry) | Out-Null
$sourceByRole['capture_job'] = $captureJobEntry
Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role 'driver' -Blob $jobObject.driver -Label 'job.driver'
Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role 'identity_manifest' `
    -Blob $jobObject.identity_manifest -Label 'job.identity_manifest'

foreach ($requiredObject in @('media', 'firmware', 'emulator', 'configuration')) {
    if (@($identityObject.PSObject.Properties.Name) -cnotcontains $requiredObject) {
        throw "Identity manifest is missing '$requiredObject'."
    }
}
if (@($identityObject.PSObject.Properties.Name) -cnotcontains 'persistent_devices') {
    throw "Identity manifest is missing 'persistent_devices'."
}
$mediaKind = [string]$identityObject.media.kind
if ($mediaKind -cne 'gdi') {
    throw "Phase 2 capture v1 supports GDI media only, not '$mediaKind'."
}
$mediaTracks = if (Test-HasProperty -Value $identityObject.media -Name 'tracks') {
    @($identityObject.media.tracks)
} else { @() }
if ($mediaTracks.Count -eq 0) {
    throw 'Phase 2 GDI capture requires identity.media.tracks.'
}
$firmwareMode = [string]$identityObject.firmware.mode
$hasBios = (Test-HasProperty -Value $identityObject.firmware -Name 'bios') -and
    $null -ne $identityObject.firmware.bios
if ($firmwareMode -ceq 'real' -and -not $hasBios) {
    throw 'Real firmware capture requires identity.firmware.bios.'
}

Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role 'media.source' `
    -Blob $identityObject.media.source -Label 'identity.media.source'
Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role 'media.ip_bin' `
    -Blob $identityObject.media.ip_bin -Label 'identity.media.ip_bin'
Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role 'media.boot_executable' `
    -Blob $identityObject.media.boot_executable -Label 'identity.media.boot_executable'
foreach ($track in $mediaTracks) {
    $trackNumber = [int]$track.track
    Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role "media.track.$trackNumber" `
        -Blob $track -Label "identity.media.track.$trackNumber"
}
if ($hasBios) {
    Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role 'firmware.bios' `
        -Blob $identityObject.firmware.bios -Label 'identity.firmware.bios'
}
Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role 'firmware.flash_initial' `
    -Blob $identityObject.firmware.flash_initial -Label 'identity.firmware.flash_initial'
foreach ($device in @($identityObject.persistent_devices)) {
    $bus = [int]$device.bus
    $port = [int]$device.port
    Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role "persistent_device.$bus.$port" `
        -Blob $device -Label "identity.persistent_device.$bus.$port"
}
Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role 'emulator.executable' `
    -Blob $identityObject.emulator.executable -Label 'identity.emulator.executable'
for ($index = 0; $index -lt @($jobObject.emulator.authenticated_inputs).Count; $index++) {
    Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role "emulator.authenticated_input.$index" `
        -Blob @($jobObject.emulator.authenticated_inputs)[$index] `
        -Label "job.emulator.authenticated_inputs[$index]"
}
for ($index = 0; $index -lt @($jobObject.emulator.runtime_files).Count; $index++) {
    $runtimeSpec = @($jobObject.emulator.runtime_files)[$index]
    Assert-ObjectProperties -Value $runtimeSpec -Allowed @('source', 'destination') `
        -Required @('source', 'destination') -Label "job.emulator.runtime_files[$index]"
    Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role "emulator.runtime_file.$index" `
        -Blob $runtimeSpec.source -Label "job.emulator.runtime_files[$index].source"
}
Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role 'validator.executable' `
    -Blob $jobObject.validator.executable -Label 'job.validator.executable'
for ($index = 0; $index -lt @($jobObject.validator.authenticated_inputs).Count; $index++) {
    Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role "validator.authenticated_input.$index" `
        -Blob @($jobObject.validator.authenticated_inputs)[$index] `
        -Label "job.validator.authenticated_inputs[$index]"
}
Add-SourceSpec -List $sourceSpecs -ByRole $sourceByRole -Role 'capture_validator.executable' `
    -Blob $jobObject.capture_validator.executable -Label 'job.capture_validator.executable'

$sourceBefore = @(Get-CurrentSourceSet -Sources @($sourceSpecs))
Assert-SourceSetsMatch -Before @($sourceSpecs) -After $sourceBefore -Label 'Pre-launch'

$captureId = [Guid]::NewGuid().ToString().ToLowerInvariant()
$candidateName = ".flycast-research-candidate-$captureId"
$candidateDirectory = Join-Path $acceptedParent $candidateName
$quarantineRoot = Join-Path $acceptedParent '.flycast-research-quarantine'
$quarantineDirectory = Join-Path $quarantineRoot $captureId
if (Test-Path -LiteralPath $candidateDirectory) { throw "Candidate directory already exists: $candidateDirectory" }
if (Test-Path -LiteralPath $quarantineDirectory) { throw "Quarantine directory already exists: $quarantineDirectory" }
if (-not [string]::Equals([IO.Path]::GetPathRoot($candidateDirectory),
        [IO.Path]::GetPathRoot($acceptedDirectory), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Candidate and accepted directory are not on the same volume.'
}

# Everything after the candidate can first appear is inside the transaction
# boundary. Preflight failures create nothing; staging and launch failures are
# retained with the same quarantine path as post-launch failures.
$currentPackageDirectory = $candidateDirectory
$runtimeDirectory = Join-Path $candidateDirectory '.runtime'
$runtimeDataDirectory = Join-Path $runtimeDirectory 'data'
$packagedJobPath = Join-Path $candidateDirectory 'job.json'
$packagedIdentityPath = Join-Path $candidateDirectory 'identity.json'
$artifactPath = Join-Path $candidateDirectory $artifactName
$receiptPath = Join-Path $candidateDirectory 'capture-validation.json'
$transcriptPath = Join-Path $candidateDirectory 'transcript.json'
$packagedJobIdentity = $null
$packagedIdentity = $null
$emulatorSource = $sourceByRole['emulator.executable']
$stagedExecutable = $null
$runtimeTrackedFiles = [Collections.Generic.List[object]]::new()
$runtimeInitial = @()
$terminalStability = $null
$artifactIdentity = $null
$validatorResult = $null
$runtimeTerminal = @()
$sourceAfter = @()
$failure = $null
$accepted = $false
$captureValidatorRun = $null

try {
[IO.Directory]::CreateDirectory($candidateDirectory) | Out-Null
Add-Record -Kind 'candidate_created' -Details @{
    capture_id = $captureId
    candidate_directory = $candidateDirectory
    accepted_directory = $acceptedDirectory
}
if ($IntegrationTestFaultToken -ceq $script:ForcedStagingFailureToken) {
    throw "$($script:ForcedStagingFailureMarker) injected before package files were staged."
}
[IO.File]::Copy($resolvedJob, $packagedJobPath, $false)
[IO.File]::Copy($identitySource.path, $packagedIdentityPath, $false)
$packagedJobIdentity = Get-FileIdentity -Path $packagedJobPath -RecordedPath 'job.json'
$packagedIdentity = Get-FileIdentity -Path $packagedIdentityPath -RecordedPath 'identity.json'
if ($packagedJobIdentity.size -ne $jobSourceIdentity.size -or
    $packagedJobIdentity.sha256 -cne $jobSourceIdentity.sha256 -or
    $packagedIdentity.size -ne $identitySource.size -or
    $packagedIdentity.sha256 -cne $identitySource.sha256) {
    throw 'Packaged job or identity changed during private staging.'
}
[IO.Directory]::CreateDirectory($runtimeDataDirectory) | Out-Null

$stagedExecutablePath = Join-Path $runtimeDirectory ([IO.Path]::GetFileName([string]$emulatorSource.path))
$stagedExecutable = Copy-AuthenticatedFile -SourceIdentity $emulatorSource `
    -Destination $stagedExecutablePath -RuntimeRoot $runtimeDirectory

$flashSource = $sourceByRole['firmware.flash_initial']
$flashName = [IO.Path]::GetFileName([string]$flashSource.path)
if ($flashName -cne 'dc_nvmem.bin') {
    throw 'Dreamcast Phase 2 flash_initial must use the canonical file name dc_nvmem.bin.'
}
$stagedFlash = Copy-AuthenticatedFile -SourceIdentity $flashSource `
    -Destination (Join-Path $runtimeDataDirectory $flashName) -RuntimeRoot $runtimeDirectory
$runtimeTrackedFiles.Add([pscustomobject][ordered]@{
    role = 'firmware.flash'; path = $stagedFlash.path
}) | Out-Null

if ($hasBios) {
    $biosSource = $sourceByRole['firmware.bios']
    $biosName = [IO.Path]::GetFileName([string]$biosSource.path)
    if ($biosName -cnotin @('dc_boot.bin', 'dc_bios.bin')) {
        throw 'Real Dreamcast firmware must use dc_boot.bin or dc_bios.bin.'
    }
    Copy-AuthenticatedFile -SourceIdentity $biosSource `
        -Destination (Join-Path $runtimeDataDirectory $biosName) -RuntimeRoot $runtimeDirectory | Out-Null
}

foreach ($device in @($identityObject.persistent_devices)) {
    if ([string]$device.kind -cne 'vmu') {
        throw "Phase 2 v1 only stages persistent device kind 'vmu'."
    }
    $bus = [int]$device.bus
    $port = [int]$device.port
    if ($bus -lt 0 -or $bus -gt 3 -or $port -lt 0 -or $port -gt 5) {
        throw "Persistent device bus/port is out of range: $bus/$port"
    }
    $portName = if ($port -eq 5) { 'x' } else { [char]([int][char]'1' + $port) }
    $vmuName = "vmu_save_$([char]([int][char]'A' + $bus))$portName.bin"
    $deviceSource = $sourceByRole["persistent_device.$bus.$port"]
    $stagedDevice = Copy-AuthenticatedFile -SourceIdentity $deviceSource `
        -Destination (Join-Path $runtimeDataDirectory $vmuName) -RuntimeRoot $runtimeDirectory
    $runtimeTrackedFiles.Add([pscustomobject][ordered]@{
        role = "persistent_device.$bus.$port"; path = $stagedDevice.path
    }) | Out-Null
}

for ($index = 0; $index -lt @($jobObject.emulator.runtime_files).Count; $index++) {
    $runtimeSpec = @($jobObject.emulator.runtime_files)[$index]
    $destinationText = [string]$runtimeSpec.destination
    if ([string]::IsNullOrWhiteSpace($destinationText) -or [IO.Path]::IsPathFullyQualified($destinationText)) {
        throw "Runtime dependency destination must be relative: $destinationText"
    }
    $destination = [IO.Path]::GetFullPath((Join-Path $runtimeDirectory $destinationText))
    Copy-AuthenticatedFile -SourceIdentity $sourceByRole["emulator.runtime_file.$index"] `
        -Destination $destination -RuntimeRoot $runtimeDirectory | Out-Null
}
$runtimeInitial = @(Get-RuntimeState -RuntimeFiles @($runtimeTrackedFiles))

$transientConfiguration = @()
if (@($identityObject.configuration.values.PSObject.Properties.Name) -contains 'flycast_transient') {
    $transientConfiguration = @(Assert-ArrayOfStrings `
        -Value $identityObject.configuration.values.flycast_transient `
        -Label 'identity.configuration.values.flycast_transient' -Maximum 128)
}
foreach ($setting in $transientConfiguration) {
    if ([string]::IsNullOrWhiteSpace($setting)) {
        throw 'flycast_transient must not contain empty settings.'
    }
    if (Test-ContainsResearchConfigSection -Value $setting) {
        throw 'flycast_transient must not supply research settings.'
    }
}

$identityConfigValue = Format-FlycastConfigValue -Value $packagedIdentityPath
$artifactConfigValue = Format-FlycastConfigValue -Value $artifactPath
$researchConfig = "research:IdentityManifest=$identityConfigValue," +
    "research:MapleRecord=$artifactConfigValue,research:MapleTraceMaxBytes=$traceMaxBytes"
$emulatorArguments = [Collections.Generic.List[string]]::new()
foreach ($argument in $emulatorPrefix) { $emulatorArguments.Add($argument) }
$emulatorArguments.Add('-config')
$emulatorArguments.Add($researchConfig)
foreach ($setting in $transientConfiguration) {
    $emulatorArguments.Add('-config')
    $emulatorArguments.Add($setting)
}
$emulatorArguments.Add([string]$sourceByRole['media.source'].path)
$script:EmulatorArguments = @($emulatorArguments)

    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $stagedExecutable.path
    $start.WorkingDirectory = $runtimeDirectory
    $start.UseShellExecute = $false
    $start.CreateNoWindow = -not [bool]$jobObject.emulator.interactive
    # Emulator log text is neither evidence nor buffered by the transaction.
    # Inheriting the caller's handles prevents unbounded ReadToEndAsync growth.
    $start.RedirectStandardOutput = $false
    $start.RedirectStandardError = $false
    foreach ($argument in $emulatorArguments) { $start.ArgumentList.Add($argument) }
    $script:Process = [Diagnostics.Process]::new()
    $script:Process.StartInfo = $start
    if (-not $script:Process.Start()) { throw 'Could not start the staged emulator.' }
    $script:EmulatorExecutablePath = [string]$stagedExecutable.path
    $script:EmulatorStartTicks = [int64]$script:Process.StartTime.ToUniversalTime().Ticks
    $script:Ownership = Get-OwnershipSnapshot -Process $script:Process `
        -ExpectedExecutable $stagedExecutable.path
    Add-Record -Kind 'process_owned' -Details @{
        pid = $script:Ownership.pid
        executable_path = $script:Ownership.path
        start_utc_ticks = $script:Ownership.start_utc_ticks
        command_line_sha256 = $script:Ownership.command_line_sha256
    }

    if ($IntegrationTestAbortToken) {
        $candidateDeadline = [DateTime]::UtcNow.AddSeconds(10)
        while (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf) -and
               [DateTime]::UtcNow -lt $candidateDeadline) {
            if ($script:Process.HasExited) { break }
            Start-Sleep -Milliseconds 100
        }
        throw "$($script:ForcedAbortMarker) injected after exact launch ownership was established."
    }
    if ($IntegrationTestFaultToken -ceq $script:ForcedCleanupRefusalToken) {
        $script:ForceCleanupRefusal = $true
        throw "$($script:ForcedCleanupRefusalMarker) injected after exact launch ownership was established."
    }

    $emulatorWatch = [Diagnostics.Stopwatch]::StartNew()
    while (-not $script:Process.WaitForExit(250)) {
        if ($emulatorWatch.Elapsed.TotalSeconds -ge $emulatorTimeoutSeconds) {
            Stop-OwnedProcess -Reason 'emulator timeout' | Out-Null
            throw "Emulator timed out after $emulatorTimeoutSeconds seconds."
        }
    }
    $script:Process.Refresh()
    $script:NaturalExit = $true
    $script:TerminationVerified = $script:Process.HasExited
    $script:ProcessExitCode = [int]$script:Process.ExitCode
    Add-Record -Kind 'process_natural_exit' -Details @{
        exit_code = $script:ProcessExitCode
        pid = $script:Ownership.pid
    }
    if (-not $script:TerminationVerified -or $script:ProcessExitCode -ne 0) {
        throw "Emulator did not exit naturally with status zero: $($script:ProcessExitCode)"
    }

    $firstSample = Get-ArtifactSample -Path $artifactPath -RecordedPath $artifactName `
        -Process $script:Process
    Start-Sleep -Milliseconds $stabilityIntervalMs
    $secondSample = Get-ArtifactSample -Path $artifactPath -RecordedPath $artifactName `
        -Process $script:Process
    $samplesMatch = Test-ArtifactSamplesMatch -First $firstSample -Second $secondSample
    $terminalStability = [pscustomobject][ordered]@{
        interval_ms = $stabilityIntervalMs
        first = $firstSample
        second = $secondSample
        matched = $samplesMatch
    }
    if (-not $samplesMatch -or -not $secondSample.exists -or
        -not $secondSample.process_exited -or $secondSample.exit_code -ne 0) {
        throw 'The terminal artifact/process state was not stable across both samples.'
    }
    Add-Record -Kind 'terminal_state_stable' -Details @{
        artifact_size = $secondSample.size
        artifact_sha256 = $secondSample.sha256
        interval_ms = $stabilityIntervalMs
    }

    $sourceAfterStop = @(Get-CurrentSourceSet -Sources @($sourceSpecs))
    Assert-SourceSetsMatch -Before $sourceBefore -After $sourceAfterStop -Label 'Post-stop'
    Add-Record -Kind 'sources_reauthenticated_after_stop'

    $validatorArguments = [Collections.Generic.List[string]]::new()
    foreach ($argument in $validatorPrefix) { $validatorArguments.Add($argument) }
    foreach ($argument in @('--trace', $artifactPath, '--identity', $packagedIdentityPath,
            '--max-bytes', [string]$traceMaxBytes)) {
        $validatorArguments.Add($argument)
    }
    $validatorResultWithText = Invoke-OwnedTool -ExecutableIdentity $sourceByRole['validator.executable'] `
        -Arguments @($validatorArguments) -TimeoutSeconds $validatorTimeoutSeconds `
        -Role 'maple_validator' -Label 'Maple validator'
    $validatorResult = [pscustomobject][ordered]@{
        executable = $validatorResultWithText.executable
        arguments = $validatorResultWithText.arguments
        pid = $validatorResultWithText.pid
        start_utc_ticks = $validatorResultWithText.start_utc_ticks
        exit_code = $validatorResultWithText.exit_code
        stdout_size = $validatorResultWithText.stdout_size
        stdout_sha256 = $validatorResultWithText.stdout_sha256
        stdout_total_size = $validatorResultWithText.stdout_total_size
        stdout_truncated = $validatorResultWithText.stdout_truncated
        stderr_size = $validatorResultWithText.stderr_size
        stderr_sha256 = $validatorResultWithText.stderr_sha256
        stderr_total_size = $validatorResultWithText.stderr_total_size
        stderr_truncated = $validatorResultWithText.stderr_truncated
    }
    if ($validatorResult.exit_code -ne 0) {
        throw "Maple validator rejected the candidate: $($validatorResultWithText.stderr)"
    }
    $artifactAfterValidator = Get-FileIdentity -Path $artifactPath -RecordedPath $artifactName
    if ($artifactAfterValidator.size -ne $secondSample.size -or
        $artifactAfterValidator.sha256 -cne $secondSample.sha256) {
        throw 'The Maple candidate changed during independent validation.'
    }
    $artifactIdentity = [pscustomobject]$artifactAfterValidator
    Add-Record -Kind 'maple_candidate_validated' -Details @{
        validator_exit_code = 0
        artifact_sha256 = $artifactIdentity.sha256
    }

    $sourceAfter = @(Get-CurrentSourceSet -Sources @($sourceSpecs))
    Assert-SourceSetsMatch -Before $sourceBefore -After $sourceAfter -Label 'Post-validation'
    $runtimeTerminal = @(Get-RuntimeState -RuntimeFiles @($runtimeTrackedFiles))
    Remove-OwnedRuntime -Runtime $runtimeDirectory -Candidate $candidateDirectory
    Add-Record -Kind 'private_runtime_removed'

    $endedUtc = [DateTime]::UtcNow
    $transcript = [ordered]@{
        schema = 'flycast-research-capture-transcript'
        schema_version = 1
        accepted = $true
        capture_id = $captureId
        started_utc = $script:StartedUtc.ToString('o')
        ended_utc = $endedUtc.ToString('o')
        elapsed_ms = [int64]$script:Watch.ElapsedMilliseconds
        job = [pscustomobject]$packagedJobIdentity
        identity_manifest = [pscustomobject]$packagedIdentity
        source_authentication = [ordered]@{
            before = $sourceBefore
            after = $sourceAfter
            matched = $true
        }
        process = [ordered]@{
            source_executable = [ordered]@{
                path = $emulatorSource.path; size = $emulatorSource.size; sha256 = $emulatorSource.sha256
            }
            staged_executable = [ordered]@{
                path = $stagedExecutable.path; size = $stagedExecutable.size; sha256 = $stagedExecutable.sha256
            }
            pid = $script:Ownership.pid
            start_utc_ticks = $script:Ownership.start_utc_ticks
            command_line = $script:Ownership.command_line
            command_line_sha256 = $script:Ownership.command_line_sha256
            arguments = @($emulatorArguments)
            stdio_policy = 'inherited_not_evidence'
            natural_exit = $script:NaturalExit
            termination_requested = $script:TerminationRequested
            termination_verified = $script:TerminationVerified
            exit_code = $script:ProcessExitCode
        }
        terminal_stability = $terminalStability
        artifact = $artifactIdentity
        validator = $validatorResult
        capture_validator = [ordered]@{
            path = $sourceByRole['capture_validator.executable'].path
            size = $sourceByRole['capture_validator.executable'].size
            sha256 = $sourceByRole['capture_validator.executable'].sha256
        }
        runtime_state = [ordered]@{
            initial = $runtimeInitial
            terminal = $runtimeTerminal
            removed_before_publication = $true
        }
        publication = [ordered]@{
            accepted_directory = $acceptedDirectory
            candidate_directory_name = $candidateName
            same_volume = $true
            state = 'ready_for_atomic_publication'
            quarantine_directory = $null
        }
        retained_processes = @($script:RetainedProcesses)
        records = @($script:Records)
        failure = $null
        cleanup_failure = $null
        claim_limit = ('Trusted-local-operator consistency record binding the owned Flycast command, ' +
            'exact input identities, stable terminal Maple bytes, independent validators, and one ' +
            'atomic package publication. It is not a signature or hostile-writer attestation and does ' +
            'not claim physical bus timing or game semantics.')
    }
    Write-AtomicJson -Path $transcriptPath -Value $transcript
    $transcriptBeforeCaptureValidation = Get-FileIdentity -Path $transcriptPath -RecordedPath 'transcript.json'

    $captureArguments = @('--package', $candidateDirectory, '--receipt', $receiptPath)
    $captureValidatorRun = Invoke-OwnedTool `
        -ExecutableIdentity $sourceByRole['capture_validator.executable'] `
        -Arguments $captureArguments -TimeoutSeconds $validatorTimeoutSeconds `
        -Role 'capture_validator' -Label 'Capture transaction validator'
    if ($captureValidatorRun.exit_code -ne 0) {
        throw "Capture transaction validator rejected the candidate: $($captureValidatorRun.stderr)"
    }
    if (-not (Test-Path -LiteralPath $receiptPath -PathType Leaf)) {
        throw 'Capture transaction validator returned success without a validation receipt.'
    }
    $transcriptAfterCaptureValidation = Get-FileIdentity -Path $transcriptPath -RecordedPath 'transcript.json'
    $artifactAfterCaptureValidation = Get-FileIdentity -Path $artifactPath -RecordedPath $artifactName
    if ($transcriptAfterCaptureValidation.size -ne $transcriptBeforeCaptureValidation.size -or
        $transcriptAfterCaptureValidation.sha256 -cne $transcriptBeforeCaptureValidation.sha256 -or
        $artifactAfterCaptureValidation.size -ne $artifactIdentity.size -or
        $artifactAfterCaptureValidation.sha256 -cne $artifactIdentity.sha256) {
        throw 'The transcript or artifact changed during capture validation.'
    }
    $sourceAfterCaptureValidation = @(Get-CurrentSourceSet -Sources @($sourceSpecs))
    Assert-SourceSetsMatch -Before $sourceBefore -After $sourceAfterCaptureValidation `
        -Label 'Post-capture-validation'
    Add-Record -Kind 'capture_transaction_validated' -Details @{
        validator_exit_code = 0
        receipt = 'capture-validation.json'
    }

    if (Test-Path -LiteralPath $acceptedDirectory) {
        throw 'Accepted output appeared before atomic publication.'
    }
    Assert-NoReparseComponents -Path $acceptedParent -Label 'Accepted output parent before publication'
    Assert-NoReparseComponents -Path $candidateDirectory -Label 'Private candidate before publication'
    if (-not [string]::Equals([IO.Path]::GetPathRoot($candidateDirectory),
            [IO.Path]::GetPathRoot($acceptedDirectory), [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Candidate and accepted directory ceased to share a volume before publication.'
    }
    [IO.Directory]::Move($candidateDirectory, $acceptedDirectory)
    $currentPackageDirectory = $acceptedDirectory
    foreach ($requiredName in @('job.json', 'identity.json', $artifactName,
            'transcript.json', 'capture-validation.json')) {
        if (-not (Test-Path -LiteralPath (Join-Path $acceptedDirectory $requiredName) -PathType Leaf)) {
            throw "Published package is missing $requiredName."
        }
    }
    $accepted = $true
}
catch {
    $failure = $_.Exception.Message
    Add-Record -Kind 'capture_failure' -Details @{ error = $failure }
}
finally {
    if (-not $accepted) {
        Stop-OwnedProcess -Reason 'capture failure cleanup' | Out-Null
        if ($script:CleanupFailure) {
            Add-Record -Kind 'cleanup_failure' -Details @{ error = $script:CleanupFailure }
        }
        if (Test-Path -LiteralPath $currentPackageDirectory -PathType Container) {
            $candidateTranscriptPath = Join-Path $currentPackageDirectory 'transcript.json'
            $candidateReceiptPath = Join-Path $currentPackageDirectory 'capture-validation.json'
            if (Test-Path -LiteralPath $candidateReceiptPath -PathType Leaf) {
                $invalidatedReceipt = Join-Path $currentPackageDirectory 'capture-validation.invalidated.json'
                if (-not (Test-Path -LiteralPath $invalidatedReceipt)) {
                    [IO.File]::Move($candidateReceiptPath, $invalidatedReceipt)
                }
            }
            try { $sourceAfter = @(Get-CurrentSourceSet -Sources @($sourceSpecs)) }
            catch { $sourceAfter = @() }
            $sourcesMatched = $false
            if ($sourceAfter.Count -eq $sourceBefore.Count) {
                try {
                    Assert-SourceSetsMatch -Before $sourceBefore -After $sourceAfter -Label 'Rejected capture'
                    $sourcesMatched = $true
                }
                catch { $sourcesMatched = $false }
            }
            if ($null -eq $artifactIdentity -and (Test-Path -LiteralPath $artifactPath -PathType Leaf)) {
                try { $artifactIdentity = [pscustomobject](Get-FileIdentity -Path $artifactPath -RecordedPath $artifactName) }
                catch { $artifactIdentity = $null }
            }
            if ($runtimeTerminal.Count -eq 0 -and (Test-Path -LiteralPath $runtimeDirectory -PathType Container)) {
                try { $runtimeTerminal = @(Get-RuntimeState -RuntimeFiles @($runtimeTrackedFiles)) }
                catch { $runtimeTerminal = @() }
            }
            $processRecord = $null
            if ($null -ne $script:Ownership) {
                $processRecord = [ordered]@{
                    source_executable = [ordered]@{
                        path = $emulatorSource.path; size = $emulatorSource.size; sha256 = $emulatorSource.sha256
                    }
                    staged_executable = [ordered]@{
                        path = $stagedExecutable.path; size = $stagedExecutable.size; sha256 = $stagedExecutable.sha256
                    }
                    pid = $script:Ownership.pid
                    start_utc_ticks = $script:Ownership.start_utc_ticks
                    command_line = $script:Ownership.command_line
                    command_line_sha256 = $script:Ownership.command_line_sha256
                    arguments = @($emulatorArguments)
                    stdio_policy = 'inherited_not_evidence'
                    natural_exit = $script:NaturalExit
                    termination_requested = $script:TerminationRequested
                    termination_verified = $script:TerminationVerified
                    exit_code = $script:ProcessExitCode
                }
            }
            $rejectedTranscript = [ordered]@{
                schema = 'flycast-research-capture-transcript'
                schema_version = 1
                accepted = $false
                capture_id = $captureId
                started_utc = $script:StartedUtc.ToString('o')
                ended_utc = [DateTime]::UtcNow.ToString('o')
                elapsed_ms = [int64]$script:Watch.ElapsedMilliseconds
                job = [pscustomobject]$packagedJobIdentity
                identity_manifest = [pscustomobject]$packagedIdentity
                source_authentication = [ordered]@{
                    before = $sourceBefore; after = $sourceAfter; matched = $sourcesMatched
                }
                process = $processRecord
                terminal_stability = $terminalStability
                artifact = $artifactIdentity
                validator = $validatorResult
                capture_validator = [ordered]@{
                    path = $sourceByRole['capture_validator.executable'].path
                    size = $sourceByRole['capture_validator.executable'].size
                    sha256 = $sourceByRole['capture_validator.executable'].sha256
                }
                runtime_state = [ordered]@{
                    initial = $runtimeInitial
                    terminal = $runtimeTerminal
                    removed_before_publication = $false
                }
                publication = [ordered]@{
                    accepted_directory = $acceptedDirectory
                    candidate_directory_name = $candidateName
                    same_volume = $true
                    state = 'candidate_retained'
                    quarantine_directory = $null
                }
                retained_processes = @($script:RetainedProcesses)
                records = @($script:Records)
                failure = $failure
                cleanup_failure = $script:CleanupFailure
                claim_limit = 'Rejected diagnostic capture; no artifact in this package is accepted evidence.'
            }
            try {
                Write-AtomicJson -Path $candidateTranscriptPath -Value $rejectedTranscript -ReplaceOwned
                if (-not $script:CleanupFailure) {
                    [IO.Directory]::CreateDirectory($quarantineRoot) | Out-Null
                    Assert-NoReparseComponents -Path $quarantineRoot -Label 'Quarantine root'
                    if ((Test-Path -LiteralPath $acceptedDirectory) -and
                        -not (Test-SamePath -Left $currentPackageDirectory -Right $acceptedDirectory)) {
                        throw 'Rejected capture left an unrelated accepted directory.'
                    }
                    $quarantineMetadata = [ordered]@{
                        schema = 'flycast-research-quarantine'
                        schema_version = 1
                        capture_id = $captureId
                        quarantined_utc = [DateTime]::UtcNow.ToString('o')
                        source_directory = $currentPackageDirectory
                        quarantine_directory = $quarantineDirectory
                        failure = $failure
                        cleanup_failure = $script:CleanupFailure
                        accepted_output_absent = (-not (Test-Path -LiteralPath $acceptedDirectory)) -or
                            (Test-SamePath -Left $currentPackageDirectory -Right $acceptedDirectory)
                    }
                    Write-AtomicJson -Path (Join-Path $currentPackageDirectory 'quarantine.json') `
                        -Value $quarantineMetadata -ReplaceOwned
                    [IO.Directory]::Move($currentPackageDirectory, $quarantineDirectory)
                    $currentPackageDirectory = $quarantineDirectory
                    $rejectedTranscript.publication.state = 'quarantined'
                    $rejectedTranscript.publication.quarantine_directory = $quarantineDirectory
                    Write-AtomicJson -Path (Join-Path $quarantineDirectory 'transcript.json') `
                        -Value $rejectedTranscript -ReplaceOwned
                }
            }
            catch {
                $quarantineFailure = $_.Exception.Message
                if ($script:CleanupFailure) { $script:CleanupFailure += "; $quarantineFailure" }
                else { $script:CleanupFailure = $quarantineFailure }
            }
        }
    }
    if ($null -ne $script:Process) {
        $script:Process.Dispose()
    }
}

if (-not $accepted) {
    $location = if (Test-Path -LiteralPath $quarantineDirectory -PathType Container) {
        $quarantineDirectory
    } else { $currentPackageDirectory }
    throw "Flycast research capture rejected: $failure; retained at: $location"
}

[pscustomobject][ordered]@{
    Accepted = $true
    CaptureId = $captureId
    Directory = $acceptedDirectory
    Artifact = Join-Path $acceptedDirectory $artifactName
    Transcript = Join-Path $acceptedDirectory 'transcript.json'
    Validation = Join-Path $acceptedDirectory 'capture-validation.json'
}

#Requires -Version 7.0

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [string]$ValidatorDirectory = '',
    [string]$Output = '',
    [ValidateRange(1, 3600)][int]$ValidatorTimeoutSeconds = 300
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8 = [Text.UTF8Encoding]::new($false)
$comparison = [StringComparison]::OrdinalIgnoreCase
$receiptNames = @(
    'package-validation.json',
    'capture-validation.json',
    'capture-package-validation.json'
)

function Test-Within([string]$Parent, [string]$Child) {
    $parentPath = [IO.Path]::GetFullPath($Parent).TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    $childPath = [IO.Path]::GetFullPath($Child)
    if ([string]::Equals($parentPath, $childPath, $comparison)) { return $true }
    $prefix = $parentPath + [IO.Path]::DirectorySeparatorChar
    return $childPath.StartsWith($prefix, $comparison)
}

function Resolve-RegularFile([string]$Path, [string]$Label) {
    $resolved = (Resolve-Path -LiteralPath ([IO.Path]::GetFullPath($Path))).Path
    $item = Get-Item -LiteralPath $resolved -Force
    if ($item.PSIsContainer -or
            (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "$Label must be a non-linked regular file."
    }
    return $resolved
}

function Get-FileIdentity([string]$Path) {
    $resolved = Resolve-RegularFile $Path 'Audited file'
    $item = Get-Item -LiteralPath $resolved -Force
    return [ordered]@{
        path = $resolved
        size = [int64]$item.Length
        sha256 = (Get-FileHash -LiteralPath $resolved -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Get-TextSha256([string]$Value) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $utf8.GetBytes($Value)
        return ([BitConverter]::ToString($algorithm.ComputeHash($bytes))).Replace(
            '-', '').ToLowerInvariant()
    }
    finally { $algorithm.Dispose() }
}

function Get-RelativeSlashPath([string]$Parent, [string]$Child) {
    return [IO.Path]::GetRelativePath($Parent, $Child).Replace('\', '/')
}

function Get-PackageInventory([string]$Package) {
    $files = [Collections.Generic.List[object]]::new()
    $stack = [Collections.Generic.Stack[string]]::new()
    $stack.Push($Package)
    while ($stack.Count -ne 0) {
        $directory = $stack.Pop()
        foreach ($item in @(Get-ChildItem -LiteralPath $directory -Force | Sort-Object Name)) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Package contains a reparse point: $(Get-RelativeSlashPath $Package $item.FullName)"
            }
            if ($item.PSIsContainer) {
                $stack.Push($item.FullName)
            }
            else {
                $files.Add([pscustomobject][ordered]@{
                    path = Get-RelativeSlashPath $Package $item.FullName
                    full_path = $item.FullName
                    size = [int64]$item.Length
                })
            }
        }
    }
    return @($files | Sort-Object path)
}

function Read-Receipt([string]$Path) {
    $item = Get-Item -LiteralPath (Resolve-RegularFile $Path 'Validation receipt') -Force
    if ($item.Length -le 0 -or $item.Length -gt 16MB) {
        throw 'Validation receipt size is outside (0, 16 MiB].'
    }
    return Get-Content -LiteralPath $item.FullName -Raw -Encoding UTF8 |
        ConvertFrom-Json -Depth 64
}

function Assert-ReceiptInventory([string]$Package, [string]$ReceiptName,
        [object]$Receipt) {
    $entryProperty = $Receipt.PSObject.Properties['entries']
    if ($null -eq $entryProperty) { return }
    $expected = [Collections.Generic.Dictionary[string, object]]::new(
        [StringComparer]::Ordinal)
    foreach ($entry in @($entryProperty.Value)) {
        if ($null -eq $entry) { throw 'Receipt contains a null inventory entry.' }
        $nameProperty = $entry.PSObject.Properties['name']
        $pathProperty = $entry.PSObject.Properties['path']
        if (($null -eq $nameProperty) -eq ($null -eq $pathProperty)) {
            throw 'Each receipt entry must contain exactly one of name or path.'
        }
        $declared = if ($null -ne $nameProperty) {
            [string]$nameProperty.Value
        }
        else {
            [string]$pathProperty.Value
        }
        if ([string]::IsNullOrWhiteSpace($declared) -or
                [IO.Path]::IsPathRooted($declared)) {
            throw 'Receipt contains an empty or rooted entry path.'
        }
        $joined = [IO.Path]::GetFullPath((Join-Path $Package $declared))
        if (-not (Test-Within $Package $joined)) {
            throw "Receipt entry escapes the package: $declared"
        }
        $normalized = Get-RelativeSlashPath $Package $joined
        if ($normalized -ceq $ReceiptName.Replace('\', '/')) {
            throw 'Receipt must not inventory itself.'
        }
        if ($expected.ContainsKey($normalized)) {
            throw "Receipt contains a duplicate entry: $normalized"
        }
        $sizeProperty = $entry.PSObject.Properties['size']
        $hashProperty = $entry.PSObject.Properties['sha256']
        if ($null -eq $sizeProperty -or $null -eq $hashProperty -or
                [int64]$sizeProperty.Value -lt 0 -or
                [string]$hashProperty.Value -cnotmatch '^[0-9a-f]{64}$') {
            throw "Receipt entry metadata is invalid: $normalized"
        }
        $expected.Add($normalized, $entry)
    }

    $actual = @(Get-PackageInventory $Package | Where-Object path -cne $ReceiptName)
    if ($actual.Count -ne $expected.Count) {
        throw "Receipt inventory count $($expected.Count) differs from package file count $($actual.Count)."
    }
    foreach ($file in $actual) {
        if (-not $expected.ContainsKey([string]$file.path)) {
            throw "Receipt does not inventory package file: $($file.path)"
        }
        $entry = $expected[[string]$file.path]
        if ([int64]$entry.size -ne [int64]$file.size) {
            throw "Receipt size differs for package file: $($file.path)"
        }
        $hash = (Get-FileHash -LiteralPath $file.full_path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -cne [string]$entry.sha256) {
            throw "Receipt SHA-256 differs for package file: $($file.path)"
        }
    }
}

function Get-PackageContentSha256([string]$Package, [string]$ReceiptName,
        [object]$Receipt) {
    $entries = [Collections.Generic.List[object]]::new()
    $entryProperty = $Receipt.PSObject.Properties['entries']
    if ($null -ne $entryProperty) {
        foreach ($entry in @($entryProperty.Value)) {
            $nameProperty = $entry.PSObject.Properties['name']
            $pathProperty = $entry.PSObject.Properties['path']
            $declared = if ($null -ne $nameProperty) {
                [string]$nameProperty.Value
            }
            else {
                [string]$pathProperty.Value
            }
            $normalized = Get-RelativeSlashPath $Package `
                ([IO.Path]::GetFullPath((Join-Path $Package $declared)))
            $entries.Add([pscustomobject][ordered]@{
                path = $normalized
                size = [int64]$entry.size
                sha256 = [string]$entry.sha256
            })
        }
    }
    else {
        foreach ($file in @(Get-PackageInventory $Package |
                Where-Object path -cne $ReceiptName)) {
            $entries.Add([pscustomobject][ordered]@{
                path = [string]$file.path
                size = [int64]$file.size
                sha256 = (Get-FileHash -LiteralPath $file.full_path -Algorithm SHA256).Hash.ToLowerInvariant()
            })
        }
    }
    $canonical = @($entries | Sort-Object path) |
        ConvertTo-Json -Compress -Depth 4
    return Get-TextSha256 $canonical
}

function Get-TrustedValidator([string]$Name) {
    $path = Resolve-RegularFile (Join-Path $script:ValidatorRoot $Name) "Trusted validator $Name"
    return Get-FileIdentity $path
}

function Invoke-TrustedProcess([string]$Executable, [string[]]$Arguments,
        [string]$Label) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        if (-not $process.Start()) { throw "$Label did not start." }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($ValidatorTimeoutSeconds * 1000)) {
            try { $process.Kill($true) } catch { }
            try { $process.WaitForExit() } catch { }
            throw "$Label timed out."
        }
        $stdout = $stdoutTask.GetAwaiter().GetResult().Trim()
        $stderr = $stderrTask.GetAwaiter().GetResult().Trim()
        if ($process.ExitCode -ne 0) {
            $detail = ($stdout + ' ' + $stderr).Trim()
            if ($detail.Length -gt 8192) { $detail = $detail.Substring(0, 8192) }
            throw "$Label rejected the package: $detail"
        }
        $output = ($stdout + "`n" + $stderr).Trim()
        if ($output.Length -gt 8192) { $output = $output.Substring(0, 8192) }
        return $output
    }
    finally {
        $process.Dispose()
    }
}

function Invoke-NativePackageValidator([string]$Package, [string]$Name,
        [string]$Label) {
    $validator = Get-TrustedValidator $Name
    $output = Invoke-TrustedProcess $validator.path @('--package', $Package) $Label
    return [pscustomobject][ordered]@{
        output = $output
        validators = @($validator)
    }
}

function Invoke-AicaPackageValidator([string]$Package) {
    $native = Get-TrustedValidator 'flycast-research-validate-aica.exe'
    $scriptPath = Resolve-RegularFile `
        (Join-Path $PSScriptRoot 'flycast-validate-aica-package-v1.ps1') `
        'Trusted AICA package validator'
    $scriptIdentity = Get-FileIdentity $scriptPath
    $shell = Resolve-RegularFile (Join-Path $PSHOME 'pwsh.exe') 'PowerShell runtime'
    $output = Invoke-TrustedProcess $shell @(
        '-NoLogo', '-NoProfile', '-NonInteractive', '-File', $scriptPath,
        '-Package', $Package,
        '-TrustedArtifactValidator', $native.path,
        '-ValidatorTimeoutSeconds', [string]$ValidatorTimeoutSeconds
    ) 'Trusted AICA package validator'
    return [pscustomobject][ordered]@{
        output = $output
        validators = @($scriptIdentity, $native)
    }
}

function Invoke-GdromPackageValidator([string]$Package) {
    $native = Get-TrustedValidator 'flycast-research-validate-gdrom.exe'
    $scriptPath = Resolve-RegularFile `
        (Join-Path $PSScriptRoot 'flycast-validate-gdrom-package-v1.ps1') `
        'Trusted GD-ROM package validator'
    $scriptIdentity = Get-FileIdentity $scriptPath
    $shell = Resolve-RegularFile (Join-Path $PSHOME 'pwsh.exe') 'PowerShell runtime'
    $output = Invoke-TrustedProcess $shell @(
        '-NoLogo', '-NoProfile', '-NonInteractive', '-File', $scriptPath,
        '-Package', $Package,
        '-TrustedArtifactValidator', $native.path,
        '-ValidatorTimeoutSeconds', [string]$ValidatorTimeoutSeconds
    ) 'Trusted GD-ROM package validator'
    return [pscustomobject][ordered]@{
        output = $output
        validators = @($scriptIdentity, $native)
    }
}

function Invoke-GdromHardwarePackageValidator([string]$Package) {
    $native = Get-TrustedValidator 'flycast-research-validate-gdrom.exe'
    $scriptPath = Resolve-RegularFile `
        (Join-Path $PSScriptRoot 'flycast-validate-gdrom-hardware-package-v2.ps1') `
        'Trusted GD-ROM hardware package validator'
    $scriptIdentity = Get-FileIdentity $scriptPath
    $shell = Resolve-RegularFile (Join-Path $PSHOME 'pwsh.exe') 'PowerShell runtime'
    $output = Invoke-TrustedProcess $shell @(
        '-NoLogo', '-NoProfile', '-NonInteractive', '-File', $scriptPath,
        '-Package', $Package,
        '-TrustedArtifactValidator', $native.path,
        '-ValidatorTimeoutSeconds', [string]$ValidatorTimeoutSeconds
    ) 'Trusted GD-ROM hardware package validator'
    return [pscustomobject][ordered]@{
        output = $output
        validators = @($scriptIdentity, $native)
    }
}

function Invoke-PresentationPackageValidator([string]$Package) {
    $ta = Get-TrustedValidator 'flycast-research-validate-pvr-ta.exe'
    $presentation = Get-TrustedValidator 'flycast-research-validate-pvr-presentation.exe'
    $taOutput = Invoke-TrustedProcess $ta.path @(
        '--artifact', (Join-Path $Package 'pvr-ta.fcpvr'),
        '--identity', (Join-Path $Package 'identity.json'),
        '--replay', (Join-Path $Package 'maple-replay.fcmt'),
        '--manifest', (Join-Path $Package 'pvr-ta-manifest.json')
    ) 'Trusted PowerVR TA artifact validator'
    $presentationOutput = Invoke-TrustedProcess $presentation.path @(
        '--artifact', (Join-Path $Package 'pvr-presentation.fcpvrp'),
        '--identity', (Join-Path $Package 'identity.json'),
        '--replay', (Join-Path $Package 'maple-replay.fcmt')
    ) 'Trusted PowerVR presentation validator'
    return [pscustomobject][ordered]@{
        output = ($taOutput + "`n" + $presentationOutput).Trim()
        validators = @($ta, $presentation)
    }
}

function Get-PackageType([string]$Schema) {
    switch -CaseSensitive ($Schema) {
        'flycast-research-aica-package-validation' { return 'aica-v1' }
        'flycast-research-gdrom-package-validation' { return 'gdrom-v1' }
        'flycast-research-gdrom-hardware-package-validation' { return 'gdrom-hardware-v2' }
        'flycast-research-pvr-draw-package-validation' { return 'pvr-draw-v1' }
        'flycast-research-pvr-presentation-package-validation' { return 'pvr-presentation-v1' }
        'flycast-research-pvr-ta-package-validation' { return 'pvr-ta-v1' }
        'flycast-research-sh4-equivalence-package-validation' { return 'sh4-equivalence-v1' }
        'flycast-research-sh4-ghidra-package-validation' { return 'sh4-ghidra-v1' }
        'flycast-research-capture-validation' { return 'capture-v1' }
        'flycast-research-capture-package-validation' { return 'capture-v2' }
        default { return 'unknown' }
    }
}

function Get-ExpectedReceiptName([string]$Type) {
    switch ($Type) {
        'capture-v1' { return 'capture-validation.json' }
        'capture-v2' { return 'capture-package-validation.json' }
        default { return 'package-validation.json' }
    }
}

function Assert-ReceiptAcceptance([object]$Receipt, [string]$Type) {
    $version = $Receipt.PSObject.Properties['schema_version']
    if ($null -eq $version) { throw 'Receipt schema_version is missing.' }
    if ($Type -eq 'capture-v2') {
        if ([int]$version.Value -ne 2) { throw 'Capture package v2 receipt version is not 2.' }
        return
    }
	if ($Type -eq 'gdrom-hardware-v2') {
		if ([int]$version.Value -ne 2) {
			throw 'GD-ROM hardware package v2 receipt version is not 2.'
		}
	}
	elseif ([int]$version.Value -ne 1) {
		throw 'Receipt schema_version is not 1.'
	}
    if ($Type -eq 'capture-v1') {
        $accepted = $Receipt.PSObject.Properties['accepted']
        if ($null -eq $accepted -or -not [bool]$accepted.Value) {
            throw 'Capture v1 receipt is not accepted.'
        }
        return
    }
    $status = $Receipt.PSObject.Properties['status']
    if ($null -eq $status -or [string]$status.Value -cne 'accepted') {
        throw 'Package receipt status is not accepted.'
    }
}

function Get-PackageId([object]$Receipt, [string]$Type) {
    if ($Type -eq 'capture-v1') { return $null }
    $property = $Receipt.PSObject.Properties['package_id']
    if ($null -eq $property -or [string]$property.Value -cnotmatch
            '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$') {
        throw 'Package receipt has no canonical lowercase UUID package_id.'
    }
    return [string]$property.Value
}

function Get-Sh4JobProvenance([string]$Package, [string]$Type) {
    if ($Type -cne 'sh4-equivalence-v1') {
        return [pscustomobject][ordered]@{ job_id = $null; source_job_sha256 = $null }
    }
    $path = Resolve-RegularFile (Join-Path $Package 'source-job.json') `
        'SH-4 equivalence source job'
    $item = Get-Item -LiteralPath $path -Force
    if ($item.Length -le 0 -or $item.Length -gt 16MB) {
        throw 'SH-4 equivalence source job size is outside (0, 16 MiB].'
    }
    $job = Get-Content -LiteralPath $path -Raw -Encoding UTF8 |
        ConvertFrom-Json -Depth 64
    if ([string]$job.schema -cne 'flycast-research-sh4-equivalence-job' -or
            [int]$job.schema_version -ne 1 -or
            [string]$job.job_id -cnotmatch
                '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$') {
        throw 'SH-4 equivalence source job identity is invalid.'
    }
    return [pscustomobject][ordered]@{
        job_id = [string]$job.job_id
        source_job_sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

function Invoke-PackageValidation([string]$Package, [string]$Type) {
    switch ($Type) {
        'aica-v1' { return Invoke-AicaPackageValidator $Package }
        'gdrom-v1' { return Invoke-GdromPackageValidator $Package }
		'gdrom-hardware-v2' { return Invoke-GdromHardwarePackageValidator $Package }
        'pvr-draw-v1' {
            return Invoke-NativePackageValidator $Package `
                'flycast-research-validate-pvr-draw-package.exe' `
                'Trusted PowerVR draw package validator'
        }
        'pvr-presentation-v1' { return Invoke-PresentationPackageValidator $Package }
        'pvr-ta-v1' {
            return Invoke-NativePackageValidator $Package `
                'flycast-research-validate-pvr-ta-package.exe' `
                'Trusted PowerVR TA package validator'
        }
        'sh4-equivalence-v1' {
            return Invoke-NativePackageValidator $Package `
                'flycast-research-validate-sh4-equivalence-package.exe' `
                'Trusted SH-4 equivalence package validator'
        }
        'sh4-ghidra-v1' {
            return Invoke-NativePackageValidator $Package `
                'flycast-research-validate-sh4-ghidra-package.exe' `
                'Trusted SH-4/Ghidra package validator'
        }
        'capture-v1' {
            return Invoke-NativePackageValidator $Package `
                'flycast-research-validate-capture.exe' `
                'Trusted capture v1 package validator'
        }
        'capture-v2' {
            return Invoke-NativePackageValidator $Package `
                'flycast-research-validate-capture-package-v2.exe' `
                'Trusted capture package v2 validator'
        }
        default { throw "Unsupported accepted evidence type: $Type" }
    }
}

function Add-Candidate([Collections.Generic.Dictionary[string, string]]$Candidates,
        [string]$Path) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd(
        [IO.Path]::DirectorySeparatorChar, [IO.Path]::AltDirectorySeparatorChar)
    if (-not $Candidates.ContainsKey($full)) { $Candidates.Add($full, $full) }
}

function Test-ExcludedResearchDirectory([string]$Name) {
    return $Name.StartsWith('.flycast-research-', [StringComparison]::Ordinal) -or
        $Name -match '(?i)quarantine'
}

$rootPath = (Resolve-Path -LiteralPath ([IO.Path]::GetFullPath($Root))).Path
$rootItem = Get-Item -LiteralPath $rootPath -Force
if (-not $rootItem.PSIsContainer -or
        (($rootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
    throw 'Audit root must be a non-linked directory.'
}
if ([string]::IsNullOrWhiteSpace($ValidatorDirectory)) {
    $repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
    $ValidatorDirectory = Join-Path $repository 'build-research-tests\Release'
}
$script:ValidatorRoot = (Resolve-Path -LiteralPath `
    ([IO.Path]::GetFullPath($ValidatorDirectory))).Path
$validatorRootItem = Get-Item -LiteralPath $script:ValidatorRoot -Force
if (-not $validatorRootItem.PSIsContainer -or
        (($validatorRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
    throw 'Validator directory must be a non-linked directory.'
}
$auditorIdentity = Get-FileIdentity $PSCommandPath

$candidates = [Collections.Generic.Dictionary[string, string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
if ($rootItem.Name.StartsWith('accepted-', [StringComparison]::OrdinalIgnoreCase)) {
    Add-Candidate $candidates $rootPath
}
foreach ($receiptName in $receiptNames) {
    if (Test-Path -LiteralPath (Join-Path $rootPath $receiptName)) {
        Add-Candidate $candidates $rootPath
    }
}

$scan = [Collections.Generic.Stack[string]]::new()
$scan.Push($rootPath)
while ($scan.Count -ne 0) {
    $directory = $scan.Pop()
    foreach ($item in @(Get-ChildItem -LiteralPath $directory -Force | Sort-Object Name)) {
        $reparse = (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
        if ($item.PSIsContainer) {
            if ($item.Name.StartsWith('accepted-', [StringComparison]::OrdinalIgnoreCase)) {
                Add-Candidate $candidates $item.FullName
            }
            if (-not $reparse -and -not (Test-ExcludedResearchDirectory $item.Name)) {
                $scan.Push($item.FullName)
            }
        }
        elseif ($receiptNames -ccontains $item.Name) {
            Add-Candidate $candidates $item.DirectoryName
        }
    }
}

$results = [Collections.Generic.List[object]]::new()
foreach ($package in @($candidates.Keys | Sort-Object)) {
    $relative = Get-RelativeSlashPath $rootPath $package
    if ($relative -ceq '.') { $relative = '.' }
    $type = 'unknown'
    $schema = $null
    $packageId = $null
    $receiptHash = $null
    $contentHash = $null
    $sh4JobId = $null
    $sh4SourceJobHash = $null
    $validators = @()
    $validatorOutput = ''
    $status = 'rejected'
    $diagnostic = ''
    try {
        if (-not (Test-Within $rootPath $package)) {
            throw 'Package resolves outside the audit root.'
        }
        if (Test-Within $package $script:ValidatorRoot) {
            throw 'Trusted validator directory must not be inside an audited package.'
        }
        $packageItem = Get-Item -LiteralPath $package -Force
        if (-not $packageItem.PSIsContainer -or
                (($packageItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
            throw 'Accepted package must be a non-linked directory.'
        }
        $directReceipts = @($receiptNames | Where-Object {
            Test-Path -LiteralPath (Join-Path $package $_) -PathType Leaf
        })
        if ($directReceipts.Count -ne 1) {
            throw "Accepted package must contain exactly one recognized receipt; found $($directReceipts.Count)."
        }
        $receiptName = [string]$directReceipts[0]
        $receiptPath = Join-Path $package $receiptName
        $receipt = Read-Receipt $receiptPath
        $schemaProperty = $receipt.PSObject.Properties['schema']
        if ($null -eq $schemaProperty) { throw 'Validation receipt schema is missing.' }
        $schema = [string]$schemaProperty.Value
        $type = Get-PackageType $schema
        if ($type -ceq 'unknown') { throw "Unknown accepted evidence schema: $schema" }
        $expectedReceipt = Get-ExpectedReceiptName $type
        if ($receiptName -cne $expectedReceipt) {
            throw "$schema must use receipt filename $expectedReceipt."
        }
        Assert-ReceiptAcceptance $receipt $type
        $packageId = Get-PackageId $receipt $type
        $receiptHash = (Get-FileHash -LiteralPath $receiptPath -Algorithm SHA256).Hash.ToLowerInvariant()
        [void](Get-PackageInventory $package)
        Assert-ReceiptInventory $package $receiptName $receipt
        $contentHash = Get-PackageContentSha256 $package $receiptName $receipt
        $sh4Job = Get-Sh4JobProvenance $package $type
        $sh4JobId = $sh4Job.job_id
        $sh4SourceJobHash = $sh4Job.source_job_sha256
        $validation = Invoke-PackageValidation $package $type
        $validators = @($validation.validators)
        $validatorOutput = [string]$validation.output
        $status = 'accepted'
    }
    catch {
        $diagnostic = $_.Exception.Message
        if ($diagnostic.Length -gt 8192) { $diagnostic = $diagnostic.Substring(0, 8192) }
    }
    $results.Add([pscustomobject][ordered]@{
        path = $relative
        type = $type
        schema = $schema
        package_id = $packageId
        status = $status
        receipt_sha256 = $receiptHash
        content_sha256 = $contentHash
        sh4_job_id = $sh4JobId
        sh4_source_job_sha256 = $sh4SourceJobHash
        validators = @($validators)
        validator_output = $validatorOutput
        diagnostic = $diagnostic
    })
}

$byId = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
foreach ($result in $results) {
    if ($null -eq $result.package_id) { continue }
    $id = [string]$result.package_id
    if (-not $byId.ContainsKey($id)) {
        $byId.Add($id, [Collections.Generic.List[object]]::new())
    }
    $byId[$id].Add($result)
}
foreach ($entry in $byId.GetEnumerator()) {
    if ($entry.Value.Count -le 1) { continue }
    $fingerprints = @($entry.Value | ForEach-Object content_sha256 |
        Where-Object { $null -ne $_ } | Sort-Object -Unique)
    $verifiedCount = @($entry.Value | Where-Object { $null -ne $_.content_sha256 }).Count
    $duplicate = if ($verifiedCount -ne $entry.Value.Count) {
        "Duplicate package_id $($entry.Key) appears in $($entry.Value.Count) packages, but only $verifiedCount had verified content fingerprints."
    }
    elseif ($fingerprints.Count -eq 1) {
        "Duplicate package_id $($entry.Key) identifies identical locked package content in $($entry.Value.Count) package directories; mirrors cannot count independently."
    }
    else {
        "Package identity collision: package_id $($entry.Key) identifies $($fingerprints.Count) different content fingerprints across $($entry.Value.Count) packages."
    }
    foreach ($result in $entry.Value) {
        $result.status = 'rejected'
        $result.diagnostic = if ([string]::IsNullOrEmpty($result.diagnostic)) {
            $duplicate
        }
        else {
            $result.diagnostic + ' ' + $duplicate
        }
    }
}

$bySh4JobId = [Collections.Generic.Dictionary[string, object]]::new(
    [StringComparer]::Ordinal)
foreach ($result in $results) {
    if ($null -eq $result.sh4_job_id) { continue }
    $id = [string]$result.sh4_job_id
    if (-not $bySh4JobId.ContainsKey($id)) {
        $bySh4JobId.Add($id, [Collections.Generic.List[object]]::new())
    }
    $bySh4JobId[$id].Add($result)
}
foreach ($entry in $bySh4JobId.GetEnumerator()) {
    if ($entry.Value.Count -le 1) { continue }
    $fingerprints = @($entry.Value | ForEach-Object sh4_source_job_sha256 |
        Where-Object { $null -ne $_ } | Sort-Object -Unique)
    $verifiedCount = @($entry.Value | Where-Object {
        $null -ne $_.sh4_source_job_sha256
    }).Count
    $duplicate = if ($verifiedCount -ne $entry.Value.Count) {
        "Duplicate SH-4 job_id $($entry.Key) appears in $($entry.Value.Count) packages, but only $verifiedCount had verified source-job fingerprints."
    }
    elseif ($fingerprints.Count -eq 1) {
        "Duplicate SH-4 job_id $($entry.Key) identifies identical source-job content in $($entry.Value.Count) packages; aliases cannot count independently."
    }
    else {
        "SH-4 job identity collision: job_id $($entry.Key) identifies $($fingerprints.Count) different source-job fingerprints across $($entry.Value.Count) packages."
    }
    foreach ($result in $entry.Value) {
        $result.status = 'rejected'
        $result.diagnostic = if ([string]::IsNullOrEmpty($result.diagnostic)) {
            $duplicate
        }
        else {
            $result.diagnostic + ' ' + $duplicate
        }
    }
}

$acceptedCount = @($results | Where-Object status -ceq 'accepted').Count
$rejectedCount = $results.Count - $acceptedCount
$overall = if ($results.Count -gt 0 -and $rejectedCount -eq 0) {
    'accepted'
}
else {
    'rejected'
}
$report = [ordered]@{
    schema = 'flycast-research-evidence-audit-report'
    schema_version = 1
    root = $rootPath
    auditor = $auditorIdentity
    validator_directory = $script:ValidatorRoot
    status = $overall
    package_count = $results.Count
    accepted_count = $acceptedCount
    rejected_count = $rejectedCount
    packages = @($results | Sort-Object path)
}
$json = ($report | ConvertTo-Json -Depth 16) + "`n"
if (-not [string]::IsNullOrWhiteSpace($Output)) {
    $outputPath = [IO.Path]::GetFullPath($Output)
    $outputParent = Split-Path -Parent $outputPath
    if (-not (Test-Path -LiteralPath $outputParent -PathType Container)) {
        throw 'Audit report parent directory does not exist.'
    }
    if (Test-Path -LiteralPath $outputPath) {
        throw 'Audit report output already exists.'
    }
    $temporary = $outputPath + ".candidate-$([Guid]::NewGuid().ToString('D'))"
    try {
        [IO.File]::WriteAllText($temporary, $json, $utf8)
        [IO.File]::Move($temporary, $outputPath)
    }
    catch {
        if (Test-Path -LiteralPath $temporary) {
            Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue
        }
        throw
    }
}
else {
    Write-Output $json
}

if ($overall -cne 'accepted') { exit 1 }

#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$CaptureDirectory,
    [Parameter(Mandatory=$true)][string]$StaticAnalysis,
    [Parameter(Mandatory=$true)][string]$AcceptedDirectory,
    [ValidateRange(5,300)][int]$ValidatorTimeoutSeconds = 60,
    [ValidateSet('','after-staging-v1','after-validation-v1')]
    [string]$IntegrationTestAbortToken = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8 = [Text.UTF8Encoding]::new($false)

function Resolve-File([string]$Path,[string]$Name) {
    $item = Get-Item -LiteralPath (Resolve-Path -LiteralPath $Path).Path -Force
    if ($item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "$Name must be a non-linked regular file."
    }
    $item.FullName
}
function Blob([string]$Path) {
    $item=Get-Item -LiteralPath $Path
    [ordered]@{ path=$item.Name; size=[int64]$item.Length;
        sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
}
function Run-Validator([string]$Exe,[string[]]$Parameters,[string]$Name) {
    $start=[Diagnostics.ProcessStartInfo]::new(); $start.FileName=$Exe
    $start.UseShellExecute=$false; $start.CreateNoWindow=$true
    foreach($arg in $Parameters){$start.ArgumentList.Add($arg)}
    $p=[Diagnostics.Process]::new(); $p.StartInfo=$start
    try {
        if(-not $p.Start()){throw "$Name did not start."}
        if (-not $p.WaitForExit($ValidatorTimeoutSeconds*1000)) { $p.Kill($true); throw "$Name timed out." }
        if ($p.ExitCode -ne 0) { throw "$Name rejected the staged package." }
    } finally {$p.Dispose()}
}

$capture=(Resolve-Path -LiteralPath $CaptureDirectory).Path
$accepted=[IO.Path]::GetFullPath($AcceptedDirectory)
$parent=Split-Path -Parent $accepted
if (-not (Test-Path -LiteralPath $parent -PathType Container)) { throw 'Accepted parent is missing.' }
if (Test-Path -LiteralPath $accepted) { throw 'Accepted directory already exists.' }
$id=[Guid]::NewGuid().ToString('D')
$candidate=Join-Path $parent ".flycast-research-pvr-presentation-candidate-$id"
$quarantine=Join-Path $parent ".flycast-research-pvr-presentation-quarantine\$id"
$sources=[ordered]@{
    'identity.json'=(Resolve-File (Join-Path $capture 'inputs\identity.json') 'identity')
    'maple-replay.fcmt'=(Resolve-File (Join-Path $capture 'inputs\maple-replay.fcmt') 'replay')
    'pvr-ta-manifest.json'=(Resolve-File (Join-Path $capture 'inputs\pvr-ta-manifest.json') 'TA manifest')
    'pvr-ta.fcpvr'=(Resolve-File (Join-Path $capture 'candidate\pvr-ta.candidate.fcpvr') 'TA artifact')
    'pvr-presentation.fcpvrp'=(Resolve-File (Join-Path $capture 'candidate\pvr-presentation.candidate.fcpvrp') 'presentation artifact')
    'artifact-validator.exe'=(Resolve-File (Join-Path $capture 'validation\artifact-validator.exe') 'TA validator')
    'presentation-validator.exe'=(Resolve-File (Join-Path $capture 'validation\presentation-validator.exe') 'presentation validator')
    'static-analysis.bin'=(Resolve-File $StaticAnalysis 'static analysis')
    'publisher.ps1'=(Resolve-File $PSCommandPath 'publisher')
}
try {
    [IO.Directory]::CreateDirectory($candidate)|Out-Null
    foreach($entry in $sources.GetEnumerator()) { [IO.File]::Copy($entry.Value,(Join-Path $candidate $entry.Key),$false) }
    if($IntegrationTestAbortToken -ceq 'after-staging-v1'){throw 'Forced abort after staging.'}
    Run-Validator (Join-Path $candidate 'artifact-validator.exe') @('--artifact',(Join-Path $candidate 'pvr-ta.fcpvr'),'--identity',(Join-Path $candidate 'identity.json'),'--replay',(Join-Path $candidate 'maple-replay.fcmt'),'--manifest',(Join-Path $candidate 'pvr-ta-manifest.json')) 'TA validator'
    Run-Validator (Join-Path $candidate 'presentation-validator.exe') @('--artifact',(Join-Path $candidate 'pvr-presentation.fcpvrp'),'--identity',(Join-Path $candidate 'identity.json'),'--replay',(Join-Path $candidate 'maple-replay.fcmt')) 'presentation validator'
    $entries=@(); foreach($name in ($sources.Keys|Sort-Object)){ $entries+=Blob (Join-Path $candidate $name) }
    $receipt=[ordered]@{schema='flycast-research-pvr-presentation-package-validation';schema_version=1;package_id=$id;status='accepted';entries=$entries}
    [IO.File]::WriteAllText((Join-Path $candidate 'package-validation.json'),($receipt|ConvertTo-Json -Depth 8)+"`n",$utf8)
    if($IntegrationTestAbortToken -ceq 'after-validation-v1'){throw 'Forced abort after validation.'}
    [IO.Directory]::Move($candidate,$accepted)
    "ACCEPTED flycast-research-pvr-presentation-package-v1 $accepted"
} catch {
    if(Test-Path -LiteralPath $candidate){[IO.Directory]::CreateDirectory((Split-Path -Parent $quarantine))|Out-Null;[IO.Directory]::Move($candidate,$quarantine)}
    throw
}

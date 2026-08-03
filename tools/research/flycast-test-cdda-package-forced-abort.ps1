#Requires -Version 7.0

[CmdletBinding()]
param(
    [string]$Publisher = (Join-Path $PSScriptRoot `
        'flycast-publish-cdda-package-v2.ps1')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Get-Blob([string]$Path) {
    $item = Get-Item -LiteralPath $Path
    return [ordered]@{
        path = $item.FullName
        size = [int64]$item.Length
        sha256 = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}

$publisherPath = (Resolve-Path -LiteralPath $Publisher).Path
$root = Join-Path ([IO.Path]::GetTempPath()) `
    ("flycast-research-cdda-integration-" + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($root) | Out-Null

try {
    $artifact = Join-Path $root 'capture.fccdda'
    $replay = Join-Path $root 'replay.fcmt'
    $state = Join-Path $root 'initial.state'
    $flycast = Join-Path $root 'flycast.exe'
    $validator = Join-Path $root 'validator.exe'
    [IO.File]::WriteAllBytes($artifact, [byte[]](1, 2, 3))
    [IO.File]::WriteAllBytes($replay, [byte[]](4, 5, 6))
    [IO.File]::WriteAllBytes($state, [byte[]](7, 8, 9))
    [IO.File]::WriteAllBytes($flycast, [byte[]](10, 11, 12))
    [IO.File]::WriteAllBytes($validator, [byte[]](13, 14, 15))
    $identity = Join-Path $root 'identity.json'
    $identityObject = [ordered]@{
        schema = 'flycast-research-identity'
        schema_version = 2
        media = [ordered]@{ kind = 'gdi' }
        emulator = [ordered]@{ executable = Get-Blob $flycast }
        initial_state = [ordered]@{
            kind = 'savestate'
            blob = Get-Blob $state
        }
    }
    [IO.File]::WriteAllText($identity,
        (($identityObject | ConvertTo-Json -Depth 16) + "`n"), $utf8NoBom)
    $accepted = Join-Path $root 'accepted'
    $failed = $false
    try {
        & $publisherPath -Artifact $artifact -Identity $identity `
            -MapleReplay $replay -FlycastExecutable $flycast `
            -ArtifactValidator $validator -OutputDirectory $accepted `
            -IntegrationTestAbortToken 'after-staging-v2' | Out-Null
    }
    catch {
        $failed = $_.Exception.Message -like '*forced abort after staging*'
    }
    if (-not $failed) { throw 'Publisher did not report the forced abort.' }
    if (Test-Path -LiteralPath $accepted) {
        throw 'Forced-abort candidate appeared at the accepted path.'
    }
    $candidates = @(Get-ChildItem -LiteralPath $root -Directory -Force |
        Where-Object Name -like '.flycast-research-cdda-candidate-*')
    if ($candidates.Count -ne 0) {
        throw 'Forced-abort candidate remained in the publication parent.'
    }
    $quarantineRoot = Join-Path $root '.flycast-research-cdda-quarantine'
    $quarantined = @(Get-ChildItem -LiteralPath $quarantineRoot -Directory -Force)
    if ($quarantined.Count -ne 1 -or
            -not (Test-Path -LiteralPath (Join-Path $quarantined[0].FullName `
                'rejection.json') -PathType Leaf)) {
        throw 'Forced-abort candidate was not recoverably quarantined.'
    }
    Write-Output 'PASS flycast-research-cdda-package-v2 forced-abort quarantine'
}
finally {
    if (Test-Path -LiteralPath $root) {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}

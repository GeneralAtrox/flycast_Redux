[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Session,
    [Parameter(Mandatory = $true)]
    [ValidateSet('capabilities', 'status', 'request-clean-exit')]
    [string]$Command,
    [ValidateRange(1, 60000)][int]$ConnectTimeoutMilliseconds = 5000
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not ('FlycastResearchControl.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
namespace FlycastResearchControl {
  public static class Native {
    [DllImport("kernel32.dll", SetLastError=true)]
    private static extern bool GetNamedPipeServerProcessId(
      SafePipeHandle pipe, out uint processId);
    public static uint ServerProcessId(SafePipeHandle pipe) {
      uint processId;
      if (!GetNamedPipeServerProcessId(pipe, out processId))
        throw new Win32Exception(Marshal.GetLastWin32Error());
      return processId;
    }
  }
}
'@
}

function Require-ExactProperties([object]$Object, [string[]]$Names) {
    $actual = @($Object.PSObject.Properties.Name | Sort-Object -CaseSensitive)
    $expected = @($Names | Sort-Object -CaseSensitive)
    if ($actual.Count -ne $expected.Count -or
            [string]::Join("`n", $actual) -cne [string]::Join("`n", $expected)) {
        throw 'Control session has an unexpected property inventory.'
    }
}

$sessionPath = (Resolve-Path -LiteralPath $Session).Path
$contract = Get-Content -LiteralPath $sessionPath -Raw -Encoding UTF8 |
    ConvertFrom-Json -Depth 20
Require-ExactProperties $contract @('schema', 'schema_version', 'pipe_name',
    'nonce', 'server_process_id', 'server_start_filetime_utc',
    'server_executable', 'server_executable_sha256', 'client_executable',
    'client_executable_sha256')
if ([string]$contract.schema -cne 'flycast-research-control-session' -or
        [int]$contract.schema_version -ne 1 -or
        [string]$contract.pipe_name -cnotmatch '^[A-Za-z0-9_.-]{1,96}$' -or
        [string]$contract.nonce -cnotmatch '^[0-9a-f]{64}$' -or
        [string]$contract.server_executable_sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        [string]$contract.client_executable_sha256 -cnotmatch '^[0-9a-f]{64}$') {
    throw 'Control session contract is invalid.'
}

$current = Get-Process -Id $PID
$currentPath = [IO.Path]::GetFullPath($current.Path)
if ($currentPath -cne [IO.Path]::GetFullPath([string]$contract.client_executable)) {
    throw 'The invoking PowerShell executable differs from the authorized client.'
}
$currentHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $currentPath).Hash.ToLowerInvariant()
if ($currentHash -cne [string]$contract.client_executable_sha256) {
    throw 'The invoking PowerShell executable hash differs from the authorized client.'
}

$pipe = [IO.Pipes.NamedPipeClientStream]::new('.', [string]$contract.pipe_name,
    [IO.Pipes.PipeDirection]::InOut, [IO.Pipes.PipeOptions]::None)
try {
    $pipe.Connect($ConnectTimeoutMilliseconds)
    $pipe.ReadMode = [IO.Pipes.PipeTransmissionMode]::Message
    $serverPid = [FlycastResearchControl.Native]::ServerProcessId($pipe.SafePipeHandle)
    if ([uint32]$contract.server_process_id -ne $serverPid) {
        throw 'Named-pipe server PID differs from the session contract.'
    }
    $server = Get-Process -Id $serverPid
    $serverPath = [IO.Path]::GetFullPath($server.Path)
    if ($serverPath -cne [IO.Path]::GetFullPath([string]$contract.server_executable)) {
        throw 'Named-pipe server executable differs from the session contract.'
    }
    if ([string]$server.StartTime.ToFileTimeUtc() -cne
            [string]$contract.server_start_filetime_utc) {
        throw 'Named-pipe server creation time differs from the session contract.'
    }
    $serverHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $serverPath).Hash.ToLowerInvariant()
    if ($serverHash -cne [string]$contract.server_executable_sha256) {
        throw 'Named-pipe server executable hash differs from the session contract.'
    }

    $request = [ordered]@{
        schema = 'flycast-research-control-request'
        schema_version = 1
        nonce = [string]$contract.nonce
        command = $Command
    } | ConvertTo-Json -Compress
    $requestBytes = [Text.UTF8Encoding]::new($false).GetBytes($request)
    if ($requestBytes.Length -gt 16384) { throw 'Control request exceeds 16384 bytes.' }
    $pipe.Write($requestBytes, 0, $requestBytes.Length)
    $pipe.Flush()

    $buffer = [byte[]]::new(4096)
    $response = [IO.MemoryStream]::new()
    do {
        $count = $pipe.Read($buffer, 0, $buffer.Length)
        if ($count -eq 0) { throw 'Control server closed before a complete response.' }
        $response.Write($buffer, 0, $count)
        if ($response.Length -gt 65536) { throw 'Control response exceeds 65536 bytes.' }
    } while (-not $pipe.IsMessageComplete)
    $text = [Text.UTF8Encoding]::new($false, $true).GetString($response.ToArray())
    $parsed = $text | ConvertFrom-Json -Depth 30
    if ([string]$parsed.schema -cne 'flycast-research-control-response' -or
            [int]$parsed.schema_version -ne 1) {
        throw 'Control response contract is invalid.'
    }
    if (-not [bool]$parsed.ok) {
        throw "Control server rejected the request: $([string]$parsed.error)"
    }
    $parsed | ConvertTo-Json -Depth 30 -Compress
}
finally {
    $pipe.Dispose()
}

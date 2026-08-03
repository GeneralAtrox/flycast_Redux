# Research agent control v1

The private control endpoint is deliberately limited to reverse-engineering
tool orchestration. It does not replace the read-only localhost GDB discovery
endpoint and does not expose guest-memory writes, register mutation,
breakpoints, instruction stepping, frame stepping, controller injection or
general emulator automation.

The supported commands are:

- `capabilities`: reports the fixed command inventory and scope;
- `status`: reports the loaded/running/paused lifecycle, exact game and CPU
  backend, configured native recorders and current native-bus subscriber/drop
  state; and
- `request-clean-exit`: asks the frontend thread to perform its normal unload
  and process exit path.

The agent controls the research tooling at process launch, where the evidence
identity is still fixed. `flycast-start-controlled.ps1` accepts
`-AdditionalConfiguration` for existing transient identity, replay, manifest,
Lua, and native-recorder settings. `status` then reports configured Maple,
memory-range, SH-4, PowerVR, GD-ROM, and AICA tools together with native-bus
subscriber/drop state. A clean-exit request drives their ordinary finalization
path. Runtime commands that would change an experiment after its authenticated
configuration was selected are intentionally absent.

`request-clean-exit` means controlled emulator termination. It does not turn an
unfinished capture into evidence. Every recorder, independent validator and
package publisher retains its existing terminal-boundary and fail-closed
requirements.

## Private launch contract

On Windows, start a controlled process with:

```powershell
tools\research\flycast-start-controlled.ps1 `
  -Emulator C:\path\flycast.exe `
  -Game 'C:\Projects\Lodoss\GameFiles\Record of Lodoss War (Europe).gdi' `
  -Session C:\work\flycast-control-session.json `
  -StateSlot 1 -CpuBackend Dynarec
```

The launcher generates a random 256-bit nonce and unpredictable pipe leaf,
binds the server to the exact invoking PowerShell executable path and SHA-256,
waits until the server is reachable, then atomically writes a session contract
containing the exact server PID, creation time, executable path and SHA-256.
The session file contains the live capability nonce and must not be shared
while that process is running.

Invoke commands from the same authorized PowerShell executable:

```powershell
tools\research\flycast-research-control.ps1 `
  -Session C:\work\flycast-control-session.json -Command status

tools\research\flycast-research-control.ps1 `
  -Session C:\work\flycast-control-session.json -Command request-clean-exit
```

The client authenticates the named-pipe server PID, process creation time,
image path and current image SHA-256 before sending the nonce-bearing request.
The server independently obtains the connected client PID, resolves its image,
and checks its canonical path and current SHA-256. The pipe has an ACL limited
to the launching user and Local System, uses `PIPE_REJECT_REMOTE_CLIENTS`, and
accepts one request/response at a time. Requests are limited to 16 KiB and
responses to 64 KiB.

All four enabling values are transient command-line configuration. A partial,
persistent, malformed or stale client binding rejects game startup rather than
opening a weaker endpoint.

The launcher owns its four control keys plus auto-load-state, save-state slot,
and CPU-backend selection. `-AdditionalConfiguration` is capped at 16 KiB and
may not repeat those keys; this keeps the session contract and explicit
`-StateSlot`/`-CpuBackend` arguments authoritative while still permitting all
research recorder settings.

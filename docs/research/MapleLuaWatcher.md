# Maple Lua watcher

`tools/research/lua/maple-watch.lua` is a game-agnostic, discovery-only
consumer of the native `maple-request` and `maple-response` Lua events. It
pairs both halves by their exact decimal transaction ordinal and writes one
flushed JSON object per line. It does not create authoritative evidence.

Use the PowerShell launcher so the repository script can be loaded without
copying it into Flycast's executable directory:

```powershell
pwsh -NoProfile -File .\tools\research\flycast-watch-maple.ps1 `
  -Emulator .\build-research-runtime\Release\flycast.exe `
  -Game "C:\Projects\Lodoss\GameFiles\Record of Lodoss War (Europe).gdi" `
  -StateSlot 1 `
  -Output "C:\Projects\Lodoss\research-output\maple-slot-1.jsonl"
```

`StateSlot` uses the displayed one-based slot number. Omit it to boot normally.
Optional `Bus`, `Port`, and `Command` parameters install native filters before
events enter the Lua queue. `QueueCapacity` defaults to 4096 per request or
response subscription. `AutoExitFrames` is intended for bounded unattended
acceptance runs and requests main-loop shutdown without unloading the game from
inside the Lua overlay callback. Existing output is rejected unless `-Force`
is explicit.

Schema version 2 keeps the complete canonical request and response payloads as
lowercase hexadecimal and adds a `decoded` object produced by
`tools/research/lua/maple-decode.lua`. The decoder currently names Maple
commands and responses, expands device function masks, identifies standard
controllers and VMUs, decodes active-low controller buttons/triggers/axes, and
decodes VMU block parameters and payloads for storage, LCD, and clock traffic.
Raw payloads remain the source observation and are never discarded.

`decoded.status` is `decoded`, `partial`, `unknown`, or `malformed`. Unknown
commands and functions are left explicit. A malformed header, inconsistent
byte count, invalid known payload length, function mismatch, or unexpected
response code produces `malformed` plus an `errors` array; the decoder does not
guess past those checks. A `consistent` value of `true` separately means the
paired halves agree on their native transaction metadata. Lifecycle records
identify game start and state load.

The terminal `summary` is acceptable for discovery only when `complete` is
`true`. Any queue drop, callback error, duplicate request, metadata mismatch,
or unmatched half makes it false. The last observed queue depths are also
reported. Output remains non-authoritative even when complete; a meaning found
with this watcher must still be promoted through a bounded typed capture and
independent validator.

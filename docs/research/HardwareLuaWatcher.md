# Hardware Lua watcher

`tools/research/lua/hardware-watch.lua` is a game-agnostic, discovery-only
consumer for the native PowerVR, GD-ROM, CD-DA, and AICA observation buses. It never
creates accepted evidence. Use it to discover which render, disc, or audio
events surround a behavior, then promote the resulting hypothesis through the
corresponding bounded native artifact and independent validator.

The watcher requires an output path and an explicit event list. It does not
subscribe broadly by default:

```powershell
$env:FLYCAST_HARDWARE_WATCH_OUTPUT = 'C:\Projects\Lodoss\hardware.jsonl'
$env:FLYCAST_HARDWARE_WATCH_EVENTS = `
  'pvr-primitive,pvr-draw,gdrom-command,cdda-control-applied,cdda-sector,aica-key-on'
$env:FLYCAST_HARDWARE_WATCH_AUTO_EXIT_FRAMES = '600'
```

Load `hardware-watch.lua` through Flycast's normal Lua script option. Global
configuration additionally permits per-event native filters:

```lua
flycast_hardware_watch = {
  output = "C:\\Projects\\Lodoss\\hardware.jsonl",
  events = { "pvr-ta-block", "gdrom-transfer", "cdda-control-applied",
    "cdda-sector", "aica-register-write" },
  queue_capacity = 4096,
  max_events = 100000,
  filters = {
    ["pvr-ta-block"] = { source = "channel2-dma" },
    ["gdrom-transfer"] = { start_fad = 45150, end_fad = 45200 },
    ["cdda-control-applied"] = { command = 0x15 },
    ["cdda-sector"] = { start_fad = 600, end_fad = 6399 },
    ["aica-register-write"] = {
      writer = "sh4-direct", start_address = 0, end_address = 0x7fff
    },
  },
}
```

The Windows launcher selects the CPU backend explicitly when repeatability
matters. `-DynarecOwnership` enables the already-qualified per-instruction
dynarec markers so initiator fields can carry exact SH-4 owners; it is accepted
only together with `-CpuBackend Dynarec`:

```powershell
tools\research\flycast-watch-hardware.ps1 `
  -Emulator C:\path\flycast.exe `
  -Game 'C:\Projects\Lodoss\GameFiles\Record of Lodoss War (Europe).gdi' `
  -Output C:\Projects\Lodoss\hardware.jsonl `
  -Events 'pvr-start-render,pvr-render-done,pvr-presentation' `
  -StateSlot 1 -CpuBackend Interpreter -AutoExitFrames 120
```

High-frequency events such as AICA samples, AICA RAM writes and VRAM writes can
fill a discovery queue faster than Lua can serialize it. Select only the event
types and native address/channel filters needed for the hypothesis. A nonzero
drop count or nonempty terminal queue makes the summary incomplete rather than
silently presenting a partial stream as complete.

The JSONL stream starts with a typed session declaration, contains one
`observation` record per callback, and ends with a summary. Raw binary Lua
strings are omitted from JSON when the event also provides their exact
lowercase hexadecimal field. `complete=true` requires a controlled bounded
exit, zero dropped events, zero callback errors, and an empty terminal queue.
Manual emulator termination is explicitly incomplete.

## Long-running CD-DA evidence monitor

`flycast-monitor-cdda-evidence.ps1` is the bounded-output discovery monitor for
finding a real CD-DA trigger during unrestricted play. It retains only causal
progress records and exits Flycast automatically after observing this exact
joined chain:

`PLAY/PLAY2 accepted -> applied successfully -> successful sector -> nonzero AICA CD-DA contribution`

Successful-sector and nonzero-contribution predicates run on the native buses
before Lua queueing. Stopped-sector polling and zero-contribution sample frames
therefore do not grow the output or saturate the Lua queue while the user plays.

```powershell
tools\research\flycast-monitor-cdda-evidence.ps1 `
  -Emulator C:\path\flycast.exe `
  -Game 'C:\Projects\Lodoss\GameFiles\Record of Lodoss War (Europe).gdi' `
  -Output C:\Projects\Lodoss\research-output\cdda-monitor-slot1.jsonl `
  -StateSlot 1 -CpuBackend Interpreter
```

The result remains discovery-only. A hit identifies the save-state and action
from which to record an authenticated Maple replay and typed `.fccdda` artifact.
Both REIOS HLE controls and real-BIOS `CD_PLAY`/`CD_SEEK` packet controls are
eligible. Real-BIOS progress records carry `path=2` and retain the exact packet
parameters from the final SH-4 GD-ROM data write.

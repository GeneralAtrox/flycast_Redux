# Memory Lua watcher

`tools/research/lua/memory-watch.lua` is a game-agnostic, discovery-only
consumer of canonical SH-4 `memory-read` and `memory-write` events. It records
which guest instruction accessed a bounded address range; it does not create
authoritative evidence or modify guest memory.

Use the PowerShell launcher to select the CPU backend and load the repository
script without copying it beside Flycast:

```powershell
pwsh -NoProfile -File .\tools\research\flycast-watch-memory.ps1 `
  -Emulator .\build-research-runtime\Release\flycast.exe `
  -Game "C:\Projects\Lodoss\GameFiles\Record of Lodoss War (Europe).gdi" `
  -StateSlot 1 `
  -StartAddress 0x8c4fcfb4 `
  -Length 0x138 `
  -Access write `
  -Output "C:\Projects\Lodoss\research-output\inventory-writes.jsonl"
```

The default interpreter backend is selected explicitly with
`Dynarec.Enabled=no`. `-Backend dynarec` selects the dynarec and enables the
transient `research.DynarecObservation=yes` instrumentation required for
native dynarec memory events. The launcher never silently substitutes one
backend for another.

`StartAddress` and optional `StartPc`/`EndPc` accept decimal or `0x`-prefixed
32-bit values. `Length` is limited to 1 MiB. `Access` is `read`, `write`, or
`both`; the address, access, backend, and optional producer-PC filters run in
native code before queueing. `MaxEvents` defaults to 100,000 and requests a
clean Flycast exit after that many delivered accesses. `AutoExitFrames` can
instead bound an unattended run by rendered frames.

## Output

Every `access` JSONL record preserves the native schema version, exact decimal
ordinal and tick strings, backend, owning SH-4 PC, opcode, delay-slot depth,
address, access width in bytes, and exact 64-bit hexadecimal value. Hexadecimal PC,
opcode, and address fields are included for direct use with disassembly and
Ghidra exports.

Lifecycle snapshots are enabled by default for ranges no larger than 64 KiB
inside one Dreamcast main-RAM alias. They are sampled at game start and state
load. A final sample is also written before watcher-controlled `MaxEvents` or
`AutoExitFrames` shutdown. Manual termination is delivered after guest memory
has been torn down, so it deliberately does not emit a misleading final
snapshot. Each snapshot is explicitly marked
`coherent_with_access_stream=false`: it is useful for before/after discovery,
but it is not claimed to be at the same native instruction boundary as a
queued access. Pass `-NoSnapshots` for MMIO, VRAM, or larger ranges. Exact
access values still come from the native observation itself.

The terminal `summary` is complete only when there are no queue drops,
callback failures, snapshot failures, or queued deliveries left at shutdown.
Output remains discovery-only even when complete. Once a writer PC and field
meaning are plausible, preserve the claim with the bounded typed SH-4 or
memory-range capture and its independent validator.

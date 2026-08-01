# Lua causal-slice watcher

`tools/research/lua/causal-slice-watch.lua` is a game-agnostic,
discovery-only second-pass tool. Given a bounded memory range and a known
producer-PC interval, it joins the canonical native events for one SH-4
instruction into a causal slice:

- pre-instruction architectural registers;
- every matching successful read or write owned by that instruction;
- successful completion and post-instruction registers, or an explicit abort;
- an exception owned by the instruction, when present; and
- the bounded sequence of selected SH-4 calls observed since the most recent
  lifecycle or exception boundary.

Use the ordinary [memory watcher](MemoryLuaWatcher.md) first when the producer
PC is unknown. The causal watcher requires a producer interval so instruction
begin/end events can be filtered natively rather than tracing every guest
instruction.

## Run

```powershell
pwsh -NoProfile -File .\tools\research\flycast-watch-causal-slice.ps1 `
  -Emulator .\build-research-runtime\Release\flycast.exe `
  -Game "C:\Projects\Lodoss\GameFiles\Record of Lodoss War (Europe).gdi" `
  -StateSlot 1 `
  -StartAddress 0x8c925668 `
  -Length 4 `
  -Access write `
  -ProducerStartPc 0x8c0b4ace `
  -ProducerEndPc 0x8c0b4ace `
  -CallSiteStartPc 0x8c0b4bc4 `
  -CallSiteEndPc 0x8c0b4bc4 `
  -ReturnStartPc 0x8c0b4b80 `
  -ReturnEndPc 0x8c0b4b80 `
  -MaxSlices 1 `
  -Output "C:\Projects\Lodoss\research-output\damage-causal-slice.jsonl"
```

All address and PC parameters accept decimal or `0x`-prefixed 32-bit values.
The memory range is limited to 1 MiB. All PC intervals are inclusive. `Access`
is `read`, `write`, or `both`.

`CallSiteStartPc`/`CallSiteEndPc` bound the call instructions retained for the
path. `ReturnStartPc`/`ReturnEndPc` bound the RTS instructions allowed to close
those calls. These native filters are required: subscribing Lua to every call
and return in a running game can outpace the bounded delivery queue. Obtain the
ranges from static analysis or a narrower first-pass observation. For one known
route, use the exact call and return PCs as both ends of their ranges.

The default interpreter backend is selected explicitly with
`Dynarec.Enabled=no`. `-Backend dynarec` enables both the dynarec and the
transient `research.DynarecObservation=yes` instrumentation required before
observed blocks are compiled. The launcher never substitutes one backend for
the other.

When `StateSlot` is nonzero, the watcher does not subscribe until Flycast
reports that the auto-loaded state is installed. A later state load discards
the prior queued segment, resets call context, and establishes fresh native
subscriptions.

## Output and boundaries

Each `causal-slice` JSONL record contains exact decimal-string native ordinals
and ticks; the producer PC, opcode, and delay-slot depth; pre/post R0-R15, PR,
GBR, VBR, MACH, MACL, SR, FPUL, and FPSCR; matching access widths and exact
64-bit hexadecimal values; and completion, abort, or exception state.

The call path contains selected BSR, BSRF, and JSR calls that completed after
the watcher attached. Calls outside the configured call-site range and returns
outside the configured return range are intentionally outside its scope. Calls
are committed only after the delayed instruction completes or a later
instruction boundary proves execution continued. An aborted call is not added.
A selected RTS closes the latest matching observed call. Exceptions and
mismatched selected returns reset the path.

The path always declares `root_complete=false`: Flycast cannot reconstruct
callers that existed before attachment, and SH-4 control flow can manipulate
PR without a conventional call. R15 and PR in each register image are exact,
but stack memory is deliberately absent. Reading memory later from the Lua
overlay would not be coherent with the native instruction boundary.

Reaching `MaxSlices` closes output at the terminal event of the final included
instruction. The summary is complete only when every included slice is
balanced and all subscriptions report zero drops, callback failures, and
state-machine anomalies. Deliveries queued after that explicit cutoff are
counted as intentionally discarded and are outside the captured prefix.

Manual Flycast termination is always marked incomplete because native Lua
subscriptions and their pending deliveries are cleared before the terminate
callback. Output remains discovery-only even when its bounded prefix is
complete. Promote a recovered meaning with the typed SH-4 recorder,
independent validator, and capture-package workflow.

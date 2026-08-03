# Investigated next research architecture

## Bounded SH-4 execution and branch profiles

The production profiler is implemented separately from the canonical
instruction-observation bus. `research.DynarecObservation` remains false, so the
x64 decoder retains normal Flycast block geometry and optimization. During
decode it retains the exact little-endian bytes that it actually read. Each
compiled generation receives an immutable id; recompilation after self-modifying
code therefore creates a new generation instead of merging different bytes.

The x64 entry and exit stubs record entered, completed and aborted executions,
guest-cycle totals, first/last boundary ticks and the actual destination of the
terminal conditional, call, jump or return. Bounds are latched without throwing
through generated code. A new entry closes an interrupted predecessor as an
abort, and an incomplete or bounded-out session cannot publish evidence.

`Sh4ProfileRecord` writes `FCSH4PR1`, a bounded typed artifact bound to the exact
identity JSON, configuration digest and Maple trace bytes. The independent
`flycast-research-validate-sh4-profile` executable verifies the header CRC,
payload SHA-256, record grammar, exact branch opcode bytes, generation ownership,
execution/cycle totals and complete dynamic-edge coverage. It rejects any block
whose exact guest bytes are incomplete.

A production profile must use Maple timing produced by the production dynarec.
The one-instruction observation dynarec has different scheduler boundaries and
its Maple trace is not interchangeable. Schema-v3 therefore permits one
same-run `MapleRecord` plus `Sh4ProfileRecord`; Maple finalizes first, then the
profile binds its exact trace digest. A later schema-v2 profile run may replay
that trace. Lodoss slot 1 produced identical payload SHA-256 on record and replay
(`5fc32f4cb6ee1a0186d1bebbf4a6d1f7b79be976862bbab26f7f3f5e866fca9d`):
3,832 block generations, 4,783 dynamic edges and 52,407,631 completed executions
with zero aborts.

The artifact identity transitively pins the exact Ghidra export. The typed
[`SH-4/Ghidra semantic join v1`](Sh4GhidraSemanticJoinV1.md) now maps every
recorded production block generation and actual terminal edge to exact Ghidra
function bodies and symbols. Its separate validator reconstructs the complete
join from the authenticated profile, Maple trace, executable, Ghidra export and
exporter script. It does not reinterpret the production profile as the older
one-instruction accumulator or promote static labels into runtime truth.

## Lua expansion over native buses

The bounded asynchronous Lua discovery queue now consumes the canonical SH-4,
Maple, PowerVR TA, semantic draw, PVR presentation, GD-ROM, and AICA buses.
AICA and GD-ROM have distinct multi-subscriber discovery entry points while
retaining exclusive evidence ownership: discovery prevents an evidence
recorder from starting, and an active evidence recorder rejects discovery.
Filters run before queue insertion, callbacks remain on the Lua-owning thread,
and newest events are dropped at declared per-token/global bounds.

The event mapping exposes raw typed TA blocks and semantics, register/VRAM
writes, framebuffer/presentation generations, GD-ROM commands/transfers, and
AICA register/RAM/key/CD-DA/sample records. Every event remains
`discovery=true` and `authoritative_evidence=false`. The generic
`hardware-watch.lua` consumer requires an explicit event inventory, records
bounded JSONL, omits duplicate raw binary fields when hexadecimal copies are
present, and marks uncontrolled termination incomplete.

## Private agent-control interface

The implemented Windows control plane is intentionally narrower than the
initial design. The existing read-only localhost GDB snapshot endpoint already
owns coherent pause/read/resume discovery, while deterministic CLI launchers,
save states, replay checkpoints and Lua watchers own bounded experiments.
Duplicating stepping, breakpoints or memory access in a second mutable protocol
would add authority without closing a research-tooling gap.

The private named pipe therefore exposes only `capabilities`, research `status`
and `request-clean-exit`. It uses a per-launch random pipe and 256-bit nonce,
current-user ACL, `PIPE_REJECT_REMOTE_CLIENTS`, fixed 16 KiB/64 KiB request and
response bounds, one request at a time, and mutual process-image
authentication. The client checks the exact server PID, creation time, path and
SHA-256; the server obtains the client PID from the pipe and checks its exact
authorized path and SHA-256. The exit request is polled and executed on the
frontend thread. It cannot make an incomplete recorder acceptable.

The command contract, launcher and client are documented in
[ResearchAgentControlV1.md](ResearchAgentControlV1.md).

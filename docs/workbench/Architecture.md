# Architecture

## Purpose

The fork answers causal questions about a running Dreamcast game: which
instruction produced an effect, and what that effect led to. It does not try
to prove anything about hardware; it produces data a reverse engineer can
query quickly.

## Observation buses

Every subsystem of interest publishes typed observations on a bus in
`core/research`:

| Bus | Emitted from | Event |
| --- | --- | --- |
| SH-4 | interpreter hooks and dynarec SHIL markers | instruction begin/end/abort, memory read/write, exception, call, return |
| Maple | `maple_if.cpp` DMA | request and response frames |
| PVR TA | `ta.cpp`, `Renderer_if.cpp` | list init, accepted 32-byte blocks, STARTRENDER, render done |
| PVR draw | `ta_vtx.cpp`, renderers | decoded primitives, consumed draws |
| PVR presentation | `pvr_regs.cpp`, `pvr_mem.cpp`, `Renderer_if.cpp` | register writes, VRAM writes, render and framebuffer generations, presents |
| GD-ROM | `gdrom_hle.cpp`, `gdromv3.cpp` | HLE commands and transfers; ATA packets, DMA, PIO |
| AICA | `aica_if.cpp`, `sgc_if.cpp`, ARM7 memory | register and RAM writes, DMA, key on/off, mixer frames |
| CD-DA | `gdromv3.cpp`, `gdrom_hle.cpp` | control accepted/applied, sectors |

A bus with no subscriber costs one atomic load at each hook. Subscribers are
called synchronously on the emulating thread and must be quick; the recorder
copies the event into a queue and returns.

## Attribution

The SH-4 runtime keeps an instruction frame open while an instruction
executes, including its delay slot. Hardware buses read the current frame
through `sh4ObservationCurrentInstructionOwner()` and stamp their events with
an owner token: generation, PC, PR, opcode, backend, delay-slot depth. The
generation counter is process-wide, so any hardware row joins back to the
exact SH-4 instruction that caused it.

Under the dynarec, exact attribution needs `research.DynarecObservation`. It
inserts markers into the generated code so frames open and close at the same
boundaries as the interpreter. A differential test compares the two backends
event for event, including delay slots, faults, interrupts, and self-modifying
code.

Ticks are SH-4 scheduler cycles. Hardware events take the later of the
scheduler tick and the owning instruction's tick so a synchronous effect never
appears to precede its cause.

## Recorder

`core/research/workbench` turns observations into SQLite rows. One thread
drains a two-lane queue (SH-4 bulk, everything else priority) in batches
inside transactions. Per-bus row binders own their prepared statements and
create their tables and indexes. Everything about what to record is a
`RecorderConfig`, built from launch options or from JSON over the control
socket.

## Control

`core/research/control` is a loopback TCP JSON-lines server. The protocol
layer is pure and unit-tested; the actions layer binds it to the emulator.
Lifecycle work (pause, resume, save states, exit) is marshalled to the UI
thread, which drains a task queue once per frame. Recorder and checkpoint
calls are thread-safe and run inline. Memory and register reads run inline
and are unsynchronized while the emulator runs.

## Determinism aids

Maple record/replay pins controller and VMU traffic, disables threaded
rendering and auto-save, and fixes the RTC seed so a boot runs identically.
The PC checkpoint pauses the emulator exactly when a given top-level
instruction completes, optionally gated on a RAM word, from launch options
or at runtime.

## What was removed

Earlier versions of this fork carried an evidence layer: SHA-256 identity
manifests, versioned binary artifacts, validators, atomic publication, and a
repository audit. It made captures defensible but slowed every experiment.
The workbench keeps the observation core and replaces the artifact formats
with one database.

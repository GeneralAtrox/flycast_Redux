# SH-4 native observation contract v1

## Purpose

`Sh4ObservationV1` is the backend-neutral in-process boundary shared by SH-4
recorders, backend-equivalence capture, and discovery subscribers. It prevents
the interpreter, dynarec, and Lua integrations from independently redefining
instruction, memory, control-flow, or exception semantics.

This is a native observation contract, not an accepted evidence artifact. An
observation becomes evidence only when a versioned native recorder writes it to
a typed artifact and an independent validator accepts that artifact. Lua output
remains discovery evidence even when it originates from this bus.

The frozen `flycast-sh4-events-v1` binary remains byte-for-byte unchanged. Its
interpreter runtime now consumes the native observation bus and continues to
derive the same manifest-filtered call, return, watch, and exception records.

## Backends and event types

Every event declares schema version `1` and exactly one backend:

- `interpreter`; or
- `dynarec`.

V1 event types are:

- `instruction-begin`;
- `instruction-end`;
- `instruction-abort`;
- `memory-read`;
- `memory-write`;
- `exception`;
- `call`; and
- `return`.

The interpreter emits production observations. The dynarec emits instruction,
call, return, memory, exception, and interrupt observations only from blocks
compiled with the transient `research.DynarecObservation=yes` option. Dynarec
evidence remains inadmissible until invalidation, authenticated differential,
and external exercise gates in the roadmap pass.

Event construction is no longer owned by the frozen interpreter recorder.
`sh4_observation_runtime` owns one backend-tagged instruction-frame stack and
the canonical register snapshot, call decoder, return fields, exception owner,
and memory-event construction. The interpreter wrappers and future dynarec JIT
markers invoke that same runtime. The frozen v1 recorder remains an
interpreter-only subscriber.

## Common fields

The native structure carries:

- a process-wide, monotonically increasing emission ordinal;
- backend and event type;
- scheduler tick;
- executed instruction PC and opcode;
- architectural next PC;
- delay-slot depth;
- R0-R15, PR, GBR, VBR, MACH, MACL, SR, FPUL, and FPSCR; and
- type-specific memory, exception, or control-flow fields.

An `available_fields` mask distinguishes fields owned by the boundary from
zero-valued fields. Instruction begin/end, call, return, and exception events
own `next_pc` and the architectural register image. Memory and abort events do
not; their zeroed register fields must not be interpreted as a guest state.

The emission ordinal establishes delivery order inside one Flycast process. It
is not compared directly between separately launched interpreter and dynarec
runs. A typed equivalence recorder assigns run-relative ordinals when it writes
each stream.

## Instruction and control-flow ordering

For a call instruction, native delivery is:

```text
instruction-begin
    -> call
    -> zero or more memory observations
    -> nested delay-slot observations
    -> instruction-end or instruction-abort
```

`call` is emitted for BSR, BSRF, and JSR. Its target is decoded from the exact
opcode, executed PC, and pre-instruction registers. It includes the architectural
return PC (`call_pc + 4`) and delay-slot PC (`call_pc + 2`). The same decoder is
used by the frozen SH-4 events recorder.

For RTS, `return` is emitted immediately before `instruction-end`. It records
the resumed PC, the PR value owned by the instruction-begin boundary, and the
delay-slot PC (`instruction_pc + 2`).

An instruction that faults emits `exception` followed by
`instruction-abort`. Delay-slot depth is the native nesting depth, so the branch
owner is depth zero and its delay-slot instruction is depth one.

`instruction-abort` is also the fail-closed cleanup boundary for a recorder or
subscriber failure. Such an abort may occur before a call/return or nested
delay-slot observation could be published; it closes the owned instruction but
cannot be accepted as a normally completed semantic instruction.

## Memory semantics

Memory events are emitted only while an SH-4 instruction owns an observation
scope. Reads are emitted after the underlying read returns and contain the value
delivered to the guest. Writes are emitted after the underlying write returns
and contain the width-masked guest value. Width is exactly 1, 2, 4, or 8 bytes.

Native subscription filters may select backends, event types, and an overlapping
32-bit memory interval. The frozen v1 recorder subscribes to interpreter events
only. Filtering occurs before a subscriber callback runs.

## Bus and fast-path rules

- With no native subscriber, one atomic active-count check returns immediately.
- Activity is also counted per backend. An interpreter-only recorder does not
  activate dynarec frame allocation or snapshot construction, and vice versa.
- Each backend subscription set has a generation. If that generation changes
  during an instruction, the runtime balances every existing/nested frame but
  suppresses its remaining observations; a replacement subscriber therefore
  cannot receive an end, delay slot, memory access, or exception without the
  matching begin.
- No recorder mutex is acquired and no observation object is published on that
  path.
- Native publication is serialized across threads; ordinal assignment and
  synchronous delivery therefore have the same order.
- A callback may unsubscribe itself.
- Cross-thread unsubscribe waits for an in-flight callback to finish. A
  subscriber is never invoked after its unsubscribe call returns.
- One callback failure does not prevent later subscribers from receiving the
  same event; the first failure is rethrown after fan-out.
- Recursive publication from inside a callback is rejected.
- The authoritative v1 recorder subscribes only while its runtime session is
  active and rejects a non-interpreter observation fail-closed.
- Recorder stop/abort detaches the session under its mutex, then unsubscribes
  and finalizes after releasing that mutex. A begin callback rechecks activity
  after acquiring the session mutex, closing the stop-versus-publish race.

The interpreter always closes an authoritative instruction scope when an
auxiliary call/return subscriber fails. Abort delivery is `noexcept`; subscriber
failures on that cleanup boundary are logged after complete fan-out.

Lua does not execute synchronously on this bus. Its native subscriber copies
matching discovery events into the bounded queue defined by
[LuaSubscriptionsV1](LuaSubscriptionsV1.md) for delivery on the Lua-owning
thread. Lua subscriptions do not enable dynarec instrumentation or alter a
native evidence recorder.

Flycast's normal interpreter applies an eight-times underclock outside strict
mode. An active interpreter observation subscription instead selects a
research-only ratio-one issue counter, and resets that counter after
control-flow completion. This makes the authoritative tick stream comparable
to the dynarec without changing ordinary interpreter execution.

Dynarec code must write allocated guest registers back to `Sh4Context` before
calling the shared runtime. JIT markers must be absent from normally compiled
blocks or guarded before any helper call; a helper call per inactive guest
instruction is not an acceptable fast path.

The current implementation chooses absence: with
`research.DynarecObservation=no` (the default), the decoder emits no research
SHIL operations and ordinary blocks retain their original register-allocation
and call profile. The option must be fixed before any observed block is
compiled. When enabled, research begin/end SHIL operations are full-context
call barriers on x64, ARM32, and ARM64: allocated registers are written to
`Sh4Context`, the marker reconstructs the architectural next PC and logical
instruction tick, and only then does the shared runtime take a snapshot.

Delayed branches keep their owner frame open across the delay instruction.
BF/S and BT/S are conditional because the authoritative interpreter only nests
the delay instruction when its condition is taken; the non-taken case closes
the branch first and observes the following instruction at depth zero. The JIT
marker uses the condition latched by `shop_jcond`, not the live T bit after the
delay instruction, because the slot may modify T. The first marker anchors a
research run to the scheduler. While dynarec observation is active, the shared
runtime then advances a research-only SH-4 issue counter in actual
instruction-completion order. This keeps ticks independent of host block
splits and Flycast's block-up-front cycle charge. The counter resets after the
same control-flow instructions as the precise interpreter counter. Division-loop
aggregation and the single-branch-target pass are disabled in this research
mode so no executed guest instruction disappears behind an optimized SHIL
operation.

Delay-slot markers use the interpreter's execution-time cycle order: the owner
begins, the slot begins at the same logical tick, the slot is charged when it
completes, and only then is the owner charged. A non-taken BF/S or BT/S closes
the owner first, resets the issue counter, and observes the following
instruction at depth zero even when dynarec block boundaries differ. A
research-only SR.FD guard sits immediately after each
FPU instruction-begin marker on every native dynarec backend. It restores the
unconsumed up-front block cycles on a fault and reports the precise instruction
or slot-FPU exception owner; the legacy MMU block-entry FPU check remains in
place only for normal, unobserved blocks.

Dynarec fallback SHIL calls invoke the opcode handler directly rather than
entering the interpreter execution loop. They therefore remain owned by the
dynarec instruction frame. The legacy opcode-handler memory wrapper inherits
that current owner; attempting to begin a differently tagged instruction while
another backend owns the frame is a contract error.

Native dynarec `readm` and `writem` operations use paired research-only SHIL
markers. The begin marker preserves the effective address before a load can
overwrite the same guest register. The end marker publishes the canonical
memory event only after the access succeeds, masking values to 8, 16, 32, or
64 bits exactly as the interpreter wrappers do. A fault between the pair emits
no memory event and instruction abort discards the pending access. Because the
pair surrounds the SHIL memory operation itself, it covers immediate accesses,
direct fastmem, fault-rewritten slow handlers, and MMU handlers without adding
checks to blocks compiled with research observation disabled. Compile-time
read folding is disabled for observed blocks so a semantic read cannot vanish.

Marker helpers are `noexcept`: subscriber failures are logged and the owned
dynarec frame is aborted rather than allowing a C++ exception to unwind through
generated machine code. The common `Do_Exception` boundary publishes the
pre-transition snapshot and aborts every open owner frame, so MMU/fallback
trampolines cannot leak a pending memory access or delayed-branch owner. The
interpreter uses that same boundary, producing one canonical exception followed
by innermost-to-outermost aborts for a delay-slot fault. Scheduler-boundary
interrupts are encoded as unowned exception observations with opcode and depth
zero, the pre-interrupt register image, `VBR + 0x600`, and the INTEVT code.
An interrupt encountered with an open observation frame is not relabelled as
instruction-owned: the runtime clears its local frame state and emits nothing,
leaving the already-published begin unmatched so typed validation fails closed.

Libretro builds compile the SH-4 emission runtime as no-op inline functions.
The generic native bus can still be used directly, but subscribing does not
enable SH-4 instruction emission in that configuration.

## Equivalence admission gates

Dynarec observations cannot be labelled equivalent until differential tests
cover:

- block entry, exit, linking, and interrupt boundaries;
- BSR, BSRF, JSR, RTS, and their delay slots;
- synchronous and delay-slot exceptions;
- self-modifying code and block invalidation;
- 1/2/4/8-byte access values and order;
- fastmem, MMU, and fallback paths; and
- exact pre/post architectural register images and scheduler timing.

Those runs require separate backend identities and a typed independent
equivalence report. Frozen identity v1, SH-4 events v1, and capture-package v2
are not relaxed in place.

# SH-4 backend observation trace v1

## Purpose and authority

`flycast-sh4-observation-trace-v1` is the typed per-run input to backend
equivalence. One file contains an interpreter stream and a separate file
contains a dynarec stream. A trace is a candidate artifact, not an equivalence
claim: admission requires the independent comparator plus the still-separate
typed equivalence job/report workflow.

The format does not relax identity v1, SH-4 events v1, or capture-package v2.
Each trace instead binds the new backend-specific identity supplied by its
equivalence job, a common deterministic Maple replay digest, and a common
manifest-set digest.

## Envelope

All integers are little-endian. The fixed 208-byte header contains:

| Offset | Size | Field |
| ---: | ---: | --- |
| 0 | 8 | magic `FCSH4OB1` |
| 8 | 4 | schema version, exactly `1` |
| 12 | 4 | header size, exactly `208` |
| 16 | 4 | endian sentinel `0x01020304` |
| 20 | 4 | flags; bit zero is `complete` |
| 24 | 4 | backend (`1` interpreter, `2` dynarec) |
| 28 | 4 | event-record size, exactly `184` |
| 32 | 8 | event count |
| 40 | 8 | payload byte count |
| 48 | 8 | dropped-event count |
| 56 | 8 | first scheduler tick |
| 64 | 8 | last scheduler tick |
| 72 | 32 | backend-specific identity digest |
| 104 | 32 | common replay digest |
| 136 | 32 | common manifest-set digest |
| 168 | 32 | payload digest |
| 200 | 4 | reserved zero |
| 204 | 4 | CRC-32 over bytes 0-203 |

The writer creates the destination exclusively, writes and flushes an
incomplete header, streams fixed records, then writes the complete header only
after the final counts and payload digest are known and the payload has been
flushed. It flushes again after the header rewrite. Abandonment, a crash, or a
partial header rewrite remains rejectable. A valid trace is nonempty, has zero
drops, has no trailing bytes, and remains the same size throughout validation.
V1 recording is synchronous and has no lossy queue or drop API: a write failure
throws and leaves the file incomplete. The drop field reserves an explicit
fail-closed check for any later capture transport without permitting such a
transport in this contract.

## Canonical event record

Every 184-byte event contains a run-relative contiguous ordinal followed by:

- event type and scheduler tick;
- executed PC, architectural next PC, opcode, and delay-slot depth;
- an availability mask and R0-R15, PR, GBR, VBR, MACH, MACL, SR, FPUL, FPSCR;
- memory address, width, and value;
- exception PC, vector PC, and code; and
- call kind, target, return PC, and delay-slot PC.

Unavailable and type-inapplicable fields are encoded as zero. Instruction
begin/end, call, return, and exception records require next-PC and register
snapshots. Memory and abort records do not. Memory widths are exactly 1, 2, 4,
or 8 bytes; the address range may not wrap and the value may not contain bits
outside that width.

Run-relative ordinals deliberately replace process emission ordinals. The two
backends run in different processes, so a process-global ordinal is not a
semantic comparison field.

## Independent semantic validation

The comparator implements its own header decoder, CRC implementation, event
decoder, canonical-field checks, and SH-4 control-flow checks. It does not call
the production single-trace validator.

Both paths reject:

- incomplete, malformed, oversized, growing, truncated, or trailing data;
- nonzero drops, bad digests, mismatched bindings, or noncontiguous ordinals;
- nonmonotonic scheduler ticks;
- events without the correct instruction or delay-slot owner;
- unclosed instruction frames;
- missing or duplicate call/return records;
- normally completed delayed instructions without exactly one nested
  `instruction_pc + 2` delay-slot observation;
- BSR, BSRF, JSR, or RTS fields inconsistent with opcode and registers; and
- a call snapshot that differs from its instruction-begin snapshot or a return
  snapshot that differs from its instruction-end snapshot.

An exception may appear without an active instruction at delay depth zero so a
scheduler-boundary interrupt can be represented without inventing instruction
ownership. Its snapshot is the architectural state at that scheduler boundary,
not an instruction-frame claim, and is still compared exactly.

An abort is intentionally different from a normal end. Native publication may
emit one as fail-closed cleanup after a recorder/subscriber failure, but a typed
trace validator accepts an abort only after a guest exception in the same open
instruction chain. An exception in a delay slot marks both the nested frame and
its delayed-control-flow owner. Pre-execution call records remain mandatory;
return and completed-delay obligations do not apply to the faulted instruction.
An unjustified cleanup abort is rejected rather than allowing two identically
failed captures to be labelled equivalent.

## Equivalence comparison

The contract supplies separate interpreter and dynarec identity digests plus
the one replay and manifest-set digest required in both files. After validating
both complete streams, the comparator checks every semantic field exactly in
ordinal order:

- event type and scheduler tick;
- executed PC, next PC, opcode, and delay-slot depth;
- every architectural register;
- memory address, width, value, direction, and order;
- exception fields; and
- call/return kind and ownership fields.

The result records the exact matching prefix and either `equivalent` or the
first divergent ordinal, field, interpreter value, and dynarec value. Invalid
inputs are rejected rather than reported as semantic divergence.

The next contract layer is [Sh4EquivalenceV1.md](Sh4EquivalenceV1.md). It
authenticates the emulator executable, Git commit, backend configurations,
replay file, manifest inventory, both traces and the independent comparator
executable, then produces a deterministic typed report.

The in-emulator recording bridge is specified in
[Sh4ObservationCaptureRuntimeV1.md](Sh4ObservationCaptureRuntimeV1.md). It uses
identity v2 and does not relax the frozen identity v1 or Maple v1 contracts.

# Flycast Research Toolkit Roadmap

This roadmap tracks emulator capabilities, not research progress for any one
game. A phase is complete only when its artifacts are versioned, independently
validated, covered by negative tests, and publishable through the evidence
transaction.

## Readiness gates

| Gate | Acceptance requirement |
| --- | --- |
| Identity | Exact media, boot, firmware, persistent devices, build, configuration, and optional static project are bound by SHA-256. |
| Determinism | Replay rejects the first metadata, timing, topology, request, or terminal mismatch. |
| Completeness | Counts, bounds, terminal state, drop counters, and payload digest all validate. |
| Independence | A separate validator process accepts or rejects candidates without trusting emulator UI or log text. |
| Publication | Only a fully validated set is renamed from private candidates to accepted paths. |
| Abort safety | Forced termination leaves no accepted artifact and does not stop or alter unrelated processes/files. |
| Provenance | Every output and comparison labels its evidence class and exact identities. |

## Phase 1: evidence core and Maple v1

Operator and format details are in [MapleTraceV1.md](MapleTraceV1.md).

Deliverables:

- `flycast-research-identity-v1` JSON schema;
- portable SHA-256 and exact-file reader;
- `flycast-maple-trace-v1` binary format;
- exclusive candidate writer and incomplete/complete finalization;
- Maple DMA begin, transaction, schedule, commit, and abort observations;
- deterministic response replay with strict request/topology/timing checks;
- independent `flycast-research-validate-maple` executable;
- round-trip, truncation, mutation, count, identity, ordering, and replay
  divergence tests; and
- operator documentation and example transient configuration.

Deferred from v1:

- accepted-output publication (owned by the Phase 2 driver);
- savestate-based replay starts;
- hot-plug/reconnect as an accepted production event;
- network/rollback sessions;
- cross-platform UI controls; and
- claims about physical Maple timing.

## Phase 2: capture transaction driver

Operator, package, failure, and portable process-contract details are in
[CaptureTransactionV1.md](CaptureTransactionV1.md).

Deliverables:

- a PowerShell driver for Windows and a portable process contract;
- private candidates located under each destination volume;
- exact PID, executable path, process start time, and command-line ownership;
- stable terminal status sampled twice;
- post-stop input re-authentication;
- validator orchestration and same-volume atomic publication;
- quarantine metadata and an atomic capture transcript; and
- forced-abort and unrelated-sentinel tests.

## Phase 3: SH-4 and Ghidra

Deliverables:

- game-agnostic, bounded memory-range manifests with one-shot interpreter-PC
  triggers and exact executable/static-analysis/hook-manifest bindings;
- typed `memory-ranges-v1` snapshots and an independent validator that reject
  address/hash mismatches without relocation search;
- instruction hooks with explicit executed-PC and delay-slot semantics;
- typed call/return/exception events without patching guest opcodes;
- range watchpoints for interpreter and dynarec paths;
- bounded stack and scheduler evidence;
- Ghidra export schemas for functions, data types, symbols, and image identity;
- hook-manifest hashing and runtime/static joins; and
- Lua subscriptions backed by the native event stream.

The interpreter is the first authoritative execution backend. Dynarec evidence
is admitted only after equivalence tests cover block boundaries, exceptions,
delay slots, self-modifying code, and memory access width/order.

The first four Phase 3 vertical slices are implemented. A fifth foundational
slice now defines the versioned backend-neutral native SH-4 observation bus,
routes the frozen interpreter recorder through it without changing v1 artifact
bytes, and publishes ordered instruction, call, return, memory, exception, and
abort observations behind per-backend inactive atomic fast paths. Canonical
frame ownership, register snapshots, call/return derivation, exception
ownership, and memory-event construction now live in a shared runtime invoked
by interpreter wrappers and dynarec JIT markers, rather than in the frozen v1
recorder. The memory-range slice
provides strict generic manifests, exact identity cross-binding, a one-shot
pre-instruction interpreter trigger, raw main-RAM range events, fail-closed
binary finalization, and a standalone streaming validator. The SH-4 events
slice adds manifest-filtered BSR/BSRF/JSR and delay-slot-correct RTS evidence,
bounded architectural-register images, call/return memory snapshots, interpreter
data read/write watchpoints, exception ownership, typed counts, and independent
validation. The Ghidra export slice adds a bounded deterministic inventory of
program identity, memory blocks, functions, symbols, and data types; exact
executable/exporter hashing; an independent validator; and exact runtime
manifest joins. The capture-package-v2 slice composes an
independently revalidated Maple v1 base with the exact identity, Ghidra export,
memory-range artifact, and/or SH-4 artifact; revalidates every typed join;
issues a deterministic receipt; and publishes the complete set by one
sibling-directory rename with forced-abort quarantine coverage. A sixth slice
adds a fail-closed fixed-record observation trace bound independently to the
interpreter or dynarec identity and jointly to replay and manifest-set digests.
Its separately implemented streaming comparator rejects malformed inputs,
validates instruction/delay-slot/call/return ownership, compares every semantic
field exactly, and identifies the first divergent ordinal and field. A seventh
slice adds opt-in research SHIL instruction markers across x64, ARM32, and ARM64,
forces coherent `Sh4Context` snapshots at each marker, reconstructs per-op ticks
from block cycle positions, preserves latched conditional delay-slot ownership,
disables instruction-eliding optimizer passes for observed blocks, and leaves
normal blocks entirely uninstrumented. An eighth slice now admits
only equivalent comparisons through a self-contained, path-localized package:
the publisher stages exact inputs, runs the staged comparator, locks every
entry, obtains an independently recomputed receipt, and publishes by one
sibling-directory rename. Mutation, divergence, and forced aborts before and
after validation are covered fail-closed. Real differential execution now
covers block boundaries, calls/returns and delay slots, synchronous and delayed
faults, interrupts, fastmem/MMU/fallback memory, exact access widths/order, and
compiled-code invalidation after self-modifying writes. Bounded Lua discovery
delivery is implemented over the same canonical bus.

The trace capture bridge now accepts only backend-specific identity v2,
authenticates one common Maple replay and manifest-set file, applies the exact
interpreter/dynarec configuration before executor selection, records the
matching native backend, and re-authenticates inputs before clean finalization.
Identity v1 and Maple trace v1 remain frozen; identity v2 explicitly binds the
original identity digest carried by the shared Maple replay and one exact
Dreamcast RTC seed shared by both processes. The authenticated
two-process job/report now binds both identity-v2 manifests, the exact emulator
build, common typed Maple replay, ordered manifest set, both traces, and the
independent comparator. It produces deterministic equivalent or
first-divergence reports. Atomic equivalence-package admission and semantic
differential fixtures are implemented. The external Lodoss admission gate
passed on 2026-07-31: the independently validated package matched all 574,470
events with zero drops and no first divergence.

The following memory slice now surrounds native dynarec `readm`/`writem` SHIL
with success-sensitive markers, preserving effective addresses and exact-width
values across fastmem, rewritten slow, MMU, and immediate-address compilation.
The exception slice now observes the common pre-transition exception boundary,
aborts complete nested delay chains, and records scheduler interrupts as
backend-specific unowned exception events. Observed blocks also use a precise
per-instruction FPU-disabled guard instead of the legacy MMU block-entry check,
with slot-FPU ownership and interpreter-ordered delay-slot ticks. The real x64
differential fixture proves invalidation and end-to-end equivalence for the
required synthetic cases. The accepted 2026-07-31 external Lodoss package
completes the project-level dynarec admission gate.

## Phase 4: PowerVR causal ownership

Deliverables:

- exact 32-byte TA input blocks from store queues, channel-2 DMA, and sort DMA;
- initiator tokens containing SH-4 PC/PR/tick where meaningful;
- TA context, list type, pass, and stream ordinals;
- PVR register and VRAM-write artifacts;
- STARTRENDER, render-complete, framebuffer, and present generations;
- backend-independent raw framebuffer artifacts where possible;
- backend readback artifacts labeled as such; and
- exact frame and temporal-delta validators.

## Phase 5: GD-ROM and AICA

Deliverables:

- ATA and SPI packet traces;
- FAD/count/mode and sector byte/digest evidence;
- PIO/DMA chunk, destination, scheduler, completion, and interrupt events;
- CD-DA sector/playback joins;
- AICA channel/common/DSP register writes and ARAM ranges;
- sample-interval artifacts with exact format and digest; and
- disc-to-audio and guest-to-slot ownership joins.

## Phase 6: comparison and audit

Deliverables:

- exact RGB/RGBA frame comparison without implicit masks or tolerances;
- exact PCM comparison and bounded diagnostic metrics;
- temporal-delta comparison;
- hardware and independent-emulator import manifests;
- reconstruction comparison reports that preserve provenance; and
- a repository-wide audit command that rejects stale, unbound, or internally
  inconsistent accepted evidence.

## Working rule

New probes first land as a complete narrow vertical slice:

```text
native observation
    -> typed candidate artifact
    -> independent validator
    -> negative tests
    -> capture transaction
    -> accepted evidence
```

Adding broad logging before this chain exists increases ambiguity and is not a
substitute for toolkit progress.

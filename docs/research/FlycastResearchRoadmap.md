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
delivery is implemented over the same canonical bus. Ready-to-run discovery
consumers now cover decoded Maple request/response traffic and bounded SH-4
memory provenance with native address/producer-PC filters, explicit overflow
accounting, and discovery-only lifecycle samples.

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

The bounded production dynarec profile and dynamic branch inventory now join
independently to a complete Ghidra export. Exact guest block bytes are checked
against the authenticated boot executable before functions, symbols and actual
edge destinations are assigned. A focused SH-4/Ghidra package v1 stages a
path-localized copy of every reconstruction input, authenticates the initial
state and Flycast executable declared by the identity, independently
reconstructs the join, locks every entry, issues a deterministic receipt and
publishes by one sibling-directory rename. Forced aborts before semantic
validation, after semantic validation and after receipt issuance remain
fail-closed. The 2026-08-02 Lodoss admission contains 3,832 block generations,
4,783 dynamic edges and 51,985,205 edge occurrences.

## Phase 4: PowerVR causal ownership

Artifact and operator details are in [PvrTaArtifactV1.md](PvrTaArtifactV1.md).

The native TA observation boundary, typed causal artifact, independent
validator, bounded capture runtime/CLI, and atomic package admission are now
implemented. The emulator
emits only successfully accepted 32-byte TA blocks, labels store-queue,
channel-2 DMA, and sort-DMA sources, assigns non-reused context and render
generations, and seals the selected context set at STARTRENDER before matching
the guest-visible render-done boundary. Where the boundary is reached inside a
tracked SH-4 instruction, the event carries that exact instruction owner rather
than sampling the current PC afterward. STARTRENDER also carries the exact
`REGION_BASE` and `FPU_PARAM_CFG` values and the ordered address/value transcript
of every VRAM read used to select those contexts; observation reuses the values
consumed by rendering rather than reading mutable VRAM a second time. Sort-DMA
RAM offsets are normalized to physical SH-4 addresses.

The bus exposes a process-lifetime monotonic drop count and an exclusive
evidence subscription. Evidence ownership rejects concurrent discovery
subscribers, and callback, allocation, reentrancy, or incomplete-selection
failures increment the counter without changing emulation. An evidence recorder
must compare the terminal count with its starting value and reject any nonzero
delta. The implemented recorder does this, reauthenticates identity/replay/
manifest inputs, checks the manifest render count before setting the complete
header, and is abandoned if the Maple replay terminal is not accepted first.

The standalone validator reconstructs TA context selection from the recorded
VRAM-read transcript, verifies every typed lifecycle and binding, and requires
a complete list/block/render/render-done vertical slice. Package v1 revalidates
the selected accepted SH-4 equivalence backend, byte-identical identity/replay,
static-analysis and executable digests, the artifact, and a fixed inventory;
it then publishes by one sibling-directory rename. Mutation, incomplete-file,
limit, identity, render-count, and forced-abort cases are covered fail-closed.

The 2026-08-01 Lodoss qualification completes the real-game TA admission gate.
Maple trace v2 typed the exact NOP control descriptor needed to extend the
deterministic cold-boot replay. DMA 4 was the first terminal containing a fully
joined accepted-block, selected-context, STARTRENDER, and render-done chain.
The independently revalidated atomic package contains seven list
initializations, four accepted blocks, one STARTRENDER, and one RenderDone.

Typed PVR register/VRAM-write ownership, framebuffer/presentation joins, raw
framebuffer validation, and DirectX 11 semantic draw ownership are implemented.
The semantic draw validator independently reconstructs material, vertices and
strips from raw TA bytes before accepting the renderer-facing artifact. A
save-state-backed Lodoss run authenticates Slot 1, proves 120-DMA interpreter/
dynarec equivalence, completes 40 renders, reaches a consumed non-background
primitive, and publishes the state-containing TA and draw packages atomically.
Other renderer backends remain outside the semantic draw v1 admission contract.

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

The HLE DMAREAD and bounded AICA/CD-DA slices are implemented. The real-BIOS
GD-ROM hardware path now has a separate typed artifact and independent
validator. It authenticates the loaded 2 MiB Dreamcast BIOS, preserves the two
SH-4 packet owners, records DMA/PIO byte delivery and interrupt boundaries,
invalidates restored in-flight state, and atomically publishes firmware-bound
packages. Synthetic tests cover Mode 1/Mode 2, 2340/2352 output, gaps,
cache/scheduler boundaries, multiple DMA sessions, PIO ownership and negative
mutations. Independent admission also validates the complete Maple replay
against its retained real-firmware recording identity and joins its boot
authorities to the GD-ROM identity.

The real-BIOS Lodoss admission gate passed on 2026-08-03 using the retail PAL
v1.01d BIOS, its authenticated initial 128 KiB PAL flash image, and the
authenticated populated A1 VMU. The independently validated artifact contains
1,107 hardware events across 21 complete read commands and 773 DMA chunks at
the exact 755-DMA Maple replay boundary. Its v2 package retains the BIOS,
pre-normalization flash, VMU, recording identity, replay, executable, artifact,
validators and publication receipt. A one-byte retained-VMU mutation was
rejected and both forced-abort boundaries remained quarantined.

Lodoss CD-DA real-game qualification remains unclaimed. It is not a toolkit
completion gate: the typed CD-DA slice, independent validator, negative tests
and atomic publisher are implemented, while no synthetic or injected playback
may be promoted as Lodoss behaviour.

Deliverables:

- ATA and SPI packet traces;
- FAD/count/mode and sector byte/digest evidence;
- PIO/DMA chunk, destination, scheduler, completion, and interrupt events;
- CD-DA sector/playback joins;
- AICA channel/common/DSP register writes and ARAM ranges;
- sample-interval artifacts with exact format and digest; and
- disc-to-audio and guest-to-slot ownership joins.

## Phase 6: evidence audit

The repository-wide evidence auditor is implemented. It discovers every known
accepted receipt outside staging/quarantine trees, rejects unknown or ambiguous
accepted directories, rehashes receipt inventories, prohibits linked package
entries and package-local trusted validators, dispatches current external
validators for every package generation, rejects duplicate package UUIDs, and
atomically emits a typed aggregate report. After the documented immutable
provenance migration, the 2026-08-03 post-build Lodoss-wide run found 21
packages; all 21 passed current independent revalidation with zero duplicate
package identities, zero SH-4 job collisions and zero rejected packages. The
report SHA-256 is
`dc294de5af5eddfc455c80fbc1f2cd9863656e718f06ddcdb682ac6e95008a79`.
See
[RepositoryEvidenceAuditV1.md](RepositoryEvidenceAuditV1.md).

External hardware and independent-emulator reference import is deliberately
outside this project's scope. Flycast's authenticated typed framebuffer,
presentation and pre-backend PCM artifacts are the authoritative Dreamcast
observation boundary. No toolkit readiness claim depends on analogue capture,
physical-console output or another emulator.

Deliverables:

- a repository-wide audit command that rejects stale, unbound, or internally
  inconsistent accepted evidence.

All roadmap phases within the declared project scope are implemented and have
passed their current admission gates.

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

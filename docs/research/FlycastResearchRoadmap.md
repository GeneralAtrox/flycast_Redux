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

The first Phase 3 vertical slice is implemented: strict generic manifest
parsing, exact identity cross-binding, a one-shot pre-instruction interpreter
trigger, raw main-RAM range events, incomplete/complete binary finalization,
and the standalone streaming `flycast-research-validate-memory-ranges`
validator. Capture-package v2, call/return events, watchpoints, bounded register
or stack snapshots, Ghidra export schemas, and Lua subscriptions remain open.

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

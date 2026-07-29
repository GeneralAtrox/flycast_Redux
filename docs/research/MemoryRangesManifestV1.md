# Memory-range capture manifest v1

## Purpose

The `flycast-research-memory-ranges-manifest` schema at version 1 is the
game-agnostic request for a bounded SH-4 memory snapshot. It identifies one
guest-PC trigger range and an ordered list of guest virtual-address ranges. A
game research repository owns the concrete manifest instance, addresses,
expected hashes, and semantic names; Flycast owns only this generic schema,
parser, recorder, and validator contract.

This manifest is not a runtime artifact and does not prove that an address is
correct. A later `memory-ranges-v1` artifact must record the observed address,
length, bytes or digest, scheduler tick, event ordinal, and exact manifest
digest. Independent validation decides whether observations match the expected
hashes. It must reject mismatches rather than relocating or searching for
similar bytes.

## Identity bindings

Every manifest binds:

- the boot executable SHA-256;
- an opaque static-analysis artifact id and SHA-256;
- an opaque hook-manifest id and SHA-256; and
- the exact manifest bytes, hashed by the recorder and runtime artifact.

Flycast does not interpret Ghidra symbols, game resource ids, or researcher
meanings. A project may require its Ghidra export in the opaque static-analysis
binding, but that policy remains outside Flycast.

## Trigger and snapshot semantics

The v1 trigger fires once, immediately before execution of the first
interpreter instruction whose executed PC is inside the half-open interval
`[start_address, end_address_exclusive)`. All ordered ranges are read at that
single interpreter boundary. The recorder must not patch guest instructions or
search for expected bytes.

V1 admits at most 64 ranges, each at most 16 MiB, with a required aggregate-byte
ceiling no larger than 1 GiB. Implementations must use checked arithmetic and
reject duplicate range ids, overlapping or wrapping guest ranges, an
empty/inverted trigger, a range sum above `maximum_total_bytes`, or a ceiling
above the implementation's own stricter limit.

The initial runtime implementation is interpreter-only. Dynarec capture is not
admissible until its PC-boundary and memory-read semantics are proven
equivalent.

## Acceptance boundary

The manifest requests, but cannot itself establish:

- exactly one trigger and one event per ordered range;
- a single shared trigger boundary;
- zero dropped events;
- exact executable/static-analysis/hook-manifest/manifest identities;
- expected SHA-256 matches; and
- a natural clean emulator exit.

These conditions belong in the typed runtime artifact and its independent
validator. Atomic multi-artifact publication belongs to capture package v2;
the exact five-entry Maple capture transaction v1 remains unchanged.

The normative JSON shape is
[`flycast-research-memory-ranges-manifest-v1.schema.json`](flycast-research-memory-ranges-manifest-v1.schema.json).

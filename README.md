<img src="shell/linux/flycast.png" alt="flycast logo" width="150"/>

# flycast_Redux

A research build of [Flycast](https://github.com/flyinghead/flycast) for
reverse engineering Dreamcast software. Stock Flycast runs a game. This fork
also records *why* something happened: which SH-4 instruction wrote which
bytes, which DMA carried them to the PowerVR, which render consumed them, and
which Maple, GD-ROM, or AICA traffic was involved. Every observation is typed,
hashed, replayable, and independently validated.

It is game-agnostic. No game addresses, names, or scripts belong here. Game
projects consume the accepted evidence packages this fork produces.

## For agents: read this first

- Upstream Flycast is unchanged in behavior. All research features are inert
  unless enabled by transient launch options (`research.*` keys). Never make
  a research feature default-on.
- The authoritative design is [docs/research/FlycastResearchArchitecture.md](docs/research/FlycastResearchArchitecture.md).
  The phase plan and current status are in [docs/research/FlycastResearchRoadmap.md](docs/research/FlycastResearchRoadmap.md).
  Each artifact has its own `docs/research/*V1.md` or `*V2.md` contract and a
  JSON schema beside it.
- Frozen contracts are listed in [docs/research/V1ContractLock.md](docs/research/V1ContractLock.md).
  Do not change the bytes of a frozen artifact format. Add a new version.
- Lua output, log lines, screenshots, and GDB reads are discovery only. They
  are never accepted evidence. Evidence is native recorder, then independent
  validator, then atomic publication.
- Every new probe lands as one narrow vertical slice, in this order:
  native observation, typed candidate artifact, standalone validator,
  negative tests, capture transaction, accepted evidence. Do not add broad
  logging ahead of that chain.
- Prefer the interpreter backend. Dynarec evidence is admitted only through
  the SH-4 equivalence package.
- Windows is the primary platform. Drivers and tests are PowerShell.

## Layout

| Path | Contents |
| --- | --- |
| `core/research/` | Native recorders, artifact writers, validators, runtimes. One `*_observation`, `*_artifact`, `*_runtime`, `*_validator` set per subsystem. |
| `core/lua/lua.cpp` | `flycast.research` discovery API (subscribe, stats, unsubscribe). |
| `core/debug/gdb_server.cpp` | Read-only, loopback-only GDB stub. Single client, no stepping, bounded reads. |
| `tools/research/` | Standalone `validate_*.cpp` executables and `flycast-*.ps1` capture, publish, validate, audit, and forced-abort drivers. |
| `tools/research/lua/` | Ready-to-run watchers: Maple decode, memory provenance, causal slice, hardware, CD-DA. |
| `tools/ghidra/` | `ExportFlycastResearch.java` for Ghidra program exports. |
| `docs/research/` | Contracts, schemas, operator guides, roadmap. |
| `tests/src/research/` | Round-trip, mutation, truncation, forced-abort, and differential tests. |
| `tests/research/*.ps1` | Process-level transaction tests. |

## Subsystems covered

| Area | Records | Contract |
| --- | --- | --- |
| Identity | SHA-256 of media tracks, IP.BIN, boot binary, BIOS/flash, VMU, build, config | `flycast-research-identity-v1/v2` |
| Maple | DMA requests, responses, schedule, commit; deterministic replay | [MapleTraceV1.md](docs/research/MapleTraceV1.md), [MapleTraceV2.md](docs/research/MapleTraceV2.md) |
| Memory ranges | Bounded RAM snapshots at an exact interpreter PC | [MemoryRangesV1.md](docs/research/MemoryRangesV1.md) |
| SH-4 events | Instructions, BSR/JSR/RTS with delay-slot ownership, watchpoints, exceptions | [Sh4EventsV1.md](docs/research/Sh4EventsV1.md), [Sh4ObservationV1.md](docs/research/Sh4ObservationV1.md) |
| SH-4 equivalence | Interpreter vs dynarec differential trace and comparator | [Sh4EquivalenceV1.md](docs/research/Sh4EquivalenceV1.md) |
| SH-4 profile | Bounded dynarec block and branch profile | [Sh4DynarecProfileV1.md](docs/research/Sh4DynarecProfileV1.md) |
| Ghidra | Functions, symbols, types, memory blocks; runtime/static joins | [GhidraExportV1.md](docs/research/GhidraExportV1.md), [Sh4GhidraSemanticJoinV1.md](docs/research/Sh4GhidraSemanticJoinV1.md) |
| PowerVR | Exact 32-byte TA blocks, source path, initiator, STARTRENDER, render-done, draw ownership, presentation | [PvrTaArtifactV1.md](docs/research/PvrTaArtifactV1.md), [PvrDrawOwnershipV1.md](docs/research/PvrDrawOwnershipV1.md) |
| GD-ROM | ATA/SPI packets, sector identity, DMA/PIO completion, hardware capture | [FlycastGdromCausalEvidence.md](docs/research/FlycastGdromCausalEvidence.md) |
| AICA / CD-DA | Channel, common, DSP writes, ARAM ranges, sample intervals, CD-DA joins | [FlycastAicaCausalEvidence.md](docs/research/FlycastAicaCausalEvidence.md), [FlycastCddaCausalEvidenceV2.md](docs/research/FlycastCddaCausalEvidenceV2.md) |
| Packaging | Atomic multi-artifact publication with receipt and quarantine | [CapturePackageV2.md](docs/research/CapturePackageV2.md), [CaptureTransactionV1.md](docs/research/CaptureTransactionV1.md) |
| Audit | Repository-wide revalidation of accepted evidence | [RepositoryEvidenceAuditV1.md](docs/research/RepositoryEvidenceAuditV1.md) |
| Agent control | Launch-time orchestration, status, clean exit, PC checkpoint | [ResearchAgentControlV1.md](docs/research/ResearchAgentControlV1.md) |
| Lua | Discovery subscriptions over the native buses | [LuaSubscriptionsV1.md](docs/research/LuaSubscriptionsV1.md) |

## Build and test

```powershell
cmake -S . -B build-research -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_RESEARCH_TOOLS=ON -DENABLE_CTEST=ON
cmake --build build-research --config RelWithDebInfo
ctest --test-dir build-research -C RelWithDebInfo `
  -R flycast-research-tests --output-on-failure
```

`BUILD_RESEARCH_TOOLS=ON` builds every `flycast-research-validate-*`
executable plus `flycast-research-tests`. Without it the fork builds like
upstream Flycast. The libretro core never includes research code.

## Typical capture

```text
flycast-start-controlled.ps1     launch with identity, replay, manifests, recorders
   -> emulator writes private candidate artifacts
   -> flycast-capture-*.ps1 / flycast-publish-*.ps1 stop the owned process,
      re-authenticate inputs, run validators, publish by one rename
   -> flycast-audit-evidence.ps1 revalidates the accepted tree later
```

Start with Maple v1. It is the base every other package revalidates.

## Agent-friendliness status

This codebase does not yet meet the agent working rule of 400 lines per
source file. Counts as of 2026-09-13:

| Scope | Files | Over 400 lines |
| --- | --- | --- |
| Research code, tools, tests | 214 | 60 |
| Whole repo excluding `core/deps` | 812 | 210 |

Largest offenders in research code are `core/lua/lua.cpp`,
`tools/research/flycast-capture-maple.ps1`, `tools/research/validate_capture.cpp`,
and `core/research/sh4_observation_runtime.cpp`. Rules for agents working here:

- New research files stay under 400 lines. Split by subsystem and role, not
  by arbitrary line count.
- When touching an oversized research file, extract the part you change into
  a new file under the limit rather than growing it.
- Upstream Flycast files are exempt. Keep diffs to them minimal so upstream
  merges stay cheap.
- Do not reformat, rename, or reorganize files you are not otherwise
  changing.

## Upstream

Based on flyinghead/flycast. Upstream README, license, and build notes for
other platforms apply unchanged. See [LICENSE](LICENSE).

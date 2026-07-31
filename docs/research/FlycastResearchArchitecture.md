# Flycast Research Toolkit Architecture

## Purpose

This fork adds game-agnostic reverse-engineering support to Flycast. It is an
evidence producer and replay environment, not a repository for conclusions,
addresses, or contracts belonging to one game.

The reusable unit is a bounded causal slice:

1. an identified Dreamcast software and machine state;
2. a guest-side action at an exact SH-4 scheduler boundary;
3. the Maple, GD-ROM, AICA, or PowerVR work caused by that action;
4. a typed artifact describing the observation;
5. an independent validation result; and
6. optional static-analysis evidence joined by explicit identities and
   addresses.

An artifact is evidence only when every required join succeeds. A screenshot,
log line, memory dump, or successful replay by itself is not accepted evidence.

## Non-goals

- No game-specific addresses, names, structures, scripts, or assertions belong
  in this repository.
- The toolkit does not claim that Flycast output is hardware evidence.
- The toolkit does not infer semantic ownership from "the latest value" seen on
  the CPU thread.
- Lua is a discovery and orchestration surface. Lua output is not authoritative
  unless a native typed recorder and validator cover the same observation.
- A normal emulator save state is not an identity manifest or a replay proof.

## Evidence classes

Every report must label observations using one of these classes:

| Class | Meaning |
| --- | --- |
| `hardware` | Captured from a physical Dreamcast with identified acquisition hardware and settings. |
| `independent-emulator` | Captured by an independently implemented emulator under an identified configuration. |
| `flycast-original` | Captured by this fork from an identified Flycast build and configuration. |
| `static-analysis` | Produced by Ghidra or another identified static-analysis project/export. |
| `reconstruction` | Produced by source code or another implementation being compared with evidence. |

Pixel equality between `reconstruction` and `flycast-original` does not upgrade
either side to hardware provenance.

## Evidence transaction

Capture is a transaction, not a collection of unrelated output files.

```text
authenticate inputs
    -> create private candidates
    -> run an owned Flycast process
    -> observe an exact terminal condition
    -> stop that exact process
    -> re-authenticate immutable inputs
    -> run independent validators
    -> publish all artifacts atomically
```

The emulator writes only candidate artifacts. A capture driver owns publication.
The driver must use same-volume renames and must never expose a partially
validated artifact under an accepted path. A failed or interrupted capture is
quarantined. Forced-abort tests must prove that no accepted output is published,
only the owned process is stopped, and unrelated sentinel processes and files
survive.

## Identity

Every typed artifact embeds the SHA-256 digest of an immutable
`flycast-research-identity-v1` manifest. The manifest is itself retained as an
artifact. The digest covers its exact bytes; changing whitespace creates a new
identity.

The v1 manifest binds at least:

- media container or descriptor bytes;
- every source track or CHD/CDI object, including size and SHA-256;
- the exact IP.BIN bytes;
- the boot executable bytes and load identity;
- real BIOS and initial flash bytes, or an explicit HLE BIOS identity;
- initial VMU and other persistent Maple media used by the run;
- Flycast Git commit and executable SHA-256;
- the complete effective research-relevant configuration;
- optional Ghidra project/export identity and hook-manifest digest.

Flycast's existing network MD5 is insufficient. For GDI and CUE media it hashes
track contents as one stream, but it does not identify the descriptor, individual
tracks, boot executable, firmware, persistent devices, executable build, or
effective configuration.

Paths are descriptive metadata. Acceptance is based on sizes, hashes, and typed
cross-field relationships. Capture inputs are authenticated before the emulator
starts and again after the owned process has stopped.

## Dreamcast subsystem mapping

| General concept | Dreamcast/Flycast boundary | Required typed evidence |
| --- | --- | --- |
| Guest execution | SH-4 interpreter instruction boundary, including delay-slot state | executed PC, opcode, pre/post registers, scheduler tick, exception/delay-slot metadata |
| Calls and returns | SH-4 BSR/JSR/RTS/RTE semantics | caller/callee, PR, stack identity, delay-slot ownership |
| Input and peripherals | Maple DMA request/response boundary | descriptor, bus/port/device, exact request and response, schedule and commit ticks |
| Disc I/O | GD-ROM ATA/SPI command acceptance and DMA/PIO completion | packet, FAD/count/mode, sector digest or bytes, destination, interrupt and timing |
| Geometry submission | PowerVR Tile Accelerator input | exact 32-byte TA blocks, source path, context/list/pass, initiating SH-4 token |
| Render state | PVR register writes and STARTRENDER | register deltas, TA context generation, render-start and render-done generations |
| Textures/framebuffers | VRAM writes, DMA, render-to-texture, framebuffer SOF changes | byte ranges, source, tick, consuming render generation |
| Presentation | renderer completion and backend present/readback | render generation, framebuffer generation, present generation, exact pixel bytes |
| Audio | AICA channel/common/DSP writes and sample production | slot/register state, ARAM ranges, CD-DA sector identity, sample interval and digest |

PowerVR ownership must follow submitted work. A CPU value sampled immediately
before presentation is not automatically the owner of that frame. The ownership
chain is:

```text
SH-4 initiator
    -> TA write source (store queue, channel-2 DMA, or sort DMA)
    -> TA context/list/pass
    -> STARTRENDER generation
    -> render-complete generation
    -> framebuffer/present generation
    -> pixel artifact
```

## Native recorder rules

Recorders are synchronous at the semantic boundary for the initial schemas. This
avoids drops and ambiguous cross-thread ownership. Performance-oriented queues
may be added later only with explicit capacity, drop counters, generation tokens,
and fail-closed validation.

All binary formats use:

- an eight-byte magic;
- an immutable schema version;
- an endian sentinel and fixed header size;
- an incomplete/complete state;
- the identity-manifest SHA-256;
- a payload SHA-256;
- exact event, transaction, and byte counts;
- a dropped-event count that must be zero for production acceptance; and
- little-endian scalar encoding independent of host ABI.

The initial header is incomplete. Graceful finalization writes final counts and
digests and changes the state to complete. A crash, forced abort, write failure,
open DMA, or unmatched event leaves the artifact non-production.

Schema versions are immutable after an artifact is accepted. Incompatible
changes require a new schema version and validator path.

The [SH-4 native observation contract](Sh4ObservationV1.md) is the shared
in-process semantic boundary for interpreter recording, dynarec equivalence,
and [Lua discovery subscriptions](LuaSubscriptionsV1.md). It is not itself an evidence
artifact. Accepted evidence still requires a typed recorder and independent
validator.

Canonical event construction is implemented once in the backend-neutral SH-4
observation runtime. Interpreter and dynarec entry points differ only in their
backend tag and where the precise instruction boundary is reached. Per-backend
atomic activity gates prevent an interpreter-only subscription from allocating
dynarec frames or snapshots. A dynarec marker must first make its allocated
architectural registers coherent in `Sh4Context`; otherwise a structurally
valid event would still contain stale guest state.

Dynarec instruction instrumentation is opt-in at decode time through the
transient `research.DynarecObservation` option. Normal blocks contain no marker
operations. Research blocks use explicit SHIL begin/end barriers implemented by
the x64, ARM32, and ARM64 compilers; every barrier writes allocated guest state
back before entering the common runtime. Marker metadata reconstructs per-op
ticks despite block-up-front cycle charging and preserves the interpreter's
taken/non-taken conditional delay-slot ownership from the condition latched
before the slot executes. Nested marker ticks follow the interpreter's
delay-slot execution order rather than the decoder's static cycle accumulation.
A precise per-instruction SR.FD guard replaces the legacy MMU block-entry FPU
check only inside observed blocks, including slot-FPU ownership and unconsumed
cycle restoration. Research compilation disables instruction-eliding division
aggregation and single-branch-target folding. Real interpreter/dynarec
differential fixtures separately prove invalidation and semantic equivalence;
the external Lodoss exercise was the final admission gate and passed on
2026-07-31 with 574,470 matched events, zero drops, and no first divergence.

Dynarec memory ownership is attached to each `readm`/`writem` operation with a
research-only pre/post marker pair. The pre-marker captures the effective guest
address and write value; the post-marker publishes after success and obtains a
load result from coherent `Sh4Context`. This placement is shared by fastmem,
rewritten slow access, MMU, and immediate-address compilation. Faulted accesses
are discarded with their instruction frame rather than being reported as
completed memory operations.

Synchronous exceptions converge at the common pre-transition `Do_Exception`
hook. It publishes one canonical event for the innermost owner and then aborts
all open delay-chain frames before any dynarec exception trampoline resumes the
dispatcher. Interrupts occur without instruction ownership and use the existing
trace contract's depth-zero exception representation, with backend selected by
the active SH-4 executor configuration and tick taken at the scheduler boundary.

The [SH-4 backend observation trace](Sh4ObservationTraceV1.md) serializes that
boundary separately for interpreter and dynarec runs. Each fail-closed stream
binds its own backend identity plus one shared replay and manifest set. The
equivalence comparator independently decodes both streams, checks SH-4
instruction/control-flow ownership, and returns the exact matching prefix or
first divergent semantic field. The typed
[equivalence job/report](Sh4EquivalenceV1.md) authenticates that comparison and
its complete input set. The
[equivalence package](Sh4EquivalencePackageV1.md) localizes and locks that
complete input set, independently recomputes an equivalent report, issues a
deterministic receipt, and publishes by one sibling-directory rename. A pair
of trace files or a standalone report is not accepted evidence.

## Maple v1 vertical slice

The first end-to-end slice records the semantic boundary already present in
`maple_DoDma`: the canonical request delivered to `maple_device::RawDma` and the
canonical response returned from it. It also records DMA begin, scheduling, and
guest-memory commit events.

The event order for each DMA is:

```text
DmaBegin
    -> zero or more Transaction events
    -> DmaSchedule
    -> DmaCommit
```

Production validation requires:

- strictly increasing global event ordinals;
- strictly increasing DMA and transaction ordinals;
- monotonic SH-4 scheduler ticks;
- exact nesting and cardinality for each DMA;
- valid bus, port, device, command, request, and response sizes;
- scheduled and committed response counts that match the transactions;
- no abort event, open DMA, pending commit, extra event, or dropped event;
- exact payload length and SHA-256;
- a complete header whose counts agree with independently parsed events; and
- an identity digest equal to the separately supplied manifest.

Replay validates every DMA field and request byte before using the recorded
response. A mismatch terminates replay; it never falls back to live input. The
live device handler may be shadow-executed so deterministic VMU, clock, and
rumble side effects still occur, but its response is not delivered to the guest.
Initial VMU and other persistent-device files therefore remain part of identity.

The trace records both planned and actual commit ticks. Vblank-triggered and
software-triggered DMA are distinct. Byte order is captured after Flycast's
`SB_MMSEL` conversion, at the device semantic boundary, while descriptor words
retain their guest-visible values.

## Static-analysis joins

Static exports are separate typed artifacts. A Ghidra join must bind:

- Ghidra project and program identity;
- imported executable SHA-256 and image base;
- function entry and body ranges;
- symbol or structure identity;
- exporter version and script SHA-256; and
- the native hook manifest used by the runtime capture.

Runtime PC membership is accepted only when address-space conversion and image
identity are explicit. A matching symbol name is not sufficient.

## Safety invariants

- Research options are inert by default.
- Record and replay modes are mutually exclusive.
- Identity, replay input, and output paths may not alias.
- Recorder output must not exist before capture and is created exclusively.
- Replay consumes only a complete, production-valid trace.
- Research mode disables state auto-load, GGPO input, dynarec execution, and
  threaded rendering for the initial schemas.
- Reset, reconnect, device-topology changes, and persistent-device mutation are
  typed events or acceptance failures; they are never silently ignored.
- Validators return a non-zero status for malformed input, identity mismatch,
  unsupported schema, truncation, growth during read, trailing bytes, count
  mismatch, digest mismatch, or impossible event ordering.

## Planned phases

1. **Evidence core and Maple v1**: identity contract, typed binary envelope,
   deterministic Maple record/replay, validator, corruption/divergence tests.
2. **Capture transaction driver**: private staging, exact process ownership,
   source re-authentication, atomic publication, quarantine, forced-abort tests.
3. **SH-4 observation and composition**: non-mutating interpreter hooks,
   delay-slot-correct calls and returns, memory watchpoints, typed stacks,
   Ghidra joins, and capture-package-v2 atomic composition over an immutable
   Maple v1 base.
4. **PowerVR ownership**: TA source tokens, raw parameter streams, PVR registers,
   VRAM writes, render/present generations, backend-independent frame artifacts.
5. **GD-ROM and AICA**: command/sector/transfer traces, AICA slot and ARAM
   ownership, CD-DA linkage, bounded audio artifacts.
6. **Comparison workflows**: exact frame/audio differences, temporal deltas,
   independent-emulator and hardware evidence import, aggregate audit reports.
7. **Lua research API**: subscriptions and orchestration over the same native
   typed events, with explicit discovery-versus-evidence labeling.

## Reference boundary

The Dolphin research fork and PCSX-Redux are architectural references. Reuse
their evidence discipline, debugger ergonomics, typed memory views, scripting,
and static-analysis bridges. Do not copy GameCube addresses, PowerPC stack
assumptions, GX FIFO semantics, PlayStation MIPS/GPU assumptions, or any
game-specific contract. Every concept must be mapped independently to SH-4,
Maple, GD-ROM, AICA, and the PowerVR Tile Accelerator.

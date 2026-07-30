# SH-4 events artifact v1

## Purpose and boundary

`flycast-sh4-events-v1` is a game-agnostic stream of selected SH-4 interpreter
facts. A separately supplied
[`flycast-research-sh4-events-manifest` v1](flycast-research-sh4-events-manifest-v1.schema.json)
defines exact function-entry PCs, non-overlapping function intervals, bounded
call/return snapshots, and non-overlapping memory watch ranges. Flycast does
not contain Lodoss addresses, symbols, data meanings, expected results, or
relocation rules.

The v1 authority is deliberately narrow:

- interpreter execution only;
- BSR, BSRF, and JSR calls whose computed target is an exact manifest entry;
- LIFO RTS returns from inside the selected hook interval to the recorded
  call-site return PC;
- successful ordinary interpreter `ReadMem`/`WriteMem` data paths, excluding
  instruction fetch and 32-byte store-queue commit destinations;
- exceptions while a selected hook is executing or an invocation is open;
- R0-R15, PR, GBR, VBR, MACH, MACL, SR, FPUL, and FPSCR at call,
  completed return, and exception;
- bounded absolute or R0-R15-relative memory snapshots at call or return;
- synchronous writing with no queue and therefore no accepted drops; and
- normal emulator unload required to mark the artifact complete.

The call event is recorded before its delay slot. A return event is recorded
after the RTS delay slot, so the register image includes the returned R0 and
the resumed PC. Data accesses made by a delay-slot instruction carry depth 1.
An exception in a delay slot is emitted once while the nested interpreter
frames unwind.

Normal unload finalization synchronizes with the interpreter's current nested
instruction scope. If another host thread requests unload while an opcode or
its delay slot is active, finalization waits for that scope to end before it
checks balanced invocations and publishes the complete header. If host event
handling requests unload reentrantly from the interpreter thread, publication
is deferred to the successful end of the outermost instruction scope. Every
stop request is generation-stamped before it can wait on the instruction
scope. An instruction abort after such a request, or a dirty request observed
before deferred publication, downgrades the candidate to incomplete. Dirty
unload, reset, and capture-hook failures abandon the candidate; a hook failure
also releases its runtime-owned instruction scope before it is rethrown.

Capture configuration forces interpreter execution with threaded rendering
disabled, and normal emulator unload stops the SH-4 executor before requesting
finalization. A clean request from another host thread may therefore wait for
the bounded current instruction scope; there is deliberately no timeout path
that could destroy a session while the interpreter still owns it. If deferred
finalization itself fails, the candidate remains incomplete and the failure is
logged instead of being injected into interpreter dispatch. When no session is
armed, an atomic fast path keeps ordinary interpreter memory accesses from
taking the lifecycle mutex.

## Identity join

The manifest is read as exact bytes and its SHA-256 is embedded in the
artifact. Flycast and the standalone validator both require:

| Manifest binding | Identity-manifest authority |
| --- | --- |
| `executable_sha256` | `media.boot_executable.sha256` and `static_analysis.program_sha256` |
| `static_analysis_sha256` | `static_analysis.export_sha256` |
| `hook_manifest_sha256` | `static_analysis.hook_manifest_sha256` |

The opaque static-analysis and hook-manifest ids remain in the exact manifest
bytes. The artifact also embeds the exact identity-manifest digest. Editing
either JSON file, including whitespace, breaks the join.

Hook intervals and watch ranges may not overlap. Snapshot ids are globally
unique, their declared sizes are at most 4 KiB, and each phase is checked
against both per-event and capture-wide limits. A required snapshot must map
to contiguous guest RAM; an optional unavailable snapshot is retained as an
explicit zero-length record.

## Binary envelope

All integers are little-endian. The fixed 232-byte header is:

| Offset | Size | Field |
| ---: | ---: | --- |
| `0x00` | 8 | ASCII magic `FCSH4EVT` |
| `0x08` | 4 | schema version, exactly `1` |
| `0x0c` | 4 | header size, exactly `232` |
| `0x10` | 4 | endian sentinel `0x01020304` |
| `0x14` | 4 | flags; bit 0 is complete and all other bits are zero |
| `0x18` | 8 | total event count |
| `0x20` | 8 | call count |
| `0x28` | 8 | return count |
| `0x30` | 8 | watched-read count |
| `0x38` | 8 | watched-write count |
| `0x40` | 8 | snapshot-record count |
| `0x48` | 8 | raw snapshot-byte count |
| `0x50` | 8 | exception count |
| `0x58` | 8 | first event scheduler tick |
| `0x60` | 8 | last event scheduler tick |
| `0x68` | 8 | payload byte count |
| `0x70` | 8 | dropped-event count |
| `0x78` | 8 | maximum observed open-invocation depth |
| `0x80` | 32 | identity-manifest SHA-256 |
| `0xa0` | 32 | SH-4 events manifest SHA-256 |
| `0xc0` | 32 | payload SHA-256 |
| `0xe0` | 4 | reserved zero |
| `0xe4` | 4 | CRC-32 of header bytes `[0x00, 0xe4)` |

The destination is created exclusively and immediately receives a durable
incomplete header. Finalization rewrites only the header after the payload is
durable. A crash, reset, dirty unload, exception limit, missing required
snapshot, byte ceiling, unbalanced invocation, unmet acceptance minimum, or
I/O failure therefore leaves a production-invalid candidate.

## Events

Each event begins with:

```text
uint32 type
uint32 event_size_including_header
uint64 contiguous_event_ordinal
```

### Call, type 1

```text
uint64 scheduler_tick
uint64 invocation_id
uint32 zero_based_hook_index
uint16 call_kind                 // 1 BSR, 2 BSRF, 3 JSR
uint16 opcode
uint32 call_pc
uint32 computed_target_pc
uint32 return_pc                 // call_pc + 4
uint32 delay_slot_pc             // call_pc + 2
uint32 delay_slot_depth          // exactly 0
uint32 snapshot_count
register_snapshot registers
snapshot_record[snapshot_count]
```

The independent validator decodes the opcode again and recomputes the target
from the recorded PC and registers. The target must equal the selected hook
entry and invocation ids must be contiguous.

### Return, type 2

```text
uint64 rts_instruction_tick
uint64 completion_tick
uint64 invocation_id
uint32 zero_based_hook_index
uint16 opcode                    // exactly RTS, 0x000b
uint16 reserved_zero
uint32 rts_instruction_pc
uint32 resumed_pc
uint32 expected_return_pc
uint32 delay_slot_depth          // exactly 0
uint32 snapshot_count
uint32 reserved_zero
register_snapshot registers
snapshot_record[snapshot_count]
```

Returns close the newest open invocation. The RTS must be inside that hook's
declared interval, and both resumed fields must equal the call's recorded
return PC.

### Watched data access, type 3

```text
uint64 scheduler_tick
uint32 zero_based_watch_index
uint32 instruction_pc
uint32 accessed_address
uint8  width                     // 1, 2, 4, or 8
uint8  kind                      // 1 read, 2 write
uint16 delay_slot_depth          // 0 or 1
uint64 value                     // masked to width
```

The access must overlap its manifest range and use an enabled direction. Read
events carry the value returned by the memory path; write events are emitted
after the underlying write returns.

### Exception, type 4

```text
uint64 scheduler_tick
uint32 active_instruction_pc
uint32 exception_pc
uint32 vector_pc
uint32 exception_code
uint16 delay_slot_depth          // 0 or 1
uint16 reserved_zero
register_snapshot registers
```

The active instruction must be inside a hook unless an invocation is open.
For a delay-slot fault, `active_instruction_pc` remains the slot PC while
`exception_pc` and the illegal/FPU-disabled code are adjusted to the owning
branch exactly as the SH-4 exception path adjusts them.
The vector must be VBR + `0x400` for TLB-miss read/write and VBR + `0x100`
for other recorded instruction exceptions.

### Register and memory snapshots

`register_snapshot` is 96 bytes: R0-R15 followed by PR, GBR, VBR, MACH, MACL,
SR, FPUL, and FPSCR, all as `uint32`.

Each memory snapshot is:

```text
uint32 zero_based_definition_index
uint32 derived_address
uint32 declared_length
uint32 captured_length
uint32 flags                     // bit 0 available, bit 1 required
uint32 reserved_zero
byte[32] captured_sha256         // zero when unavailable
byte[captured_length] raw_memory
```

Snapshot definitions appear in manifest order for the event phase. The
validator re-derives register-relative addresses, hashes the raw bytes, and
checks availability, required state, sizes, counts, and limits.

## Record and validate

All research paths are transient and must be mutually distinct from identity,
Maple, memory-range, and SH-4 inputs and outputs.

```powershell
& .\flycast.exe `
  -config "research:IdentityManifest='C:\evidence\identity.json',research:Sh4EventsManifest='C:\evidence\sh4-events.json',research:Sh4EventsRecord='C:\evidence\sh4-events.candidate.fcsh4'" `
  "D:\games\disc.gdi"
```

The default artifact ceiling is 256 MiB. Override it transiently with
`research:Sh4EventsMaxBytes=<count>`. Close or unload Flycast normally after
the manifest acceptance minimums have been reached.

Validate in a separate process:

```powershell
& .\flycast-research-validate-sh4-events.exe `
  --artifact "C:\evidence\sh4-events.candidate.fcsh4" `
  --identity "C:\evidence\identity.json" `
  --manifest "C:\evidence\sh4-events.json"
```

Exit status `0` accepts the typed artifact, `1` rejects content or identity,
and `2` reports command-line misuse. The validator independently checks the
header, CRC, exact-file digests, payload digest, event grammar, opcode-derived
targets, LIFO returns, snapshot bytes, watch permissions, exception ownership,
monotonic ticks, acceptance minimums, balanced calls, and zero drops.

The deterministic mixed-event test artifact is locked by complete-file
SHA-256:

```text
f3513e69e6634983114574ea555f64693c3f0e0d4fef70780eba02e08a72cff0
```

An incompatible binary or validation change requires a new artifact version
and validator path.

The candidate alone is not an atomic multi-artifact evidence package.
Capture-package v2 will bind this artifact to the existing immutable Maple v1
record without changing Maple v1 or capture-transaction v1.

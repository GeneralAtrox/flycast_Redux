# Maple research trace v1

## Status and scope

`flycast-maple-trace-v1` is the first game-agnostic vertical slice of the
Flycast research toolkit. It records and replays the semantic Maple boundary:
DMA ownership, canonical requests, canonical responses, scheduling, and guest
memory commit timing.

The recorder produces a **candidate artifact**. The independent validator can
accept the candidate's format and causal invariants, but that exit status alone
does not authorize publication. Use Phase 2's staged transaction in
[CaptureTransactionV1.md](CaptureTransactionV1.md) to authenticate inputs,
establish process ownership, validate stable terminal bytes, and publish one
atomic evidence package.

## Build and test

Enable the research tools when configuring CMake:

```powershell
cmake -S . -B build-research -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_RESEARCH_TOOLS=ON -DENABLE_CTEST=ON
cmake --build build-research --config RelWithDebInfo `
  --target flycast-research-validate-maple flycast-research-tests
ctest --test-dir build-research -C RelWithDebInfo `
  -R flycast-research-tests --output-on-failure
```

The standalone validator is built only when `BUILD_RESEARCH_TOOLS=ON`. All
runtime research options are inert by default.

## Prepare an identity manifest

Supply an immutable manifest conforming to
`flycast-research-identity-v1.schema.json`. Its exact file bytes are hashed and
that SHA-256 is embedded in the trace. Whitespace changes therefore create a
new identity.

The following values are mandatory in `configuration.values` for Maple v1:

```json
{
  "autoload_state": false,
  "autosave_state": false,
  "cpu_backend": "interpreter",
  "ggpo": false,
  "threaded_rendering": false
}
```

Additional effective configuration may be included. The declared
`configuration.sha256` is SHA-256 over the compact UTF-8 serialization of the
`values` object produced by `nlohmann::json::dump()`; object keys are ordered
lexicographically.

The emulator validates the manifest schema, configuration digest, and the five
mandatory runtime values. It then forces and rechecks the corresponding
Flycast options. It does **not yet** re-hash every media, firmware, executable,
or persistent-device path named by the manifest. Those inputs must be hashed
by trusted preparation tooling; the Phase 2 capture transaction performs
pre-run and post-run authentication automatically.

## Record

The output path must not already exist. It must not alias the identity path,
and its parent directory must already exist.

```powershell
& .\flycast.exe `
  -config "research:IdentityManifest='C:\evidence\identity.json',research:MapleRecord='C:\evidence\maple.candidate.fcmr'" `
  "D:\games\disc.gdi"
```

Flycast creates the output exclusively and immediately writes an incomplete
header. Unload the game or exit Flycast normally to finalize it. A crash,
forced termination, write failure, replay divergence, reset, topology change,
unsupported control descriptor, open DMA, or pending commit leaves an
incomplete or production-invalid candidate.

The default trace input and output limit is 512 MiB. Exceeding the output limit
invalidates the candidate. Override it transiently when needed:

```text
research:MapleTraceMaxBytes=1073741824
```

For automated paired captures, the optional transient
`research:MapleDmaCheckpoint=N` pauses Flycast immediately after the Nth DMA
commit has been recorded or replayed. A nonzero value must be present as
`configuration.values.maple_dma_checkpoint` in the bound identity. Recording
and both backend replays use the same value, so clean unload occurs at one
exact scheduler boundary rather than at an operator-timed window close. Zero
keeps the existing behavior and adds no checkpoint work to normal execution.

## Replay

Use the same identity, media, firmware, initial persistent devices, controller
topology, and effective configuration:

```powershell
& .\flycast.exe `
  -config "research:IdentityManifest='C:\evidence\identity.json',research:MapleReplay='C:\evidence\maple.candidate.fcmr'" `
  "D:\games\disc.gdi"
```

Replay first validates the complete artifact. At each transaction it compares
DMA identity, scheduler ticks, descriptor words, destination, device type,
bus, port, command, flags, and every request byte. It shadow-executes the live
device handler for deterministic VMU/clock/rumble side effects, discards that
handler's response, and injects the recorded response. The first mismatch
terminates replay; there is no fallback to live input.

## Validate independently

Run the standalone executable, not the recording process, against a separately
supplied identity manifest:

```powershell
& .\flycast-research-validate-maple.exe `
  --trace "C:\evidence\maple.candidate.fcmr" `
  --identity "C:\evidence\identity.json"
```

Exit status is:

- `0`: accepted as a structurally and causally valid v1 candidate;
- `1`: rejected artifact or identity; or
- `2`: invalid command-line usage.

Acceptance checks include the identity digest, complete flag, endian sentinel,
header CRC-32, exact payload length and SHA-256, counts, monotonically ordered
ticks and ordinals, DMA lifecycle, request/response shape, topology fields,
wire-byte accounting, and absence of drops, aborts, trailing bytes, open DMA,
or pending work. The standalone and capture validators read one event at a time
and incrementally hash the payload; they do not load or retain the complete
trace. The maximum trace size is therefore a validation ceiling rather than a
memory-allocation size.

## Binary envelope

All scalars are little-endian and host-ABI independent. The file consists of a
160-byte header followed by typed events. Every event has a 16-byte envelope:
event type, total event size, and global ordinal. The fixed header contains:

- magic `FCRMAPLE`;
- schema version `1`, header size, and endian sentinel `0x01020304`;
- incomplete/complete flags;
- event, transaction, DMA, tick, payload-byte, and dropped-event counts;
- identity-manifest and payload SHA-256 digests; and
- header CRC-32.

The permitted production order for each DMA is:

```text
DmaBegin -> Transaction* -> DmaSchedule -> DmaCommit
```

`DmaAbort` exists to retain diagnostic causality but always makes a production
candidate invalid.

## V1 limitations

- Only ordinary Maple START descriptors are accepted. SDCKB occupy/cancel,
  RESET, and NOP descriptors are not representable in v1. Typed NOP support is
  defined by [Maple trace v2](MapleTraceV2.md).
- Reset and controller hot-plug/reconnect invalidate an active session.
- The format claims emulator-observed timing, not physical bus timing.
- Replay begins from normal boot; savestate-based starts are not supported.
- Network/rollback sessions and threaded rendering are not supported.
- Atomic publication, source re-authentication, quarantine, and forced-abort
  process ownership are supplied by the Phase 2 capture driver; they remain
  outside the binary trace format itself.

See `FlycastResearchArchitecture.md` for the evidence model and
`FlycastResearchRoadmap.md` for the subsequent SH-4, PowerVR, GD-ROM, AICA,
comparison, and Ghidra-join work.

The byte and JSON-schema fingerprints that freeze v1 are recorded in
[V1ContractLock.md](V1ContractLock.md). An incompatible change requires a new
format or package version and a separate validator path.

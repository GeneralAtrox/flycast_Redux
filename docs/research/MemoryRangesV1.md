# Memory-ranges artifact v1

## Purpose and scope

`flycast-memory-ranges-v1` is a game-agnostic, one-shot SH-4 RAM snapshot. A
separately supplied
[`flycast-research-memory-ranges-manifest` v1](MemoryRangesManifestV1.md)
selects the interpreter-PC trigger and the ordered virtual-address ranges.
Flycast does not contain a game address, symbol, resource meaning, expected
byte sequence, or relocation rule.

The initial implementation is deliberately narrow:

- interpreter execution only;
- exactly one trigger and one synchronous snapshot;
- contiguous Dreamcast main-RAM ranges accepted through `GetMemPtr`;
- raw bytes retained in the local candidate so an independent process can
  recompute every SHA-256;
- no queue and therefore no accepted nonzero drop count; and
- clean emulator unload required to change the header from incomplete to
  complete.

An expected-byte mismatch does not cause address search or relocation. The
candidate can retain the actually observed bytes, but the independent
validator rejects it.

## Identity join

The manifest is read as exact bytes and its SHA-256 is embedded in the
artifact. Before arming, Flycast and the independent validator require these
joins:

| Manifest binding | Identity-manifest authority |
| --- | --- |
| `executable_sha256` | `media.boot_executable.sha256` and `static_analysis.program_sha256` |
| `static_analysis_sha256` | `static_analysis.export_sha256` |
| `hook_manifest_sha256` | `static_analysis.hook_manifest_sha256` |

The opaque static-analysis and hook-manifest ids remain in the exact manifest
bytes. The artifact additionally embeds the exact identity-manifest digest.
Changing either JSON file, including whitespace, changes the accepted join.

## Binary envelope

All integers are little-endian. The fixed 192-byte header is:

| Offset | Size | Field |
| ---: | ---: | --- |
| `0x00` | 8 | ASCII magic `FCMEMRNG` |
| `0x08` | 4 | schema version, exactly `1` |
| `0x0c` | 4 | header size, exactly `192` |
| `0x10` | 4 | endian sentinel `0x01020304` |
| `0x14` | 4 | flags; bit 0 is complete and all other bits are zero |
| `0x18` | 8 | total event count |
| `0x20` | 8 | trigger-event count |
| `0x28` | 8 | range-event count |
| `0x30` | 8 | total raw memory bytes |
| `0x38` | 8 | start scheduler tick |
| `0x40` | 8 | end scheduler tick |
| `0x48` | 8 | payload byte count |
| `0x50` | 8 | dropped-event count |
| `0x58` | 32 | identity-manifest SHA-256 |
| `0x78` | 32 | memory-ranges manifest SHA-256 |
| `0x98` | 32 | payload SHA-256 |
| `0xb8` | 4 | reserved zero |
| `0xbc` | 4 | CRC-32 of header bytes `[0x00, 0xbc)` |

The writer exclusively creates the destination and immediately flushes an
incomplete header. Finalization rewrites only the header after all payload
bytes and digests are durable. A crash, forced stop, reset, topology change,
unsupported range, I/O failure, missing trigger, or dirty emulator unload
leaves a production-invalid candidate.

## Events

Every event starts with a 16-byte header:

```text
uint32 type
uint32 event_size_including_header
uint64 contiguous_event_ordinal
```

Event 0 is the sole trigger event (`type = 1`):

```text
uint64 scheduler_tick
uint32 executed_pc
uint32 reserved_zero
```

Each following event is one manifest range in manifest order (`type = 2`):

```text
uint64 scheduler_tick
uint32 zero_based_range_index
uint32 guest_virtual_address
uint32 byte_length
uint16 range_id_utf8_length
uint16 reserved_zero
byte[32] observed_sha256
byte[] range_id_utf8
byte[] raw_guest_memory
```

Every range tick must equal the trigger tick. The validator checks the index,
id, address, length, event size, raw-byte digest, expected manifest digest,
counts, total bytes, payload digest, header CRC, zero drops, and exact identity
digests. It streams the raw data rather than retaining an allowed 1 GiB
artifact in memory.

The deterministic two-range test artifact is locked by complete-file SHA-256:

```text
8006b60d75bbcb20666b294f0dac7560e3061e6c78cadd8c995d5d4037a81b40
```

An incompatible binary or validation change requires a new artifact version
and validator path.

## Record

All three paths are transient configuration and must be distinct. The output
must not exist, and its parent directory must exist.

```powershell
& .\flycast.exe `
  -config "research:IdentityManifest='C:\evidence\identity.json',research:MemoryRangesManifest='C:\evidence\memory-ranges.json',research:MemoryRangesRecord='C:\evidence\memory-ranges.candidate.fcmr'" `
  "D:\games\disc.gdi"
```

The default artifact ceiling is 1,025 MiB, which covers the manifest's 1 GiB
raw-byte ceiling plus bounded event metadata. Override it transiently with
`research:MemoryRangesMaxBytes=<count>`. Close or unload Flycast normally only
after the requested snapshot has occurred. The candidate alone does not prove
external process ownership or atomic multi-artifact publication. A validated
candidate can be composed with the immutable Maple v1 base through
[capture package v2](CapturePackageV2.md).

## Validate independently

```powershell
& .\flycast-research-validate-memory-ranges.exe `
  --artifact "C:\evidence\memory-ranges.candidate.fcmr" `
  --identity "C:\evidence\identity.json" `
  --manifest "C:\evidence\memory-ranges.json"
```

Exit status `0` accepts the complete typed artifact, `1` rejects its content or
identity join, and `2` reports command-line misuse.

Build and test the narrow slice with:

```powershell
cmake --build build-research-tests --config RelWithDebInfo `
  --target flycast-research-tests flycast-research-validate-memory-ranges
ctest --test-dir build-research-tests -C RelWithDebInfo `
  -R '^flycast-research-tests$' --output-on-failure
```

The tests cover duplicate manifest keys, invalid semantic constraints,
identity mismatches, one-shot trigger filtering, exact tick capture, raw-byte
validation, mutated and wrong bytes, incomplete output, unsupported memory,
exclusive creation, configured size bounds, and the runtime lifecycle.

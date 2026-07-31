# Ghidra static-analysis export v1

## Purpose and evidence boundary

`flycast-research-ghidra-export-v1` is a deterministic inventory of one Ghidra
program used by the Phase 3 runtime/static evidence join. It records exact
program-image and exporter identities plus bounded inventories of default-space
memory blocks, functions, symbols, and data types. It does not claim that a
recovered name, signature, structure, or semantic interpretation is correct.
Those remain static-analysis evidence until runtime observations independently
join them to executed SH-4 addresses.

The normative shape is
[`flycast-research-ghidra-export-v1.schema.json`](flycast-research-ghidra-export-v1.schema.json).
The producer is [`ExportFlycastResearch.java`](../../tools/ghidra/ExportFlycastResearch.java),
and the independent consumer is `flycast-research-validate-ghidra-export`.
Schema acceptance is a structural pre-flight check, not evidence acceptance:
canonical ordering, aggregate counts, address parity, range containment,
non-overlap, and cryptographic joins require the independent validator.

## Program identity

The export requires Ghidra language `SuperH4:LE:32:default`, a 32-bit default
address space, little-endian decoding, and an initialized executable memory
block containing the image base. It embeds:

- the exact imported executable byte count and SHA-256;
- Ghidra version, language id, compiler-spec id, and default address-space id;
- image base and exported minimum/maximum default-space addresses; and
- the exact SHA-256 of the Java exporter source that produced the JSON.

The validator hashes the supplied executable and exporter again. It also
requires the export bytes, executable hash, and image base to equal the
`static_analysis` and boot-executable bindings in the identity manifest. A
similar-looking program, a relocated import, or a modified exporter is rejected;
the validator never searches for a replacement address or image.
Ghidra must execute the repository copy of the exporter supplied later to
`--script`; its exact bytes, including line endings, are deliberately part of
the producer identity.

Raw-binary imports can leave Ghidra's `Program.getImageBase()` outside the
loaded executable even when the loader placed the initialized code block at the
correct Dreamcast address. The export always records that raw value as
`ghidra_image_base`. If it is not inside initialized executable memory, v1
derives `image_base` from the lowest initialized executable block and records
`image_base_source=minimum-initialized-executable-block`. The validator checks
that derivation exactly before joining it to the identity manifest.

## Deterministic inventories

The exporter writes arrays in canonical order:

- memory blocks by start address and name;
- functions by unique, even SH-4 entry address;
- each function's disjoint body ranges by address;
- symbols by address, kind, namespace, and name; and
- data types by unique category path.

String bounds and canonical string ordering use UTF-8 bytes in both producer
and validator, including for non-ASCII Ghidra names and category paths.

The validator independently enforces those orders, exact summary counts,
non-wrapping 32-bit ranges, non-overlapping default-space memory blocks and
function bodies, executable containment for functions, and memory containment
for symbols. Function return and parameter types are paths in Ghidra's data-type
manager. Data-type definitions are Ghidra's textual definitions and are pinned
as exact export bytes; they are not a cross-version canonical C type language.

V1 is bounded to a 64 MiB JSON file, 4,096 memory blocks, 250,000 functions,
1,000,000 symbols, 100,000 data types, 1,000,000 function body ranges, 64
parameters per function, 4 KiB ordinary strings, and 64 KiB type definitions.
The Java producer applies the same byte and collection limits and the core
function-range invariants before creating its candidate; the separate C++
validator rechecks them before acceptance.

## Runtime/static join

When `--sh4-manifest` is supplied, the validator also loads the strict SH-4
events manifest and requires:

| Runtime manifest binding | Static export authority |
| --- | --- |
| `bindings.executable_sha256` | `program.executable_sha256` |
| `bindings.static_analysis_id` | `export_id` |
| `bindings.static_analysis_sha256` | SHA-256 of the exact export JSON |

The existing identity-to-hook-manifest hash check remains independent. Thus a
runtime hook request is joined to one exact Ghidra export and one exact boot
image without treating Ghidra labels as runtime truth.

## Export and validate

Run the script from a read-only headless Ghidra session or the Script Manager:

```text
ExportFlycastResearch.java C:\evidence\ghidra-export.json STATIC-PROJECT-V1
```

The output path must not exist and its parent must already exist. Add the exact
JSON digest, executable digest, and image base to the research identity and the
exact export id/digest to the SH-4 events manifest. Then validate in a separate
process:

Headless Ghidra can return status zero even when a post-script reports an
exception. Automation must therefore require that the new output exists as a
regular file and passes the independent validator; the headless exit code alone
does not establish a successful export.

```powershell
& .\flycast-research-validate-ghidra-export.exe `
  --export "C:\evidence\ghidra-export.json" `
  --identity "C:\evidence\identity.json" `
  --program "C:\evidence\1ST_READ.BIN" `
  --script ".\tools\ghidra\ExportFlycastResearch.java" `
  --sh4-manifest "C:\evidence\sh4-events.json"
```

Exit status `0` accepts the exact static/runtime join, `1` rejects content or
identity, and `2` reports command-line misuse. [Capture package
v2](CapturePackageV2.md) provides atomic multi-artifact composition; this
export does not alter the frozen five-entry capture transaction v1.

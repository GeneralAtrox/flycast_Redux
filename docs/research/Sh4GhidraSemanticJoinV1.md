# SH-4/Ghidra semantic join v1

## Evidence boundary

`flycast-research-sh4-ghidra-semantic-join-v1` maps the exact production
dynarec block generations and actual terminal edges in an `FCSH4PR1` profile
to one exact `flycast-research-ghidra-export-v1`. It is a static/dynamic join:
execution and destination counts are runtime evidence, while names, namespaces,
signatures, function bodies and symbols remain Ghidra static-analysis evidence.

The join does not infer a call stack, declare an indirect transfer to be a jump
table, or reinterpret a cross-function jump as a tail call. Every edge retains
its raw generation, address, opcode, kind, taken state, count and boundary
ticks so later consumers can test those hypotheses without weakening this
artifact.

## Fail-closed identity and byte checks

Creation and validation require the exact profile, identity, production Maple
trace, Ghidra export, boot executable and exporter script. The profile is
revalidated against its identity/configuration/Maple bindings. The Ghidra
export is revalidated against its identity, executable and exporter bindings.

For every block wholly inside `[image_base, image_base + program_size)`, the
join requires:

- complete captured guest bytes;
- exact equality with the authenticated executable at that address; and
- containment in one initialized executable Ghidra memory block.

A block that partially overlaps the authenticated range is rejected. A block
wholly outside it is retained as `runtime-only`; v1 does not mask SH-4 aliases
or search for relocated bytes. A future alias mapping must be an explicit,
independently verified contract.

## Ownership

A block is function-owned only when its entire guest range lies inside one
Ghidra function body range. Edge endpoints are mapped independently as exact
function entries, function interiors, executable-but-unassigned addresses,
authenticated non-executable addresses, or runtime-only addresses. Multiple
block generations at the same virtual address remain separate.

The artifact includes the full metadata for every referenced function and all
Ghidra symbols at relevant block starts, edge endpoints and referenced function
entries. Multiple symbols at one address are preserved in canonical export
order; `primary` is recorded but does not erase aliases.

## Independent validation

The validator revalidates every source, reconstructs the complete canonical
join, and requires exact byte equality with the candidate. This rejects changes
to owners, function names, symbols, edges, counts, exclusions, coverage or
bindings. The normative structural pre-flight schema is
[`flycast-research-sh4-ghidra-semantic-join-v1.schema.json`](flycast-research-sh4-ghidra-semantic-join-v1.schema.json).

Accepted joins can be preserved with the independently reconstructed and
atomically published package described in
[`Sh4GhidraPackageV1.md`](Sh4GhidraPackageV1.md).

Create and validate:

```powershell
flycast-research-create-sh4-ghidra-join.exe `
  --output run.sh4-ghidra.json `
  --profile run.fcsh4profile `
  --identity identity.json `
  --replay production.fcmt `
  --ghidra-export ghidra-export.json `
  --program 1ST_READ.BIN `
  --script ExportFlycastResearch.java

flycast-research-validate-sh4-ghidra-join.exe `
  --artifact run.sh4-ghidra.json `
  --profile run.fcsh4profile `
  --identity identity.json `
  --replay production.fcmt `
  --ghidra-export ghidra-export.json `
  --program 1ST_READ.BIN `
  --script ExportFlycastResearch.java
```

Outputs are created exclusively and never overwrite an existing file.

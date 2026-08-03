# SH-4/Ghidra evidence package v1

`flycast-research-sh4-ghidra-package-v1` atomically admits one independently
reconstructable dynamic SH-4 to static Ghidra semantic join. It is a focused
package contract and does not extend the frozen capture-package-v2 inventory.

## Fixed evidence set

The publisher stages exact copies of the source job, its path-localized job,
identity, optional authenticated initial state, authenticated Flycast
executable, production Maple trace, typed SH-4 profile, Ghidra export, boot
executable, exporter script, semantic join, publisher, independent semantic
validator and package validator. `package.json` locks every staged entry by
relative path, size and SHA-256. `package-validation.json` is added only by the
staged package validator.

The initial-state entry is present exactly when the identity declares one. The
packaged state and emulator must match the authorities in the identity. The
package does not copy disc tracks; their exact identities remain in the
identity manifest and they are not inputs to semantic reconstruction.

## Independent admission

The publisher first runs the staged semantic validator over only staged inputs.
The staged package validator then independently validates the fixed inventory,
localized job, identity authorities, Maple trace, SH-4 profile, Ghidra export,
program bytes and exporter before reconstructing every semantic row. It
re-authenticates all locked entries after reconstruction and exclusively writes
a deterministic receipt containing the semantic coverage totals.

The candidate and accepted directory are siblings. Successful admission is one
directory rename. Failure leaves no accepted directory and moves the candidate
to `.flycast-research-sh4-ghidra-quarantine/<package-id>` with a typed rejection
record. Integration-only aborts are available after staging, semantic
validation and package validation, but are accepted only beneath a dedicated
temporary integration directory.

## Commands

```powershell
pwsh -NoProfile -File tools/research/flycast-publish-sh4-ghidra-package-v1.ps1 `
  -Job C:\evidence\sh4-ghidra-package-job.json

flycast-research-validate-sh4-ghidra-package.exe `
  --package C:\evidence\accepted-sh4-ghidra-package

pwsh -NoProfile -File tools/research/flycast-test-sh4-ghidra-package-forced-abort.ps1 `
  -Job C:\evidence\sh4-ghidra-package-job.json `
  -Publisher tools/research/flycast-publish-sh4-ghidra-package-v1.ps1
```

The normative structural schemas are:

- `flycast-research-sh4-ghidra-package-job-v1.schema.json`;
- `flycast-research-sh4-ghidra-package-v1.schema.json`;
- `flycast-research-sh4-ghidra-package-validation-v1.schema.json`; and
- `flycast-research-sh4-ghidra-package-quarantine-v1.schema.json`.

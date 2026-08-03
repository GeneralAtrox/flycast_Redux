# Repository evidence audit v1

`flycast-audit-evidence.ps1` is the fail-closed Phase 6 inventory and
revalidation boundary for accepted Flycast research evidence. It scans one
directory tree, identifies every non-quarantined directory containing a known
validation receipt and every directory whose leaf starts with `accepted-`, and
revalidates each package with validators from one explicitly trusted build
directory.

The auditor never runs `package-validator.exe`, `artifact-validator.exe`, or a
PowerShell validator copied into an evidence package. Packaged validator bytes
remain part of their package's locked inventory, while semantic revalidation is
performed by the current binaries in `-ValidatorDirectory`. AICA and GD-ROM
use the current repository package validator script with an explicit external
artifact-validator override. The audit report records the exact path, size and
SHA-256 of every trusted validator it actually used.

## Command

```powershell
tools\research\flycast-audit-evidence.ps1 `
  -Root C:\Projects\Lodoss\research-output `
  -ValidatorDirectory C:\Projects\FlycastRedux\build-research-tests\Release `
  -Output C:\Projects\Lodoss\research-output\evidence-audit.json
```

When `-ValidatorDirectory` is omitted, the script uses
`build-research-tests\Release` in the current Flycast checkout. The directory
must be non-linked and must not reside inside an audited package. `-Output` is
optional; without it the JSON report is written to standard output. A requested
output must not already exist and is published by a same-directory temporary
file followed by one rename.

Exit code 0 means at least one package was found and every package passed. Exit
code 1 means the report was produced but one or more packages were rejected, or
that no accepted evidence was found. Invocation, filesystem, or report-output
failures also terminate nonzero.

## Supported receipts

The v1 auditor dispatches these receipt contracts:

- capture transaction v1 and capture package v2;
- SH-4 equivalence package v1;
- SH-4/Ghidra package v1;
- PowerVR TA, semantic draw, and presentation packages v1;
- GD-ROM package v1; and
- AICA package v1.

An `accepted-*` directory with no receipt, multiple recognized receipts, an
unknown receipt schema, or a receipt stored under the wrong fixed filename is
rejected. Hidden `.flycast-research-*` staging/quarantine trees and directories
whose names contain `quarantine` are not presented as accepted evidence.

Before invoking a semantic validator, the auditor rejects package-directory or
entry reparse points. Receipts with an `entries` array are checked independently
against the exact recursive regular-file inventory, excluding only the receipt
itself. Every path must remain inside the package and every byte count and
SHA-256 must match. The package-specific trusted validator then enforces the
full typed artifact, identity, replay, source-job, static-analysis, and fixed
inventory contract.

Package UUIDs are canonical lowercase UUIDs and must be unique across the
entire audited tree. The report fingerprints each package's verified locked
content, so an identical mirror is distinguished from one UUID assigned to
different bytes. Both are rejected rather than silently counted as independent
evidence. SH-4 equivalence packages additionally expose the source `job_id` and
source-job digest. Reused job IDs are rejected, with conflicting source-job
bytes diagnosed as an identity collision. Capture transaction v1 predates
package UUIDs and is therefore identified by its path and receipt digest.

## Report and claim boundary

The JSON report follows
`flycast-research-evidence-audit-report-v1.schema.json`. It identifies the
auditor script and trusted-validator directory; package records are sorted by
root-relative path and contain the detected contract, package UUID, receipt and
locked-content digests, optional SH-4 job provenance, trusted validator
identities, bounded validator output, and a bounded rejection diagnostic.

The report is an audit result, not a new accepted evidence package or a hostile
writer attestation. It does not repair, delete, relabel, or republish rejected
evidence. A package can be rejected as stale even though its historical receipt
says `accepted`: for example, a source job that binds a publisher or validator
path whose current bytes differ is no longer reconstructable under the current
repository state.

The final 2026-08-02 Lodoss repository audit, after rebuilding the current
research runtime, tests and validators, found 23 receipt-bearing accepted
directories. Sixteen independently revalidated and seven were rejected. Six
rejections are distinct SH-4 captures that were incorrectly assigned the same
package and job UUIDs by an earlier Lodoss runner. The seventh is a distinct
early cold-boot PowerVR TA package whose publisher-script bytes no longer match
its source declaration; the later save-state TA and semantic-draw packages
pass. Current real AICA and GD-ROM packages both pass using external trusted
artifact validators.

The subsequent immutable provenance migration republished the documented
first-Maple qualification, the slot-1/AICA backend qualification, the DMA4
backend base and its dependent cold-boot PowerVR TA package under fresh package
and SH-4 job UUIDs. It then moved all seven rejected originals, without editing
their bytes, beneath an auditor-excluded recoverable quarantine. The final
current-auditor report contains 20 packages, all 20 accepted, with zero package
or SH-4 job identity collisions. Its SHA-256 is
`d4ca781572c505d47fbd63b69ede350447345bd21cff7b4a3ee3d892611d19e6`.

The 2026-08-03 authoritative refresh added the qualified real-BIOS Lodoss
GD-ROM hardware package. The first audit correctly rejected two deliberately
mutated VMU package copies because they reused the accepted package UUID; both
test fixtures were moved without byte changes into the GD-ROM capture's
auditor-excluded recoverable quarantine. The fresh report then independently
revalidated 21 packages: all 21 were accepted, with zero rejections, duplicate
package UUIDs or SH-4 job collisions. The report is
`C:\Projects\Lodoss\research-output\evidence-audit-20260803-02.json`, with
SHA-256
`dc294de5af5eddfc455c80fbc1f2cd9863656e718f06ddcdb682ac6e95008a79`.

The repeatable integration test requires one current known-good package. It
first proves the positive dispatch path, then constructs fail-closed cases for
an untyped accepted directory, a mutated receipt entry, identical package
mirrors, one package UUID assigned to different content, and one SH-4 job UUID
assigned to different source jobs:

```powershell
tools\research\flycast-test-evidence-audit.ps1 `
  -KnownGoodPackage C:\Projects\Lodoss\research-output\aica-real-slot1-20260802-03\accepted-aica-package `
  -ValidatorDirectory C:\Projects\FlycastRedux\build-research-tests\Release
```

The test directory is intentionally retained so its two typed reports and
synthetic rejected inputs remain inspectable.

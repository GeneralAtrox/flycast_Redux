# Research capture package v2

## Purpose and claim boundary

Capture package v2 atomically publishes a validated Phase 3 evidence set while
leaving the frozen five-entry capture transaction v1 unchanged. A v2 package
binds one already-published and independently revalidated Maple v1 base capture
to the exact same identity manifest, one exact Ghidra export, and one or both
of these typed runtime artifacts:

- `flycast-memory-ranges-v1`; and
- `flycast-sh4-events-v1`.

The v2 publisher is a composition transaction. It does not claim that the
Phase 3 candidates came from the Maple base capture's process merely because
their identities match. A research workflow that needs common-process timing
must record that provenance separately or use a later multi-recorder process
driver. V2 proves exact byte identity, typed validation, static/runtime joins,
base-package validity, and atomic visibility of the composed accepted set.

## Package entries

Every accepted package contains exactly:

```text
job.json
identity.json
static-analysis.json
capture-package-validation.json
```

plus the fixed pair for every job artifact:

| Kind | Manifest entry | Artifact entry |
| --- | --- | --- |
| `memory-ranges-v1` | `memory-ranges-manifest.json` | `memory-ranges.fcmr` |
| `sh4-events-v1` | `sh4-events-manifest.json` | `sh4-events.fcsh4` |

Missing, extra, linked, or non-regular entries are rejected. Artifact kinds
are unique and sorted. User-controlled package filenames are not admitted.

The normative job and receipt shapes are:

- [`flycast-research-capture-package-job-v2.schema.json`](flycast-research-capture-package-job-v2.schema.json)
- [`flycast-research-capture-package-validation-v2.schema.json`](flycast-research-capture-package-validation-v2.schema.json)
- [`flycast-research-capture-package-quarantine-v2.schema.json`](flycast-research-capture-package-quarantine-v2.schema.json)

Semantic constraints, exact hashes, path ownership, typed artifact parsing,
and cross-artifact joins are enforced by the independent C++ validator rather
than JSON Schema alone.

The v2 schema bytes are contract-locked. An incompatible schema change requires
a new package version:

| Schema | SHA-256 |
| --- | --- |
| job v2 | `8ba9a017b9aa074489089b602d08a9636ffe7d57f6ccf51444c1f712ae2419e5` |
| validation v2 | `3d722fbfae10949fc14b0fa59fcfc897f49cd06b04ae99f218c74b968ebd2bad` |
| quarantine v2 | `c751dba2f4e9d963f45e4f4f477d85820f7c4cdee7e55e37560dc16b1f4146db` |

## Validation chain

Before issuing a receipt, `flycast-research-validate-capture-package-v2`:

1. rejects duplicate keys in every packaged JSON input and JSON nesting deeper
   than 64 levels in the v2 job, identity, and receipt;
2. revalidates the original capture-transaction-v1 package at its declared
   accepted path, including current external-source authentication;
3. hashes the five exact v1 entries declared by the v2 job;
4. requires the packaged identity to be byte-identical to the v1 identity;
5. re-authenticates the external boot program and exporter script, validates
   the packaged Ghidra export, and enforces its identity binding;
6. streams every memory-range or SH-4 artifact through its native independent
   validator using the packaged manifest and identity;
7. joins every runtime manifest to the exact Ghidra export id, bytes, and
   executable identity;
8. authenticates the v2 publisher script and validator executable; and
9. atomically creates `capture-package-validation.json` inside the private
   candidate.

Read-only validation repeats the whole chain and requires the deterministic
receipt to equal the current package inventory. The receipt is a consistency
record, not a signature or hostile-writer attestation.

## Atomic publication and abort safety

`flycast-publish-capture-package-v2.ps1` creates the private sibling
`.flycast-research-package-v2-candidate-<package-id>`, copies authenticated
sources to fixed entry names, invokes the independent validator, and performs
one same-parent directory rename to the accepted path. Consumers therefore see
either no accepted v2 directory or the complete package. This is an atomic
visibility guarantee, not a claim of power-loss durability.

A failed gate leaves the accepted path absent and moves the candidate beneath
`.flycast-research-package-v2-quarantine/<package-id>` with `rejection.json`.
Validator stdout and stderr are captured, byte-counted, hashed, and previewed
with a 4,096-character bound in the rejection reason. If quarantine itself
fails, the publisher preserves the original failure as the primary error and
adds the quarantine failure instead of replacing it.
Integration-only abort tokens are restricted to dedicated parents named
`flycast-research-package-v2-integration-<hex>` and cover failures after staging
and after receipt issuance. They are test controls, not production recovery
options.

The transaction assumes a trusted local operator and an exclusive writer for
declared sources and the destination parent, matching capture transaction v1.
It does not defend against an administrator racing path components or replacing
authenticated tools between checks. Some inputs are parsed, hashed, and streamed
in separate reads; byte-exact receipt claims therefore rely on that exclusive-
writer boundary during a validation run.

## Job preparation

The job records absolute source blobs as `{path, size, sha256}`. It includes:

- the future accepted directory and a new lowercase UUID package id;
- the accepted path and the sorted five-file inventory of the Maple v1 base;
- the exact identity used by that base;
- the Ghidra export, imported boot program, and exporter script;
- a sorted typed artifact list with each candidate, manifest, and admission
  byte ceiling; and
- the v2 publisher script, validator executable, and validator timeout.

The publisher copies only the identity, export, manifests, and runtime
artifacts. The boot program, exporter script, v1 base, publisher, and validator
remain authenticated external authorities and must still exist for later
read-only validation, just as capture transaction v1 re-authenticates its
external sources.

## Publish and revalidate

Build the validator without global CTest sources:

```powershell
cmake -S . -B build-research-runtime -DBUILD_RESEARCH_TOOLS=ON `
  -DENABLE_CTEST=OFF -DUSE_DX9=OFF
cmake --build build-research-runtime --config RelWithDebInfo `
  --target flycast-research-validate-capture-package-v2
```

Publish a prepared job whose accepted directory does not exist:

```powershell
pwsh -NoProfile -File .\tools\research\flycast-publish-capture-package-v2.ps1 `
  -Job C:\evidence\capture-package-v2-job.json
```

Revalidate the accepted package and every referenced source later:

```powershell
& .\flycast-research-validate-capture-package-v2.exe `
  --package C:\evidence\accepted-phase3-package
```

Exit status `0` accepts, `1` rejects content or identity, and `2` reports
command-line misuse.

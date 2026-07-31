# SH-4 equivalence package v1

The equivalence package is the atomic admission boundary for an `equivalent`
SH-4 backend comparison. A standalone trace pair or report is not accepted
evidence.

The PowerShell publisher authenticates a typed publication job, stages a
private sibling directory, copies the exact source equivalence job and every
authenticated input, and writes a localized job whose only changes are the
fixed package-relative blob paths. It then runs the staged comparator. A
semantic divergence is a valid report but is quarantined rather than accepted.

An equivalent candidate receives a sorted `package.json` inventory that locks
every staged file by path, size, and SHA-256. The staged independent package
validator requires exact directory contents, proves that `job.json` is the
path-only localization of `source-job.json`, recomputes the equivalence report,
requires `equivalent`, reauthenticates the locked inventory, and exclusively
issues `package-validation.json`. Only then does the publisher rename the
private sibling directory to its declared accepted path.

The fixed entries are:

- the publication job, original equivalence job, localized equivalence job,
  package manifest, and validation receipt;
- both backend identities and traces, the emulator, Maple replay, manifest
  set, and each manifest under `manifests/`;
- the comparator, package validator, publisher, and equivalence report.

The receipt deterministically binds the package manifest, equivalence report,
packaged validator, package ID, and locked-entry count. Read-only validation at
the accepted path repeats all authentication and semantic checks. Linked,
missing, extra, changed, divergent, or structurally invalid entries fail
closed. Forced aborts after comparison and after validation leave no accepted
directory and retain the candidate plus rejection metadata in quarantine.

Schemas:

- `flycast-research-sh4-equivalence-package-job-v1.schema.json`
- `flycast-research-sh4-equivalence-package-v1.schema.json`
- `flycast-research-sh4-equivalence-package-validation-v1.schema.json`
- `flycast-research-sh4-equivalence-package-quarantine-v1.schema.json`

# SH-4 backend equivalence job and report v1

The v1 equivalence job is the typed join between two backend-specific identity
v2 manifests, one deterministic Maple trace, one ordered manifest set, the two
native observation traces, the exact emulator executable and the exact
independent comparator executable. Every source is an absolute-path blob with
an exact size and SHA-256 declaration. Source jobs normally use absolute paths;
safe relative paths are resolved against the job file so a staged package stays
self-contained after its directory is atomically renamed.

The comparator rejects a job unless the interpreter and dynarec identities are
distinct, declare their respective backends, bind the same emulator build and
Maple replay identity, and have identical configuration values after removing
only `cpu_backend` and `dynarec_observation`. The replay is parsed as a complete
Maple trace. The manifest set is a path-independent, uniquely ordered inventory
of manifest kind, package name, size, and digest. The job maps every inventory
entry to an authenticated source file; separating inventory from location keeps
the exact manifest-set bytes stable across capture and packaging. Both
observation traces must bind
the job's exact identity, replay and manifest-set bytes.

Identity v2 requires one unsigned 32-bit `dreamcast_rtc_seed`. It remains in
the normalized configuration comparison, so both processes must start from
the same emulated RTC value; the comparator never treats host launch time as a
backend difference or normalizes the resulting guest data after capture.

The independent trace reader does not call the production trace validator. It
validates each stream's binary grammar and SH-4 ownership rules, then compares
all semantic fields in ordinal order. Malformed or unauthenticated inputs are
rejected; semantic disagreement instead produces a valid `divergent` report
containing the exact matched prefix and first divergent field and values.

Reports are deterministic JSON. `flycast-research-compare-sh4` creates a new
report with `--report` or recomputes and byte-compares an existing report with
`--validate-report`. Exit code 0 means equivalent, 3 means a valid divergence,
1 means rejected input/report, and 2 means invalid command-line usage. Lua and
other discovery consumers cannot issue or modify this evidence.

The report is an authenticated comparison result, not by itself accepted
evidence. The separate [equivalence-package v1](Sh4EquivalencePackageV1.md)
layer stages the exact inputs, invokes this comparator, independently
recomputes the report, and exposes the complete directory only after receipt
issuance and one sibling-directory rename.

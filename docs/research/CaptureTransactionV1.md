# Research capture transaction v1

## Purpose

The Phase 2 driver turns a complete `flycast-maple-trace-v1` candidate into an
accepted evidence package. It does not accept emulator log text, a successful
GUI session, or a validator exit code by itself. Acceptance requires one
transaction containing authenticated inputs, a positively owned process,
stable terminal bytes, independent validation, and same-volume publication.

The Windows implementation is `tools/research/flycast-capture-maple.ps1`. The
process and artifact contract in this document is platform-neutral; another
host implementation must produce the same v1 job, transcript, and validation
receipt semantics.

## Atomic publication unit

An accepted output is a directory, not a set of separately promoted files:

```text
accepted-directory/
  job.json
  identity.json
  maple.fcmr
  transcript.json
  capture-validation.json
```

The driver first creates a private sibling named
`.flycast-research-candidate-<capture-id>`. All observation and validation
happens there. The final operation is one directory rename on the destination
volume. Consequently, consumers see either no accepted directory or the whole
package.

The accepted directory is part of the publication contract. Read-only
validation requires the package to remain at that declared path, requires the
validation receipt, and rejects missing, extra, linked, or non-regular package
entries. This prevents a sealed-but-unpublished candidate left by abrupt host
failure from being mistaken for accepted evidence.

The driver canonicalizes the absolute accepted-directory text before deriving
the sibling candidate, transcript paths, or process arguments. The independent
validator performs the same normalization from the immutable job. Dot segments
and trailing separators therefore cannot make the two components derive
different publication locations.

`capture-validation.json` is written by
`flycast-research-validate-capture`, not by the emulator or capture driver. It
binds the exact transcript, Maple artifact, and independent validator as a
self-contained consistency receipt. It is not a signature.

## Trust and concurrency boundary

Capture transaction v1 assumes a trusted local operator and an exclusive
writer for the source paths and accepted-output parent while a capture runs.
The authenticated driver and validators may be inspected and reproduced, but
the package does not defend against an administrator or another process that
can replace those programs, race directory entries, forge a transcript, or
rerun the validator to mint a new internally consistent receipt. Hostile-writer
attestation requires an external trust anchor such as a signed package digest
or an append-only evidence ledger; that is outside v1.

The driver rechecks reparse components and volume identity immediately before
publication, but the trusted single-writer assumption is still required because
Windows path APIs used by v1 do not pin every directory component by handle for
the full transaction.

## Input contract

The immutable job conforms to
`flycast-research-capture-job-v1.schema.json`. Every blob has an absolute path,
byte count, and lowercase SHA-256. The driver rejects missing paths, links or
reparse points, size/hash mismatches, an existing accepted destination, path
traversal in runtime dependencies, and any private/public cross-volume rename.

The identity manifest's blob paths become operational in Phase 2. Capture v1
accepts GDI media only; other Phase 1 identity kinds remain valid for format
inspection but are rejected by this driver. A GDI capture requires at least one
declared track. Both the driver and the independent capture validator enforce
that capture-v1 scope. The driver authenticates all of these before launch and
again after the emulator stops:

- media descriptor/image, IP.BIN, boot executable, and every GDI track;
- BIOS when real firmware is selected, plus the initial flash/NVRAM;
- each initial persistent device;
- emulator executable and declared process support files;
- Maple validator, capture validator, driver, and their support files; and
- the original job and identity-manifest bytes.

Relative or absent identity blob paths are valid for Phase 1 format inspection
but are not admissible to a Phase 2 capture.

Additional transient Flycast settings may be bound inside
`identity.configuration.values.flycast_transient` as an array of complete
`section:key=value` strings. Neither that array nor
`job.emulator.arguments_prefix` may name the `research` configuration section;
the driver owns every research setting, including the identity, output, replay,
and byte-limit options. The prefix must also be token-complete: it cannot end in
a bare `-config` or `--config` that would consume the driver's following token.
Transient entries must contain non-whitespace text. The driver and independent
capture validator both apply Flycast's configuration-string grammar when
enforcing these rules. The transient array remains part of the configuration
digest embedded indirectly through the exact identity-manifest digest.

For Windows Dreamcast runs, use the canonical seed names expected by Flycast:
`dc_nvmem.bin` for flash/NVRAM, `dc_boot.bin` or `dc_bios.bin` for a real BIOS,
and persistent-device bus/port metadata for VMU staging. The driver derives
VMU names such as `vmu_save_A1.bin` and confines all writable copies to the
private runtime home.

## Transaction state machine

```text
preflight
  -> exact inputs authenticated
  -> private candidate created
  -> isolated executable/data home staged
  -> emulator launched and ownership tuple captured
  -> emulator exits normally
  -> terminal artifact sampled twice
  -> exact inputs re-authenticated
  -> Maple validator accepts unchanged candidate
  -> private runtime removed
  -> atomic transcript written
  -> capture validator writes validation receipt
  -> accepted directory published by one rename
```

The ownership tuple is the PID, resolved executable path, process start time in
UTC ticks, and the exact Windows command line obtained from the operating
system. The transcript also retains the exact argument array supplied through
`ProcessStartInfo.ArgumentList`. The independent capture validator reconstructs
that array from the job, identity, private candidate path and configured limits,
then parses the recorded Windows command line and compares every argument. A
timeout or failure may terminate only a process that still matches the captured
path and start identity; emulator cleanup additionally checks the captured OS
command line. PID-only or process-name termination is forbidden.

Very short-lived processes may exit before the Windows ownership tuple can be
acquired. They fail closed and cannot produce accepted evidence. Emulator
stdout and stderr are inherited by the caller and explicitly are not evidence;
the driver never buffers them. Validator stdout and stderr are drained
concurrently as raw bytes, retained only up to the first 1 MiB per stream, and
record exact captured-prefix hashes, total byte counts, and truncation flags.
UTF-8 decoding is diagnostic only and happens after byte accounting, so a
multi-byte character split across pipe reads cannot alter the evidence counts.

The capture validator hashes external media, firmware, executable, and support
blobs in fixed 64 KiB windows. It validates the Maple artifact event-by-event
with a fixed 4 KiB event buffer while incrementally hashing the payload. Thus
the configured trace ceiling is an admission limit, not a request to allocate
the whole file; validator memory does not scale with track or trace length.

Both terminal samples include process exit state, exit code, artifact length,
last-write time, and SHA-256. They must be identical after the configured
stability interval. The artifact is hashed again after the Maple validator; a
validator that mutates its input causes rejection.

Only a natural emulator exit with status zero is eligible for acceptance.
Timeouts and injected aborts are diagnostic failures even if a candidate file
looks complete.

## Isolation and writable state

The emulator executable is copied byte-for-byte into `.runtime` beneath the
private candidate. Firmware, flash, VMUs, configuration, logs, and declared
runtime dependencies are staged beside that copy. The process therefore cannot
alter the authoritative source seeds or the normal Flycast installation.

Initial and terminal identities of known writable runtime files are retained
in the transcript. The private runtime is removed before successful
publication. On failure it remains in quarantine for diagnosis and must never
be treated as accepted evidence.

## Failure and quarantine

Any failed gate keeps the public destination absent. After exact owned-process
cleanup, the driver writes a rejected transcript and
`flycast-research-quarantine-v1` metadata into the private directory, then
renames that directory under the sibling `.flycast-research-quarantine`
directory. If ownership cannot be proved, the driver refuses to kill the
process and reports a cleanup failure rather than guessing. In that case the
private directory remains in place with publication state `candidate_retained`;
it is not moved to quarantine while a possibly live process may still own files
inside it. The rejected transcript's `retained_processes` array records each
known emulator or validator by role, PID, canonical executable path, process
start UTC ticks, and exact launch argument vector (plus the OS command-line hash
when available). Operators must verify that exact tuple, stop that process, and
only then remove the candidate. Accepted and fully cleaned rejected transcripts
have an empty array.

Integration-only abort and fault tokens are guarded by exact token values and
dedicated destination-parent leaf names. They exercise earliest-staging
failure, normal owned cleanup, explicit cleanup refusal, and publication
invariants; they are not production recovery controls.

## Run a capture

Build the production emulator and validators without enabling Flycast's global
test configuration:

```powershell
cmake -S . -B build-research-runtime -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_RESEARCH_TOOLS=ON -DBUILD_TESTING=OFF -DUSE_DX9=OFF
cmake --build build-research-runtime --config RelWithDebInfo `
  --target flycast flycast-research-validate-maple `
           flycast-research-validate-capture
```

`USE_DX9=OFF` avoids Flycast's optional legacy DirectX SDK dependency on
Windows. DX11 and Vulkan remain available.

Use a separate target-focused test configuration:

```powershell
cmake -S . -B build-research-tests -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_RESEARCH_TOOLS=ON -DENABLE_CTEST=ON
cmake --build build-research-tests --config RelWithDebInfo `
  --target flycast-research-tests flycast-research-capture-fixture `
           flycast-research-validate-maple flycast-research-validate-capture
ctest --test-dir build-research-tests -C RelWithDebInfo `
  -R '^flycast-research-tests$|^flycast-research-capture-transaction$' `
  --output-on-failure
```

Prepare a job whose accepted directory does not exist, then run:

```powershell
pwsh -NoProfile -File .\tools\research\flycast-capture-maple.ps1 `
  -Job C:\evidence\capture-job.json
```

The interactive Flycast window remains visible. Exercise only the causal slice
the capture is intended to preserve, then close Flycast normally. A successful
command prints the accepted directory, capture ID, artifact, transcript, and
validation receipt.

Revalidate a package independently later, while every absolute source path
recorded in the job and identity manifest still exists with the same bytes:

```powershell
flycast-research-validate-capture.exe `
  --package C:\evidence\accepted-capture
```

Without `--receipt`, validation is read-only. It intentionally re-authenticates
the current external sources; an otherwise intact package is rejected if those
paths have disappeared or changed. Exit status is `0` for accepted, `1` for a
rejected package, and `2` for invalid command-line usage.

## Claim boundary

Under the trusted-local-operator and exclusive-writer assumptions, an accepted
v1 package is independently checkable evidence that one owned Flycast command,
the declared input identities, stable terminal Maple bytes, and atomic
publication are mutually consistent. The receipt is neither cryptographic
process attestation nor proof against a hostile local writer. The package also
does not prove physical Dreamcast bus timing, game semantics, completeness
outside the captured interval, or correctness of future SH-4, PowerVR, GD-ROM,
or AICA evidence formats.

# PowerVR TA causal artifact v1

The PowerVR TA slice records the causal path from guest-owned TA input through
render selection and the guest-visible render-complete boundary. It is an
evidence format, not an all-PVR-state dump and not a renderer debug log.

## Accepted chain

```text
SH-4 instruction owner
    -> TA list initialization or continuation
    -> accepted 32-byte TA block
    -> STARTRENDER selection transcript
    -> render-complete generation
    -> independent validation
    -> atomic package publication
```

`flycast-pvr-ta-v1` is a bounded binary artifact. Its incomplete header is
created exclusively before emulation starts. A clean capture rewrites that
header as complete only after immutable-input reauthentication, zero delivery
drops, the manifest render count, and the replay terminal have all succeeded.
Abort, replay divergence, input mutation, writer failure, or count mismatch
leaves the candidate incomplete.

The header binds:

- interpreter or research-dynarec backend;
- identity-v2 SHA-256;
- exact Maple replay-file SHA-256;
- PVR capture-manifest SHA-256;
- event/type counts, bounds, tick and emission ranges; and
- payload SHA-256 and CRC-protected framing.

The identity's `maple_replay_identity_sha256` authenticates the identity token
inside the Maple trace. This is intentionally distinct from the replay-file
SHA-256 carried by the PVR artifact.

## Events

`ListInit` and `ListContinue` assign non-reused generations to raw TA context
addresses. `AcceptedBlock` contains the exact 32 bytes accepted by the TA
parser, source class and address, parser/list transitions, render pass, and a
per-context block ordinal. Rejected or overflowed input is never presented as
accepted evidence.

`StartRender` contains the exact ordered VRAM address/value reads used by
`getTAContextAddresses`, the selected context addresses, availability results,
sealed context generations, and a new render generation. The validator
independently replays the selection algorithm from this transcript; it does
not call the producer's selection helper. `RenderDone` must close the pending
render generation, and render generations cannot overlap. A complete v1
artifact requires at least one accepted block whose exact context generation
is selected by `StartRender` and subsequently closed by its matching
`RenderDone`.

Synchronous events carry an SH-4 instruction-owner token. When the scheduler's
slice-base tick lags the precise open instruction tick, the event tick is
canonicalized to the owner tick; it can never claim to precede its cause.
Asynchronous render completion is explicitly unowned.

## Capture CLI

Build Flycast and `flycast-research-validate-pvr-ta`, then invoke:

```powershell
pwsh -NoProfile -File tools/research/flycast-capture-pvr-ta-v1.ps1 `
  -FlycastExecutable C:\path\to\flycast.exe `
  -ValidatorExecutable C:\path\to\flycast-research-validate-pvr-ta.exe `
  -Game C:\path\to\game.gdi `
  -Identity C:\path\to\interpreter-identity.json `
  -MapleReplay C:\path\to\maple-replay.fcmt `
  -Manifest C:\path\to\pvr-ta-manifest.json `
  -FlashSeed C:\path\to\dc_nvmem.bin `
  -OutputDirectory C:\path\to\new-capture-directory
```

The command requires a new output directory. It authenticates the emulator,
game descriptor, media tracks, IP.BIN, boot executable, flash, identity,
replay, and manifest; stages private copies; runs the manifest-bounded
recorder; waits for a bounded stable terminal window; closes only its owned
Flycast process; reauthenticates inputs; and runs the staged independent
validator. Its output remains a `validated-candidate`, not accepted evidence.

The capture manifest is
`flycast-research-pvr-ta-capture-manifest-v1`. Its exact static-analysis and
boot-executable digests must match identity v2. `capture.start_dma` selects the
authenticated one-based Maple DMA begin at which observation starts; zero
means observation begins with emulation. `capture.render_done_count` is an
acceptance assertion, not a discovery counter.

## Independent validation

The validator can also be run directly:

```powershell
flycast-research-validate-pvr-ta.exe `
  --artifact C:\path\to\candidate.fcpvr `
  --identity C:\path\to\identity.json `
  --replay C:\path\to\maple-replay.fcmt `
  --manifest C:\path\to\pvr-ta-manifest.json
```

It rejects incomplete or malformed framing, binding mismatches, noncanonical
owners/sources, invalid context lifecycles or ordinals, altered render-selection
reads, unmatched renders, missing vertical-slice event classes, wrong manifest
counts, and limit mismatches.

## Atomic package publication

`flycast-research-pvr-ta-package-job-v1` selects one already accepted SH-4
equivalence package/backend and declares exact blobs for the matching identity,
Maple replay, PVR manifest/candidate, static-analysis bytes, boot executable,
publisher, artifact validator, and package validator.

Run:

```powershell
pwsh -NoProfile -File tools/research/flycast-publish-pvr-ta-package-v1.ps1 `
  -Job C:\path\to\pvr-ta-package-job.json
```

The publisher stages a fixed nine-entry candidate beside the destination,
runs the staged artifact validator, obtains a deterministic receipt from the
staged package validator, and exposes the ten-entry accepted directory with one
sibling-directory rename. Any failure remains outside the accepted path and is
moved to the package quarantine with a rejection record. Read-only revalidation
uses:

```powershell
flycast-research-validate-pvr-ta-package.exe --package C:\path\to\accepted
```

## Current boundary

This slice ends at TA input, context selection, and render completion. PVR
register/VRAM-write ownership and framebuffer/present artifacts are separate
later slices.

The authenticated Lodoss cold-boot progression established the minimum
terminal precisely. DMA 2 still contains only six list initializations. DMA 3
adds four accepted blocks and one `StartRender`, but terminates before render
completion. DMA 4 contains the first complete causal chain and independently
validates with seven list initializations, four accepted blocks, one
`StartRender`, and one matching `RenderDone`.

That interpreter artifact is atomically published against the accepted DMA-4
interpreter/dynarec equivalence package as PowerVR package
`ecd6220b-0989-4edd-abe9-ac2697ff73b5`. Its Maple replay SHA-256 is
`6e443dd19a728628178d822eb4503778be4722bc4ebf2e29b259f80fae5fadee`
and its PVR artifact SHA-256 is
`14509b4a9bd47d945961d6af2135938c93f16209230968a61f109d77a17053bc`.

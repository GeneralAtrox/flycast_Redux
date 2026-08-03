# Flycast CD-DA Causal Evidence v2

## Claim boundary

`flycast-research-cdda-v2` proves one bounded causal chain:

`SH-4 GD-ROM control owner -> applied drive state -> authenticated raw GDI audio sector -> AICA frame -> CD-DA mix contribution -> final PCM`

It does not reinterpret AICA v1. Existing `.fcaica` files remain keyed-sound
artifacts and do not become CD-DA evidence.

The v2 control authority covers both Dreamcast execution paths:

- REIOS HLE commands PLAY (`0x14`), PLAY2 (`0x15`), PAUSE (`0x16`),
  RELEASE (`0x17`), SEEK (`0x1b`), and STOP (`0x21`); and
- real-BIOS GD-ROM packets CD_PLAY (`0x20`) and CD_SEEK (`0x21`).

For the packet path, the exact 12 packet bytes are retained in the four typed
parameter words. The owner is the SH-4 instruction whose sixth 16-bit GD-ROM
data-register write completes the packet. Application is synchronous, so the
same token is retained into the applied drive state without inference.

## Fail-closed acceptance

The independent validator accepts an artifact only when all of the following
are true:

- the identity, Maple replay and AICA configuration digests match;
- the identity authenticates a GDI descriptor and every declared track;
- at least one REIOS PLAY/PLAY2 or bounded real-BIOS CD_PLAY command was
  accepted and successfully applied;
- a successful sector FAD resolves to a type-0, 2352-byte GDI audio track;
- the captured sector exactly equals the raw track bytes at that FAD;
- pre/post drive state exactly follows Flycast repeat, termination or failure
  behavior;
- every retained AICA frame references a successful authenticated sector and
  proceeds contiguously from frame zero;
- AICA input samples equal signed little-endian stereo values from the raw
  sector;
- at least one retained frame has a nonzero CD-DA contribution; and
- no CD-DA or AICA observation was dropped.

For real-BIOS packets, the independent validator reconstructs FAD or MSF
addresses, repeat count, resume behavior, pause/stop/seek behavior and the
resulting drive state directly from the raw packet bytes. A CD_PLAY resume
packet does not count as establishing playback unless an earlier bounded play
range was captured.

When CD_PLAY omits its end address, validation reproduces Flycast's GD-ROM
session lead-out FAD (`549300`) rather than inferring an end from the last
file-backed track. This matches `Disc::FillGDSession()` and
`libGDR_GetSessionInfo(..., 0)` exactly.

Paused, stopped, non-playing and failed reads may be represented as zero
sectors, but they cannot satisfy the successful-sector or contribution gates.

## Save-state boundary

Save-state version 60 serializes the active AICA CD-DA buffer generation,
source FAD, pre-read status and repeat count, read-success flag, and control
generation. It also serializes the active GD-ROM control generation. This
prevents a loaded mid-sector buffer from being mislabeled using the current
drive address.

States older than v60 load with `source_available=false` and generation zero.
They remain usable for gameplay, but samples consumed from their active buffer
cannot qualify as CD-DA evidence. A new control and successful sector observed
after capture starts can qualify normally.

## Capture

The runtime options are transient:

```text
research:CddaRecord=C:\absolute\capture.candidate.fccdda
research:CddaMaxBytes=67108864
research:CddaSampleFrames=588
```

`research:IdentityManifest` and `research:MapleReplay` are also required. CD-DA
capture cannot run concurrently with AICA v1 capture because both require
exclusive lossless ownership of the AICA observation bus.

Validate independently:

```powershell
flycast-research-validate-cdda.exe `
  C:\absolute\capture.fccdda `
  C:\absolute\identity.json `
  C:\absolute\maple-replay.fcmt
```

Publish atomically:

```powershell
Tools\research\flycast-publish-cdda-package-v2.ps1 `
  -Artifact C:\absolute\capture.fccdda `
  -Identity C:\absolute\identity.json `
  -MapleReplay C:\absolute\maple-replay.fcmt `
  -FlycastExecutable C:\absolute\flycast.exe `
  -ArtifactValidator C:\absolute\flycast-research-validate-cdda.exe `
  -OutputDirectory C:\absolute\accepted-cdda-package
```

Publication stages immutable copies under a fresh package identity, validates
the staged copy, writes a typed receipt, and renames the directory atomically.
Failures are moved to the CD-DA quarantine and never appear at the accepted
path.

## Lodoss gate

The Lodoss identity contains audio tracks, but track presence is not proof that
the game requests playback. A Lodoss package is acceptable only after an exact
replay produces a genuine PLAY or PLAY2 request and reaches a successful,
nonzero audio-sector contribution. Injected or synthetic playback cannot be
reported as Lodoss behavior.

For an open-ended discovery session under either REIOS or a real BIOS, use
`tools/research/flycast-monitor-cdda-evidence.ps1`. Its native filters discard
stopped-sector polling and zero CD-DA mixer contributions before Lua queueing.
It exits automatically only after a genuine accepted/applied PLAY command,
successful sector and joined nonzero AICA contribution have all occurred.

`-AutoExitFrames <n> -AllowNoHit` changes the launcher into a bounded state
probe. A complete no-hit result proves only that the tested window did not
reach CD-DA; it is not a whole-game absence claim.

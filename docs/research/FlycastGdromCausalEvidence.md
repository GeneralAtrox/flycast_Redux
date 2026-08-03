# Flycast GD-ROM causal evidence

This slice records the exact REIOS HLE `GDCC_DMAREAD` path used by Record of
Lodoss War during an authenticated cold boot. It does not claim low-level
ATA/SPI behavior.

The typed chain is:

`initiating SH-4 instruction -> REIOS request generation -> 2048-byte sector
chunks -> canonical RAM destinations -> status-polled completion`

The standalone validator independently parses the authenticated GDI descriptor,
authenticates every declared track, reconstructs Mode 1 user bytes from raw
2352-byte sectors, and rejects altered bytes, non-contiguous FADs or destinations,
chunks larger than REIOS's five-sector scheduler quantum, incomplete commands,
dropped observations, and any completion mechanism other than status polling.
It does not call Flycast's `libGDR` image reader.

## Real-BIOS hardware path v2

Real-firmware identities select a separate `FCGDHW01` artifact. The legacy
`FCGDROM1` bytes and REIOS validator remain unchanged. Before the hardware bus
is subscribed, capture authenticates the identity's 2 MiB BIOS file, initial
flash file, running Flycast executable and Maple replay. After Flycast has
loaded firmware, capture also hashes the bytes in the emulated boot-ROM chip.
Consequently, a missing BIOS followed by Flycast's normal silent REIOS fallback
is rejected before an artifact can be accepted.

Flash authentication uses the immutable 128 KiB snapshot taken immediately
after `dc_nvmem.bin` is loaded and before Flycast repairs partitions, writes the
deterministic RTC/language block, or updates console identity checksums. Those
later writes remain visible in the live flash chip but cannot change which
initial file supplied the run.

The hardware causal chain is:

`ATA PACKET SH-4 owner -> sixth SPI-word SH-4 owner -> exact CD_READ/CD_READ2
packet -> decoded FAD/count/sector format -> libGDR cache fill -> GD-DMA start
owner or GD_DATA read owners -> exact transferred bytes -> DMA/command interrupt
assertion -> packet completion -> GD_STATUS acknowledgement owner`

Reset, GD-DMA disable, overlap and save-state restoration invalidate an
in-flight candidate. A restored cache or packet is never accepted as a newly
observed read. PIO records the exact register words and their SH-4 readers; it
does not invent a later RAM destination.

The independent hardware validator parses the packet without using the drive
decoder and reconstructs sector bytes without using `libGDR`. It authenticates
the GDI descriptor and every track, implements raw 2352 to 2048 Mode 1/Mode 2,
2340 and 2352 conversion, and reproduces zero-filled read misses. It verifies
16-sector cache fills, 10,240-byte scheduler chunks, partial-sector stream
offsets, multiple DMA sessions, PIO word order, destinations and interrupt
ordering. Flycast's partial 2048-source-to-raw conversion is deliberately
rejected as non-evidentiary.

The replay is not treated as an opaque hash. Validation requires the original
Maple recording identity, parses the complete replay independently, requires
that identity to select real firmware, and joins its BIOS, initial flash,
media/tracks, emulator, initial state and persistent-device authorities to the
GD-ROM capture identity. The replay DMA count must equal the capture checkpoint
and the GD-ROM artifact may not extend past its terminal tick.

Capture uses transient configuration:

```text
research:IdentityManifest=<identity-v2.json>
research:MapleReplay=<maple-replay.fcmt>
research:GdromRecord=<candidate.fcgd>
research:GdromMaxBytes=536870912
```

Validate with:

```powershell
flycast-research-validate-gdrom.exe `
  --artifact <candidate.fcgd> `
  --identity <identity-v2.json> `
  --replay <maple-replay.fcmt>
```

Durable publication must use `flycast-publish-gdrom-package-v1.ps1`. The
publisher reauthenticates the exact Flycast executable named by the identity,
stages a fixed inventory in a private sibling directory, runs the staged
independent validator, writes a validation receipt, runs the read-only package
validator, and atomically renames the candidate. The same package validator can
be rerun later to detect any mutation. Forced-abort tokens are restricted to dedicated integration-test
parents; failed candidates are moved to quarantine and never exposed at the
accepted path.

For real-BIOS artifacts use:

```powershell
flycast-capture-gdrom-hardware-v2.ps1 <capture arguments>

flycast-research-validate-gdrom.exe `
  --artifact <candidate.fcgd> `
  --identity <real-bios-identity-v2.json> `
  --replay <real-bios-maple-replay.fcmt> `
  --replay-identity <real-bios-maple-record-identity.json>

flycast-publish-gdrom-hardware-package-v2.ps1 `
  -Artifact <candidate.fcgd> `
  -Identity <real-bios-identity-v2.json> `
  -MapleReplay <real-bios-maple-replay.fcmt> `
  -MapleReplayIdentity <real-bios-maple-record-identity.json> `
  -FlycastExecutable <flycast.exe> `
  -ArtifactValidator <flycast-research-validate-gdrom.exe> `
  -OutputDirectory <accepted-package>
```

The v2 package contains immutable copies of `dc_boot.bin`, the initial
`dc_nvmem.bin`, every declared persistent VMU, and
`maple-record-identity.json`. A capture with an authenticated A1 VMU must bind
`config:PerGameVmu=no`, ensuring Flycast consumes the staged
`vmu_save_A1.bin` rather than creating an unbound game-specific file. Its validator passes the
firmware copies as explicit authorities and the record identity as the replay
authority, so retained package validation does not depend on the identity's
original firmware or persistent-device paths. Publication uses a sibling candidate directory,
independent validation, a receipt, atomic directory rename and recoverable
quarantine on any failure. The forced-abort checker exercises both pre- and
post-validation interruption points.

The real Lodoss hardware gate passed on 2026-08-03. The accepted run uses the
retail PAL v1.01d BIOS, its authenticated initial 128 KiB PAL flash image, and
the authenticated populated A1 VMU. At the exact 755-DMA Maple replay boundary,
the artifact contains 1,107 events covering 21 complete CD_READ/CD_READ2
commands and 773 DMA chunks. The independent artifact and package validators
accepted it, the atomic forced-abort test passed at both boundaries, and a
one-byte mutation of the retained VMU was rejected. The accepted package is
`C:\Projects\Lodoss\research-output\real-bios-gdrom-capture-20260803-06\accepted-gdrom-hardware-package`.

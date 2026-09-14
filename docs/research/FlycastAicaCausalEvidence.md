# Flycast AICA causal evidence v1

The AICA slice records one bounded, authenticated sound-producing interval. It
is intended for save-state-started Lodoss probes, not continuous audio logging.

The typed chain is:

`pre-KYON AICA checkpoint -> stable KYON batch -> register/RAM/DMA mutations
-> keyed sample-source bytes -> CD-DA sector/frame joins -> exact final PCM`

The checkpoint contains the 0x8000-byte AICA register file, all configured AICA
RAM, the 64 saved channel states, saved DSP state, and the current 2352-byte
CD-DA buffer. Subsequent records carry writer ownership, exact write bytes, G2
DMA generations, raw key register snapshots, dry/CD-DA/DSP contributions, and
signed PCM16 stereo output. ARM7 writes are deliberately labelled as ARM7-domain
ownership without claiming an exact ARM7 PC.

Capture is fail-closed. Clean finalization requires exactly the requested sample
count, a stable key batch, at least one non-noise key-on with a bounded source
range, at least one nonzero PCM frame, no reset, no mute/fast-forward
suppression, no dropped observations, and no open G2 DMA. The required output
configuration is the identity-v2 `aica_configuration`: the game's declared DSP
mode, VMU sound disabled, 44.1 kHz signed PCM16 LE stereo, sampled before the
backend and user volume. Capture does not alter that declared mode. The Options
menu route used for DSP-tail authority declares and runs with DSP enabled.

The identity-v2 configuration values must include:

```json
"aica_configuration": {
  "dsp_enabled": true,
  "vmu_sound": false,
  "sample_rate": 44100,
  "sample_format": "signed-pcm16-le-stereo",
  "output_stage": "pre-backend-pre-user-volume"
}
```

As with every identity edit, `configuration.sha256` is the SHA-256 of the
canonical compact JSON bytes for the complete `configuration.values` object.

Use transient configuration (22,050 frames is 0.5 seconds):

```powershell
flycast.exe -config (`
  "research:IdentityManifest=C:\path\identity-v2.json," +
  "research:MapleReplay=C:\path\sound-action.fcmt," +
  "research:MapleTraceMaxBytes=536870912," +
  "research:AicaRecord=C:\path\lodoss-sound.candidate.fcaica," +
  "research:AicaMaxBytes=536870912," +
  "research:AicaSampleFrames=22050") C:\Projects\Lodoss\GameFiles\game.gdi
```

The identity may bind Slot 1 or Slot 2 as its initial state. The replay must
perform an action that really keys a sound; a neutral replay cannot produce an
accepted artifact. Close Flycast normally after the target interval is reached,
then validate independently:

```powershell
flycast-research-validate-aica.exe `
  C:\path\lodoss-sound.candidate.fcaica `
  C:\path\identity-v2.json `
  C:\path\sound-action.fcmt
```

Schema 2 starts at a `PreKeyBatch` checkpoint and assigns every key transition
and sample an exact sample-cut ordinal. The independent validator reconstructs
all 64 channel runtimes, replays PCM16, PCM8, ADPCM, noise, envelopes, filter,
LFO, pitch, looping, attenuation and pan, and compares the dry mix and all 16
DSP input lanes sample by sample. It separately executes MPRO with captured
COEF, MADRS, TEMP, MEMS, ring RAM, RBP, RBL and MDEC state, compares all EFREG
outputs, applies captured effect/CD-DA routing and the master stage, and then
compares final PCM. It does not call Flycast's channel or DSP implementation.

The normal validator mode also requires a DSP tail proof. The final 65,536
sample transitions must have no active channels and zero dry, CD-DA, DSP-send,
EFREG, DSP-return and final output. The validator snapshots the zero-output
anchor and requires the terminal TEMP, MEMS, MIXS, EFREG, EXTS, RBP, RBL,
MDEC, MPRO, COEF, MADRS, effect routing and full 131,072-byte ring image to be
identical. It canonically serializes those fields in fixed little-endian order,
requires matching SHA-256 state digests as well as direct field equality, and
reports both digests. RBL must remain 65,535, RBP must remain fixed, MDEC must
remain in range at every sample, and the program must execute exactly 65,536
MDEC steps between the matching-phase snapshots; a stopped program therefore
cannot satisfy the ring-cycle claim vacuously. Both snapshots are taken
immediately after their boundary samples, and the final SampleFrame must be the
last artifact record, so later mutations cannot restore or contaminate terminal
state. Checkpoint RBP and RBL must also decode exactly from common register
`0x2804`. `--semantic-prefix` runs the same sample-by-sample
reconstruction but deliberately omits only this terminal proof and is
diagnostic, not acceptable package evidence.

The authenticated Lodoss Options investigation disproved the earlier
zero-output assumption. In its 500,000-frame DSP-enabled capture, dry/DSP-send
input ends at sample ordinal 445,879, while DSP and final PCM remain nonzero at
the terminal ordinal 732,716. The final 1,024 frames of the preceding
400,000-frame capture are exactly `-1,-1`, but the hidden state is not static:
across the final ring of the longer capture, independent replay reports
`TEMP[0]` changing from -244 to -228. The strict validator therefore rejects
the capture both for nonzero DSP output and for lack of a persistent-state
cycle. Consequently the bounded semantic replay is sample-exact and
field-complete, but it is not evidence for trimming, resetting, or looping the
tail. A consumer that needs later cue overlap must carry the reconstructed DSP
state forward continuously.

An earlier schema-2 run extended that evidence to 882,000 frames under an
authenticated 1,100-DMA interpreter route. Independent replay accepts all
2,900,419 events and reports complete measured coverage: present mask 2,097,151,
consumed mask 2,096,319, proven-irrelevant mask 832, and owner-writer mask 14
(SH-4 direct, SH-4 G2 DMA, and ARM7; no unknown owner). Dry/DSP-send input still
ends at ordinal 445,879, while DSP/final output remains nonzero through terminal
ordinal 1,114,716. At the first sample of the final ring, ordinal 1,049,180,
active channels, dry, CD-DA and DSP-send input are all zero, but DSP/final PCM
is `-1,-1`; over that ring `TEMP[0]` changes from -236 to -232. Both required
tail conditions therefore remained disproved at that duration.

The current maximum authority window is 1,300,000 frames. An authenticated
1,600-DMA Options route produced a 500,681,998-byte artifact containing
4,266,956 events. It independently replays through terminal ordinal 1,532,716
with the same complete coverage masks and owner set. The last dry/DSP-send
input remains ordinal 445,879, so the captured no-channel interval exceeds
1,086,000 samples. Nevertheless, at the final-ring anchor ordinal 1,467,180,
DSP/final remains `-1,-1` and `TEMP[0]` changes from -244 to -240 over the
matching MDEC cycle. The anchor persistent-state digest is
`02dd1b70127df8f49e53fbfc29e0850f81cd2f0096d0f10a6476f0b32883032e`; the
terminal digest is
`4ee44d589c7e34d59391a1f66bd936272abe6d9c6a7e0926a8dbf046535f1e83`.
The strict replay measures exactly 65,536 executed MDEC steps over that interval
with stable 65,536-word ring geometry, so matching phase is not inferred from
equal counters or a stopped program.
Direct parsing of the recorded sample payload identifies EFREG lanes 0 and 1
as `-1` throughout the terminal frames. Captured MPRO steps 75 and 73 write
those lanes respectively, and captured output controls `0x0f1f` and `0x0f0f`
route them at full volume to opposite stereo sides. This places the residue in
the game's live DSP program and routing rather than in final-mix inference.
Per-channel reporting partitions the captured interval into checkpoint-active,
key-target, observed-active, consumed, and inactive/unkeyed masks. It is
reported separately from authoritative field-group completeness. Checkpoint
activity is cross-checked against all 64 reconstructed enabled fields, and every
sample's active mask is independently recomputed before output generation. A
serialized key target is bound by artifact/event authentication plus agreement
with the selected channel's register mirror, then classified consumed because
replay applies that event and reads its lifecycle state. Identical channel
register blocks are not independently distinguishable by that mirror check.
For this captured interval the
checkpoint-active mask is zero, while the key-target and observed-active
masks identify channels 48, 49 and 50. The resulting present mask is
`0xffffffffffffffff`, consumed mask is `0x0007000000000000`, and
inactive/unkeyed mask is `0xfff8ffffffffffff`. Schema v6 encodes all channel
masks as fixed-width hexadecimal strings so JSON consumers cannot lose 64-bit
precision, and labels the scope as `captured-interval`. Semantic-prefix output
does not claim those owners remain inactive after its cut; interval-wide strict
status is reported only by `dsp_tail_proof_verified`.
The exact 44,100 Hz AICA clock also makes one 25 Hz Lodoss logic period exactly
1,764 samples; key events retain their captured sample-cut ordinal rather than
being scheduled from host presentation time.

Field coverage for schema 2 is as follows:

| Captured contract field | Validator use |
|---|---|
| Channel registers and 21 runtime fields for all 64 channels | Reconstruct key transitions and every dry/DSP-send sample. Checkpoint-enabled owners are cross-checked against reconstructed state; active owners are recomputed and compared per sample; artifact-bound key targets are consumed when replay reads/applies their lifecycle state. The captured-interval complement reports owners not enabled, active or keyed before the cut and is not folded into `field_coverage_complete`. The saved `looped` bit is status-only; generation never reads it and subsequent loop transitions are independently reproduced. |
| Full AICA register image and register-write stream | Drive channel format/rates/routing, common master state, MPRO, COEF, MADRS, EFREG and EXTS. Decoded checkpoint RBP/RBL are cross-checked against common register `0x2804`. Timer/interrupt-only registers are retained for provenance but are proven outside the audio-generation call graph. |
| Full AICA RAM and RAM/DMA write stream | Serve every channel decode and DSP ring/table read and write with exact wrap behavior. Bytes never addressed by the captured execution are irrelevant to that route. |
| TEMP, MEMS, RBP, RBL and MDEC | Seed independent DSP execution and participate in the terminal persistent-state comparison. |
| Checkpoint MIXS | Proven irrelevant at the boundary: `AICA_Sample` clears all 16 lanes before stepping any channel. Per-sample MIXS is captured and compared instead. |
| CD-DA sector bytes, generation and frame index | Reconstruct EXTS input and CD-DA return contribution. FAD/status/repeat/read metadata authenticates provenance but cannot alter output after the exact sector bytes have been joined. |
| Owner tokens, key masks and sample-cut ordinals | Authenticate every external mutation and prove total event/key/sample ordering. Raw scheduler ticks are retained and replay-bounded; emission ordinal is the total order because nested AICA callbacks can report a slightly earlier scheduler tick. |
| Dry, CD-DA, DSP, EFREG and final sample components | Independently recomputed and compared sample by sample; none is trusted as an input to its own comparison. |

Any unknown record, trailing field, unjoined CD-DA generation, unmatched DMA,
sample-cut gap, owner mismatch, or component mismatch rejects the artifact.

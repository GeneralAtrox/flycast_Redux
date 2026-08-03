# Flycast AICA causal evidence v1

The AICA slice records one bounded, authenticated sound-producing interval. It
is intended for save-state-started Lodoss probes, not continuous audio logging.

The typed chain is:

`stable KYON batch -> post-batch AICA checkpoint -> register/RAM/DMA mutations
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
backend and user volume. In particular, the current Lodoss state uses DSP
disabled; capture does not turn it on.

The identity-v2 configuration values must include:

```json
"aica_configuration": {
  "dsp_enabled": false,
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

The validator independently parses the binary state machine, reconstructs the
register and RAM mirrors, decodes each keyed PCM/ADPCM source range, checks
CD-DA frame samples against their captured sectors, rebuilds interleaved PCM,
verifies the payload, PCM, and keyed-source SHA-256 digests, authenticates the
Maple trace against its record-identity digest, requires its DMA count to equal
the identity checkpoint, and requires every AICA tick to lie within that replay
boundary. It does not call
Flycast's AICA mixer, DSP, or GD-ROM reader, so it does not claim an independent
hardware-level resynthesis of the audio.

# SH-4 production dynarec profile v1

`research.Sh4ProfileRecord` captures the real x64 dynarec block inventory and
actual terminal control-flow edges. It does not enable instruction observation
markers and cannot be combined with another typed recorder.

The first capture for a state must record Maple in the same run because normal
dynarec scheduler boundaries differ from the one-instruction equivalence mode.
Use an identity-v3 configuration with `cpu_backend: "dynarec"`,
`dynarec_observation: false`, `dynarec_profile: true`, an authenticated initial
state and a non-zero `maple_dma_checkpoint`:

```powershell
flycast.exe -config "research:IdentityManifest=C:\evidence\identity-v3.json,research:MapleRecord=C:\evidence\production.fcmt,research:Sh4ProfileRecord=C:\evidence\run.fcsh4profile,research:MapleDmaCheckpoint=120,research:DreamcastRtcSeed=2415919104" "C:\games\game.gdi"
```

Close Flycast cleanly after the checkpoint stops execution. Maple publication
must succeed before the profile is published. Existing outputs are never
overwritten.

A repeat run may use identity v2 with
`equivalence.maple_replay_identity_sha256` equal to the exact identity-v3 digest
and replace `MapleRecord` with `MapleReplay`.

Validate either form independently:

```powershell
flycast-research-validate-sh4-profile.exe `
  --artifact C:\evidence\run.fcsh4profile `
  --identity C:\evidence\identity.json `
  --replay C:\evidence\production.fcmt
```

Acceptance requires a finalized typed artifact, exact identity/configuration/
Maple bindings, complete decoder bytes, internally consistent execution and
cycle totals, and one recorded actual edge for every completed branch-bearing
block execution.

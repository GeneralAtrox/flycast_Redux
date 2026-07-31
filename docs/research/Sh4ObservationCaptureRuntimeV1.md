# SH-4 observation capture runtime v1

## Scope

This runtime is the narrow bridge from the backend-neutral native observation
bus to one `flycast-sh4-observation-trace-v1` candidate. It does not compare
backends or publish accepted evidence. An equivalence job launches Flycast
twice and supplies a different identity v2 for each backend while keeping the
exact Maple replay and manifest-set files common.

The required transient options are:

- `research.IdentityManifest` — one backend-specific identity v2;
- `research.MapleReplay` — the exact production Maple replay used by both runs;
- `research.Sh4ObservationManifestSet` — the exact manifest inventory shared
  by both runs;
- `research.Sh4ObservationRecord` — an exclusive candidate trace path; and
- `research.DreamcastRtcSeed` — the exact 32-bit Dreamcast RTC value bound by
  both backend identities; and
- optional positive byte limits through `research.MapleTraceMaxBytes` and
  `research.Sh4ObservationMaxBytes`.

An equivalence run may also use the identity-bound transient
`research.MapleDmaCheckpoint=N`. The common replay must contain exactly N
committed DMA operations. Flycast pauses after replaying the Nth commit, which
gives both backends the same exact scheduler endpoint before clean unload.

Long boot prefixes can use the independently identity-bound transient
`research.Sh4ObservationStartDma=N`. The writer and immutable bindings are
created before execution, but the native observation-bus subscription is
attached only after the Nth replayed DMA begin has been authenticated. Combined
with `MapleDmaCheckpoint=N`, this records the exact DMA causal window and stops
at its terminal commit. A missed start boundary leaves an empty, incomplete
candidate; zero preserves immediate subscription.

Deferred backend-equivalence runs retain the interpreter's ordinary warm-up
timing before that boundary. In research dynarec mode, generated blocks therefore
debit instruction cycles at guest-instruction end using the same persistent
`Sh4Cycles` state and interpreter warm-up ratio; once the native capture runtime
authenticates and activates its boundary, both backends switch at the next
instruction boundary to the precise ratio-one model. The transition is not
derived from the canonical bus subscriber count, so a discovery-only Lua
subscription cannot change guest scheduler timing or move the native capture
boundary. This research-only path replaces dynarec's
block-upfront debit so device-visible ticks and scheduler checkpoints are not
shifted by future instructions in the compiled block. Normal dynarec compilation
and execution retain the existing block debit and do not call the timing helper.
Research blocks contain one top-level guest instruction, with an architectural
delay slot retained in its owning branch block, so the existing host scheduler
check runs at the same legal boundary as the interpreter.

Identity v1 remains frozen and interpreter-only. Identity v2 adds
`configuration.values.dynarec_observation`, a required
`configuration.values.dreamcast_rtc_seed`, and an `equivalence` object whose
`maple_replay_identity_sha256` identifies the original identity bound inside
the Maple v1 trace. This permits the same replay bytes to drive two explicitly
different backend identities without weakening Maple v1 validation or changing
its header.

For an interpreter identity, the runtime forces `Dynarec.Enabled=false` and
`research.DynarecObservation=false`. For a dynarec identity, both are true.
Threaded rendering, state auto-load/save, and GGPO are forced off for both.
The identity's RTC seed is also forced before machine reset, preventing the
normal host-wall-clock initialization from making sequential backend runs
different machine states.
The overrides are applied before reset selects the executor and again before
capture starts; the loaded identity must agree with the resulting settings.

At start, the runtime authenticates the identity, replay, and manifest-set
bytes, creates the trace exclusively with an incomplete header, and subscribes
only to the identity's backend. It re-authenticates all three immutable inputs
after CPU execution has stopped. A write failure, reset, dirty unload, changed
input, or non-clean stop abandons the writer and leaves a rejectable incomplete
candidate. Clean finalization unsubscribes first, waits for any in-flight native
delivery, then writes the complete trace header.

The runtime only hashes the manifest-set file. Typed manifest-inventory
validation belongs to the equivalence job/report layer so the emulator cannot
declare its own inputs acceptable.

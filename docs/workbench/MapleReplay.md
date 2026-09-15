# Maple record and replay

Maple carries controller, keyboard, and VMU traffic. Recording it once and
replaying it makes a boot deterministic: the same inputs arrive at the same
scheduler ticks, so two runs can be compared event for event and a moment
can be reached again without a human at the controller.

## Record

```text
-config research:MapleRecord=C:\runs\boot.fcmr
```

Play. On clean exit the trace is finalized. The runtime pins non-threaded
rendering, disables auto-save state and GGPO, enables
`research.DynarecObservation` when the dynarec is on (so ticks match the
interpreter's instruction-boundary model), and fixes `research.DreamcastRtcSeed`
to 2000-01-01 unless you set one. Use the same backend and seed for replay.

## Replay

```text
-config research:MapleReplay=C:\runs\boot.fcmr
```

Each DMA, transaction, schedule, and commit is checked against the recording.
The first mismatch stops the run with a `Maple research replay divergence`
error naming the field. Controller hot-plug and reset invalidate a session.

`research:MapleDmaCheckpoint=N` pauses the emulator after the N-th committed
DMA, in both modes, so a recording can be resumed from an exact point or a
capture can be stopped there. The control socket `status` reports
`maple_recording` and `maple_replaying`.

## File format

`.fcmr`: a 160-byte little-endian header (magic `FCRMAPLE`, schema version 1
or 2, endian sentinel `0x01020304`, complete flag, event/transaction/DMA
counts, first and last tick, payload bytes, dropped events, a 32-byte tag,
payload SHA-256, header CRC-32) followed by events with a 16-byte envelope
(type, size, ordinal). Per DMA the order is `DmaBegin`, `Transaction*`,
`DmaSchedule`, `DmaCommit`; version 2 adds `ControlDescriptor` for NOP
descriptors. The 32-byte tag was an identity digest in earlier builds; it is
now written as zeros and ignored on load, so older traces still replay.

Readers and writers live in `core/research/maple_trace*`, the replay session
in `core/research/maple_runtime*`.

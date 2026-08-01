# Maple research trace v2

Maple trace v2 preserves every v1 event and header binding while adding one
typed control-descriptor event. Existing v1 files remain byte-for-byte stable
and are still accepted by the loader and independent validator. New emulator
recordings use v2.

## NOP control descriptor

`ControlDescriptor` records the DMA ordinal, a monotonic control ordinal,
scheduler tick, exact guest descriptor address and header word, decoded
operation, and terminal bit. V2 currently admits only Maple operation 7
(`NOP`). RESET and SDCKB occupy/cancel remain rejected.

The recorder, replay runtime, and independent validator require the descriptor
address chain to begin at `SB_MDSTAR`. A START descriptor advances by its two
header words plus request bytes; a NOP advances by four bytes. Exactly one
terminal descriptor must be observed before `DmaSchedule`. NOP contributes no
device response, bus-state mutation, or transfer-time bytes, matching the
emulator boundary in `DoMapleDma`.

The v2 header uses eight bytes previously reserved by v1 for the exact control
descriptor count. All other header and event framing rules remain unchanged.

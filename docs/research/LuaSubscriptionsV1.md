# Lua research discovery subscriptions

## Boundary

`flycast.research` is a discovery-only view of the canonical native SH-4,
Maple, PowerVR, GD-ROM, and AICA observation buses. Lua receives a newly
constructed table copied from a native observation. It cannot modify the
native observation, recorder state, typed trace, validator, equivalence
receipt, or atomic evidence package.

Lua output is never accepted evidence. Evidence still comes from the native
typed recorder, independent validator, and atomic publication workflow.

For a ready-to-run Maple request/response consumer, see
[Maple Lua watcher](MapleLuaWatcher.md). It pairs transactions and emits
discovery-only JSONL with explicit overflow and completeness reporting.
For bounded read/write provenance, see
[Memory Lua watcher](MemoryLuaWatcher.md). It records exact native access
events with their owning SH-4 instruction and optional lifecycle snapshots.
For a targeted second pass after a producer PC is known, see
[Lua causal-slice watcher](CausalSliceLuaWatcher.md). It joins the producer's
pre/post registers, matching accesses, outcome, and explicitly partial
observed call path without tracing every instruction.
For filtered hardware discovery, see the
[hardware Lua watcher](HardwareLuaWatcher.md). It records selected TA,
semantic draw, PVR register/VRAM, framebuffer/presentation, GD-ROM, and AICA
events in a bounded JSONL session.

## API

```lua
local token = flycast.research.subscribe({
    event = "memory-write",
    backend = "interpreter", -- "interpreter", "dynarec", or "any"
    start_address = 0x8c000000,
    end_address = 0x8c00ffff, -- inclusive
    start_pc = 0x8c010000,
    end_pc = 0x8c0101ff, -- inclusive writer/instruction owner PC
    queue_capacity = 4096
}, function(event)
    print(event.event, event.pc, event.address, event.value_hex)
end)

local stats = flycast.research.subscription_stats(token)
print(stats.queued, stats.delivered, stats.dropped, stats.callback_errors)

flycast.research.unsubscribe(token)
```

The initial `event` values are:

- `instruction-begin` (pre-execution registers and next PC);
- `instruction` (a successfully completed instruction);
- `instruction-abort` (an instruction that did not complete);
- `call`;
- `return`;
- `memory-read`;
- `memory-write`;
- `exception`;
- `maple-request`; and
- `maple-response`.

PowerVR events are `pvr-ta-list-init`, `pvr-ta-list-continue`, `pvr-ta-block`,
`pvr-start-render`, `pvr-render-done`, `pvr-ta-reset`, `pvr-register-write`,
`pvr-vram-write`, `pvr-render-queued`, `pvr-render-completed`,
`pvr-framebuffer`, `pvr-presentation`, `pvr-presentation-reset`,
`pvr-primitive`, `pvr-draw`, `pvr-draw-render-completed`, and `pvr-draw-reset`.
GD-ROM events are `gdrom-command`, `gdrom-transfer`, `gdrom-complete`,
`gdrom-abort`, and `gdrom-reset`. AICA events are `aica-register-write`,
`aica-ram-write`, `aica-dma-begin`, `aica-dma-transfer`, `aica-dma-complete`,
`aica-key-on`, `aica-key-off`, `aica-sample`, `aica-reset`, `aica-key-batch`,
`aica-cdda-sector`, and `aica-sample-suppressed`.

`event` is required. For SH-4 events, `backend` defaults to `any`. Address
fields must be provided together, form an inclusive 32-bit interval, and are
valid only for memory events. `start_pc` and `end_pc` must also be provided
together and form an inclusive 32-bit interval. They filter the canonical
owning instruction PC for every SH-4 event kind, including the writer PC of
memory events. Both filters run natively before queueing, so a script may
combine an address range with a producer-PC range without flooding its Lua
queue. Maple-only filters on SH-4 events and SH-4-only filters on Maple events
are rejected rather than ignored. `queue_capacity` defaults to 4096 and may be
from 1 through 65536. At most 64 Lua subscriptions and 65536 total queued
deliveries are allowed in one Lua runtime.

`pvr-ta-block` accepts `source` (`store-queue`, `channel2-dma`, or `sort-dma`).
PVR register/VRAM writes accept an inclusive `start_address`/`end_address`
range. PVR draw events accept `render_generation`. `gdrom-transfer` accepts an
inclusive `start_fad`/`end_fad` range. AICA register/RAM writes accept an
inclusive address range; AICA events accept `writer`; and key events accept a
zero-based `channel`. These filters execute before an event enters the Lua
queue. Discovery subscriptions cannot coexist with an exclusive evidence
subscriber on the same PowerVR, GD-ROM, or AICA bus.

Maple subscriptions accept optional zero-based `bus` (`0..3`), resolved
`port` (`0..5`, where `5` is the main device), and initiating `command`
(`0..255`) filters. The filters execute natively before queueing. A successful
Maple transaction publishes its canonical request first and then the final
response selected for the pending guest transfer. A later DMA abort or overrun
can still prevent the transfer from committing. During deterministic replay
the selected payload is the authenticated replay response, not the discarded
shadow-device response.

```lua
local maple = flycast.research.subscribe({
    event = "maple-response",
    bus = 0,
    port = 5,
    command = 0x09
}, function(event)
    print(event.transaction_ordinal_decimal, event.response_code,
          event.payload_hex)
end)
```

The returned token is process-local and supports explicit unsubscribe.
`subscription_stats(token)` returns `nil` after unsubscribe or lifecycle
cleanup. Its table contains `active`, `capacity`, `queued`, `delivered`,
`dropped`, and `callback_errors`, plus `discovery = true`.

## Event tables

Every event table contains:

- `discovery = true` and `authoritative_evidence = false`;
- `schema_version` and `event`;
- the native process emission `ordinal` and exact `ordinal_decimal` string;
- `tick` and exact `tick_decimal` string.

SH-4 event tables additionally contain:

- `backend`;
- `pc`, `opcode`, and `delay_slot_depth`; and
- `next_pc` and `registers` when the native observation declares them present.

The register table contains one-based `r[1]` through `r[16]` corresponding to
SH-4 R0 through R15, plus `pr`, `gbr`, `vbr`, `mach`, `macl`, `sr`, `fpul`, and
`fpscr`.

Memory events add `address`, `width`, `value`, and an exact 16-digit
`value_hex`. Exception events add `exception_pc`, `vector_pc`, and
`exception_code`. Call and return events add `target_pc`, `return_pc`, and
`delay_slot_pc`; calls also add `call_kind` (`bsr`, `bsrf`, or `jsr`). Lua 5.2
uses floating-point numbers, so consumers needing exact 64-bit values must use
the supplied decimal/hex strings.

Maple events add exact decimal strings for `dma_ordinal` and
`transaction_ordinal`; descriptor and destination addresses; both descriptor
headers; `bus`, `port`, the initiating `command`, and device presence/type.
`payload` is the complete canonical binary Maple frame and `payload_hex` is its
lowercase hexadecimal representation; `byte_count` and `frame_code` describe
that frame. A response also exposes `response_code`. Request and response
events for one transaction share the same transaction ordinal. These are
process-local discovery ordinals, not frozen Maple trace ordinals.

## Delivery and overflow

The native observation buses and discovery adapters apply backend, event,
address, PC, Maple topology/command, hardware source, generation, FAD, writer,
and channel filters before queuing. Emulator producer threads never call Lua.
Matching observations are copied into one bounded native queue in
canonical enqueue order and delivered from `lua::overlay` on the thread that
created the Lua runtime. Each overlay delivers at most 1024 callbacks so a
discovery script cannot monopolize a frame.

When a per-subscription or global limit is full, the newest matching delivery
is dropped. This preserves the queued prefix and increments that token's
`dropped` count. A callback exception increments `callback_errors`, is logged,
and does not prevent later subscribers from running.

Request/response pairing is best-effort under overflow because each matching
event consumes one queue entry. Consumers subscribing to both kinds must join
on `transaction_ordinal_decimal` and treat a nonzero drop count as incomplete
discovery output.

Nested delivery is suppressed. A callback may safely unsubscribe itself; its
remaining queued deliveries are removed. Event tables are independent copies,
so modifying one cannot change native artifact contents.

## Lifecycle

All native subscriptions and queued events are removed before an emulator
`Terminate` Lua callback, before an explicitly loaded script is executed again,
and before the Lua state closes. Subscribing from a terminate callback is
rejected. A later emulator `Start` callback may establish new subscriptions.
No queued research callback can run after unload or against a replaced/closed
Lua state.

Dynarec events exist only when the transient
`research.DynarecObservation=yes` mode compiled the active blocks with native
research markers. A Lua subscription does not enable that mode, select a CPU
backend, flush compiled blocks, change the native capture's execution-timing
gate, or alter an evidence recorder.

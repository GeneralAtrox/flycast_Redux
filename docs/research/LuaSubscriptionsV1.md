# Lua SH-4 discovery subscriptions

## Boundary

`flycast.research` is a discovery-only view of the canonical native
`Sh4ObservationV1` bus. Lua receives a newly constructed table copied from a
native observation. It cannot modify the native observation, recorder state,
typed trace, validator, equivalence receipt, or atomic evidence package.

Lua output is never accepted evidence. Evidence still comes from the native
typed recorder, independent validator, and atomic publication workflow.

## API

```lua
local token = flycast.research.subscribe({
    event = "memory-write",
    backend = "interpreter", -- "interpreter", "dynarec", or "any"
    start_address = 0x8c000000,
    end_address = 0x8c00ffff, -- inclusive
    queue_capacity = 4096
}, function(event)
    print(event.event, event.pc, event.address, event.value_hex)
end)

local stats = flycast.research.subscription_stats(token)
print(stats.queued, stats.delivered, stats.dropped, stats.callback_errors)

flycast.research.unsubscribe(token)
```

The initial `event` values are:

- `instruction` (a successfully completed instruction);
- `call`;
- `return`;
- `memory-read`;
- `memory-write`; and
- `exception`.

`event` is required. `backend` defaults to `any`. Address fields must be
provided together, form an inclusive 32-bit interval, and are valid only for
memory events. `queue_capacity` defaults to 4096 and may be from 1 through
65536. At most 64 Lua subscriptions and 65536 total queued deliveries are
allowed in one Lua runtime.

The returned token is process-local and supports explicit unsubscribe.
`subscription_stats(token)` returns `nil` after unsubscribe or lifecycle
cleanup. Its table contains `active`, `capacity`, `queued`, `delivered`,
`dropped`, and `callback_errors`, plus `discovery = true`.

## Event tables

Every event table contains:

- `discovery = true` and `authoritative_evidence = false`;
- `schema_version`, `event`, and `backend`;
- the native process emission `ordinal` and exact `ordinal_decimal` string;
- `tick` and exact `tick_decimal` string;
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

## Delivery and overflow

The native observation bus applies backend, event, and address filters before
queuing. The SH-4 execution thread never calls Lua. Matching observations are
copied into a bounded native queue in canonical emission order and delivered
from `lua::overlay` on the thread that created the Lua runtime. Each overlay
delivers at most 1024 callbacks so a discovery script cannot monopolize a
frame.

When a per-subscription or global limit is full, the newest matching delivery
is dropped. This preserves the queued prefix and increments that token's
`dropped` count. A callback exception increments `callback_errors`, is logged,
and does not prevent later subscribers from running.

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

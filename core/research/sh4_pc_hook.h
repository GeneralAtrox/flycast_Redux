#pragma once

// A few guest PCs that call back on the emulator thread just before their
// instruction runs (flycast.research.pc_hook). The dynarec decoder emits a
// research_pc_hook op ahead of a hooked instruction; the op adds no guest
// cycles and does not count toward the block's size, so blocks, cycle counts
// and interrupt timing are the same as without the hook. Guest memory is
// current at the call (stores are never deferred); the callback also
// gets r4 and r5 at the hooked instruction (the op reads them as sources).
// The interpreter does not call hooks. Hooks must be set before the game
// starts (blocks compiled earlier do not have them).

#include "types.h"

#include <functional>

namespace research
{

constexpr unsigned SH4_PC_HOOK_MAX = 8;

// Decoder: whether the instruction at pc gets a hook op.
bool sh4PcHookAt(u32 pc) noexcept;
// Replaces the hooked PCs and the callback (count <= SH4_PC_HOOK_MAX).
void sh4PcHookSet(const u32 *pcs, unsigned count, std::function<void(u32, u32, u32)> callback);
void sh4PcHookClear() noexcept;
// The hook op's call. Never throws into the generated code.
void sh4PcHookRun(u32 pc, u32 r4, u32 r5) noexcept;

} // namespace research

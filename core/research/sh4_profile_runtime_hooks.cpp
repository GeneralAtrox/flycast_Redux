#include "research/sh4_profile.h"

#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_sched.h"

namespace research
{
namespace
{
std::uint64_t currentDynarecTick() noexcept
{
	const std::int64_t tick = static_cast<std::int64_t>(sh4_sched_now64())
			+ SH4_TIMESLICE - Sh4cntx.cycle_counter;
	return tick < 0 ? 0 : static_cast<std::uint64_t>(tick);
}
}

void sh4DynarecProfileBlockEnter(std::uint64_t generation) noexcept
{
	sh4DynarecProfileBlockEnterAt(generation, currentDynarecTick());
}

void sh4DynarecProfileBlockExit(std::uint64_t generation,
		std::uint32_t destination) noexcept
{
	sh4DynarecProfileBlockExitAt(generation, destination, currentDynarecTick());
}

} // namespace research

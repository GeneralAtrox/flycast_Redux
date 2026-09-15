#pragma once

// Private CD-DA observation bus state shared between cdda_observation.cpp
// (subscription and publish skeleton) and cdda_observation_events.cpp
// (observe*/reset* emitters). Every definition lives in cdda_observation.cpp.

#include "research/cdda_observation.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace research
{
namespace detail_cdda
{

// The accepted/applied protocol stashes the accepted control here under
// stateMutex; the matching applied observation consumes it.
struct PendingControl
{
	bool active = false;
	std::uint64_t generation = 0;
	CddaControlPath path = CddaControlPath::ReiosHle;
	Sh4InstructionOwnerToken initiator;
	std::uint32_t requestId = 0;
	std::uint32_t command = 0;
	std::array<std::uint32_t, 4> parameters {};
};

extern std::mutex stateMutex;
extern std::atomic<std::uint64_t> nextControlGeneration;
extern std::atomic<std::uint32_t> nextPacketRequest;
extern PendingControl pendingControl;
extern std::uint64_t activeControlGeneration;

void dropped() noexcept;
bool publish(CddaObservation observation) noexcept;

} // namespace detail_cdda
} // namespace research

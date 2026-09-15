#pragma once

// Private PowerVR TA observation bus state shared between
// pvr_ta_observation.cpp (subscription and publish skeleton) and
// pvr_ta_observation_events.cpp (observe*/reset* emitters). Every definition
// lives in pvr_ta_observation.cpp.

#include "research/pvr_ta_observation.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace research
{
namespace detail_pvr_ta
{

struct ContextState
{
	std::uint64_t generation = 0;
	std::uint64_t nextBlockOrdinal = 0;
};

extern std::mutex stateMutex;
extern std::unordered_map<std::uint32_t, ContextState> contexts;
extern std::unordered_set<std::uint64_t> observedRenderGenerations;
extern bool observedRenderGenerationWindowFrozen;
extern std::uint64_t pendingRenderGeneration;
extern std::atomic<std::uint64_t> nextContextGeneration;
extern std::atomic<std::uint64_t> nextRenderGeneration;

void noteDroppedObservation() noexcept;
bool publish(PvrTaObservation observation) noexcept;

} // namespace detail_pvr_ta
} // namespace research

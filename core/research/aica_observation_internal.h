#pragma once

// Private AICA observation bus state shared between aica_observation.cpp
// (subscription and publish skeleton) and aica_observation_events.cpp
// (observe*/reset* emitters). Every definition lives in aica_observation.cpp.

#include "research/aica_observation.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace research
{
namespace detail_aica
{

struct DmaState
{
	bool active = false;
	std::uint64_t generation = 0;
	AicaOwnerToken owner;
	std::uint32_t sourceAddress = 0;
	std::uint32_t destinationAddress = 0;
	std::uint32_t transferLength = 0;
	bool aicaRamIsDestination = false;
};

extern std::mutex stateMutex;
extern std::atomic<std::uint64_t> nextDmaGeneration;
extern std::atomic<std::uint64_t> nextCddaGeneration;
extern std::atomic<std::uint64_t> nextSampleOrdinal;
extern DmaState dmaState;
extern thread_local AicaWriter currentWriter;

void dropped() noexcept;
bool publish(AicaObservation observation) noexcept;

} // namespace detail_aica
} // namespace research

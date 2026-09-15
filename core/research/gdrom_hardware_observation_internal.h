#pragma once

// Private GD-ROM hardware observation bus state shared between
// gdrom_hardware_observation.cpp (subscription and publish skeleton) and
// gdrom_hardware_observation_events.cpp (observe*/reset* emitters). Every
// definition lives in gdrom_hardware_observation.cpp.

#include "research/gdrom_hardware_observation.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace research
{
namespace detail_gdrom_hardware
{

struct PendingAta
{
	bool active = false;
	Sh4InstructionOwnerToken owner;
	std::uint64_t tick = 0;
	std::uint32_t features = 0;
	std::uint32_t byteCount = 0;
	std::uint32_t driveState = 0;
};

struct ActiveCommand
{
	bool active = false;
	std::uint64_t generation = 0;
	std::uint64_t dmaGeneration = 0;
	std::uint64_t transferred = 0;
	std::uint64_t expected = 0;
	std::uint64_t produced = 0;
	GdromHardwareDelivery delivery = GdromHardwareDelivery::Pio;
};

extern std::mutex stateMutex;
extern std::atomic<std::uint64_t> nextCommand;
extern PendingAta pendingAta;
extern ActiveCommand command;
extern std::uint64_t completedAwaitingStatus;
extern std::uint64_t completedTransferred;

void dropped(const char* reason = "internal observation failure") noexcept;
bool publish(GdromHardwareObservation observation) noexcept;

} // namespace detail_gdrom_hardware
} // namespace research

#pragma once

#include "research/sh4_observation_runtime.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace research
{

constexpr std::uint32_t GdromObservationSchemaVersion = 1;

enum class GdromObservationType : std::uint8_t
{
	CommandBegin = 1,
	TransferChunk = 2,
	Complete = 3,
	Abort = 4,
	Reset = 5,
};

enum class GdromPath : std::uint8_t
{
	ReiosHle = 1,
};

enum class GdromCompletionMechanism : std::uint8_t
{
	Status = 1,
	GdromCommandInterrupt = 2,
	GdromDmaInterrupt = 3,
};

struct GdromObservation
{
	std::uint32_t schemaVersion = GdromObservationSchemaVersion;
	std::uint64_t emissionOrdinal = 0;
	GdromObservationType type = GdromObservationType::CommandBegin;
	std::uint64_t tick = 0;
	std::uint64_t commandGeneration = 0;
	GdromPath path = GdromPath::ReiosHle;
	Sh4InstructionOwnerToken initiator;
	std::uint32_t requestId = 0;
	std::uint32_t command = 0;
	std::array<std::uint32_t, 4> parameters {};
	std::uint64_t chunkOrdinal = 0;
	std::uint32_t fad = 0;
	std::uint32_t sectorCount = 0;
	std::uint32_t destination = 0;
	std::vector<std::uint8_t> bytes;
	GdromCompletionMechanism completion = GdromCompletionMechanism::Status;
	std::uint64_t transferredBytes = 0;
};

using GdromObservationSubscription = std::uint64_t;
using GdromObservationCallback = std::function<void(const GdromObservation&)>;

#ifdef LIBRETRO
inline GdromObservationSubscription subscribeGdromObservations(
		GdromObservationCallback) { return 0; }
inline GdromObservationSubscription subscribeGdromEvidenceObservations(
		GdromObservationCallback) { return 0; }
inline bool unsubscribeGdromObservations(GdromObservationSubscription) noexcept { return false; }
inline bool gdromObservationBusActive() noexcept { return false; }
inline bool reiosGdromObservationCommandActive() noexcept { return false; }
inline std::uint64_t gdromObservationDroppedCount() noexcept { return 0; }
inline void observeReiosGdromCommand(std::uint32_t, std::uint32_t,
		const std::uint32_t*, std::uint64_t) noexcept {}
inline void observeReiosGdromTransfer(std::uint32_t, std::uint32_t,
		std::uint32_t, const std::uint8_t*, std::size_t, std::uint64_t) noexcept {}
inline void observeReiosGdromComplete(std::uint64_t) noexcept {}
inline void observeReiosGdromAbort(std::uint32_t, std::uint64_t) noexcept {}
inline void resetGdromObservation(std::uint64_t) noexcept {}
#else
GdromObservationSubscription subscribeGdromObservations(
		GdromObservationCallback callback);
GdromObservationSubscription subscribeGdromEvidenceObservations(
		GdromObservationCallback callback);
bool unsubscribeGdromObservations(GdromObservationSubscription subscription) noexcept;
bool gdromObservationBusActive() noexcept;
bool reiosGdromObservationCommandActive() noexcept;
std::uint64_t gdromObservationDroppedCount() noexcept;
void observeReiosGdromCommand(std::uint32_t requestId, std::uint32_t command,
		const std::uint32_t parameters[4], std::uint64_t tick) noexcept;
void observeReiosGdromTransfer(std::uint32_t fad, std::uint32_t sectorCount,
		std::uint32_t destination, const std::uint8_t* bytes,
		std::size_t byteCount, std::uint64_t tick) noexcept;
void observeReiosGdromComplete(std::uint64_t tick) noexcept;
void observeReiosGdromAbort(std::uint32_t requestId, std::uint64_t tick) noexcept;
void resetGdromObservation(std::uint64_t tick) noexcept;
#endif

} // namespace research

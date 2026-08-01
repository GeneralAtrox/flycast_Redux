#pragma once

#include "research/maple_trace.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace research
{

constexpr std::uint32_t MapleObservationSchemaVersion = 1;

enum class MapleObservationType : std::uint8_t
{
	Request = 1,
	Response = 2,
};

constexpr std::uint32_t mapleObservationTypeBit(MapleObservationType type)
{
	return std::uint32_t {1} << (static_cast<unsigned>(type) - 1u);
}

constexpr std::uint32_t AllMapleObservationTypes =
		mapleObservationTypeBit(MapleObservationType::Request)
		| mapleObservationTypeBit(MapleObservationType::Response);

struct MapleObservation
{
	std::uint32_t schemaVersion = MapleObservationSchemaVersion;
	std::uint64_t emissionOrdinal = 0;
	std::uint64_t dmaOrdinal = 0;
	std::uint64_t transactionOrdinal = 0;
	MapleObservationType type = MapleObservationType::Request;
	std::uint64_t tick = 0;
	std::uint32_t descriptorAddress = 0;
	std::uint32_t destinationAddress = 0;
	std::uint32_t descriptorHeader1 = 0;
	std::uint32_t descriptorHeader2 = 0;
	std::uint32_t deviceType = UINT32_MAX;
	std::uint8_t bus = 0;
	std::uint8_t port = 0;
	std::uint8_t command = 0;
	std::uint8_t flags = 0;
	std::vector<std::uint8_t> payload;
};

struct MapleObservationFilter
{
	std::uint32_t typeMask = AllMapleObservationTypes;
	std::uint8_t busMask = 0x0f;
	std::uint8_t portMask = 0x3f;
	bool hasCommand = false;
	std::uint8_t command = 0;
};

using MapleObservationSubscription = std::uint64_t;
using MapleObservationCallback = std::function<void(const MapleObservation&)>;

#ifdef LIBRETRO

inline MapleObservationSubscription subscribeMapleObservations(
		const MapleObservationFilter&, MapleObservationCallback)
{
	return 0;
}
inline bool unsubscribeMapleObservations(MapleObservationSubscription) noexcept
{
	return false;
}
inline bool mapleObservationBusActive() noexcept { return false; }
inline std::size_t mapleObservationSubscriberCount() noexcept { return 0; }
inline std::uint64_t beginMapleObservationDma() noexcept { return UINT64_MAX; }
inline bool publishMapleTransactionObservations(std::uint64_t,
		const MapleTransactionEvent&) noexcept

{
	return false;
}
inline bool publishMapleTransactionObservations(std::uint64_t,
		const MapleTransactionEvent&, const std::vector<std::uint8_t>&) noexcept
{
	return false;
}

#else

MapleObservationSubscription subscribeMapleObservations(
		const MapleObservationFilter& filter, MapleObservationCallback callback);
bool unsubscribeMapleObservations(MapleObservationSubscription subscription) noexcept;
bool mapleObservationBusActive() noexcept;
std::size_t mapleObservationSubscriberCount() noexcept;

// Returns a process-local zero-based DMA ordinal when observation is active.
// UINT64_MAX means no subscriber owned the DMA begin boundary.
std::uint64_t beginMapleObservationDma() noexcept;

// Publishes a successful canonical transaction as an ordered request followed
// by the final response delivered to the guest. The trace event is copied and
// neither the frozen Maple trace nor replay behavior is changed.
bool publishMapleTransactionObservations(std::uint64_t observationDmaOrdinal,
		const MapleTransactionEvent& transaction) noexcept;
bool publishMapleTransactionObservations(std::uint64_t observationDmaOrdinal,
		const MapleTransactionEvent& transaction,
		const std::vector<std::uint8_t>& selectedResponse) noexcept;

#endif

} // namespace research

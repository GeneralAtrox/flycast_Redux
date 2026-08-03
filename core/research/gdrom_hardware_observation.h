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

constexpr std::uint32_t GdromHardwareObservationSchemaVersion = 1;

enum class GdromHardwareObservationType : std::uint8_t
{
	PacketAccepted = 1,
	BufferFill = 2,
	DmaBegin = 3,
	DmaTransfer = 4,
	PioReady = 5,
	PioWord = 6,
	DmaInterrupt = 7,
	CommandInterrupt = 8,
	Complete = 9,
	Abort = 10,
	Reset = 11,
	LoadState = 12,
	StatusAcknowledged = 13,
};

enum class GdromHardwareDelivery : std::uint8_t
{
	Pio = 1,
	Dma = 2,
};

enum class GdromHardwareAbortReason : std::uint8_t
{
	DmaDisabled = 1,
	Reset = 2,
	LoadState = 3,
	OverlappingPacket = 4,
};

struct GdromHardwareObservation
{
	std::uint32_t schemaVersion = GdromHardwareObservationSchemaVersion;
	std::uint64_t emissionOrdinal = 0;
	GdromHardwareObservationType type = GdromHardwareObservationType::PacketAccepted;
	std::uint64_t tick = 0;
	std::uint64_t commandGeneration = 0;
	std::uint64_t dmaGeneration = 0;
	Sh4InstructionOwnerToken ataOwner;
	Sh4InstructionOwnerToken packetOwner;
	std::array<std::uint8_t, 12> packet {};
	std::uint32_t features = 0;
	std::uint32_t byteCountRegister = 0;
	std::uint32_t driveState = 0;
	std::uint32_t startFad = 0;
	std::uint32_t sectorCount = 0;
	std::uint32_t sectorBytes = 0;
	GdromHardwareDelivery delivery = GdromHardwareDelivery::Pio;
	bool readSuccessful = false;
	std::uint64_t streamOffset = 0;
	std::uint32_t destination = 0;
	std::uint32_t dmaStar = 0;
	std::uint32_t dmaLength = 0;
	std::uint32_t dmaDirection = 0;
	std::uint32_t dmaEnabled = 0;
	std::uint32_t pioWord = 0;
	std::uint32_t statusRegister = 0;
	std::vector<std::uint8_t> bytes;
	GdromHardwareAbortReason abortReason = GdromHardwareAbortReason::Reset;
	std::uint64_t transferredBytes = 0;
};

using GdromHardwareObservationSubscription = std::uint64_t;
using GdromHardwareObservationCallback =
		std::function<void(const GdromHardwareObservation&)>;

#ifdef LIBRETRO
inline GdromHardwareObservationSubscription subscribeGdromHardwareObservations(
		GdromHardwareObservationCallback) { return 0; }
inline GdromHardwareObservationSubscription subscribeGdromHardwareEvidenceObservations(
		GdromHardwareObservationCallback) { return 0; }
inline bool unsubscribeGdromHardwareObservations(
		GdromHardwareObservationSubscription) noexcept { return false; }
inline bool gdromHardwareObservationBusActive() noexcept { return false; }
inline std::uint64_t gdromHardwareObservationDroppedCount() noexcept { return 0; }
inline void observeGdromHardwareAtaPacket(std::uint32_t, std::uint32_t,
		std::uint32_t, std::uint64_t) noexcept {}
inline void observeGdromHardwarePacket(const std::uint8_t*, std::uint32_t,
		std::uint32_t, std::uint32_t, std::uint32_t, bool, std::uint64_t) noexcept {}
inline void observeGdromHardwareBufferFill(std::uint32_t, std::uint32_t,
		std::uint32_t, bool, std::uint64_t) noexcept {}
inline void observeGdromHardwareDmaBegin(std::uint32_t, std::uint32_t,
		std::uint32_t, std::uint32_t, std::uint64_t) noexcept {}
inline void observeGdromHardwareDmaTransfer(std::uint32_t,
		const std::uint8_t*, std::size_t, std::uint64_t) noexcept {}
inline void observeGdromHardwarePioReady(std::uint32_t, std::uint32_t,
		std::uint32_t, bool, std::uint64_t) noexcept {}
inline void observeGdromHardwarePioWord(std::uint16_t, std::uint64_t) noexcept {}
inline void observeGdromHardwareDmaInterrupt(std::uint64_t) noexcept {}
inline void observeGdromHardwareCommandInterrupt(std::uint64_t) noexcept {}
inline void observeGdromHardwareComplete(std::uint64_t) noexcept {}
inline void observeGdromHardwareStatusAcknowledged(std::uint32_t,
		std::uint64_t) noexcept {}
inline void observeGdromHardwareAbort(GdromHardwareAbortReason,
		std::uint64_t) noexcept {}
inline void resetGdromHardwareObservation(std::uint64_t) noexcept {}
inline void loadStateGdromHardwareObservation(std::uint64_t) noexcept {}
#else
GdromHardwareObservationSubscription subscribeGdromHardwareObservations(
		GdromHardwareObservationCallback callback);
GdromHardwareObservationSubscription subscribeGdromHardwareEvidenceObservations(
		GdromHardwareObservationCallback callback);
bool unsubscribeGdromHardwareObservations(
		GdromHardwareObservationSubscription subscription) noexcept;
bool gdromHardwareObservationBusActive() noexcept;
std::uint64_t gdromHardwareObservationDroppedCount() noexcept;
void observeGdromHardwareAtaPacket(std::uint32_t features,
		std::uint32_t byteCount, std::uint32_t driveState,
		std::uint64_t tick) noexcept;
void observeGdromHardwarePacket(const std::uint8_t packet[12],
		std::uint32_t startFad, std::uint32_t sectorCount,
		std::uint32_t sectorBytes, bool dma, std::uint64_t tick) noexcept;
void observeGdromHardwareBufferFill(std::uint32_t startFad,
		std::uint32_t sectorCount, std::uint32_t sectorBytes,
		bool successful, std::uint64_t tick) noexcept;
void observeGdromHardwareDmaBegin(std::uint32_t star, std::uint32_t length,
		std::uint32_t direction, std::uint32_t enabled,
		std::uint64_t tick) noexcept;
void observeGdromHardwareDmaTransfer(std::uint32_t destination,
		const std::uint8_t* bytes, std::size_t byteCount,
		std::uint64_t tick) noexcept;
void observeGdromHardwarePioReady(std::uint32_t startFad,
		std::uint32_t sectorCount, std::uint32_t sectorBytes,
		bool successful, std::uint64_t tick) noexcept;
void observeGdromHardwarePioWord(std::uint16_t value, std::uint64_t tick) noexcept;
void observeGdromHardwareDmaInterrupt(std::uint64_t tick) noexcept;
void observeGdromHardwareCommandInterrupt(std::uint64_t tick) noexcept;
void observeGdromHardwareComplete(std::uint64_t tick) noexcept;
void observeGdromHardwareStatusAcknowledged(std::uint32_t status,
		std::uint64_t tick) noexcept;
void observeGdromHardwareAbort(GdromHardwareAbortReason reason,
		std::uint64_t tick) noexcept;
void resetGdromHardwareObservation(std::uint64_t tick) noexcept;
void loadStateGdromHardwareObservation(std::uint64_t tick) noexcept;
#endif

} // namespace research

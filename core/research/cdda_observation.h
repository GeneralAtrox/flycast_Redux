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

constexpr std::uint32_t CddaObservationSchemaVersion = 2;

enum class CddaObservationType : std::uint8_t
{
	ControlAccepted = 1,
	ControlApplied = 2,
	Sector = 3,
	Reset = 4,
};

enum class CddaControlPath : std::uint8_t
{
	ReiosHle = 1,
	GdromPacket = 2,
};

struct CddaDriveState
{
	std::uint32_t status = 0;
	std::uint32_t repeats = 0;
	std::uint32_t currentFad = 0;
	std::uint32_t startFad = 0;
	std::uint32_t endFad = 0;
};

struct CddaObservation
{
	std::uint32_t schemaVersion = CddaObservationSchemaVersion;
	std::uint64_t emissionOrdinal = 0;
	CddaObservationType type = CddaObservationType::ControlAccepted;
	std::uint64_t tick = 0;
	std::uint64_t controlGeneration = 0;
	CddaControlPath path = CddaControlPath::ReiosHle;
	Sh4InstructionOwnerToken initiator;
	std::uint32_t requestId = 0;
	std::uint32_t command = 0;
	std::array<std::uint32_t, 4> parameters {};
	CddaDriveState before;
	CddaDriveState after;
	bool appliedSuccessfully = false;

	std::uint64_t aicaGeneration = 0;
	std::uint32_t fad = 0;
	bool readSuccessful = false;
	std::vector<std::uint8_t> bytes;
};

using CddaObservationSubscription = std::uint64_t;
using CddaObservationCallback = std::function<void(const CddaObservation&)>;

bool isReiosCddaControlCommand(std::uint32_t command) noexcept;
bool isGdromPacketCddaControlCommand(std::uint32_t command) noexcept;
bool isCddaControlCommand(std::uint32_t command) noexcept;

#ifdef LIBRETRO
inline CddaObservationSubscription subscribeCddaObservations(
		CddaObservationCallback) { return 0; }
inline CddaObservationSubscription subscribeCddaEvidenceObservations(
		CddaObservationCallback) { return 0; }
inline bool unsubscribeCddaObservations(CddaObservationSubscription) noexcept { return false; }
inline bool cddaObservationBusActive() noexcept { return false; }
inline std::uint64_t cddaObservationDroppedCount() noexcept { return 0; }
inline void observeReiosCddaControlAccepted(std::uint32_t, std::uint32_t,
		const std::uint32_t*, std::uint64_t) noexcept {}
inline void observeReiosCddaControlApplied(std::uint32_t, std::uint32_t,
		const CddaDriveState&, const CddaDriveState&, bool, std::uint64_t) noexcept {}
inline void observeGdromPacketCddaControlAccepted(const std::uint8_t*,
		std::uint64_t) noexcept {}
inline void observeGdromPacketCddaControlApplied(std::uint32_t,
		const CddaDriveState&, const CddaDriveState&, bool, std::uint64_t) noexcept {}
inline void observeCddaSector(std::uint64_t, std::uint32_t,
		const CddaDriveState&, const CddaDriveState&, bool,
		const std::uint8_t*, std::size_t, std::uint64_t) noexcept {}
inline std::uint64_t currentCddaControlGeneration() noexcept { return 0; }
inline void restoreCddaControlGeneration(std::uint64_t) noexcept {}
inline void resetCddaObservation(std::uint64_t) noexcept {}
#else
CddaObservationSubscription subscribeCddaObservations(
		CddaObservationCallback callback);
CddaObservationSubscription subscribeCddaEvidenceObservations(
		CddaObservationCallback callback);
bool unsubscribeCddaObservations(CddaObservationSubscription subscription) noexcept;
bool cddaObservationBusActive() noexcept;
std::uint64_t cddaObservationDroppedCount() noexcept;
void observeReiosCddaControlAccepted(std::uint32_t requestId,
		std::uint32_t command, const std::uint32_t parameters[4],
		std::uint64_t tick) noexcept;
void observeReiosCddaControlApplied(std::uint32_t requestId,
		std::uint32_t command, const CddaDriveState& before,
		const CddaDriveState& after, bool appliedSuccessfully,
		std::uint64_t tick) noexcept;
void observeGdromPacketCddaControlAccepted(const std::uint8_t packet[12],
		std::uint64_t tick) noexcept;
void observeGdromPacketCddaControlApplied(std::uint32_t command,
		const CddaDriveState& before, const CddaDriveState& after,
		bool appliedSuccessfully, std::uint64_t tick) noexcept;
void observeCddaSector(std::uint64_t aicaGeneration, std::uint32_t fad,
		const CddaDriveState& before, const CddaDriveState& after,
		bool readSuccessful, const std::uint8_t* bytes,
		std::size_t byteCount, std::uint64_t tick) noexcept;
std::uint64_t currentCddaControlGeneration() noexcept;
void restoreCddaControlGeneration(std::uint64_t generation) noexcept;
void resetCddaObservation(std::uint64_t tick) noexcept;
#endif

} // namespace research

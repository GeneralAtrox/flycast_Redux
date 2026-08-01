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

constexpr std::uint32_t PvrTaObservationSchemaVersion = 1;
constexpr std::size_t MaxPvrTaRenderSelectionReads = 64;

enum class PvrTaObservationType : std::uint8_t
{
	ListInit = 1,
	ListContinue = 2,
	AcceptedBlock = 3,
	StartRender = 4,
	RenderDone = 5,
	Reset = 6,
};

constexpr std::uint32_t pvrTaObservationTypeBit(PvrTaObservationType type)
{
	return std::uint32_t {1} << (static_cast<unsigned>(type) - 1u);
}

constexpr std::uint32_t AllPvrTaObservationTypes =
		pvrTaObservationTypeBit(PvrTaObservationType::ListInit)
		| pvrTaObservationTypeBit(PvrTaObservationType::ListContinue)
		| pvrTaObservationTypeBit(PvrTaObservationType::AcceptedBlock)
		| pvrTaObservationTypeBit(PvrTaObservationType::StartRender)
		| pvrTaObservationTypeBit(PvrTaObservationType::RenderDone)
		| pvrTaObservationTypeBit(PvrTaObservationType::Reset);

enum class PvrTaInputSource : std::uint8_t
{
	StoreQueue = 1,
	Channel2Dma = 2,
	SortDma = 3,
};

constexpr std::uint32_t pvrTaInputSourceBit(PvrTaInputSource source)
{
	return std::uint32_t {1} << (static_cast<unsigned>(source) - 1u);
}

constexpr std::uint32_t AllPvrTaInputSources =
		pvrTaInputSourceBit(PvrTaInputSource::StoreQueue)
		| pvrTaInputSourceBit(PvrTaInputSource::Channel2Dma)
		| pvrTaInputSourceBit(PvrTaInputSource::SortDma);

struct PvrTaContextRef
{
	std::uint32_t address = UINT32_MAX;
	std::uint64_t generation = 0;
	bool available = false;
};

struct PvrTaVramRead
{
	std::uint32_t address = UINT32_MAX;
	std::uint32_t value = 0;
};

struct PvrTaRenderSelectionTranscript
{
	std::uint32_t regionBase = UINT32_MAX;
	std::uint32_t fpuParamCfg = UINT32_MAX;
	std::array<PvrTaVramRead, MaxPvrTaRenderSelectionReads> reads {};
	std::size_t readCount = 0;
	bool initialized = false;
	bool overflow = false;

	void record(std::uint32_t address, std::uint32_t value) noexcept
	{
		if (readCount == reads.size())
		{
			overflow = true;
			return;
		}
		reads[readCount++] = {address, value};
	}
};

constexpr std::uint32_t canonicalPvrTaSystemRamAddress(
		std::uint32_t ramOffset) noexcept
{
	return 0x0c000000u | ramOffset;
}

struct PvrTaObservation
{
	std::uint32_t schemaVersion = PvrTaObservationSchemaVersion;
	std::uint64_t emissionOrdinal = 0;
	PvrTaObservationType type = PvrTaObservationType::ListInit;
	std::uint64_t tick = 0;
	Sh4InstructionOwnerToken initiator;

	std::uint32_t contextAddress = UINT32_MAX;
	std::uint64_t contextGeneration = 0;
	std::uint64_t contextBlockOrdinal = 0;
	std::uint32_t renderPass = 0;
	std::uint32_t listTypeBefore = UINT32_MAX;
	std::uint32_t listTypeAfter = UINT32_MAX;
	std::uint32_t parserStateBefore = UINT32_MAX;
	std::uint32_t parserStateAfter = UINT32_MAX;

	PvrTaInputSource source = PvrTaInputSource::StoreQueue;
	std::uint32_t sourceAddress = UINT32_MAX;
	std::uint32_t taAddress = UINT32_MAX;
	std::array<std::uint8_t, 32> block {};

	std::uint64_t renderGeneration = 0;
	bool renderContextAvailable = false;
	std::vector<PvrTaContextRef> selectedContexts;
	std::uint32_t regionBase = UINT32_MAX;
	std::uint32_t fpuParamCfg = UINT32_MAX;
	std::array<PvrTaVramRead, MaxPvrTaRenderSelectionReads> renderSelectionReads {};
	std::size_t renderSelectionReadCount = 0;
};

struct PvrTaObservationFilter
{
	std::uint32_t typeMask = AllPvrTaObservationTypes;
	std::uint32_t sourceMask = AllPvrTaInputSources;
};

using PvrTaObservationSubscription = std::uint64_t;
using PvrTaObservationCallback = std::function<void(const PvrTaObservation&)>;

#ifdef LIBRETRO

inline PvrTaObservationSubscription subscribePvrTaObservations(
		const PvrTaObservationFilter&, PvrTaObservationCallback) { return 0; }
inline PvrTaObservationSubscription subscribePvrTaEvidenceObservations(
		PvrTaObservationCallback) { return 0; }
inline bool unsubscribePvrTaObservations(PvrTaObservationSubscription) noexcept
{
	return false;
}
inline bool pvrTaObservationBusActive() noexcept { return false; }
inline bool pvrTaEvidenceSubscriptionActive() noexcept { return false; }
inline std::size_t pvrTaObservationSubscriberCount() noexcept { return 0; }
inline std::uint64_t pvrTaObservationDroppedCount() noexcept { return 0; }
inline void observePvrTaListBoundary(bool, std::uint32_t, std::uint32_t,
		std::uint64_t) noexcept {}
inline void observePvrTaAcceptedBlock(PvrTaInputSource, std::uint32_t,
		std::uint32_t, const std::uint8_t*, std::uint32_t, std::uint32_t,
		std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t,
		std::uint64_t) noexcept {}
inline std::uint64_t observePvrTaStartRender(const std::uint32_t*, const bool*,
		std::size_t,
		const PvrTaRenderSelectionTranscript*, std::uint64_t) noexcept { return 0; }
inline void observePvrTaRenderDone(std::uint64_t) noexcept {}
inline void resetPvrTaObservation(std::uint64_t) noexcept {}

#else

PvrTaObservationSubscription subscribePvrTaObservations(
		const PvrTaObservationFilter& filter, PvrTaObservationCallback callback);
PvrTaObservationSubscription subscribePvrTaEvidenceObservations(
		PvrTaObservationCallback callback);
bool unsubscribePvrTaObservations(PvrTaObservationSubscription subscription) noexcept;
bool pvrTaObservationBusActive() noexcept;
bool pvrTaEvidenceSubscriptionActive() noexcept;
std::size_t pvrTaObservationSubscriberCount() noexcept;
std::uint64_t pvrTaObservationDroppedCount() noexcept;

void observePvrTaListBoundary(bool continuation, std::uint32_t contextAddress,
		std::uint32_t renderPass, std::uint64_t tick) noexcept;
void observePvrTaAcceptedBlock(PvrTaInputSource source,
		std::uint32_t sourceAddress, std::uint32_t taAddress,
		const std::uint8_t* block, std::uint32_t contextAddress,
		std::uint32_t renderPass, std::uint32_t listTypeBefore,
		std::uint32_t listTypeAfter, std::uint32_t parserStateBefore,
		std::uint32_t parserStateAfter, std::uint64_t tick) noexcept;
std::uint64_t observePvrTaStartRender(const std::uint32_t* contextAddresses,
		const bool* contextAvailability, std::size_t contextCount,
		const PvrTaRenderSelectionTranscript* transcript,
		std::uint64_t tick) noexcept;
void observePvrTaRenderDone(std::uint64_t tick) noexcept;
void resetPvrTaObservation(std::uint64_t tick) noexcept;

#endif

} // namespace research

#pragma once

#include "research/pvr_ta_observation.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace research
{

constexpr std::uint32_t PvrDrawObservationSchemaVersion = 1;

enum class PvrDrawObservationType : std::uint8_t
{
	PrimitiveDecoded = 1,
	DrawConsumed = 2,
	RenderCompleted = 3,
	Reset = 4,
};

enum class PvrPrimitiveKind : std::uint8_t
{
	Background = 1,
	PolygonStrip = 2,
	Sprite = 3,
	ModifierVolume = 4,
};

enum class PvrPrimitiveOwnerClass : std::uint8_t
{
	Unowned = 1,
	Exact = 2,
	Mixed = 3,
};

enum class PvrDrawBackend : std::uint8_t
{
	DirectX9 = 1,
	DirectX11 = 2,
	OpenGL = 3,
	OpenGL4 = 4,
	Vulkan = 5,
};

enum class PvrDrawPass : std::uint8_t
{
	Background = 1,
	Depth = 2,
	Color = 3,
	Translucent = 4,
	OrderIndependentTransparency = 5,
	ModifierVolume = 6,
	ModifierResolve = 7,
};

struct PvrPrimitiveBounds
{
	float minimumX = 0;
	float minimumY = 0;
	float minimumZ = 0;
	float maximumX = 0;
	float maximumY = 0;
	float maximumZ = 0;
	bool available = false;
};

struct PvrDrawObservation
{
	std::uint32_t schemaVersion = PvrDrawObservationSchemaVersion;
	std::uint64_t emissionOrdinal = 0;
	PvrDrawObservationType type = PvrDrawObservationType::PrimitiveDecoded;
	std::uint64_t tick = 0;

	std::uint64_t renderGeneration = 0;
	std::uint64_t primitiveGeneration = 0;
	std::vector<std::uint64_t> primitiveGenerations;
	std::uint64_t rasterGeneration = 0;
	std::uint32_t contextAddress = UINT32_MAX;
	std::uint64_t contextGeneration = 0;
	std::uint32_t renderPass = 0;
	std::uint32_t listType = UINT32_MAX;
	PvrPrimitiveKind primitiveKind = PvrPrimitiveKind::PolygonStrip;
	PvrPrimitiveOwnerClass ownerClass = PvrPrimitiveOwnerClass::Unowned;

	std::uint32_t pcw = 0;
	std::uint32_t isp = 0;
	std::uint32_t tsp = 0;
	std::uint32_t tcw = 0;
	std::uint32_t tsp1 = UINT32_MAX;
	std::uint32_t tcw1 = UINT32_MAX;
	std::uint32_t tileClip = 0;
	std::uint32_t first = 0;
	std::uint32_t count = 0;
	PvrPrimitiveBounds bounds;
	std::vector<PvrTaBlockProvenance> parameterBlocks;
	std::vector<PvrTaBlockProvenance> vertexBlocks;

	PvrDrawBackend backend = PvrDrawBackend::DirectX11;
	PvrDrawPass drawPass = PvrDrawPass::Color;
	bool indexed = false;
	bool successful = false;
};

using PvrDrawObservationSubscription = std::uint64_t;
using PvrDrawObservationCallback = std::function<void(const PvrDrawObservation&)>;

#ifdef LIBRETRO

inline PvrDrawObservationSubscription subscribePvrDrawObservations(
		PvrDrawObservationCallback) { return 0; }
inline PvrDrawObservationSubscription subscribePvrDrawEvidenceObservations(
		PvrDrawObservationCallback) { return 0; }
inline bool unsubscribePvrDrawObservations(PvrDrawObservationSubscription) noexcept
{
	return false;
}
inline bool pvrDrawObservationBusActive() noexcept { return false; }
inline bool pvrDrawEvidenceSubscriptionActive() noexcept { return false; }
inline std::uint64_t pvrDrawObservationDroppedCount() noexcept { return 0; }
inline std::uint64_t allocatePvrPrimitiveGeneration() noexcept { return 0; }
inline void observePvrPrimitiveDecoded(PvrDrawObservation) noexcept {}
inline std::uint64_t observePvrDrawConsumed(std::uint64_t,
		const std::vector<std::uint64_t>&,
		PvrDrawBackend, PvrDrawPass, std::uint32_t, std::uint32_t, bool,
		std::uint64_t) noexcept { return 0; }
inline void observePvrDrawRenderCompleted(std::uint64_t, bool,
		std::uint64_t) noexcept {}
inline void resetPvrDrawObservation(std::uint64_t) noexcept {}

#else

PvrDrawObservationSubscription subscribePvrDrawObservations(
		PvrDrawObservationCallback callback);
PvrDrawObservationSubscription subscribePvrDrawEvidenceObservations(
		PvrDrawObservationCallback callback);
bool unsubscribePvrDrawObservations(
		PvrDrawObservationSubscription subscription) noexcept;
bool pvrDrawObservationBusActive() noexcept;
bool pvrDrawEvidenceSubscriptionActive() noexcept;
std::uint64_t pvrDrawObservationDroppedCount() noexcept;

std::uint64_t allocatePvrPrimitiveGeneration() noexcept;
void observePvrPrimitiveDecoded(PvrDrawObservation observation) noexcept;
std::uint64_t observePvrDrawConsumed(std::uint64_t renderGeneration,
		const std::vector<std::uint64_t>& primitiveGenerations,
		PvrDrawBackend backend,
		PvrDrawPass drawPass, std::uint32_t first, std::uint32_t count,
		bool indexed, std::uint64_t tick) noexcept;
void observePvrDrawRenderCompleted(std::uint64_t renderGeneration,
		bool successful, std::uint64_t tick) noexcept;
void resetPvrDrawObservation(std::uint64_t tick) noexcept;

#endif

PvrPrimitiveOwnerClass classifyPvrPrimitiveOwnership(
		const std::vector<PvrTaBlockProvenance>& parameterBlocks,
		const std::vector<PvrTaBlockProvenance>& vertexBlocks) noexcept;

} // namespace research

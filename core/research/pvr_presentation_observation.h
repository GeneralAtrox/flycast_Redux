#pragma once

#include "research/sha256.h"

#include "research/sh4_observation_runtime.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace research
{

constexpr std::uint32_t PvrPresentationObservationSchemaVersion = 2;

enum class PvrPresentationObservationType : std::uint8_t
{
	RegisterWrite = 1,
	VramWrite = 2,
	RenderQueued = 3,
	RenderCompleted = 4,
	FramebufferCaptured = 5,
	Presentation = 6,
	Reset = 7,
	InitialRegisterState = 8,
};

enum class PvrRegisterWriteDisposition : std::uint8_t
{
	Stored = 1,
	MaskedAndStored = 2,
	SideEffectOnly = 3,
	IgnoredReadOnly = 4,
	IgnoredCondition = 5,
};

enum class PvrVramWriteSource : std::uint8_t
{
	Sh4Area1Direct = 1,
	Sh4Area1Mapped = 2,
	Sh4Area4 = 3,
	StoreQueue = 4,
	Channel2Dma = 5,
	TaInternal = 6,
	YuvConverter = 7,
	RendererRtt = 8,
	RendererFramebuffer = 9,
	Naomi2Elan = 10,
};

enum class PvrRenderKind : std::uint8_t
{
	Screen = 1,
	RenderToTexture = 2,
	FramebufferEmulation = 3,
	DirectFramebuffer = 4,
};

enum class PvrPresentationSource : std::uint8_t
{
	Render = 1,
	Framebuffer = 2,
};

enum class PvrFramebufferKind : std::uint8_t
{
	DreamcastVram = 1,
	PresentedRgb24 = 2,
};

struct PvrFramebufferConfig
{
	std::uint32_t fbReadSize = 0;
	std::uint32_t fbReadControl = 0;
	std::uint32_t spgControl = 0;
	std::uint32_t spgStatus = 0;
	std::uint32_t fbReadSof1 = 0;
	std::uint32_t fbReadSof2 = 0;
	std::uint32_t videoControl = 0;
	std::uint32_t borderColor = 0;
};

struct PvrPresentationObservation
{
	std::uint32_t schemaVersion = PvrPresentationObservationSchemaVersion;
	std::uint64_t emissionOrdinal = 0;
	PvrPresentationObservationType type =
			PvrPresentationObservationType::RegisterWrite;
	std::uint64_t tick = 0;
	Sh4InstructionOwnerToken initiator;

	std::uint32_t registerPhysicalAddress = UINT32_MAX;
	std::uint32_t registerAddress = UINT32_MAX;
	std::uint32_t requestedValue = 0;
	std::uint32_t previousValue = 0;
	std::uint32_t effectiveValue = 0;
	PvrRegisterWriteDisposition registerDisposition =
			PvrRegisterWriteDisposition::Stored;

	PvrVramWriteSource vramSource = PvrVramWriteSource::Sh4Area1Direct;
	std::uint32_t logicalAddress = UINT32_MAX;
	std::uint32_t physicalAddress = UINT32_MAX;
	std::vector<std::uint8_t> bytes;

	std::uint64_t renderGeneration = 0;
	PvrRenderKind renderKind = PvrRenderKind::Screen;
	bool successful = false;
	std::uint32_t framebufferWriteAddress = UINT32_MAX;

	std::uint64_t framebufferGeneration = 0;
	std::uint64_t framebufferSourceRenderGeneration = 0;
	PvrFramebufferKind framebufferKind = PvrFramebufferKind::DreamcastVram;
	PvrFramebufferConfig framebufferConfig;
	std::uint32_t framebufferWidth = 0;
	std::uint32_t framebufferHeight = 0;
	std::uint32_t framebufferRowBytes = 0;
	bool framebufferDigestAvailable = false;
	Sha256Digest framebufferDigest {};

	std::uint64_t presentationGeneration = 0;
	PvrPresentationSource presentationSource = PvrPresentationSource::Render;
	std::uint64_t sourceGeneration = 0;
};

using PvrPresentationObservationSubscription = std::uint64_t;
using PvrPresentationObservationCallback =
		std::function<void(const PvrPresentationObservation&)>;

class ScopedPvrRenderObservation
{
public:
	ScopedPvrRenderObservation(std::uint64_t renderGeneration,
			PvrRenderKind renderKind) noexcept;
	~ScopedPvrRenderObservation();
	ScopedPvrRenderObservation(const ScopedPvrRenderObservation&) = delete;
	ScopedPvrRenderObservation& operator=(const ScopedPvrRenderObservation&) = delete;

private:
	std::uint64_t previousGeneration = 0;
	PvrRenderKind previousKind = PvrRenderKind::Screen;
};

class ScopedPvrVramWriteSource
{
public:
	explicit ScopedPvrVramWriteSource(PvrVramWriteSource source) noexcept;
	~ScopedPvrVramWriteSource();
	ScopedPvrVramWriteSource(const ScopedPvrVramWriteSource&) = delete;
	ScopedPvrVramWriteSource& operator=(const ScopedPvrVramWriteSource&) = delete;

private:
	PvrVramWriteSource previousSource = PvrVramWriteSource::Sh4Area1Mapped;
};

std::uint64_t pvrCurrentRenderGeneration() noexcept;
PvrRenderKind pvrCurrentRenderKind() noexcept;
PvrVramWriteSource pvrCurrentVramWriteSource() noexcept;

#ifdef LIBRETRO

inline ScopedPvrRenderObservation::ScopedPvrRenderObservation(
		std::uint64_t, PvrRenderKind) noexcept {}
inline ScopedPvrRenderObservation::~ScopedPvrRenderObservation() = default;
inline ScopedPvrVramWriteSource::ScopedPvrVramWriteSource(
		PvrVramWriteSource) noexcept {}
inline ScopedPvrVramWriteSource::~ScopedPvrVramWriteSource() = default;
inline std::uint64_t pvrCurrentRenderGeneration() noexcept { return 0; }
inline PvrRenderKind pvrCurrentRenderKind() noexcept { return PvrRenderKind::Screen; }
inline PvrVramWriteSource pvrCurrentVramWriteSource() noexcept
{
	return PvrVramWriteSource::Sh4Area1Mapped;
}
inline PvrPresentationObservationSubscription subscribePvrPresentationObservations(
		PvrPresentationObservationCallback) { return 0; }
inline PvrPresentationObservationSubscription
subscribePvrPresentationEvidenceObservations(
		PvrPresentationObservationCallback) { return 0; }
inline bool unsubscribePvrPresentationObservations(
		PvrPresentationObservationSubscription) noexcept { return false; }
inline bool pvrPresentationObservationBusActive() noexcept { return false; }
inline bool pvrPresentationEvidenceSubscriptionActive() noexcept { return false; }
inline std::uint64_t pvrPresentationObservationDroppedCount() noexcept { return 0; }
inline void observePvrRegisterWrite(std::uint32_t, std::uint32_t,
		std::uint32_t, std::uint32_t, std::uint32_t,
		PvrRegisterWriteDisposition, std::uint64_t, std::uint64_t) noexcept {}
inline void observePvrVramWrite(PvrVramWriteSource, std::uint32_t,
		std::uint32_t, const void*, std::size_t, std::uint64_t,
		std::uint64_t) noexcept {}
inline void observePvrRenderQueued(std::uint64_t, PvrRenderKind,
		std::uint32_t, std::uint64_t) noexcept {}
inline void observePvrRenderCompleted(std::uint64_t, PvrRenderKind, bool,
		std::uint64_t) noexcept {}
inline std::uint64_t observePvrFramebufferCaptured(PvrFramebufferKind,
		std::uint64_t, const PvrFramebufferConfig&,
		std::uint32_t, std::uint32_t, std::uint32_t, const void*,
		std::size_t, std::uint64_t) noexcept { return 0; }
inline std::uint64_t observePvrPresentation(PvrPresentationSource,
		std::uint64_t, bool, std::uint64_t) noexcept { return 0; }
inline void resetPvrPresentationObservation(std::uint64_t) noexcept {}
inline void observePvrInitialRegisterState(const void*, std::size_t,
		std::uint64_t, std::uint64_t) noexcept {}

#else

PvrPresentationObservationSubscription subscribePvrPresentationObservations(
		PvrPresentationObservationCallback callback);
PvrPresentationObservationSubscription subscribePvrPresentationEvidenceObservations(
		PvrPresentationObservationCallback callback);
bool unsubscribePvrPresentationObservations(
		PvrPresentationObservationSubscription subscription) noexcept;
bool pvrPresentationObservationBusActive() noexcept;
bool pvrPresentationEvidenceSubscriptionActive() noexcept;
std::uint64_t pvrPresentationObservationDroppedCount() noexcept;

void observePvrRegisterWrite(std::uint32_t physicalAddress,
		std::uint32_t registerAddress, std::uint32_t requestedValue,
		std::uint32_t previousValue, std::uint32_t effectiveValue,
		PvrRegisterWriteDisposition disposition, std::uint64_t renderGeneration,
		std::uint64_t tick) noexcept;
void observePvrVramWrite(PvrVramWriteSource source,
		std::uint32_t logicalAddress, std::uint32_t physicalAddress,
		const void* bytes, std::size_t size, std::uint64_t renderGeneration,
		std::uint64_t tick) noexcept;
void observePvrRenderQueued(std::uint64_t renderGeneration, PvrRenderKind kind,
		std::uint32_t framebufferWriteAddress, std::uint64_t tick) noexcept;
void observePvrRenderCompleted(std::uint64_t renderGeneration, PvrRenderKind kind,
		bool successful, std::uint64_t tick) noexcept;
std::uint64_t observePvrFramebufferCaptured(PvrFramebufferKind kind,
		std::uint64_t sourceRenderGeneration,
		const PvrFramebufferConfig& config,
		std::uint32_t width, std::uint32_t height, std::uint32_t rowBytes,
		const void* bytes, std::size_t size, std::uint64_t tick) noexcept;
std::uint64_t observePvrPresentation(PvrPresentationSource source,
		std::uint64_t sourceGeneration, bool successful,
		std::uint64_t tick) noexcept;
void resetPvrPresentationObservation(std::uint64_t tick) noexcept;
void observePvrInitialRegisterState(const void* bytes, std::size_t size,
		std::uint64_t renderGeneration, std::uint64_t tick) noexcept;

#endif

} // namespace research

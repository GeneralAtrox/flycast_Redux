#include "research/pvr_presentation_observation.h"
#include "research/pvr_presentation_observation_internal.h"
#include "research/pvr_ta_observation.h"

#include <algorithm>
#include <utility>

namespace research
{

using namespace detail_pvr_presentation;

namespace
{

bool synchronousSource(PvrVramWriteSource source)
{
	return source != PvrVramWriteSource::RendererRtt
			&& source != PvrVramWriteSource::RendererFramebuffer
			&& source != PvrVramWriteSource::Naomi2Elan;
}

PvrPresentationObservation baseObservation(PvrPresentationObservationType type,
		std::uint64_t tick, bool retainOwner)
{
	PvrPresentationObservation observation;
	observation.type = type;
	if (retainOwner)
		observation.initiator = sh4ObservationCurrentInstructionOwner();
	observation.tick = observation.initiator.valid
			? std::max(tick, observation.initiator.tick) : tick;
	return observation;
}

} // namespace

ScopedPvrRenderObservation::ScopedPvrRenderObservation(
		std::uint64_t renderGeneration, PvrRenderKind renderKind) noexcept
	: previousGeneration(activeRenderGeneration), previousKind(activeRenderKind)
{
	activeRenderGeneration = renderGeneration;
	activeRenderKind = renderKind;
}

ScopedPvrRenderObservation::~ScopedPvrRenderObservation()
{
	activeRenderGeneration = previousGeneration;
	activeRenderKind = previousKind;
}

ScopedPvrVramWriteSource::ScopedPvrVramWriteSource(
		PvrVramWriteSource source) noexcept
	: previousSource(activeVramWriteSource)
{
	activeVramWriteSource = source;
}

ScopedPvrVramWriteSource::~ScopedPvrVramWriteSource()
{
	activeVramWriteSource = previousSource;
}

std::uint64_t pvrCurrentRenderGeneration() noexcept
{
	return activeRenderGeneration;
}

PvrRenderKind pvrCurrentRenderKind() noexcept
{
	return activeRenderKind;
}

PvrVramWriteSource pvrCurrentVramWriteSource() noexcept
{
	return activeVramWriteSource;
}

void observePvrRegisterWrite(std::uint32_t physicalAddress,
		std::uint32_t registerAddress, std::uint32_t requestedValue,
		std::uint32_t previousValue, std::uint32_t effectiveValue,
		PvrRegisterWriteDisposition disposition, std::uint64_t renderGeneration,
		std::uint64_t tick) noexcept
{
	if (!pvrPresentationObservationBusActive())
		return;
	if (renderGeneration != 0 && !pvrTaRenderGenerationObserved(renderGeneration))
		return;
	auto observation = baseObservation(
			PvrPresentationObservationType::RegisterWrite, tick, true);
	observation.registerPhysicalAddress = physicalAddress;
	observation.registerAddress = registerAddress;
	observation.requestedValue = requestedValue;
	observation.previousValue = previousValue;
	observation.effectiveValue = effectiveValue;
	observation.registerDisposition = disposition;
	observation.renderGeneration = renderGeneration;
	publish(std::move(observation));
}

void observePvrVramWrite(PvrVramWriteSource source,
		std::uint32_t logicalAddress, std::uint32_t physicalAddress,
		const void* bytes, std::size_t size, std::uint64_t renderGeneration,
		std::uint64_t tick) noexcept
{
	if (!pvrPresentationObservationBusActive())
		return;
	if (renderGeneration != 0 && !pvrTaRenderGenerationObserved(renderGeneration))
		return;
	if (bytes == nullptr || size == 0)
	{
		noteDroppedObservation();
		return;
	}
	try
	{
		auto observation = baseObservation(
				PvrPresentationObservationType::VramWrite, tick,
				synchronousSource(source));
		observation.vramSource = source;
		observation.logicalAddress = logicalAddress;
		observation.physicalAddress = physicalAddress;
		observation.renderGeneration = renderGeneration;
		const auto* first = static_cast<const std::uint8_t*>(bytes);
		observation.bytes.assign(first, first + size);
		publish(std::move(observation));
	}
	catch (...)
	{
		noteDroppedObservation();
	}
}

void observePvrRenderQueued(std::uint64_t renderGeneration, PvrRenderKind kind,
		std::uint32_t framebufferWriteAddress, std::uint64_t tick) noexcept
{
	if (!pvrPresentationObservationBusActive())
		return;
	if (kind != PvrRenderKind::DirectFramebuffer
			&& !pvrTaRenderGenerationObserved(renderGeneration))
		return;
	auto observation = baseObservation(
			PvrPresentationObservationType::RenderQueued, tick, false);
	observation.renderGeneration = renderGeneration;
	observation.renderKind = kind;
	observation.framebufferWriteAddress = framebufferWriteAddress;
	observation.successful = true;
	publish(std::move(observation));
}

void observePvrRenderCompleted(std::uint64_t renderGeneration, PvrRenderKind kind,
		bool successful, std::uint64_t tick) noexcept
{
	if (!pvrPresentationObservationBusActive())
		return;
	if (kind != PvrRenderKind::DirectFramebuffer
			&& !pvrTaRenderGenerationObserved(renderGeneration))
		return;
	auto observation = baseObservation(
			PvrPresentationObservationType::RenderCompleted, tick, false);
	observation.renderGeneration = renderGeneration;
	observation.renderKind = kind;
	observation.successful = successful;
	publish(std::move(observation));
}

std::uint64_t observePvrFramebufferCaptured(PvrFramebufferKind kind,
		std::uint64_t sourceRenderGeneration,
		const PvrFramebufferConfig& config,
		std::uint32_t width, std::uint32_t height, std::uint32_t rowBytes,
		const void* bytes, std::size_t size, std::uint64_t tick) noexcept
{
	if (!pvrPresentationObservationBusActive())
		return 0;
	if (sourceRenderGeneration != 0
			&& !pvrTaRenderGenerationObserved(sourceRenderGeneration))
		return 0;
	if (bytes == nullptr || size == 0 || width == 0 || height == 0
			|| rowBytes == 0 || size != static_cast<std::size_t>(rowBytes) * height)
	{
		noteDroppedObservation();
		return 0;
	}
	try
	{
		auto observation = baseObservation(
				PvrPresentationObservationType::FramebufferCaptured, tick, false);
		observation.framebufferGeneration = nextFramebufferGeneration.fetch_add(1,
				std::memory_order_relaxed);
		observation.framebufferKind = kind;
		observation.framebufferSourceRenderGeneration = sourceRenderGeneration;
		observation.framebufferConfig = config;
		observation.framebufferWidth = width;
		observation.framebufferHeight = height;
		observation.framebufferRowBytes = rowBytes;
		const auto* first = static_cast<const std::uint8_t*>(bytes);
		observation.bytes.assign(first, first + size);
		const auto generation = observation.framebufferGeneration;
		publish(std::move(observation));
		return generation;
	}
	catch (...)
	{
		noteDroppedObservation();
		return 0;
	}
}

std::uint64_t observePvrPresentation(PvrPresentationSource source,
		std::uint64_t sourceGeneration, bool successful,
		std::uint64_t tick) noexcept
{
	if (!pvrPresentationObservationBusActive())
		return 0;
	if (source == PvrPresentationSource::Render
			&& !pvrTaRenderGenerationObserved(sourceGeneration))
		return 0;
	if (sourceGeneration == 0)
	{
		noteDroppedObservation();
		return 0;
	}
	auto observation = baseObservation(
			PvrPresentationObservationType::Presentation, tick, false);
	observation.presentationGeneration = nextPresentationGeneration.fetch_add(1,
			std::memory_order_relaxed);
	observation.presentationSource = source;
	observation.sourceGeneration = sourceGeneration;
	observation.successful = successful;
	const auto generation = observation.presentationGeneration;
	publish(std::move(observation));
	return generation;
}

void resetPvrPresentationObservation(std::uint64_t tick) noexcept
{
	if (!pvrPresentationObservationBusActive())
		return;
	publish(baseObservation(PvrPresentationObservationType::Reset, tick, false));
}

void observePvrInitialRegisterState(const void* bytes, std::size_t size,
		std::uint64_t renderGeneration, std::uint64_t tick) noexcept
{
	if (!pvrPresentationObservationBusActive())
		return;
	if (bytes == nullptr || size == 0)
	{
		noteDroppedObservation();
		return;
	}
	try
	{
		auto observation = baseObservation(
				PvrPresentationObservationType::InitialRegisterState, tick, false);
		observation.renderGeneration = renderGeneration;
		const auto* first = static_cast<const std::uint8_t*>(bytes);
		observation.bytes.assign(first, first + size);
		publish(std::move(observation));
	}
	catch (...)
	{
		noteDroppedObservation();
	}
}

} // namespace research

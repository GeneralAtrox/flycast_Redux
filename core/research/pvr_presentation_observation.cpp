#include "research/pvr_presentation_observation.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace research
{
namespace
{

struct SubscriptionEntry
{
	PvrPresentationObservationSubscription id = 0;
	PvrPresentationObservationCallback callback;
	std::atomic<bool> active {true};
	bool evidence = false;
};

std::mutex subscriptionsMutex;
std::recursive_mutex dispatchMutex;
std::vector<std::shared_ptr<SubscriptionEntry>> subscriptions;
std::atomic<std::size_t> activeSubscriptionCount {0};
std::atomic<bool> activeEvidenceSubscription {false};
std::atomic<std::uint64_t> nextSubscription {1};
std::atomic<std::uint64_t> nextEmissionOrdinal {0};
std::atomic<std::uint64_t> nextFramebufferGeneration {1};
std::atomic<std::uint64_t> nextPresentationGeneration {1};
std::atomic<std::uint64_t> droppedObservationCount {0};
thread_local bool publishingObservation = false;
thread_local std::uint64_t activeRenderGeneration = 0;
thread_local PvrRenderKind activeRenderKind = PvrRenderKind::Screen;
thread_local PvrVramWriteSource activeVramWriteSource =
		PvrVramWriteSource::Sh4Area1Mapped;

void noteDroppedObservation() noexcept
{
	droppedObservationCount.fetch_add(1, std::memory_order_relaxed);
}

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

void publish(PvrPresentationObservation observation) noexcept
{
	try
	{
		if (!pvrPresentationObservationBusActive())
			return;
		if (publishingObservation)
		{
			noteDroppedObservation();
			return;
		}
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		if (!pvrPresentationObservationBusActive())
		{
			noteDroppedObservation();
			return;
		}
		publishingObservation = true;
		struct PublishingReset
		{
			~PublishingReset() { publishingObservation = false; }
		} publishingReset;
		observation.emissionOrdinal = nextEmissionOrdinal.fetch_add(1,
				std::memory_order_relaxed);
		std::vector<std::shared_ptr<SubscriptionEntry>> snapshot;
		{
			const std::lock_guard<std::mutex> lock(subscriptionsMutex);
			snapshot = subscriptions;
		}
		for (const auto& entry : snapshot)
		{
			if (!entry->active.load(std::memory_order_acquire))
				continue;
			try
			{
				entry->callback(observation);
			}
			catch (...)
			{
				noteDroppedObservation();
			}
		}
	}
	catch (...)
	{
		noteDroppedObservation();
	}
}

PvrPresentationObservationSubscription subscribe(
		PvrPresentationObservationCallback callback, bool evidence)
{
	if (!callback)
		throw std::invalid_argument("PowerVR presentation observation callback is empty");
	const auto id = nextSubscription.fetch_add(1, std::memory_order_relaxed);
	if (id == 0)
		throw std::overflow_error("PowerVR presentation subscription id overflow");
	auto entry = std::make_shared<SubscriptionEntry>();
	entry->id = id;
	entry->callback = std::move(callback);
	entry->evidence = evidence;
	{
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		const std::lock_guard<std::mutex> lock(subscriptionsMutex);
		if (evidence && !subscriptions.empty())
			throw std::logic_error(
					"PowerVR presentation evidence requires exclusive ownership");
		if (!evidence && activeEvidenceSubscription.load(std::memory_order_acquire))
			throw std::logic_error(
					"PowerVR presentation evidence owns the bus exclusively");
		subscriptions.push_back(entry);
		retainSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
		retainSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
		if (evidence)
			activeEvidenceSubscription.store(true, std::memory_order_release);
		activeSubscriptionCount.fetch_add(1, std::memory_order_release);
	}
	return id;
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

PvrPresentationObservationSubscription subscribePvrPresentationObservations(
		PvrPresentationObservationCallback callback)
{
	return subscribe(std::move(callback), false);
}

PvrPresentationObservationSubscription subscribePvrPresentationEvidenceObservations(
		PvrPresentationObservationCallback callback)
{
	return subscribe(std::move(callback), true);
}

bool unsubscribePvrPresentationObservations(
		PvrPresentationObservationSubscription subscription) noexcept
{
	if (subscription == 0)
		return false;
	const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
	const std::lock_guard<std::mutex> lock(subscriptionsMutex);
	const auto found = std::find_if(subscriptions.begin(), subscriptions.end(),
			[subscription](const auto& entry) { return entry->id == subscription; });
	if (found == subscriptions.end())
		return false;
	(*found)->active.store(false, std::memory_order_release);
	if ((*found)->evidence)
		activeEvidenceSubscription.store(false, std::memory_order_release);
	subscriptions.erase(found);
	activeSubscriptionCount.fetch_sub(1, std::memory_order_release);
	releaseSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
	releaseSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
	return true;
}

bool pvrPresentationObservationBusActive() noexcept
{
	return activeSubscriptionCount.load(std::memory_order_acquire) != 0;
}

bool pvrPresentationEvidenceSubscriptionActive() noexcept
{
	return activeEvidenceSubscription.load(std::memory_order_acquire);
}

std::uint64_t pvrPresentationObservationDroppedCount() noexcept
{
	return droppedObservationCount.load(std::memory_order_acquire);
}

void observePvrRegisterWrite(std::uint32_t physicalAddress,
		std::uint32_t registerAddress, std::uint32_t requestedValue,
		std::uint32_t previousValue, std::uint32_t effectiveValue,
		PvrRegisterWriteDisposition disposition, std::uint64_t renderGeneration,
		std::uint64_t tick) noexcept
{
	if (!pvrPresentationObservationBusActive())
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

} // namespace research

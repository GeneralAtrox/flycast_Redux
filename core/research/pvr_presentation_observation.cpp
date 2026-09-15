#include "research/pvr_presentation_observation.h"
#include "research/pvr_presentation_observation_internal.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace research
{
namespace detail_pvr_presentation
{

std::atomic<std::uint64_t> nextFramebufferGeneration {1};
std::atomic<std::uint64_t> nextPresentationGeneration {1};
thread_local std::uint64_t activeRenderGeneration = 0;
thread_local PvrRenderKind activeRenderKind = PvrRenderKind::Screen;
thread_local PvrVramWriteSource activeVramWriteSource =
		PvrVramWriteSource::Sh4Area1Mapped;

} // namespace detail_pvr_presentation

using namespace detail_pvr_presentation;

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
std::atomic<std::uint64_t> droppedObservationCount {0};
thread_local bool publishingObservation = false;

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

namespace detail_pvr_presentation
{

void noteDroppedObservation() noexcept
{
	droppedObservationCount.fetch_add(1, std::memory_order_relaxed);
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

} // namespace detail_pvr_presentation

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

} // namespace research

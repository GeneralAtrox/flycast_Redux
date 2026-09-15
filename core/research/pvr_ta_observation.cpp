#include "research/pvr_ta_observation.h"
#include "research/pvr_ta_observation_internal.h"
#include "research/pvr_draw_observation.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace research
{
namespace detail_pvr_ta
{

std::mutex stateMutex;
std::unordered_map<std::uint32_t, ContextState> contexts;
std::unordered_set<std::uint64_t> observedRenderGenerations;
bool observedRenderGenerationWindowFrozen = false;
std::uint64_t pendingRenderGeneration = 0;
std::atomic<std::uint64_t> nextContextGeneration {1};
std::atomic<std::uint64_t> nextRenderGeneration {1};

} // namespace detail_pvr_ta

using namespace detail_pvr_ta;

namespace
{

struct SubscriptionEntry
{
	PvrTaObservationSubscription id = 0;
	PvrTaObservationFilter filter;
	PvrTaObservationCallback callback;
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

void validateFilter(const PvrTaObservationFilter& filter)
{
	if (filter.typeMask == 0
			|| (filter.typeMask & ~AllPvrTaObservationTypes) != 0)
		throw std::invalid_argument("PowerVR TA observation filter has an invalid type mask");
	if (filter.sourceMask == 0
			|| (filter.sourceMask & ~AllPvrTaInputSources) != 0)
		throw std::invalid_argument("PowerVR TA observation filter has an invalid source mask");
}

bool matches(const PvrTaObservationFilter& filter,
		const PvrTaObservation& observation)
{
	if ((filter.typeMask & pvrTaObservationTypeBit(observation.type)) == 0)
		return false;
	return observation.type != PvrTaObservationType::AcceptedBlock
			|| (filter.sourceMask & pvrTaInputSourceBit(observation.source)) != 0;
}

PvrTaObservationSubscription subscribe(
		const PvrTaObservationFilter& filter, PvrTaObservationCallback callback,
		bool evidence)
{
	validateFilter(filter);
	if (!callback)
		throw std::invalid_argument("PowerVR TA observation callback is empty");
	const PvrTaObservationSubscription id = nextSubscription.fetch_add(1,
			std::memory_order_relaxed);
	if (id == 0)
		throw std::overflow_error("PowerVR TA observation subscription id overflow");
	auto entry = std::make_shared<SubscriptionEntry>();
	entry->id = id;
	entry->filter = filter;
	entry->callback = std::move(callback);
	entry->evidence = evidence;
	{
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		const std::lock_guard<std::mutex> subscriptionLock(subscriptionsMutex);
		if (evidence && !subscriptions.empty())
			throw std::logic_error(
					"PowerVR TA evidence observation requires exclusive ownership");
		if (!evidence && activeEvidenceSubscription.load(std::memory_order_acquire))
			throw std::logic_error(
					"PowerVR TA evidence observation owns the bus exclusively");
		if (subscriptions.empty() && !pvrDrawObservationBusActive())
		{
			const std::lock_guard<std::mutex> stateLock(stateMutex);
			contexts.clear();
			observedRenderGenerations.clear();
			observedRenderGenerationWindowFrozen = false;
			pendingRenderGeneration = 0;
		}
		subscriptions.push_back(std::move(entry));
		retainSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
		retainSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
		if (evidence)
			activeEvidenceSubscription.store(true, std::memory_order_release);
		activeSubscriptionCount.fetch_add(1, std::memory_order_release);
	}
	return id;
}

} // namespace

namespace detail_pvr_ta
{

void noteDroppedObservation() noexcept
{
	droppedObservationCount.fetch_add(1, std::memory_order_relaxed);
}

bool publish(PvrTaObservation observation) noexcept
{
	try
	{
		if (!pvrTaObservationBusActive())
			return false;
		if (publishingObservation)
		{
			noteDroppedObservation();
			return false;
		}
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		if (!pvrTaObservationBusActive())
		{
			noteDroppedObservation();
			return false;
		}
		if (publishingObservation)
		{
			noteDroppedObservation();
			return false;
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
		bool delivered = false;
		for (const std::shared_ptr<SubscriptionEntry>& entry : snapshot)
		{
			if (!entry->active.load(std::memory_order_acquire)
					|| !matches(entry->filter, observation))
				continue;
			try
			{
				entry->callback(observation);
			}
			catch (...)
			{
				// Callbacks cannot change TA parsing or render timing, but evidence
				// recorders must be able to fail closed when delivery was lost.
				noteDroppedObservation();
			}
			delivered = true;
		}
		return delivered;
	}
	catch (...)
	{
		noteDroppedObservation();
		return false;
	}
}

} // namespace detail_pvr_ta

PvrTaObservationSubscription subscribePvrTaObservations(
		const PvrTaObservationFilter& filter, PvrTaObservationCallback callback)
{
	return subscribe(filter, std::move(callback), false);
}

PvrTaObservationSubscription subscribePvrTaEvidenceObservations(
		PvrTaObservationCallback callback)
{
	return subscribe(PvrTaObservationFilter {}, std::move(callback), true);
}

bool unsubscribePvrTaObservations(
		PvrTaObservationSubscription subscription) noexcept
{
	if (subscription == 0)
		return false;
	const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
	const std::lock_guard<std::mutex> lock(subscriptionsMutex);
	const auto found = std::find_if(subscriptions.begin(), subscriptions.end(),
			[subscription](const std::shared_ptr<SubscriptionEntry>& entry) {
				return entry->id == subscription;
			});
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

bool pvrTaObservationBusActive() noexcept
{
	return activeSubscriptionCount.load(std::memory_order_acquire) != 0;
}

bool pvrTaEvidenceSubscriptionActive() noexcept
{
	return activeEvidenceSubscription.load(std::memory_order_acquire);
}

std::size_t pvrTaObservationSubscriberCount() noexcept
{
	return activeSubscriptionCount.load(std::memory_order_acquire);
}

std::uint64_t pvrTaObservationDroppedCount() noexcept
{
	return droppedObservationCount.load(std::memory_order_acquire);
}

bool pvrTaRenderGenerationObserved(std::uint64_t renderGeneration) noexcept
{
	if (renderGeneration == 0)
		return false;
	// Presentation and draw observation buses can be used independently of TA
	// evidence capture. In that mode there is no delayed TA slice boundary to
	// enforce, so retain their established generation semantics.
	if (!pvrTaObservationBusActive())
		return true;
	try
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		return observedRenderGenerations.count(renderGeneration) != 0;
	}
	catch (...)
	{
		return false;
	}
}

void freezePvrTaObservedRenderGenerationWindow() noexcept
{
	try
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		observedRenderGenerationWindowFrozen = true;
	}
	catch (...)
	{
		noteDroppedObservation();
	}
}

void beginPvrTaProvenanceSession() noexcept
{
	try
	{
		if (pvrTaObservationBusActive())
			return;
		const std::lock_guard<std::mutex> lock(stateMutex);
		contexts.clear();
		observedRenderGenerations.clear();
		observedRenderGenerationWindowFrozen = false;
		pendingRenderGeneration = 0;
	}
	catch (...)
	{
		noteDroppedObservation();
	}
}

} // namespace research

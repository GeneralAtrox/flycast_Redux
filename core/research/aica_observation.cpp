#include "research/aica_observation.h"
#include "research/aica_observation_internal.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace research
{
namespace detail_aica
{

std::mutex stateMutex;
std::atomic<std::uint64_t> nextDmaGeneration {1};
std::atomic<std::uint64_t> nextCddaGeneration {1};
std::atomic<std::uint64_t> nextSampleOrdinal {0};
DmaState dmaState;
thread_local AicaWriter currentWriter = AicaWriter::Unknown;

} // namespace detail_aica

using namespace detail_aica;

namespace
{

struct Subscription
{
	AicaObservationSubscription id = 0;
	AicaObservationCallback callback;
	std::atomic<bool> active {true};
	bool evidence = false;
};

std::recursive_mutex dispatchMutex;
std::mutex subscriptionMutex;
std::vector<std::shared_ptr<Subscription>> subscriptions;
std::atomic<bool> activeSubscription {false};
std::atomic<bool> activeEvidenceSubscription {false};
std::atomic<std::uint64_t> nextSubscription {1};
std::atomic<std::uint64_t> nextEmissionOrdinal {0};
std::atomic<std::uint64_t> droppedCount {0};
thread_local bool publishing = false;

void reserveCddaGeneration(std::uint64_t generation) noexcept
{
	std::uint64_t expected = nextCddaGeneration.load(std::memory_order_relaxed);
	while (expected <= generation &&
			!nextCddaGeneration.compare_exchange_weak(expected, generation + 1,
					std::memory_order_relaxed)) {}
}

AicaObservationSubscription subscribe(
		AicaObservationCallback callback, bool evidence)
{
	if (!callback)
		throw std::invalid_argument("AICA observation callback is empty");
	const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
	const std::lock_guard<std::mutex> lock(subscriptionMutex);
	if (evidence && !subscriptions.empty())
		throw std::logic_error("AICA evidence observation requires exclusive ownership");
	if (!evidence && activeEvidenceSubscription.load(std::memory_order_acquire))
		throw std::logic_error("AICA evidence observation owns the bus exclusively");
	auto next = std::make_shared<Subscription>();
	next->id = nextSubscription.fetch_add(1, std::memory_order_relaxed);
	if (next->id == 0)
		throw std::overflow_error("AICA observation subscription id overflow");
	next->callback = std::move(callback);
	next->evidence = evidence;
	if (subscriptions.empty()) {
		const std::lock_guard<std::mutex> stateLock(stateMutex);
		dmaState = {};
	}
	subscriptions.push_back(next);
	retainSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
	retainSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
	activeSubscription.store(true, std::memory_order_release);
	if (evidence)
		activeEvidenceSubscription.store(true, std::memory_order_release);
	return next->id;
}

} // namespace

namespace detail_aica
{

void dropped() noexcept
{
	droppedCount.fetch_add(1, std::memory_order_relaxed);
}

bool publish(AicaObservation observation) noexcept
{
	try
	{
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		std::vector<std::shared_ptr<Subscription>> targets;
		{
			const std::lock_guard<std::mutex> lock(subscriptionMutex);
			targets = subscriptions;
		}
		if (targets.empty())
			return false;
		if (publishing)
		{
			dropped();
			return false;
		}
		publishing = true;
		struct Reset { ~Reset() { publishing = false; } } reset;
		observation.emissionOrdinal = nextEmissionOrdinal.fetch_add(1,
				std::memory_order_relaxed);
		bool delivered = false;
		for (const auto& target : targets)
		{
			if (!target->active.load(std::memory_order_acquire))
				continue;
			try { target->callback(observation); }
			catch (...) { dropped(); continue; }
			delivered = true;
		}
		return delivered;
	}
	catch (...) { dropped(); return false; }
}

} // namespace detail_aica

AicaObservationSubscription subscribeAicaObservations(
		AicaObservationCallback callback)
{
	return subscribe(std::move(callback), false);
}

AicaObservationSubscription subscribeAicaEvidenceObservations(
		AicaObservationCallback callback)
{
	return subscribe(std::move(callback), true);
}

bool unsubscribeAicaObservations(AicaObservationSubscription id) noexcept
{
	try
	{
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		const std::lock_guard<std::mutex> lock(subscriptionMutex);
		const auto found = std::find_if(subscriptions.begin(), subscriptions.end(),
				[id](const std::shared_ptr<Subscription>& entry) {
					return entry->id == id;
				});
		if (found == subscriptions.end())
			return false;
		(*found)->active.store(false, std::memory_order_release);
		if ((*found)->evidence)
			activeEvidenceSubscription.store(false, std::memory_order_release);
		subscriptions.erase(found);
		activeSubscription.store(!subscriptions.empty(), std::memory_order_release);
		if (subscriptions.empty()) {
			const std::lock_guard<std::mutex> stateLock(stateMutex);
			dmaState = {};
		}
		releaseSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
		releaseSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
		return true;
	}
	catch (...) { dropped(); return false; }
}

bool aicaObservationBusActive() noexcept
{
	return activeSubscription.load(std::memory_order_acquire);
}

std::uint64_t aicaObservationDroppedCount() noexcept
{
	return droppedCount.load(std::memory_order_acquire);
}

bool aicaObservationDmaActive() noexcept
{
	const std::lock_guard<std::mutex> lock(stateMutex);
	return dmaState.active;
}

void restoreAicaCddaGeneration(std::uint64_t generation) noexcept
{
	reserveCddaGeneration(generation);
}

std::uint64_t aicaObservationNextSampleOrdinal() noexcept
{
	return nextSampleOrdinal.load(std::memory_order_acquire);
}

} // namespace research

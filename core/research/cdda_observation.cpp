#include "research/cdda_observation.h"
#include "research/cdda_observation_internal.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace research
{
namespace detail_cdda
{

std::mutex stateMutex;
std::atomic<std::uint64_t> nextControlGeneration {1};
std::atomic<std::uint32_t> nextPacketRequest {1};
PendingControl pendingControl;
std::uint64_t activeControlGeneration = 0;

} // namespace detail_cdda

using namespace detail_cdda;

namespace
{

// REIOS GD-ROM control command values. They are kept local so the research
// format does not depend on the HLE implementation header.
constexpr std::uint32_t Play = 0x14;
constexpr std::uint32_t Play2 = 0x15;
constexpr std::uint32_t Pause = 0x16;
constexpr std::uint32_t Release = 0x17;
constexpr std::uint32_t Seek = 0x1b;
constexpr std::uint32_t Stop = 0x21;
constexpr std::uint32_t PacketPlay = 0x20;
constexpr std::uint32_t PacketSeek = 0x21;

struct Subscription
{
	CddaObservationSubscription id = 0;
	CddaObservationCallback callback;
	std::atomic<bool> active {true};
	bool evidence = false;
};

std::recursive_mutex dispatchMutex;
std::mutex subscriptionMutex;
std::vector<std::shared_ptr<Subscription>> subscriptions;
std::atomic<std::size_t> activeSubscriptionCount {0};
std::atomic<bool> activeEvidenceSubscription {false};
std::atomic<std::uint64_t> nextSubscription {1};
std::atomic<std::uint64_t> nextEmissionOrdinal {0};
std::atomic<std::uint64_t> droppedCount {0};
thread_local bool publishing = false;

void reserveGeneration(std::uint64_t generation) noexcept
{
	std::uint64_t expected = nextControlGeneration.load(std::memory_order_relaxed);
	while (expected <= generation &&
			!nextControlGeneration.compare_exchange_weak(expected, generation + 1,
					std::memory_order_relaxed)) {}
}

CddaObservationSubscription subscribe(CddaObservationCallback callback,
		bool evidence)
{
	if (!callback)
		throw std::invalid_argument("CD-DA observation callback is empty");
	const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
	const std::lock_guard<std::mutex> lock(subscriptionMutex);
	if (evidence && !subscriptions.empty())
		throw std::logic_error("CD-DA evidence observation requires exclusive ownership");
	if (!evidence && activeEvidenceSubscription.load(std::memory_order_acquire))
		throw std::logic_error("CD-DA evidence observation owns the bus exclusively");
	auto next = std::make_shared<Subscription>();
	next->id = nextSubscription.fetch_add(1, std::memory_order_relaxed);
	if (next->id == 0)
		throw std::overflow_error("CD-DA observation subscription id overflow");
	next->callback = std::move(callback);
	next->evidence = evidence;
	subscriptions.push_back(next);
	retainSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
	retainSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
	activeSubscriptionCount.fetch_add(1, std::memory_order_release);
	if (evidence)
		activeEvidenceSubscription.store(true, std::memory_order_release);
	return next->id;
}

} // namespace

namespace detail_cdda
{

void dropped() noexcept
{
	droppedCount.fetch_add(1, std::memory_order_relaxed);
}

bool publish(CddaObservation observation) noexcept
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

} // namespace detail_cdda

bool isReiosCddaControlCommand(std::uint32_t command) noexcept
{
	return command == Play || command == Play2 || command == Pause ||
			command == Release || command == Seek || command == Stop;
}

bool isGdromPacketCddaControlCommand(std::uint32_t command) noexcept
{
	return command == PacketPlay || command == PacketSeek;
}

bool isCddaControlCommand(std::uint32_t command) noexcept
{
	return isReiosCddaControlCommand(command)
			|| isGdromPacketCddaControlCommand(command);
}

CddaObservationSubscription subscribeCddaObservations(
		CddaObservationCallback callback)
{
	return subscribe(std::move(callback), false);
}

CddaObservationSubscription subscribeCddaEvidenceObservations(
		CddaObservationCallback callback)
{
	return subscribe(std::move(callback), true);
}

bool unsubscribeCddaObservations(CddaObservationSubscription id) noexcept
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
		activeSubscriptionCount.fetch_sub(1, std::memory_order_release);
		releaseSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
		releaseSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
		return true;
	}
	catch (...) { dropped(); return false; }
}

bool cddaObservationBusActive() noexcept
{
	return activeSubscriptionCount.load(std::memory_order_acquire) != 0;
}

std::uint64_t cddaObservationDroppedCount() noexcept
{
	return droppedCount.load(std::memory_order_acquire);
}

std::uint64_t currentCddaControlGeneration() noexcept
{
	const std::lock_guard<std::mutex> lock(stateMutex);
	return activeControlGeneration;
}

void restoreCddaControlGeneration(std::uint64_t generation) noexcept
{
	const std::lock_guard<std::mutex> lock(stateMutex);
	pendingControl = {};
	activeControlGeneration = generation;
	reserveGeneration(generation);
}

} // namespace research

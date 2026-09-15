#include "research/gdrom_hardware_observation.h"
#include "research/gdrom_hardware_observation_internal.h"

#include "log/Log.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace research
{
namespace detail_gdrom_hardware
{

std::mutex stateMutex;
std::atomic<std::uint64_t> nextCommand {1};
PendingAta pendingAta;
ActiveCommand command;
std::uint64_t completedAwaitingStatus = 0;
std::uint64_t completedTransferred = 0;

} // namespace detail_gdrom_hardware

using namespace detail_gdrom_hardware;

namespace
{
struct Subscription
{
	GdromHardwareObservationSubscription id = 0;
	GdromHardwareObservationCallback callback;
	std::atomic<bool> active {true};
	bool evidence = false;
};

std::recursive_mutex dispatchMutex;
std::mutex subscriptionMutex;
std::vector<std::shared_ptr<Subscription>> subscriptions;
std::atomic<std::size_t> activeSubscriptions {0};
std::atomic<bool> evidenceActive {false};
std::atomic<std::uint64_t> nextSubscription {1};
std::atomic<std::uint64_t> nextEmission {0};
std::atomic<std::uint64_t> droppedCount {0};
thread_local bool publishing = false;

GdromHardwareObservationSubscription subscribe(
		GdromHardwareObservationCallback callback, bool evidence)
{
	if (!callback) throw std::invalid_argument("GD-ROM hardware callback is empty");
	const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
	const std::lock_guard<std::mutex> lock(subscriptionMutex);
	if (evidence && !subscriptions.empty())
		throw std::logic_error("GD-ROM hardware evidence requires exclusive ownership");
	if (!evidence && evidenceActive.load(std::memory_order_acquire))
		throw std::logic_error("GD-ROM hardware evidence owns the bus exclusively");
	auto next = std::make_shared<Subscription>();
	next->id = nextSubscription.fetch_add(1, std::memory_order_relaxed);
	if (next->id == 0) throw std::overflow_error("GD-ROM hardware subscription overflow");
	next->callback = std::move(callback);
	next->evidence = evidence;
	if (subscriptions.empty())
	{
		const std::lock_guard<std::mutex> stateLock(stateMutex);
		pendingAta = {}; command = {};
		completedAwaitingStatus = 0; completedTransferred = 0;
	}
	subscriptions.push_back(next);
	retainSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
	retainSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
	activeSubscriptions.fetch_add(1, std::memory_order_release);
	if (evidence) evidenceActive.store(true, std::memory_order_release);
	return next->id;
}
} // namespace

namespace detail_gdrom_hardware
{

void dropped(const char* reason) noexcept
{
	droppedCount.fetch_add(1, std::memory_order_relaxed);
	WARN_LOG(GDROM, "Dropped typed GD-ROM hardware observation: %s", reason);
}

bool publish(GdromHardwareObservation observation) noexcept
{
	try
	{
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		std::vector<std::shared_ptr<Subscription>> targets;
		{
			const std::lock_guard<std::mutex> lock(subscriptionMutex);
			targets = subscriptions;
		}
		if (targets.empty()) return false;
		if (publishing) { dropped("reentrant publication"); return false; }
		publishing = true;
		struct Guard { ~Guard() { publishing = false; } } guard;
		observation.emissionOrdinal = nextEmission.fetch_add(1,
				std::memory_order_relaxed);
		bool delivered = false;
		for (const auto& target : targets)
		{
			if (!target->active.load(std::memory_order_acquire)) continue;
			try { target->callback(observation); }
			catch (...) { dropped("subscriber callback failed"); continue; }
			delivered = true;
		}
		return delivered;
	}
	catch (...) { dropped("publication failed"); return false; }
}

} // namespace detail_gdrom_hardware

GdromHardwareObservationSubscription subscribeGdromHardwareObservations(
		GdromHardwareObservationCallback callback)
{ return subscribe(std::move(callback), false); }
GdromHardwareObservationSubscription subscribeGdromHardwareEvidenceObservations(
		GdromHardwareObservationCallback callback)
{ return subscribe(std::move(callback), true); }

bool unsubscribeGdromHardwareObservations(
		GdromHardwareObservationSubscription id) noexcept
{
	try
	{
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		const std::lock_guard<std::mutex> lock(subscriptionMutex);
		const auto found = std::find_if(subscriptions.begin(), subscriptions.end(),
				[id](const auto& item) { return item->id == id; });
		if (found == subscriptions.end()) return false;
		(*found)->active.store(false, std::memory_order_release);
		if ((*found)->evidence) evidenceActive.store(false, std::memory_order_release);
		subscriptions.erase(found);
		activeSubscriptions.fetch_sub(1, std::memory_order_release);
		releaseSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
		releaseSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
		return true;
	}
	catch (...) { dropped("unsubscribe failed"); return false; }
}

bool gdromHardwareObservationBusActive() noexcept
{ return activeSubscriptions.load(std::memory_order_acquire) != 0; }
std::uint64_t gdromHardwareObservationDroppedCount() noexcept
{ return droppedCount.load(std::memory_order_acquire); }

} // namespace research

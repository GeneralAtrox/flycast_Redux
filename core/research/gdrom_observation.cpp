#include "research/gdrom_observation.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace research
{
namespace
{

struct Subscription
{
	GdromObservationSubscription id = 0;
	GdromObservationCallback callback;
	std::atomic<bool> active {true};
	bool evidence = false;
};

struct CommandState
{
	bool active = false;
	std::uint64_t generation = 0;
	std::uint64_t nextChunkOrdinal = 0;
	std::uint64_t transferredBytes = 0;
	std::uint32_t requestId = 0;
};

std::recursive_mutex dispatchMutex;
std::mutex subscriptionMutex;
std::mutex stateMutex;
std::vector<std::shared_ptr<Subscription>> subscriptions;
std::atomic<std::size_t> activeSubscriptionCount {0};
std::atomic<bool> activeEvidenceSubscription {false};
std::atomic<std::uint64_t> nextSubscription {1};
std::atomic<std::uint64_t> nextEmissionOrdinal {0};
std::atomic<std::uint64_t> nextCommandGeneration {1};
std::atomic<std::uint64_t> droppedCount {0};
CommandState commandState;
thread_local bool publishing = false;

void dropped() noexcept
{
	droppedCount.fetch_add(1, std::memory_order_relaxed);
}

bool publish(GdromObservation observation) noexcept
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

} // namespace

GdromObservationSubscription subscribe(
		GdromObservationCallback callback, bool evidence)
{
	if (!callback)
		throw std::invalid_argument("GD-ROM observation callback is empty");
	const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
	const std::lock_guard<std::mutex> lock(subscriptionMutex);
	if (evidence && !subscriptions.empty())
		throw std::logic_error("GD-ROM evidence observation requires exclusive ownership");
	if (!evidence && activeEvidenceSubscription.load(std::memory_order_acquire))
		throw std::logic_error("GD-ROM evidence observation owns the bus exclusively");
	auto next = std::make_shared<Subscription>();
	next->id = nextSubscription.fetch_add(1, std::memory_order_relaxed);
	if (next->id == 0)
		throw std::overflow_error("GD-ROM observation subscription id overflow");
	next->callback = std::move(callback);
	next->evidence = evidence;
	if (subscriptions.empty()) {
		const std::lock_guard<std::mutex> stateLock(stateMutex);
		commandState = {};
	}
	subscriptions.push_back(next);
	retainSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
	retainSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
	activeSubscriptionCount.fetch_add(1, std::memory_order_release);
	if (evidence)
		activeEvidenceSubscription.store(true, std::memory_order_release);
	return next->id;
}

GdromObservationSubscription subscribeGdromObservations(
		GdromObservationCallback callback)
{
	return subscribe(std::move(callback), false);
}

GdromObservationSubscription subscribeGdromEvidenceObservations(
		GdromObservationCallback callback)
{
	return subscribe(std::move(callback), true);
}

bool unsubscribeGdromObservations(GdromObservationSubscription id) noexcept
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

bool gdromObservationBusActive() noexcept
{
	return activeSubscriptionCount.load(std::memory_order_acquire) != 0;
}

bool reiosGdromObservationCommandActive() noexcept
{
	const std::lock_guard<std::mutex> lock(stateMutex);
	return commandState.active;
}

std::uint64_t gdromObservationDroppedCount() noexcept
{
	return droppedCount.load(std::memory_order_acquire);
}

void observeReiosGdromCommand(std::uint32_t requestId, std::uint32_t command,
		const std::uint32_t parameters[4], std::uint64_t tick) noexcept
{
	if (!gdromObservationBusActive())
		return;
	try
	{
		GdromObservation observation;
		observation.type = GdromObservationType::CommandBegin;
		observation.tick = tick;
		observation.path = GdromPath::ReiosHle;
		observation.initiator = sh4ObservationCurrentInstructionOwner();
		if (observation.initiator.valid)
			observation.tick = std::max(observation.tick,
					observation.initiator.tick);
		observation.requestId = requestId;
		observation.command = command;
		std::copy(parameters, parameters + 4, observation.parameters.begin());
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			if (commandState.active)
			{
				dropped();
				return;
			}
			commandState.active = true;
			commandState.generation = nextCommandGeneration.fetch_add(1,
					std::memory_order_relaxed);
			commandState.requestId = requestId;
			observation.commandGeneration = commandState.generation;
		}
		if (!publish(std::move(observation)))
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			commandState = {};
		}
	}
	catch (...) { dropped(); }
}

void observeReiosGdromTransfer(std::uint32_t fad, std::uint32_t sectorCount,
		std::uint32_t destination, const std::uint8_t* bytes,
		std::size_t byteCount, std::uint64_t tick) noexcept
{
	if (!gdromObservationBusActive())
		return;
	try
	{
		GdromObservation observation;
		observation.type = GdromObservationType::TransferChunk;
		observation.tick = tick;
		observation.fad = fad;
		observation.sectorCount = sectorCount;
		observation.destination = destination;
		observation.bytes.assign(bytes, bytes + byteCount);
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			if (!commandState.active) { dropped(); return; }
			observation.commandGeneration = commandState.generation;
			observation.chunkOrdinal = commandState.nextChunkOrdinal++;
			commandState.transferredBytes += byteCount;
		}
		publish(std::move(observation));
	}
	catch (...) { dropped(); }
}

void observeReiosGdromComplete(std::uint64_t tick) noexcept
{
	if (!gdromObservationBusActive())
		return;
	GdromObservation observation;
	observation.type = GdromObservationType::Complete;
	observation.tick = tick;
	observation.completion = GdromCompletionMechanism::Status;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (!commandState.active) { dropped(); return; }
		observation.commandGeneration = commandState.generation;
		observation.transferredBytes = commandState.transferredBytes;
		commandState = {};
	}
	publish(std::move(observation));
}

void observeReiosGdromAbort(std::uint32_t requestId, std::uint64_t tick) noexcept
{
	if (!gdromObservationBusActive())
		return;
	GdromObservation observation;
	observation.type = GdromObservationType::Abort;
	observation.tick = tick;
	observation.requestId = requestId;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (!commandState.active || commandState.requestId != requestId)
			return;
		observation.commandGeneration = commandState.generation;
		observation.transferredBytes = commandState.transferredBytes;
		commandState = {};
	}
	publish(std::move(observation));
}

void resetGdromObservation(std::uint64_t tick) noexcept
{
	if (!gdromObservationBusActive())
		return;
	GdromObservation observation;
	observation.type = GdromObservationType::Reset;
	observation.tick = tick;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		observation.commandGeneration = commandState.generation;
		observation.transferredBytes = commandState.transferredBytes;
		commandState = {};
	}
	publish(std::move(observation));
}

} // namespace research

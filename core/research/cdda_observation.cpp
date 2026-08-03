#include "research/cdda_observation.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace research
{
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

struct PendingControl
{
	bool active = false;
	std::uint64_t generation = 0;
	CddaControlPath path = CddaControlPath::ReiosHle;
	Sh4InstructionOwnerToken initiator;
	std::uint32_t requestId = 0;
	std::uint32_t command = 0;
	std::array<std::uint32_t, 4> parameters {};
};

std::recursive_mutex dispatchMutex;
std::mutex subscriptionMutex;
std::mutex stateMutex;
std::vector<std::shared_ptr<Subscription>> subscriptions;
std::atomic<std::size_t> activeSubscriptionCount {0};
std::atomic<bool> activeEvidenceSubscription {false};
std::atomic<std::uint64_t> nextSubscription {1};
std::atomic<std::uint64_t> nextEmissionOrdinal {0};
std::atomic<std::uint64_t> nextControlGeneration {1};
std::atomic<std::uint32_t> nextPacketRequest {1};
std::atomic<std::uint64_t> droppedCount {0};
PendingControl pendingControl;
std::uint64_t activeControlGeneration = 0;
thread_local bool publishing = false;

void dropped() noexcept
{
	droppedCount.fetch_add(1, std::memory_order_relaxed);
}

void reserveGeneration(std::uint64_t generation) noexcept
{
	std::uint64_t expected = nextControlGeneration.load(std::memory_order_relaxed);
	while (expected <= generation &&
			!nextControlGeneration.compare_exchange_weak(expected, generation + 1,
					std::memory_order_relaxed)) {}
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

void observeReiosCddaControlAccepted(std::uint32_t requestId,
		std::uint32_t command, const std::uint32_t parameters[4],
		std::uint64_t tick) noexcept
{
	if (!isReiosCddaControlCommand(command))
		return;
	try
	{
		CddaObservation observation;
		observation.type = CddaObservationType::ControlAccepted;
		observation.tick = tick;
		observation.path = CddaControlPath::ReiosHle;
		observation.initiator = sh4ObservationCurrentInstructionOwner();
		if (observation.initiator.valid)
			observation.tick = std::max(observation.tick, observation.initiator.tick);
		observation.requestId = requestId;
		observation.command = command;
		std::copy(parameters, parameters + 4, observation.parameters.begin());
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			if (pendingControl.active)
			{
				dropped();
				return;
			}
			pendingControl.active = true;
			pendingControl.generation = nextControlGeneration.fetch_add(1,
					std::memory_order_relaxed);
			pendingControl.path = observation.path;
			pendingControl.initiator = observation.initiator;
			pendingControl.requestId = requestId;
			pendingControl.command = command;
			pendingControl.parameters = observation.parameters;
			observation.controlGeneration = pendingControl.generation;
		}
		if (cddaObservationBusActive())
			publish(std::move(observation));
	}
	catch (...) { dropped(); }
}

void observeReiosCddaControlApplied(std::uint32_t requestId,
		std::uint32_t command, const CddaDriveState& before,
		const CddaDriveState& after, bool appliedSuccessfully,
		std::uint64_t tick) noexcept
{
	if (!isReiosCddaControlCommand(command))
		return;
	CddaObservation observation;
	observation.type = CddaObservationType::ControlApplied;
	observation.tick = tick;
	observation.before = before;
	observation.after = after;
	observation.appliedSuccessfully = appliedSuccessfully;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (!pendingControl.active || pendingControl.requestId != requestId ||
				pendingControl.command != command)
		{
			dropped();
			return;
		}
		observation.controlGeneration = pendingControl.generation;
		observation.path = pendingControl.path;
		observation.initiator = pendingControl.initiator;
		observation.requestId = pendingControl.requestId;
		observation.command = pendingControl.command;
		observation.parameters = pendingControl.parameters;
		if (appliedSuccessfully)
			activeControlGeneration = pendingControl.generation;
		pendingControl = {};
	}
	if (cddaObservationBusActive())
		publish(std::move(observation));
}

void observeGdromPacketCddaControlAccepted(const std::uint8_t packet[12],
		std::uint64_t tick) noexcept
{
	if (packet == nullptr || !isGdromPacketCddaControlCommand(packet[0]))
		return;
	try
	{
		CddaObservation observation;
		observation.type = CddaObservationType::ControlAccepted;
		observation.tick = tick;
		observation.path = CddaControlPath::GdromPacket;
		observation.initiator = sh4ObservationCurrentInstructionOwner();
		if (observation.initiator.valid)
			observation.tick = std::max(observation.tick, observation.initiator.tick);
		observation.requestId = nextPacketRequest.fetch_add(1,
				std::memory_order_relaxed);
		if (observation.requestId == 0)
			observation.requestId = nextPacketRequest.fetch_add(1,
					std::memory_order_relaxed);
		observation.command = packet[0];
		for (std::size_t word = 0; word < 3; ++word)
			for (std::size_t byte = 0; byte < 4; ++byte)
				observation.parameters[word] |= std::uint32_t(packet[word * 4 + byte])
						<< (byte * 8);
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			if (pendingControl.active)
			{
				dropped();
				return;
			}
			pendingControl.active = true;
			pendingControl.generation = nextControlGeneration.fetch_add(1,
					std::memory_order_relaxed);
			pendingControl.path = observation.path;
			pendingControl.initiator = observation.initiator;
			pendingControl.requestId = observation.requestId;
			pendingControl.command = observation.command;
			pendingControl.parameters = observation.parameters;
			observation.controlGeneration = pendingControl.generation;
		}
		if (cddaObservationBusActive())
			publish(std::move(observation));
	}
	catch (...) { dropped(); }
}

void observeGdromPacketCddaControlApplied(std::uint32_t command,
		const CddaDriveState& before, const CddaDriveState& after,
		bool appliedSuccessfully, std::uint64_t tick) noexcept
{
	if (!isGdromPacketCddaControlCommand(command))
		return;
	CddaObservation observation;
	observation.type = CddaObservationType::ControlApplied;
	observation.tick = tick;
	observation.before = before;
	observation.after = after;
	observation.appliedSuccessfully = appliedSuccessfully;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (!pendingControl.active
				|| pendingControl.path != CddaControlPath::GdromPacket
				|| pendingControl.command != command)
		{
			dropped();
			return;
		}
		observation.controlGeneration = pendingControl.generation;
		observation.path = pendingControl.path;
		observation.initiator = pendingControl.initiator;
		observation.requestId = pendingControl.requestId;
		observation.command = pendingControl.command;
		observation.parameters = pendingControl.parameters;
		if (appliedSuccessfully)
			activeControlGeneration = pendingControl.generation;
		pendingControl = {};
	}
	if (cddaObservationBusActive())
		publish(std::move(observation));
}

void observeCddaSector(std::uint64_t aicaGeneration, std::uint32_t fad,
		const CddaDriveState& before, const CddaDriveState& after,
		bool readSuccessful, const std::uint8_t* bytes,
		std::size_t byteCount, std::uint64_t tick) noexcept
{
	if (!cddaObservationBusActive())
		return;
	try
	{
		CddaObservation observation;
		observation.type = CddaObservationType::Sector;
		observation.tick = tick;
		observation.aicaGeneration = aicaGeneration;
		observation.fad = fad;
		observation.before = before;
		observation.after = after;
		observation.readSuccessful = readSuccessful;
		observation.bytes.assign(bytes, bytes + byteCount);
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			observation.controlGeneration = activeControlGeneration;
		}
		publish(std::move(observation));
	}
	catch (...) { dropped(); }
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

void resetCddaObservation(std::uint64_t tick) noexcept
{
	CddaObservation observation;
	observation.type = CddaObservationType::Reset;
	observation.tick = tick;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		observation.controlGeneration = activeControlGeneration;
		pendingControl = {};
		activeControlGeneration = 0;
	}
	if (cddaObservationBusActive())
		publish(std::move(observation));
}

} // namespace research

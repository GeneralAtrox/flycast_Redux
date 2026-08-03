#include "research/gdrom_hardware_observation.h"

#include "log/Log.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace research
{
namespace
{
struct Subscription
{
	GdromHardwareObservationSubscription id = 0;
	GdromHardwareObservationCallback callback;
	std::atomic<bool> active {true};
	bool evidence = false;
};
struct PendingAta
{
	bool active = false;
	Sh4InstructionOwnerToken owner;
	std::uint64_t tick = 0;
	std::uint32_t features = 0;
	std::uint32_t byteCount = 0;
	std::uint32_t driveState = 0;
};
struct ActiveCommand
{
	bool active = false;
	std::uint64_t generation = 0;
	std::uint64_t dmaGeneration = 0;
	std::uint64_t transferred = 0;
	std::uint64_t expected = 0;
	std::uint64_t produced = 0;
	GdromHardwareDelivery delivery = GdromHardwareDelivery::Pio;
};

std::recursive_mutex dispatchMutex;
std::mutex subscriptionMutex;
std::mutex stateMutex;
std::vector<std::shared_ptr<Subscription>> subscriptions;
std::atomic<std::size_t> activeSubscriptions {0};
std::atomic<bool> evidenceActive {false};
std::atomic<std::uint64_t> nextSubscription {1};
std::atomic<std::uint64_t> nextEmission {0};
std::atomic<std::uint64_t> nextCommand {1};
std::atomic<std::uint64_t> droppedCount {0};
PendingAta pendingAta;
ActiveCommand command;
std::uint64_t completedAwaitingStatus = 0;
std::uint64_t completedTransferred = 0;
thread_local bool publishing = false;

void dropped(const char* reason = "internal observation failure") noexcept
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

GdromHardwareObservation base(GdromHardwareObservationType type,
		std::uint64_t tick)
{
	GdromHardwareObservation event;
	event.type = type;
	event.tick = tick;
	return event;
}

void boundary(GdromHardwareObservationType type,
		GdromHardwareAbortReason reason, std::uint64_t tick) noexcept
{
	if (!gdromHardwareObservationBusActive()) return;
	GdromHardwareObservation event = base(type, tick);
	event.abortReason = reason;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		event.commandGeneration = command.generation;
		event.transferredBytes = command.transferred;
			pendingAta = {}; command = {};
			completedAwaitingStatus = 0; completedTransferred = 0;
	}
	publish(std::move(event));
}
} // namespace

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

void observeGdromHardwareAtaPacket(std::uint32_t features,
		std::uint32_t byteCount, std::uint32_t driveState,
		std::uint64_t tick) noexcept
{
	if (!gdromHardwareObservationBusActive()) return;
	try
	{
		PendingAta next;
		next.active = true;
		next.owner = sh4ObservationCurrentInstructionOwner();
		next.tick = std::max(tick, next.owner.valid ? next.owner.tick : tick);
		next.features = features;
		next.byteCount = byteCount;
		next.driveState = driveState;
		const std::lock_guard<std::mutex> lock(stateMutex);
		pendingAta = next;
	}
	catch (...) { dropped("ATA packet observation failed"); }
}

void observeGdromHardwarePacket(const std::uint8_t packetBytes[12],
		std::uint32_t startFad, std::uint32_t sectorCount,
		std::uint32_t sectorBytes, bool dma, std::uint64_t tick) noexcept
{
	if (!gdromHardwareObservationBusActive()) return;
	try
	{
		if (packetBytes == nullptr || (packetBytes[0] != 0x30 && packetBytes[0] != 0x31))
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			pendingAta = {};
			return;
		}
		GdromHardwareObservation event = base(
				GdromHardwareObservationType::PacketAccepted, tick);
		event.packetOwner = sh4ObservationCurrentInstructionOwner();
		if (event.packetOwner.valid)
			event.tick = std::max(event.tick, event.packetOwner.tick);
		std::copy(packetBytes, packetBytes + 12, event.packet.begin());
		event.startFad = startFad;
		event.sectorCount = sectorCount;
		event.sectorBytes = sectorBytes;
		event.delivery = dma ? GdromHardwareDelivery::Dma
				: GdromHardwareDelivery::Pio;
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			if (!pendingAta.active || !pendingAta.owner.valid
					|| !event.packetOwner.valid || sectorCount == 0
					|| (sectorBytes != 2048 && sectorBytes != 2340
							&& sectorBytes != 2352))
			{
				dropped("read packet lacks exact ATA/SPI ownership or a supported transfer size");
				pendingAta = {}; return;
			}
			if (command.active || completedAwaitingStatus != 0)
			{
				dropped("read packet overlaps an active or unacknowledged command");
				pendingAta = {}; command = {}; return;
			}
			command.active = true;
			command.generation = nextCommand.fetch_add(1, std::memory_order_relaxed);
			command.expected = std::uint64_t(sectorCount) * sectorBytes;
			command.delivery = event.delivery;
			event.commandGeneration = command.generation;
			event.ataOwner = pendingAta.owner;
			event.features = pendingAta.features;
			event.byteCountRegister = pendingAta.byteCount;
			event.driveState = pendingAta.driveState;
			pendingAta = {};
		}
		publish(std::move(event));
	}
	catch (...) { dropped("read packet observation failed"); }
}

void observeGdromHardwareBufferFill(std::uint32_t startFad,
		std::uint32_t sectorCount, std::uint32_t sectorBytes,
		bool successful, std::uint64_t tick) noexcept
{
	if (!gdromHardwareObservationBusActive()) return;
	GdromHardwareObservation event = base(GdromHardwareObservationType::BufferFill, tick);
	event.startFad = startFad; event.sectorCount = sectorCount;
	event.sectorBytes = sectorBytes; event.readSuccessful = successful;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (!command.active) { dropped("buffer fill has no active read command"); return; }
		event.commandGeneration = command.generation;
		event.streamOffset = command.produced;
		command.produced += std::uint64_t(sectorCount) * sectorBytes;
	}
	publish(std::move(event));
}

void observeGdromHardwareDmaBegin(std::uint32_t star, std::uint32_t length,
		std::uint32_t direction, std::uint32_t enabled, std::uint64_t tick) noexcept
{
	if (!gdromHardwareObservationBusActive()) return;
	GdromHardwareObservation event = base(GdromHardwareObservationType::DmaBegin, tick);
	event.packetOwner = sh4ObservationCurrentInstructionOwner();
	if (event.packetOwner.valid) event.tick = std::max(event.tick, event.packetOwner.tick);
	event.dmaStar = star; event.dmaLength = length;
	event.dmaDirection = direction; event.dmaEnabled = enabled;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (!command.active || command.delivery != GdromHardwareDelivery::Dma
				|| !event.packetOwner.valid) {
			dropped("DMA begin lacks an active DMA read or exact guest owner"); return;
		}
		event.commandGeneration = command.generation;
		event.dmaGeneration = ++command.dmaGeneration;
		event.streamOffset = command.transferred;
	}
	publish(std::move(event));
}

void observeGdromHardwareDmaTransfer(std::uint32_t destination,
		const std::uint8_t* bytes, std::size_t byteCount, std::uint64_t tick) noexcept
{
	if (!gdromHardwareObservationBusActive()) return;
	try
	{
		if (bytes == nullptr || byteCount == 0) {
			dropped("DMA transfer has no bytes"); return;
		}
		GdromHardwareObservation event = base(
				GdromHardwareObservationType::DmaTransfer, tick);
		event.destination = destination;
		event.bytes.assign(bytes, bytes + byteCount);
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			if (!command.active || command.delivery != GdromHardwareDelivery::Dma
					|| command.dmaGeneration == 0
					|| byteCount > command.expected - command.transferred)
			{ dropped("DMA transfer is outside the active command extent"); return; }
			event.commandGeneration = command.generation;
			event.dmaGeneration = command.dmaGeneration;
			event.streamOffset = command.transferred;
			command.transferred += byteCount;
			event.transferredBytes = command.transferred;
		}
		publish(std::move(event));
	}
	catch (...) { dropped("DMA transfer observation failed"); }
}

void observeGdromHardwarePioReady(std::uint32_t startFad,
		std::uint32_t sectorCount, std::uint32_t sectorBytes,
		bool successful, std::uint64_t tick) noexcept
{
	if (!gdromHardwareObservationBusActive()) return;
	GdromHardwareObservation event = base(GdromHardwareObservationType::PioReady, tick);
	event.startFad = startFad; event.sectorCount = sectorCount;
	event.sectorBytes = sectorBytes; event.readSuccessful = successful;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (!command.active)
			return;
		if (command.delivery != GdromHardwareDelivery::Pio)
		{ dropped("PIO ready belongs to an active non-PIO read command"); return; }
		event.commandGeneration = command.generation;
		event.streamOffset = command.produced;
		command.produced += std::uint64_t(sectorCount) * sectorBytes;
	}
	publish(std::move(event));
}

void observeGdromHardwarePioWord(std::uint16_t value, std::uint64_t tick) noexcept
{
	if (!gdromHardwareObservationBusActive()) return;
	GdromHardwareObservation event = base(GdromHardwareObservationType::PioWord, tick);
	event.packetOwner = sh4ObservationCurrentInstructionOwner();
	if (event.packetOwner.valid) event.tick = std::max(event.tick, event.packetOwner.tick);
	event.pioWord = value;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (!command.active)
			return;
		if (command.delivery != GdromHardwareDelivery::Pio
				|| !event.packetOwner.valid || command.expected - command.transferred < 2)
		{ dropped("PIO word is outside the active command or lacks a guest owner"); return; }
		event.commandGeneration = command.generation;
		event.streamOffset = command.transferred;
		command.transferred += 2;
		event.transferredBytes = command.transferred;
	}
	publish(std::move(event));
}

void observeGdromHardwareDmaInterrupt(std::uint64_t tick) noexcept
{
	if (!gdromHardwareObservationBusActive()) return;
	GdromHardwareObservation event = base(GdromHardwareObservationType::DmaInterrupt, tick);
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (!command.active || command.dmaGeneration == 0) {
			dropped("DMA interrupt has no active DMA generation"); return;
		}
		event.commandGeneration = command.generation;
		event.dmaGeneration = command.dmaGeneration;
		event.transferredBytes = command.transferred;
	}
	publish(std::move(event));
}

void observeGdromHardwareCommandInterrupt(std::uint64_t tick) noexcept
{
	if (!gdromHardwareObservationBusActive()) return;
	GdromHardwareObservation event = base(GdromHardwareObservationType::CommandInterrupt, tick);
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (!command.active) return;
		event.commandGeneration = command.generation;
		event.transferredBytes = command.transferred;
	}
	publish(std::move(event));
}

void observeGdromHardwareComplete(std::uint64_t tick) noexcept
{
	if (!gdromHardwareObservationBusActive()) return;
	GdromHardwareObservation event = base(GdromHardwareObservationType::Complete, tick);
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (!command.active) return;
		event.commandGeneration = command.generation;
		event.dmaGeneration = command.dmaGeneration;
		event.transferredBytes = command.transferred;
		completedAwaitingStatus = command.generation;
		completedTransferred = command.transferred;
		command = {};
	}
	publish(std::move(event));
}

void observeGdromHardwareStatusAcknowledged(std::uint32_t status,
		std::uint64_t tick) noexcept
{
	if (!gdromHardwareObservationBusActive()) return;
	GdromHardwareObservation event = base(
			GdromHardwareObservationType::StatusAcknowledged, tick);
	event.packetOwner = sh4ObservationCurrentInstructionOwner();
	if (event.packetOwner.valid) event.tick = std::max(event.tick, event.packetOwner.tick);
	event.statusRegister = status;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (!event.packetOwner.valid) {
			dropped("status acknowledgement lacks an exact guest owner"); return;
		}
		if (command.active)
		{
			event.commandGeneration = command.generation;
			event.transferredBytes = command.transferred;
		}
		else if (completedAwaitingStatus != 0)
		{
			event.commandGeneration = completedAwaitingStatus;
			event.transferredBytes = completedTransferred;
			completedAwaitingStatus = 0; completedTransferred = 0;
		}
		else return;
	}
	publish(std::move(event));
}

void observeGdromHardwareAbort(GdromHardwareAbortReason reason,
		std::uint64_t tick) noexcept
{
	if (!gdromHardwareObservationBusActive()) return;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (!command.active) return;
	}
	boundary(GdromHardwareObservationType::Abort, reason, tick);
}
void resetGdromHardwareObservation(std::uint64_t tick) noexcept
{ boundary(GdromHardwareObservationType::Reset, GdromHardwareAbortReason::Reset, tick); }
void loadStateGdromHardwareObservation(std::uint64_t tick) noexcept
{ boundary(GdromHardwareObservationType::LoadState, GdromHardwareAbortReason::LoadState, tick); }

} // namespace research

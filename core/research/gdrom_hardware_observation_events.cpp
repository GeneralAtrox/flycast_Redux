#include "research/gdrom_hardware_observation.h"
#include "research/gdrom_hardware_observation_internal.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace research
{

using namespace detail_gdrom_hardware;

namespace
{

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

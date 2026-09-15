#include "research/cdda_observation.h"
#include "research/cdda_observation_internal.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace research
{

using namespace detail_cdda;

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

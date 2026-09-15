// Structural validation of Maple traces: per-transaction shape checks shared
// with the writer, and the streaming production-trace validator used by the
// reader.
#include "research/maple_trace_internal.h"

#include <variant>

namespace research
{
namespace maple_trace_detail
{
namespace
{

std::uint64_t eventTick(const MapleTraceEvent& event)
{
	return std::visit([](const auto& value) { return value.tick; }, event.data);
}

std::uint32_t responseFrameSize(const std::vector<std::uint8_t>& response)
{
	if (response.empty())
		return 0;
	return (static_cast<std::uint32_t>(response[3]) + 1) * 4;
}

} // namespace

void validateTransactionShape(const MapleTransactionEvent& event)
{
	require(event.bus <= 3, "transaction bus is out of range");
	require(event.port <= 5, "transaction port is out of range");
	require((event.flags & ~MapleTransactionDevicePresent) == 0,
			"transaction has unknown flags");
	require(!event.request.empty() && event.request.size() <= 1024
			&& event.request.size() % 4 == 0,
			"transaction request size is invalid");
	require(event.response.size() <= 1024 && event.response.size() % 4 == 0,
			"transaction response size is invalid");
	require(event.request[0] == event.command,
			"transaction command does not match request frame");
	require(((event.descriptorHeader1 & 0xffu) + 1u) * 4u == event.request.size(),
			"transaction request length does not match descriptor");
	require((static_cast<std::uint32_t>(event.request[3]) + 1u) * 4u
				== event.request.size(),
			"transaction request frame length is inconsistent");
	require(((event.descriptorHeader1 >> 8) & 7u) == 0,
			"transaction descriptor is not a START operation");
	require(((event.descriptorHeader1 >> 16) & 3u) == event.bus,
			"transaction bus does not match descriptor");
	require((event.descriptorHeader2 & 0x1fffffe0u) == event.destinationAddress,
			"transaction destination does not match descriptor");
	require(event.destinationAddress != 0 && (event.destinationAddress & 31u) == 0,
			"transaction destination is invalid");
	require((event.descriptorAddress & 3u) == 0,
			"transaction descriptor address is unaligned");
	const bool present = (event.flags & MapleTransactionDevicePresent) != 0;
	require(present == (event.deviceType != UINT32_MAX),
			"transaction device presence and type disagree");
	if (present && !event.response.empty())
		require(responseFrameSize(event.response) == event.response.size(),
				"transaction response frame length is inconsistent");
}

ProductionTraceValidator::ProductionTraceValidator(std::uint32_t schemaVersion)
	: schemaVersion(schemaVersion)
{
}

void ProductionTraceValidator::consume(const MapleTraceEvent& event)
{
	require(event.ordinal == parsedEventCount, "event ordinal is not contiguous");
	const std::uint64_t tick = eventTick(event);
	if (!firstTick)
		require(tick >= previousTick, "event tick regressed");
	else
	{
		startTick = tick;
		firstTick = false;
	}
	previousTick = tick;
	++parsedEventCount;

	switch (event.type)
	{
	case MapleTraceEventType::DmaBegin:
	{
		const auto& value = std::get<MapleDmaBeginEvent>(event.data);
		require(!openDma && pending.empty(), "DMA began before prior DMA committed");
		require(value.dmaOrdinal == expectedDma++, "DMA ordinal is not contiguous");
		require(value.trigger == MapleDmaTrigger::Software
				|| value.trigger == MapleDmaTrigger::VBlank,
				"DMA trigger is invalid");
		require(value.mden == 1 && value.mdst == 1, "DMA begin register state is invalid");
		require(value.mmsel <= 1 && value.swapMsb == (value.mmsel == 0),
				"DMA byte-order state is inconsistent");
		require((value.descriptorAddress & 31u) == 0,
				"DMA descriptor table is not 32-byte aligned");
		openDma = true;
		currentDma = value.dmaOrdinal;
		currentResponses = 0;
		currentInputWireBytes = 0;
		currentOutputWireBytes = 0;
		nextDescriptorAddress = value.descriptorAddress;
		descriptorTerminalSeen = false;
		++parsedDmaCount;
		break;
	}
	case MapleTraceEventType::Transaction:
	{
		const auto& value = std::get<MapleTransactionEvent>(event.data);
		require(openDma && value.dmaOrdinal == currentDma,
				"transaction is outside its DMA");
		require(value.transactionOrdinal == expectedTransaction++,
				"transaction ordinal is not contiguous");
		validateTransactionShape(value);
		if (schemaVersion == MapleTraceSchemaVersionV2)
		{
			require(!descriptorTerminalSeen,
					"transaction follows the terminal DMA descriptor");
			require(value.descriptorAddress == nextDescriptorAddress,
					"transaction breaks descriptor-table continuity");
			nextDescriptorAddress += static_cast<std::uint32_t>(
					8u + value.request.size());
			descriptorTerminalSeen = (value.descriptorHeader1 >> 31) != 0;
		}
		if ((value.flags & MapleTransactionDevicePresent) != 0)
		{
			require(value.request.size() <= UINT32_MAX - 3u
					&& currentInputWireBytes <= UINT32_MAX - value.request.size() - 3u,
					"DMA input wire-byte count overflow");
			require(value.response.size() <= UINT32_MAX - 3u
					&& currentOutputWireBytes <= UINT32_MAX - value.response.size() - 3u,
					"DMA output wire-byte count overflow");
			currentInputWireBytes += static_cast<std::uint32_t>(value.request.size()) + 3u;
			currentOutputWireBytes += static_cast<std::uint32_t>(value.response.size()) + 3u;
		}
		++currentResponses;
		++parsedTransactionCount;
		break;
	}
	case MapleTraceEventType::ControlDescriptor:
	{
		require(schemaVersion == MapleTraceSchemaVersionV2,
				"control descriptor is not permitted by this schema");
		const auto& value = std::get<MapleControlDescriptorEvent>(event.data);
		require(openDma && value.dmaOrdinal == currentDma,
				"control descriptor is outside its DMA");
		require(value.controlOrdinal == expectedControl++,
				"control descriptor ordinal is not contiguous");
		require(!descriptorTerminalSeen,
				"control descriptor follows the terminal DMA descriptor");
		require(value.descriptorAddress == nextDescriptorAddress,
				"control descriptor breaks descriptor-table continuity");
		require(value.operation == MapleControlOperation::Nop,
				"unsupported typed control descriptor operation");
		require(((value.descriptorHeader >> 8) & 7u)
					== static_cast<std::uint32_t>(value.operation),
				"control operation differs from descriptor header");
		require(value.last == ((value.descriptorHeader >> 31) != 0),
				"control terminal flag differs from descriptor header");
		require((value.descriptorAddress & 3u) == 0,
				"control descriptor address is unaligned");
		nextDescriptorAddress += 4;
		descriptorTerminalSeen = value.last;
		++parsedControlCount;
		break;
	}
	case MapleTraceEventType::DmaSchedule:
	{
		const auto& value = std::get<MapleDmaScheduleEvent>(event.data);
		require(openDma && value.dmaOrdinal == currentDma,
				"DMA schedule does not close the active DMA");
		require(value.responseCount == currentResponses,
				"DMA schedule response count mismatch");
		require(value.inputWireBytes == currentInputWireBytes
				&& value.outputWireBytes == currentOutputWireBytes,
				"DMA schedule wire-byte counts mismatch");
		if (schemaVersion == MapleTraceSchemaVersionV2)
			require(descriptorTerminalSeen,
					"DMA schedule precedes the terminal descriptor");
		require((value.flags & ~MapleScheduleDeferredUntilVBlank) == 0,
				"DMA schedule has unknown flags");
		if ((value.flags & MapleScheduleDeferredUntilVBlank) != 0)
			require(value.scheduledCycles == 0,
					"deferred DMA must not have scheduled cycles");
		pending.emplace_back(currentDma, currentResponses);
		openDma = false;
		currentDma = UINT64_MAX;
		break;
	}
	case MapleTraceEventType::DmaCommit:
	{
		const auto& value = std::get<MapleDmaCommitEvent>(event.data);
		require(!openDma && !pending.empty(), "DMA commit has no pending DMA");
		require(value.dmaOrdinal == pending.front().first,
				"DMA commit ordinal mismatch");
		require(value.responseCount == pending.front().second,
				"DMA commit response count mismatch");
		require(value.flags == MapleCommitInterruptRaised,
				"production DMA commit must raise exactly the DMA interrupt");
		pending.pop_front();
		break;
	}
	case MapleTraceEventType::DmaAbort:
		invalid("production trace contains a DMA abort");
	default:
		invalid("unknown event type");
	}
}

void ProductionTraceValidator::finish(const MapleTraceSummary& summary) const
{
	require(summary.droppedEvents == 0, "dropped event count is nonzero");
	require(summary.eventCount == parsedEventCount, "header event count mismatch");
	require(parsedEventCount != 0, "trace has no events");
	require(!openDma, "trace ends with an open DMA");
	require(pending.empty(), "trace ends with an uncommitted DMA");
	require(parsedDmaCount != 0, "trace contains no DMA");
	require(parsedTransactionCount != 0, "trace contains no transactions");
	require(summary.dmaCount == parsedDmaCount, "header DMA count mismatch");
	require(summary.transactionCount == parsedTransactionCount,
			"header transaction count mismatch");
	require(summary.controlDescriptorCount == parsedControlCount,
			"header control-descriptor count mismatch");
	require(summary.startTick == startTick, "header start tick mismatch");
	require(summary.endTick == previousTick, "header end tick mismatch");
}

void validateProductionTrace(const MapleTrace& trace)
{
	ProductionTraceValidator validator(trace.summary.schemaVersion);
	for (const MapleTraceEvent& event : trace.events)
		validator.consume(event);
	validator.finish(trace.summary);
}

} // namespace maple_trace_detail
} // namespace research

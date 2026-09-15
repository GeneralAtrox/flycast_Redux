#include "research/maple_session.h"

#include "types.h"

#include <string>
#include <utility>
#include <variant>

namespace research
{
namespace
{

MapleCheckpointHandler checkpointHandler = nullptr;
MapleDmaBeginHandler dmaBeginHandler = nullptr;

[[noreturn]] void divergence(const std::string& field)
{
	throw FlycastException("Maple research replay divergence: " + field);
}

template<typename T>
void exact(const T& observed, const T& expected, const char *field)
{
	if (observed != expected)
		divergence(field);
}

void exact(std::uint64_t observed, std::uint64_t expected, const char *field)
{
	if (observed != expected)
		throw FlycastException("Maple research replay divergence: "
				+ std::string(field) + " observed=" + std::to_string(observed)
				+ " expected=" + std::to_string(expected));
}

} // namespace

MapleSession::MapleSession(MapleSessionMode mode, std::unique_ptr<MapleTraceWriter> writer,
		MapleTrace replay, std::uint64_t dmaCheckpoint)
	: mode(mode), writer(std::move(writer)), replay(std::move(replay)),
		dmaCheckpoint(dmaCheckpoint)
{
}

void MapleSession::setCheckpointHandler(MapleCheckpointHandler handler)
{
	checkpointHandler = handler;
}

void MapleSession::setDmaBeginHandler(MapleDmaBeginHandler handler)
{
	dmaBeginHandler = handler;
}

std::uint64_t MapleSession::beginDma(MapleDmaBeginEvent event)
{
	if (mode == MapleSessionMode::Record)
	{
		const std::uint64_t ordinal = writer->beginDma(event);
		notifyDmaBegin(ordinal);
		return ordinal;
	}
	const MapleDmaBeginEvent& expected = next<MapleDmaBeginEvent>(MapleTraceEventType::DmaBegin);
	exact(event.tick, expected.tick, "dma_begin tick");
	exact(event.descriptorAddress, expected.descriptorAddress, "DMA descriptor address");
	exact(event.mden, expected.mden, "SB_MDEN");
	exact(event.mdst, expected.mdst, "SB_MDST");
	exact(event.mmsel, expected.mmsel, "SB_MMSEL");
	exact(event.trigger, expected.trigger, "DMA trigger");
	exact(event.swapMsb, expected.swapMsb, "DMA byte order");
	notifyDmaBegin(expected.dmaOrdinal);
	return expected.dmaOrdinal;
}

std::vector<std::uint8_t> MapleSession::transaction(MapleTransactionEvent event)
{
	if (mode == MapleSessionMode::Record)
	{
		writer->writeTransaction(event);
		return event.response;
	}
	const MapleTransactionEvent& expected =
			next<MapleTransactionEvent>(MapleTraceEventType::Transaction);
	exact(event.dmaOrdinal, expected.dmaOrdinal, "transaction DMA ordinal");
	exact(event.tick, expected.tick, "transaction tick");
	exact(event.descriptorAddress, expected.descriptorAddress,
			"transaction descriptor address");
	exact(event.destinationAddress, expected.destinationAddress,
			"transaction destination address");
	exact(event.descriptorHeader1, expected.descriptorHeader1,
			"transaction descriptor header 1");
	exact(event.descriptorHeader2, expected.descriptorHeader2,
			"transaction descriptor header 2");
	exact(event.deviceType, expected.deviceType, "transaction device type");
	exact(event.bus, expected.bus, "transaction bus");
	exact(event.port, expected.port, "transaction port");
	exact(event.command, expected.command, "transaction command");
	exact(event.flags, expected.flags, "transaction flags");
	exact(event.request, expected.request, "transaction request bytes");
	return expected.response;
}

void MapleSession::controlDescriptor(MapleControlDescriptorEvent event)
{
	if (mode == MapleSessionMode::Record)
	{
		writer->writeControlDescriptor(event);
		return;
	}
	const MapleControlDescriptorEvent& expected = next<MapleControlDescriptorEvent>(
			MapleTraceEventType::ControlDescriptor);
	exact(event.dmaOrdinal, expected.dmaOrdinal, "control descriptor DMA ordinal");
	exact(event.tick, expected.tick, "control_descriptor tick");
	exact(event.descriptorAddress, expected.descriptorAddress,
			"control descriptor address");
	exact(event.descriptorHeader, expected.descriptorHeader,
			"control descriptor header");
	exact(event.operation, expected.operation, "control descriptor operation");
	exact(event.last, expected.last, "control descriptor terminal flag");
}

void MapleSession::scheduleDma(MapleDmaScheduleEvent event)
{
	if (mode == MapleSessionMode::Record)
	{
		writer->scheduleDma(event);
		return;
	}
	const MapleDmaScheduleEvent& expected =
			next<MapleDmaScheduleEvent>(MapleTraceEventType::DmaSchedule);
	exact(event.dmaOrdinal, expected.dmaOrdinal, "DMA schedule ordinal");
	exact(event.tick, expected.tick, "dma_schedule tick");
	exact(event.inputWireBytes, expected.inputWireBytes, "DMA input wire bytes");
	exact(event.outputWireBytes, expected.outputWireBytes, "DMA output wire bytes");
	exact(event.scheduledCycles, expected.scheduledCycles, "DMA scheduled cycles");
	exact(event.responseCount, expected.responseCount, "DMA scheduled response count");
	exact(event.flags, expected.flags, "DMA schedule flags");
}

void MapleSession::commitDma(MapleDmaCommitEvent event)
{
	if (mode == MapleSessionMode::Record)
	{
		writer->commitDma(event);
		checkpointAfterCommit();
		return;
	}
	const MapleDmaCommitEvent& expected =
			next<MapleDmaCommitEvent>(MapleTraceEventType::DmaCommit);
	exact(event.dmaOrdinal, expected.dmaOrdinal, "DMA commit ordinal");
	exact(event.tick, expected.tick, "dma_commit tick");
	exact(event.callbackCycles, expected.callbackCycles, "DMA callback cycles");
	exact(event.jitter, expected.jitter, "DMA callback jitter");
	exact(event.responseCount, expected.responseCount, "DMA committed response count");
	exact(event.flags, expected.flags, "DMA commit flags");
	checkpointAfterCommit();
}

void MapleSession::abortDma(MapleDmaAbortEvent event)
{
	if (mode == MapleSessionMode::Record)
	{
		writer->abortDma(event);
		return;
	}
	divergence("unexpected DMA abort");
}

void MapleSession::finish()
{
	if (dmaCheckpoint != 0 && !checkpointReached)
		divergence("DMA checkpoint was not reached");
	if (mode == MapleSessionMode::Record)
	{
		const MapleTraceSummary summary = writer->finalize();
		NOTICE_LOG(MAPLE, "Research Maple trace complete: %llu DMA, %llu transactions",
				static_cast<unsigned long long>(summary.dmaCount),
				static_cast<unsigned long long>(summary.transactionCount));
	}
	else if (cursor != replay.events.size())
	{
		divergence("replay stopped before the terminal event");
	}
}

void MapleSession::abandon() noexcept
{
	if (writer != nullptr)
		writer->abandon();
}

void MapleSession::notifyDmaBegin(std::uint64_t zeroBasedOrdinal)
{
	if (dmaBeginHandler != nullptr)
		dmaBeginHandler(zeroBasedOrdinal + 1);
}

void MapleSession::checkpointAfterCommit()
{
	++committedDmaCount;
	if (dmaCheckpoint == 0 || committedDmaCount != dmaCheckpoint)
		return;
	checkpointReached = true;
	NOTICE_LOG(MAPLE,
			"Research Maple DMA checkpoint reached after %llu committed DMA",
			static_cast<unsigned long long>(committedDmaCount));
	if (checkpointHandler == nullptr)
		divergence("DMA checkpoint handler is unavailable");
	checkpointHandler();
}

template<typename T>
const T& MapleSession::next(MapleTraceEventType type)
{
	if (cursor >= replay.events.size())
		divergence("event stream exhausted");
	const MapleTraceEvent& event = replay.events[cursor++];
	if (event.type != type || !std::holds_alternative<T>(event.data))
		divergence("event type/order");
	return std::get<T>(event.data);
}

} // namespace research

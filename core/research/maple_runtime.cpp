#include "research/maple_runtime.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "types.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace research
{
namespace
{

enum class Mode
{
	None,
	Record,
	Replay,
};

constexpr std::uint64_t MaximumMapleDmaCheckpoint = 10'000'000;
// Deterministic wall clock (2000-01-01T00:00:00Z) applied when a record/replay
// session did not pin research.DreamcastRtcSeed itself. Replays must see the
// same RTC bytes the recording saw.
constexpr std::int64_t DefaultDeterministicRtcSeed = 946684800;

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

class Session
{
public:
	Session(Mode mode, std::unique_ptr<MapleTraceWriter> writer, MapleTrace replay,
			std::uint64_t dmaCheckpoint)
		: mode(mode), writer(std::move(writer)), replay(std::move(replay)),
			dmaCheckpoint(dmaCheckpoint)
	{
	}

	std::uint64_t beginDma(MapleDmaBeginEvent event)
	{
		if (mode == Mode::Record)
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

	std::vector<std::uint8_t> transaction(MapleTransactionEvent event)
	{
		if (mode == Mode::Record)
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

	void controlDescriptor(MapleControlDescriptorEvent event)
	{
		if (mode == Mode::Record)
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

	void scheduleDma(MapleDmaScheduleEvent event)
	{
		if (mode == Mode::Record)
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

	void commitDma(MapleDmaCommitEvent event)
	{
		if (mode == Mode::Record)
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

	void abortDma(MapleDmaAbortEvent event)
	{
		if (mode == Mode::Record)
		{
			writer->abortDma(event);
			return;
		}
		divergence("unexpected DMA abort");
	}

	void finish()
	{
		if (dmaCheckpoint != 0 && !checkpointReached)
			divergence("DMA checkpoint was not reached");
		if (mode == Mode::Record)
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

	void abandon() noexcept
	{
		if (writer != nullptr)
			writer->abandon();
	}

	Mode getMode() const { return mode; }
	bool replayConsumed() const
	{
		return mode == Mode::Replay && cursor == replay.events.size();
	}

private:
	void notifyDmaBegin(std::uint64_t zeroBasedOrdinal)
	{
		if (dmaBeginHandler != nullptr)
			dmaBeginHandler(zeroBasedOrdinal + 1);
	}

	void checkpointAfterCommit()
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
	const T& next(MapleTraceEventType type)
	{
		if (cursor >= replay.events.size())
			divergence("event stream exhausted");
		const MapleTraceEvent& event = replay.events[cursor++];
		if (event.type != type || !std::holds_alternative<T>(event.data))
			divergence("event type/order");
		return std::get<T>(event.data);
	}

	Mode mode;
	std::unique_ptr<MapleTraceWriter> writer;
	MapleTrace replay;
	std::size_t cursor = 0;
	std::uint64_t dmaCheckpoint = 0;
	std::uint64_t committedDmaCount = 0;
	bool checkpointReached = false;
};

Mode configuredMode = Mode::None;
std::unique_ptr<Session> session;

std::filesystem::path researchPath(const std::string& value)
{
#ifdef __cpp_char8_t
	std::u8string utf8(value.size(), u8'\0');
	std::memcpy(utf8.data(), value.data(), value.size());
	return std::filesystem::path(utf8);
#else
	return std::filesystem::u8path(value);
#endif
}

// Record and replay share one timing model. Everything that could make the
// same inputs produce different ticks is pinned here for the session.
void applyDeterministicOverrides()
{
	config::ThreadedRendering.override(false);
	config::AutoSaveState.override(false);
	config::GGPOEnable.override(false);
	if (config::DynarecEnabled.get() && !config::ResearchDynarecObservation.get())
	{
		// Replay ticks are recorded at instruction granularity. The dynarec
		// reproduces them only with the research per-instruction timing model.
		config::ResearchDynarecObservation.override(true);
		NOTICE_LOG(MAPLE, "Maple record/replay enabled research.DynarecObservation for exact dynarec timing");
	}
	if (config::ResearchDreamcastRtcSeed.get() < 0)
	{
		config::ResearchDreamcastRtcSeed.override(DefaultDeterministicRtcSeed);
		NOTICE_LOG(MAPLE, "Maple record/replay pinned research.DreamcastRtcSeed to %lld",
				static_cast<long long>(DefaultDeterministicRtcSeed));
	}
}

} // namespace

void configureRuntime()
{
	abortRuntime();
	const bool recording = !config::ResearchMapleRecordPath.get().empty();
	const bool replaying = !config::ResearchMapleReplayPath.get().empty();
	if (recording && replaying)
		throw FlycastException("Maple research record and replay modes are mutually exclusive");
	configuredMode = recording ? Mode::Record : replaying ? Mode::Replay : Mode::None;
	if (configuredMode == Mode::None)
		return;
	if (config::ResearchMapleTraceMaxBytes.get() <= 0)
		throw FlycastException("research.MapleTraceMaxBytes must be positive");
	if (config::ResearchMapleTraceMaxBytes.get() < MapleTraceHeaderSize)
		throw FlycastException("research.MapleTraceMaxBytes is smaller than the trace header");
	if (config::ResearchMapleDmaCheckpoint.get() < 0
			|| static_cast<std::uint64_t>(config::ResearchMapleDmaCheckpoint.get())
					> MaximumMapleDmaCheckpoint)
		throw FlycastException("research.MapleDmaCheckpoint is outside [0, 10000000]");
	applyDeterministicOverrides();
}

void startRuntime()
{
	if (configuredMode == Mode::None)
		return;
	if (session != nullptr)
		throw FlycastException("research runtime is already active");
	const std::filesystem::path tracePath = configuredMode == Mode::Record
			? researchPath(config::ResearchMapleRecordPath.get())
			: researchPath(config::ResearchMapleReplayPath.get());
	const std::uint64_t maximumBytes =
			static_cast<std::uint64_t>(config::ResearchMapleTraceMaxBytes.get());
	const std::uint64_t dmaCheckpoint = static_cast<std::uint64_t>(
			config::ResearchMapleDmaCheckpoint.get());

	if (configuredMode == Mode::Record)
	{
		auto writer = std::make_unique<MapleTraceWriter>(tracePath, Sha256Digest {},
				maximumBytes, MapleTraceCurrentSchemaVersion);
		session = std::make_unique<Session>(configuredMode, std::move(writer),
				MapleTrace {}, dmaCheckpoint);
		NOTICE_LOG(MAPLE, "Recording Maple research trace to %s", tracePath.string().c_str());
	}
	else
	{
		MapleTrace replay = loadProductionMapleTrace(tracePath, maximumBytes);
		session = std::make_unique<Session>(configuredMode, nullptr, std::move(replay),
				dmaCheckpoint);
		NOTICE_LOG(MAPLE, "Replaying Maple research trace from %s", tracePath.string().c_str());
	}
}

void stopRuntime(bool clean)
{
	if (session == nullptr)
	{
		configuredMode = Mode::None;
		return;
	}
	std::unique_ptr<Session> finishing = std::move(session);
	configuredMode = Mode::None;
	if (!clean)
	{
		finishing->abandon();
		return;
	}
	try
	{
		finishing->finish();
	}
	catch (...)
	{
		finishing->abandon();
		throw;
	}
}

void abortRuntime() noexcept
{
	configuredMode = Mode::None;
	if (session != nullptr)
	{
		session->abandon();
		session.reset();
	}
}

bool runtimeActive()
{
	return session != nullptr;
}

bool mapleRecording()
{
	return session != nullptr && session->getMode() == Mode::Record;
}

bool mapleReplaying()
{
	return session != nullptr && session->getMode() == Mode::Replay;
}

bool mapleReplayConsumed()
{
	return session != nullptr && session->replayConsumed();
}

void setMapleCheckpointHandler(MapleCheckpointHandler handler)
{
	checkpointHandler = handler;
}

void setMapleDmaBeginHandler(MapleDmaBeginHandler handler)
{
	dmaBeginHandler = handler;
}

std::uint64_t mapleBeginDma(MapleDmaBeginEvent event)
{
	return session == nullptr ? UINT64_MAX : session->beginDma(event);
}

std::vector<std::uint8_t> mapleTransaction(MapleTransactionEvent event)
{
	return session == nullptr ? std::move(event.response) : session->transaction(std::move(event));
}

void mapleControlDescriptor(MapleControlDescriptorEvent event)
{
	if (session != nullptr)
		session->controlDescriptor(event);
}

void mapleScheduleDma(MapleDmaScheduleEvent event)
{
	if (session != nullptr)
		session->scheduleDma(event);
}

void mapleCommitDma(MapleDmaCommitEvent event)
{
	if (session != nullptr)
		session->commitDma(event);
}

void mapleAbortDma(MapleDmaAbortEvent event)
{
	if (session != nullptr)
		session->abortDma(event);
}

} // namespace research

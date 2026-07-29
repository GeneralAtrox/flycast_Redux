#include "research/maple_runtime.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "research/identity_manifest.h"
#include "types.h"

#include <filesystem>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

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

class Session
{
public:
	Session(Mode mode, IdentityManifest identity, std::unique_ptr<MapleTraceWriter> writer,
			MapleTrace replay)
		: mode(mode), identity(std::move(identity)), writer(std::move(writer)), replay(std::move(replay))
	{
	}

	std::uint64_t beginDma(MapleDmaBeginEvent event)
	{
		if (mode == Mode::Record)
			return writer->beginDma(event);
		const MapleDmaBeginEvent& expected = next<MapleDmaBeginEvent>(MapleTraceEventType::DmaBegin);
		exact(event.tick, expected.tick, "DMA begin tick");
		exact(event.descriptorAddress, expected.descriptorAddress, "DMA descriptor address");
		exact(event.mden, expected.mden, "SB_MDEN");
		exact(event.mdst, expected.mdst, "SB_MDST");
		exact(event.mmsel, expected.mmsel, "SB_MMSEL");
		exact(event.trigger, expected.trigger, "DMA trigger");
		exact(event.swapMsb, expected.swapMsb, "DMA byte order");
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
		exact(event.tick, expected.tick, "DMA schedule tick");
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
			return;
		}
		const MapleDmaCommitEvent& expected =
				next<MapleDmaCommitEvent>(MapleTraceEventType::DmaCommit);
		exact(event.dmaOrdinal, expected.dmaOrdinal, "DMA commit ordinal");
		exact(event.tick, expected.tick, "DMA commit tick");
		exact(event.callbackCycles, expected.callbackCycles, "DMA callback cycles");
		exact(event.jitter, expected.jitter, "DMA callback jitter");
		exact(event.responseCount, expected.responseCount, "DMA committed response count");
		exact(event.flags, expected.flags, "DMA commit flags");
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
		if (mode == Mode::Record)
		{
			const MapleTraceSummary summary = writer->finalize();
			NOTICE_LOG(MAPLE, "Research Maple trace complete: %llu DMA, %llu transactions, identity %s",
					static_cast<unsigned long long>(summary.dmaCount),
					static_cast<unsigned long long>(summary.transactionCount),
					sha256ToHex(summary.identityDigest).c_str());
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

private:
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
	IdentityManifest identity;
	std::unique_ptr<MapleTraceWriter> writer;
	MapleTrace replay;
	std::size_t cursor = 0;
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

void applyDeterministicOverrides()
{
	config::DynarecEnabled.override(false);
	config::ThreadedRendering.override(false);
	config::AutoLoadState.override(false);
	config::AutoSaveState.override(false);
	config::GGPOEnable.override(false);
}

void verifyRuntimeConfiguration(const IdentityManifest& identity)
{
	const IdentityRuntimeConfiguration& expected = identity.runtimeConfiguration;
	if (expected.cpuBackend != "interpreter" || config::DynarecEnabled.get())
		throw FlycastException("research identity/runtime CPU backend mismatch");
	if (expected.threadedRendering != config::ThreadedRendering.get())
		throw FlycastException("research identity/runtime threaded-rendering mismatch");
	if (expected.autoLoadState != config::AutoLoadState.get())
		throw FlycastException("research identity/runtime auto-load-state mismatch");
	if (expected.autoSaveState != config::AutoSaveState.get())
		throw FlycastException("research identity/runtime auto-save-state mismatch");
	if (expected.ggpo != config::GGPOEnable.get())
		throw FlycastException("research identity/runtime GGPO mismatch");
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
	if (config::ResearchIdentityManifestPath.get().empty())
		throw FlycastException("Maple research mode requires research.IdentityManifest");
	if (config::ResearchMapleTraceMaxBytes.get() <= 0)
		throw FlycastException("research.MapleTraceMaxBytes must be positive");
	if (config::ResearchMapleTraceMaxBytes.get() < MapleTraceHeaderSize)
		throw FlycastException("research.MapleTraceMaxBytes is smaller than the trace header");
	const char *modeKey = recording ? "MapleRecord" : "MapleReplay";
	if (!config::isTransient("research", "IdentityManifest")
			|| !config::isTransient("research", modeKey))
		throw FlycastException("Maple research paths must be supplied as transient options");
	applyDeterministicOverrides();
}

void startRuntime()
{
	if (configuredMode == Mode::None)
		return;
	if (session != nullptr)
		throw FlycastException("research runtime is already active");
	applyDeterministicOverrides();

	const std::filesystem::path identityPath = researchPath(config::ResearchIdentityManifestPath.get());
	const std::filesystem::path tracePath = configuredMode == Mode::Record
			? researchPath(config::ResearchMapleRecordPath.get())
			: researchPath(config::ResearchMapleReplayPath.get());
	if (pathsAlias(identityPath, tracePath))
		throw FlycastException("research identity and Maple trace paths alias");
	IdentityManifest identity = loadIdentityManifest(identityPath);
	verifyRuntimeConfiguration(identity);

	if (configuredMode == Mode::Record)
	{
		auto writer = std::make_unique<MapleTraceWriter>(tracePath, identity.digest,
				static_cast<std::uint64_t>(config::ResearchMapleTraceMaxBytes.get()));
		session = std::make_unique<Session>(configuredMode, std::move(identity),
				std::move(writer), MapleTrace {});
		NOTICE_LOG(MAPLE, "Recording typed Maple research trace to %s", tracePath.string().c_str());
	}
	else
	{
		MapleTrace replay = loadProductionMapleTrace(tracePath, identity.digest,
				static_cast<std::uint64_t>(config::ResearchMapleTraceMaxBytes.get()));
		session = std::make_unique<Session>(configuredMode, std::move(identity), nullptr,
				std::move(replay));
		NOTICE_LOG(MAPLE, "Replaying typed Maple research trace from %s", tracePath.string().c_str());
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

std::uint64_t mapleBeginDma(MapleDmaBeginEvent event)
{
	return session == nullptr ? UINT64_MAX : session->beginDma(event);
}

std::vector<std::uint8_t> mapleTransaction(MapleTransactionEvent event)
{
	return session == nullptr ? std::move(event.response) : session->transaction(std::move(event));
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

#include "research/maple_runtime.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "research/maple_session.h"
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

constexpr std::uint64_t MaximumMapleDmaCheckpoint = 10'000'000;
// Deterministic wall clock (2000-01-01T00:00:00Z) applied when a record/replay
// session did not pin research.DreamcastRtcSeed itself. Replays must see the
// same RTC bytes the recording saw.
constexpr std::int64_t DefaultDeterministicRtcSeed = 946684800;

MapleSessionMode configuredMode = MapleSessionMode::None;
std::unique_ptr<MapleSession> session;

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
	configuredMode = recording ? MapleSessionMode::Record
			: replaying ? MapleSessionMode::Replay : MapleSessionMode::None;
	if (configuredMode == MapleSessionMode::None)
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
	if (configuredMode == MapleSessionMode::None)
		return;
	if (session != nullptr)
		throw FlycastException("research runtime is already active");
	const std::filesystem::path tracePath = configuredMode == MapleSessionMode::Record
			? researchPath(config::ResearchMapleRecordPath.get())
			: researchPath(config::ResearchMapleReplayPath.get());
	const std::uint64_t maximumBytes =
			static_cast<std::uint64_t>(config::ResearchMapleTraceMaxBytes.get());
	const std::uint64_t dmaCheckpoint = static_cast<std::uint64_t>(
			config::ResearchMapleDmaCheckpoint.get());

	if (configuredMode == MapleSessionMode::Record)
	{
		auto writer = std::make_unique<MapleTraceWriter>(tracePath, Sha256Digest {},
				maximumBytes, MapleTraceCurrentSchemaVersion);
		session = std::make_unique<MapleSession>(configuredMode, std::move(writer),
				MapleTrace {}, dmaCheckpoint);
		NOTICE_LOG(MAPLE, "Recording Maple research trace to %s", tracePath.string().c_str());
	}
	else
	{
		MapleTrace replay = loadProductionMapleTrace(tracePath, maximumBytes);
		session = std::make_unique<MapleSession>(configuredMode, nullptr, std::move(replay),
				dmaCheckpoint);
		NOTICE_LOG(MAPLE, "Replaying Maple research trace from %s", tracePath.string().c_str());
	}
}

void stopRuntime(bool clean)
{
	if (session == nullptr)
	{
		configuredMode = MapleSessionMode::None;
		return;
	}
	std::unique_ptr<MapleSession> finishing = std::move(session);
	configuredMode = MapleSessionMode::None;
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
	configuredMode = MapleSessionMode::None;
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
	return session != nullptr && session->getMode() == MapleSessionMode::Record;
}

bool mapleReplaying()
{
	return session != nullptr && session->getMode() == MapleSessionMode::Replay;
}

bool mapleReplayConsumed()
{
	return session != nullptr && session->replayConsumed();
}

void setMapleCheckpointHandler(MapleCheckpointHandler handler)
{
	MapleSession::setCheckpointHandler(handler);
}

void setMapleDmaBeginHandler(MapleDmaBeginHandler handler)
{
	MapleSession::setDmaBeginHandler(handler);
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

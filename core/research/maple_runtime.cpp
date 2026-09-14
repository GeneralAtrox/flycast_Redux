#include "research/maple_runtime.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "hw/flashrom/nvmem.h"
#include "research/identity_manifest.h"
#include "research/sh4_observation_runtime.h"
#include "oslib/oslib.h"
#include "types.h"

#include <algorithm>
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

MapleCheckpointHandler checkpointHandler = nullptr;
MapleDmaBeginHandler dmaBeginHandler = nullptr;

const char *timingDiagnosticStateName(
		Sh4DynarecTimingDiagnosticState state) noexcept
{
	switch (state)
	{
	case Sh4DynarecTimingDiagnosticState::Open:
		return "open";
	case Sh4DynarecTimingDiagnosticState::Completed:
		return "completed";
	case Sh4DynarecTimingDiagnosticState::Exception:
		return "exception";
	case Sh4DynarecTimingDiagnosticState::Interrupt:
		return "interrupt";
	}
	return "unknown";
}

void logTimingDiagnosticSnapshot(std::uint64_t zeroBasedOrdinal,
		std::uint64_t observed, std::uint64_t expected)
{
	const Sh4DynarecTimingDiagnosticSnapshot snapshot =
			sh4DynarecTimingDiagnosticSnapshot(zeroBasedOrdinal, observed, expected);
	NOTICE_LOG(SH4,
			"Research SH-4 dynarec timing history begin zero_based_ordinal=%llu observed=%llu expected=%llu active=%u capacity=%llu record_count=%llu next_sequence=%llu",
			static_cast<unsigned long long>(zeroBasedOrdinal),
			static_cast<unsigned long long>(observed),
			static_cast<unsigned long long>(expected), snapshot.active ? 1u : 0u,
			static_cast<unsigned long long>(
					Sh4DynarecTimingDiagnosticHistoryCapacity),
			static_cast<unsigned long long>(snapshot.records.size()),
			static_cast<unsigned long long>(snapshot.nextSequence));
	for (const Sh4DynarecTimingDiagnosticRecord& record : snapshot.records)
	{
		NOTICE_LOG(SH4,
				"Research SH-4 dynarec timing history record zero_based_ordinal=%llu sequence=%llu state=%s pc=%08x opcode=%04x next_pc=%08x depth=%u precise=%u instruction_cycles=%d boundary_code=%08x begin_scheduler_tick=%llu end_scheduler_tick=%llu begin_execution_tick=%llu end_execution_tick=%llu begin_cycle_counter=%lld end_cycle_counter=%lld",
				static_cast<unsigned long long>(zeroBasedOrdinal),
				static_cast<unsigned long long>(record.sequence),
				timingDiagnosticStateName(record.state), record.pc,
				record.opcode, record.nextPc, record.depth,
				record.precise ? 1u : 0u, record.instructionCycles,
				record.boundaryCode,
				static_cast<unsigned long long>(record.schedulerTickBegin),
				static_cast<unsigned long long>(record.schedulerTickEnd),
				static_cast<unsigned long long>(record.executionTickBegin),
				static_cast<unsigned long long>(record.executionTickEnd),
				static_cast<long long>(record.cycleCounterBegin),
				static_cast<long long>(record.cycleCounterEnd));
	}
	NOTICE_LOG(SH4,
			"Research SH-4 dynarec timing history end zero_based_ordinal=%llu record_count=%llu",
			static_cast<unsigned long long>(zeroBasedOrdinal),
			static_cast<unsigned long long>(snapshot.records.size()));
}

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
	Session(Mode mode, IdentityManifest identity, std::unique_ptr<MapleTraceWriter> writer,
			MapleTrace replay, std::uint64_t dmaCheckpoint)
		: mode(mode), identity(std::move(identity)), writer(std::move(writer)),
			replay(std::move(replay)), dmaCheckpoint(dmaCheckpoint)
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
		exactReplayTick(event.tick, expected.tick, "dma_begin",
				expected.dmaOrdinal);
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
		exactReplayTick(event.tick, expected.tick, "transaction",
				expected.dmaOrdinal);
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
		exactReplayTick(event.tick, expected.tick, "control_descriptor",
				expected.dmaOrdinal);
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
		exactReplayTick(event.tick, expected.tick, "dma_schedule",
				expected.dmaOrdinal);
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
		exactReplayTick(event.tick, expected.tick, "dma_commit",
				expected.dmaOrdinal);
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
		authenticateFirmwareFiles(identity);
		if (dmaCheckpoint != 0 && !checkpointReached)
			divergence("DMA checkpoint was not reached");
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
		if (mode == Mode::Replay
				&& identity.runtimeConfiguration.dynarecReplayDiagnostic)
		{
			NOTICE_LOG(MAPLE,
					"Research Maple diagnostic timing summary mismatch_count=%llu first_zero_based_ordinal=%llu last_zero_based_ordinal=%llu maximum_absolute_delta=%llu",
					static_cast<unsigned long long>(timingMismatchCount),
					static_cast<unsigned long long>(firstTimingMismatchOrdinal),
					static_cast<unsigned long long>(lastTimingMismatchOrdinal),
					static_cast<unsigned long long>(maximumAbsoluteTimingDelta));
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
	void exactReplayTick(std::uint64_t observed, std::uint64_t expected,
			const char *event, std::uint64_t zeroBasedOrdinal)
	{
		if (observed == expected)
			return;
		if (!identity.runtimeConfiguration.dynarecReplayDiagnostic)
			exact(observed, expected, event);
		const bool firstMismatchForDma = timingMismatchCount == 0
				|| lastTimingMismatchOrdinal != zeroBasedOrdinal;
		const std::uint64_t absoluteDelta = observed >= expected
				? observed - expected : expected - observed;
		if (timingMismatchCount == 0)
			firstTimingMismatchOrdinal = zeroBasedOrdinal;
		lastTimingMismatchOrdinal = zeroBasedOrdinal;
		maximumAbsoluteTimingDelta = std::max(maximumAbsoluteTimingDelta,
				absoluteDelta);
		++timingMismatchCount;
		if (firstMismatchForDma)
			logTimingDiagnosticSnapshot(zeroBasedOrdinal, observed, expected);
		NOTICE_LOG(MAPLE,
				"Research Maple diagnostic timing mismatch event=%s zero_based_ordinal=%llu observed=%llu expected=%llu delta=%lld",
				event, static_cast<unsigned long long>(zeroBasedOrdinal),
				static_cast<unsigned long long>(observed),
				static_cast<unsigned long long>(expected),
				static_cast<long long>(observed) - static_cast<long long>(expected));
	}

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
	IdentityManifest identity;
	std::unique_ptr<MapleTraceWriter> writer;
	MapleTrace replay;
	std::size_t cursor = 0;
	std::uint64_t dmaCheckpoint = 0;
	std::uint64_t committedDmaCount = 0;
	std::uint64_t timingMismatchCount = 0;
	std::uint64_t firstTimingMismatchOrdinal = 0;
	std::uint64_t lastTimingMismatchOrdinal = 0;
	std::uint64_t maximumAbsoluteTimingDelta = 0;
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

void applyDeterministicOverrides(const IdentityManifest *identity = nullptr)
{
	// Backend-equivalence capture owns this setting from its v2 identity before
	// dc_reset selects the executor. Frozen Maple-only v1 sessions remain
	// interpreter-only.
	const bool diagnosticDynarec = identity != nullptr
			&& identity->runtimeConfiguration.dynarecReplayDiagnostic;
	if (diagnosticDynarec)
		config::DynarecEnabled.override(true);
	if (!diagnosticDynarec
			&& config::ResearchSh4ObservationRecordPath.get().empty()
			&& config::ResearchSh4EventsRecordPath.get().empty()
			&& config::ResearchSh4ProfileRecordPath.get().empty()
			&& config::ResearchPvrTaRecordPath.get().empty()
			&& config::ResearchGdromRecordPath.get().empty())
		config::DynarecEnabled.override(false);
	config::ThreadedRendering.override(false);
	if (identity != nullptr)
	{
		if (diagnosticDynarec)
			config::ResearchDynarecObservation.override(
					identity->runtimeConfiguration.dynarecObservation);
		config::ResearchDreamcastRtcSeed.override(
				identity->runtimeConfiguration.dreamcastRtcSeed);
		config::UseReios.override(identity->firmware.mode == FirmwareMode::Hle);
	}
	config::AutoLoadState.override(identity != nullptr
			&& identity->runtimeConfiguration.autoLoadState);
	if (identity != nullptr && identity->initialState.available)
		config::SavestateSlot.override(static_cast<int>(identity->initialState.slot));
	config::AutoSaveState.override(false);
	config::GGPOEnable.override(false);
}

void verifyRuntimeConfiguration(const IdentityManifest& identity)
{
	const IdentityRuntimeConfiguration& expected = identity.runtimeConfiguration;
	const bool expectedDynarec = expected.cpuBackend == "dynarec";
	if ((expected.cpuBackend != "interpreter" && !expectedDynarec)
			|| config::DynarecEnabled.get() != expectedDynarec)
		throw FlycastException("research identity/runtime CPU backend mismatch");
	if (expected.dynarecObservation
			!= config::ResearchDynarecObservation.get())
		throw FlycastException("research identity/runtime dynarec observation mismatch");
	if (expected.dreamcastRtcSeed
			!= static_cast<std::uint32_t>(config::ResearchDreamcastRtcSeed.get()))
		throw FlycastException("research identity/runtime Dreamcast RTC seed mismatch");
	if (expected.threadedRendering != config::ThreadedRendering.get())
		throw FlycastException("research identity/runtime threaded-rendering mismatch");
	if (expected.autoLoadState != config::AutoLoadState.get())
		throw FlycastException("research identity/runtime auto-load-state mismatch");
	if (expected.autoLoadState
			&& static_cast<std::uint32_t>(config::SavestateSlot.get())
					!= expected.savestateSlot)
		throw FlycastException("research identity/runtime save-state slot mismatch");
	if (expected.autoSaveState != config::AutoSaveState.get())
		throw FlycastException("research identity/runtime auto-save-state mismatch");
	if (expected.ggpo != config::GGPOEnable.get())
		throw FlycastException("research identity/runtime GGPO mismatch");
	if (expected.mapleDmaCheckpoint
			!= static_cast<std::uint64_t>(config::ResearchMapleDmaCheckpoint.get()))
		throw FlycastException("research identity/runtime Maple DMA checkpoint mismatch");
	if (expected.sh4PcCheckpoint
			!= static_cast<std::uint64_t>(config::ResearchSh4PcCheckpoint.get()))
		throw FlycastException("research identity/runtime SH-4 PC checkpoint mismatch");
	if (expected.sh4PcCheckpointU32Address
			!= static_cast<std::uint64_t>(
					config::ResearchSh4PcCheckpointU32Address.get())
			|| expected.sh4PcCheckpointU32Value
					!= static_cast<std::uint64_t>(
							config::ResearchSh4PcCheckpointU32Value.get()))
		throw FlycastException("research identity/runtime SH-4 PC checkpoint U32 gate mismatch");
	if (expected.pvrTaStartDma
			!= static_cast<std::uint64_t>(config::ResearchPvrTaStartDma.get()))
		throw FlycastException("research identity/runtime PowerVR TA start DMA mismatch");
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
	if (config::ResearchMapleDmaCheckpoint.get() < 0
			|| static_cast<std::uint64_t>(config::ResearchMapleDmaCheckpoint.get())
					> MaximumMapleDmaCheckpoint)
		throw FlycastException("research.MapleDmaCheckpoint is outside [0, 10000000]");
	const char *modeKey = recording ? "MapleRecord" : "MapleReplay";
	if (!config::isTransient("research", "IdentityManifest")
			|| !config::isTransient("research", modeKey))
		throw FlycastException("Maple research paths must be supplied as transient options");
	if (config::ResearchMapleDmaCheckpoint.get() != 0
			&& !config::isTransient("research", "MapleDmaCheckpoint"))
		throw FlycastException("research.MapleDmaCheckpoint must be transient");
	const IdentityManifest identity = loadIdentityManifest(researchPath(
			config::ResearchIdentityManifestPath.get()));
	authenticateFirmwareFiles(identity);
	applyDeterministicOverrides(&identity);
}

void startRuntime()
{
	if (configuredMode == Mode::None)
		return;
	if (session != nullptr)
		throw FlycastException("research runtime is already active");
	const std::filesystem::path identityPath = researchPath(config::ResearchIdentityManifestPath.get());
	const std::filesystem::path tracePath = configuredMode == Mode::Record
			? researchPath(config::ResearchMapleRecordPath.get())
			: researchPath(config::ResearchMapleReplayPath.get());
	if (pathsAlias(identityPath, tracePath))
		throw FlycastException("research identity and Maple trace paths alias");
	IdentityManifest identity = loadIdentityManifest(identityPath);
	authenticateFirmwareFiles(identity);
	applyDeterministicOverrides(&identity);
	authenticateLoadedDreamcastFirmware(identity, config::UseReios.get(),
			nvmem::getBiosData(), DreamcastBiosBytes);
	if (identity.firmware.initialFlash.available
			&& (identity.firmware.mode == FirmwareMode::Real
					|| identity.firmware.initialFlash.size != 0))
	{
		authenticateLoadedDreamcastFlash(identity, nvmem::getInitialFlashData(),
				nvmem::getInitialFlashSize());
		NOTICE_LOG(MAPLE,
				"Authenticated loaded Dreamcast initial flash SHA-256 %s",
				sha256ToHex(identity.firmware.initialFlash.digest).c_str());
	}
	if (identity.initialState.available)
		authenticateInitialStateFile(identity, researchPath(
				hostfs::getSavestatePath(static_cast<int>(identity.initialState.slot),
						false)));
	const std::uint64_t dmaCheckpoint = static_cast<std::uint64_t>(
			config::ResearchMapleDmaCheckpoint.get());
	if (identity.schemaVersion == 2)
	{
		if (identity.runtimeConfiguration.dynarecReplayDiagnostic)
			requireSh4DynarecReplayDiagnosticIdentityV2(identity);
		else if (identity.runtimeConfiguration.dynarecProfile)
			requireSh4DynarecProfileIdentityV2(identity);
		else
			requireSh4EquivalenceIdentityV2(identity);
	}
	verifyRuntimeConfiguration(identity);
	sh4DynarecTimingDiagnosticSetActive(
			identity.runtimeConfiguration.dynarecReplayDiagnostic);

	if (configuredMode == Mode::Record)
	{
		if (identity.schemaVersion == 1)
			requireCaptureV1Identity(identity);
		else
			requireMapleRecordIdentityV3(identity);
		auto writer = std::make_unique<MapleTraceWriter>(tracePath, identity.digest,
				static_cast<std::uint64_t>(config::ResearchMapleTraceMaxBytes.get()),
				MapleTraceCurrentSchemaVersion);
		session = std::make_unique<Session>(configuredMode, std::move(identity),
				std::move(writer), MapleTrace {}, dmaCheckpoint);
		NOTICE_LOG(MAPLE, "Recording typed Maple research trace to %s", tracePath.string().c_str());
	}
	else
	{
		const Sha256Digest& replayIdentity = identity.hasMapleReplayIdentityDigest
				? identity.mapleReplayIdentityDigest : identity.digest;
		MapleTrace replay = loadProductionMapleTrace(tracePath, replayIdentity,
				static_cast<std::uint64_t>(config::ResearchMapleTraceMaxBytes.get()));
		session = std::make_unique<Session>(configuredMode, std::move(identity), nullptr,
				std::move(replay), dmaCheckpoint);
		NOTICE_LOG(MAPLE, "Replaying typed Maple research trace from %s", tracePath.string().c_str());
	}
}

void stopRuntime(bool clean)
{
	if (session == nullptr)
	{
		configuredMode = Mode::None;
		sh4DynarecTimingDiagnosticSetActive(false);
		return;
	}
	std::unique_ptr<Session> finishing = std::move(session);
	configuredMode = Mode::None;
	if (!clean)
	{
		finishing->abandon();
		sh4DynarecTimingDiagnosticSetActive(false);
		return;
	}
	try
	{
		finishing->finish();
	}
	catch (...)
	{
		finishing->abandon();
		sh4DynarecTimingDiagnosticSetActive(false);
		throw;
	}
	sh4DynarecTimingDiagnosticSetActive(false);
}

void abortRuntime() noexcept
{
	configuredMode = Mode::None;
	sh4DynarecTimingDiagnosticSetActive(false);
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

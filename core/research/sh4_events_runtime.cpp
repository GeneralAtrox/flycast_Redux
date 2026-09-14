#include "research/sh4_events_runtime.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"
#include "oslib/oslib.h"
#include "research/identity_manifest.h"
#include "research/sh4_observation.h"
#include "research/sh4_observation_runtime.h"
#include "research/sh4_events_capture.h"
#include "research/sh4_events_manifest.h"
#include "types.h"

#include <atomic>
#include <cstring>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace research
{
namespace
{

bool configured = false;
std::unique_ptr<Sh4EventsCapture> session;
std::atomic<bool> sessionActive {false};
Sh4ObservationSubscription sessionSubscription = 0;
std::recursive_mutex sessionMutex;
thread_local std::uint32_t instructionLockDepth = 0;
bool deferredStopRequested = false;
bool deferredStopClean = false;
bool awaitingInitialStateLoad = false;
std::atomic<std::uint64_t> stopRequestGeneration {0};
std::atomic<std::uint64_t> dirtyStopGeneration {0};
std::uint64_t sessionStopRequestGeneration = 0;
std::uint64_t sessionDirtyStopGeneration = 0;
Sh4ObservationBackend sessionBackend = Sh4ObservationBackend::Interpreter;
std::function<void()> sessionCompletion;

void consumeSh4Observation(const Sh4Observation& observation);

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

Sh4ObservationBackend backendFor(const IdentityManifest& identity)
{
	return identity.runtimeConfiguration.cpuBackend == "dynarec"
			? Sh4ObservationBackend::Dynarec
			: Sh4ObservationBackend::Interpreter;
}

void applyDeterministicOverrides(const IdentityManifest& identity)
{
	const bool dynarec = backendFor(identity) == Sh4ObservationBackend::Dynarec;
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	config::ResearchDreamcastRtcSeed.override(
			identity.runtimeConfiguration.dreamcastRtcSeed);
	config::UseReios.override(identity.firmware.mode == FirmwareMode::Hle);
	config::ThreadedRendering.override(false);
	config::AutoLoadState.override(identity.runtimeConfiguration.autoLoadState);
	if (identity.initialState.available)
		config::SavestateSlot.override(static_cast<int>(identity.initialState.slot));
	config::AutoSaveState.override(false);
	config::GGPOEnable.override(false);
}

void applyPreResetOverrides(const IdentityManifest& identity)
{
	const bool dynarec = backendFor(identity) == Sh4ObservationBackend::Dynarec;
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	config::ThreadedRendering.override(false);
	config::AutoLoadState.override(false);
	config::AutoSaveState.override(false);
	config::GGPOEnable.override(false);
}

void verifyRuntimeConfiguration(const IdentityManifest& identity)
{
	const IdentityRuntimeConfiguration& expected = identity.runtimeConfiguration;
	const bool expectedDynarec = expected.cpuBackend == "dynarec";
	if ((expected.cpuBackend != "interpreter" && !expectedDynarec)
			|| config::DynarecEnabled.get() != expectedDynarec)
		throw FlycastException("SH-4 events identity/runtime CPU backend mismatch");
	if (expected.dynarecObservation
			!= config::ResearchDynarecObservation.get())
		throw FlycastException("SH-4 events identity/runtime dynarec observation mismatch");
	if (expected.dreamcastRtcSeed
			!= static_cast<std::uint32_t>(config::ResearchDreamcastRtcSeed.get()))
		throw FlycastException("SH-4 events identity/runtime Dreamcast RTC seed mismatch");
	if (expected.threadedRendering != config::ThreadedRendering.get())
		throw FlycastException("SH-4 events identity/runtime threaded-rendering mismatch");
	if (expected.autoLoadState != config::AutoLoadState.get())
		throw FlycastException("SH-4 events identity/runtime auto-load-state mismatch");
	if (expected.autoLoadState
			&& static_cast<std::uint32_t>(config::SavestateSlot.get())
					!= expected.savestateSlot)
		throw FlycastException("SH-4 events identity/runtime save-state slot mismatch");
	if (expected.autoSaveState != config::AutoSaveState.get())
		throw FlycastException("SH-4 events identity/runtime auto-save-state mismatch");
	if (expected.ggpo != config::GGPOEnable.get())
		throw FlycastException("SH-4 events identity/runtime GGPO mismatch");
	if (expected.mapleDmaCheckpoint
			!= static_cast<std::uint64_t>(config::ResearchMapleDmaCheckpoint.get()))
		throw FlycastException("SH-4 events identity/runtime Maple DMA checkpoint mismatch");
	if (expected.sh4PcCheckpoint
			!= static_cast<std::uint64_t>(config::ResearchSh4PcCheckpoint.get()))
		throw FlycastException("SH-4 events identity/runtime SH-4 PC checkpoint mismatch");
	if (expected.sh4PcCheckpointU32Address
			!= static_cast<std::uint64_t>(
					config::ResearchSh4PcCheckpointU32Address.get())
			|| expected.sh4PcCheckpointU32Value
					!= static_cast<std::uint64_t>(
							config::ResearchSh4PcCheckpointU32Value.get()))
		throw FlycastException("SH-4 events identity/runtime SH-4 PC checkpoint U32 gate mismatch");
}

void requireDistinctPaths(const std::vector<std::pair<const char *, std::filesystem::path>>& paths)
{
	for (std::size_t lhs = 0; lhs < paths.size(); ++lhs)
		for (std::size_t rhs = lhs + 1; rhs < paths.size(); ++rhs)
			if (pathsAlias(paths[lhs].second, paths[rhs].second))
				throw FlycastException(std::string("research paths alias: ") + paths[lhs].first
						+ " and " + paths[rhs].first);
}

Sh4GuestMemoryReader guestMemoryReader()
{
	return [](std::uint32_t address, std::uint32_t length) -> const std::uint8_t * {
		return GetMemPtr(address, length);
	};
}

struct DetachedSession
{
	std::unique_ptr<Sh4EventsCapture> capture;
	Sh4ObservationSubscription subscription = 0;
	bool clean = false;
};

DetachedSession detachSession(bool clean)
{
	DetachedSession detached;
	if (session == nullptr)
		return detached;
	detached.clean = clean
			&& dirtyStopGeneration.load(std::memory_order_acquire)
					== sessionDirtyStopGeneration;
	sessionActive.store(false, std::memory_order_release);
	awaitingInitialStateLoad = false;
	detached.subscription = sessionSubscription;
	sessionSubscription = 0;
	detached.capture = std::move(session);
	sessionCompletion = {};
	return detached;
}

void finishDetachedSession(DetachedSession detached)
{
	unsubscribeSh4Observations(detached.subscription);
	if (detached.capture == nullptr)
		return;
	if (!detached.clean)
	{
		detached.capture->abandon();
		return;
	}
	try
	{
		const Sh4EventsArtifactSummary summary = detached.capture->finish();
		NOTICE_LOG(SH4,
				"SH-4 events artifact complete: %llu calls, %llu returns, %llu watches, %llu exceptions",
				static_cast<unsigned long long>(summary.callCount),
				static_cast<unsigned long long>(summary.returnCount),
				static_cast<unsigned long long>(summary.watchReadCount
						+ summary.watchWriteCount),
				static_cast<unsigned long long>(summary.exceptionCount));
	}
	catch (...)
	{
		detached.capture->abandon();
		throw;
	}
}

void requestDirtyStop() noexcept
{
	dirtyStopGeneration.fetch_add(1, std::memory_order_acq_rel);
}

void requestStop(bool clean) noexcept
{
	stopRequestGeneration.fetch_add(1, std::memory_order_acq_rel);
	if (!clean)
		requestDirtyStop();
}

void abandonSessionForCaptureFailure() noexcept
{
	requestDirtyStop();
	configured = false;
	deferredStopRequested = false;
	deferredStopClean = false;
	DetachedSession detached = detachSession(false);
	while (instructionLockDepth != 0)
	{
		--instructionLockDepth;
		sessionMutex.unlock();
	}
	try
	{
		finishDetachedSession(std::move(detached));
	}
	catch (...) { }
}

void releaseInstructionLock() noexcept
{
	if (instructionLockDepth == 0)
		return;
	--instructionLockDepth;
	DetachedSession detached;
	const bool finalize = instructionLockDepth == 0 && deferredStopRequested;
	if (finalize)
	{
		const bool clean = deferredStopClean;
		deferredStopRequested = false;
		deferredStopClean = false;
		detached = detachSession(clean);
	}
	sessionMutex.unlock();
	if (!finalize)
		return;
	try
	{
		finishDetachedSession(std::move(detached));
	}
	catch (const std::exception& exception)
	{
		ERROR_LOG(SH4, "Deferred SH-4 events finalization failed: %s",
				exception.what());
	}
	catch (...)
	{
		ERROR_LOG(SH4, "Deferred SH-4 events finalization failed");
	}
}

} // namespace

void configureSh4EventsRuntime()
{
	abortSh4EventsRuntime();
	const std::lock_guard<std::recursive_mutex> lock(sessionMutex);
	const bool hasManifest = !config::ResearchSh4EventsManifestPath.get().empty();
	const bool hasOutput = !config::ResearchSh4EventsRecordPath.get().empty();
	if (!hasManifest && !hasOutput)
		return;
	if (!hasManifest || !hasOutput)
		throw FlycastException(
				"SH-4 events capture requires both research.Sh4EventsManifest and Sh4EventsRecord");
	if (config::ResearchIdentityManifestPath.get().empty())
		throw FlycastException("SH-4 events capture requires research.IdentityManifest");
	if (config::ResearchSh4EventsMaxBytes.get() < Sh4EventsArtifactHeaderSize)
		throw FlycastException("research.Sh4EventsMaxBytes is smaller than the artifact header");
	for (const char *key : {"IdentityManifest", "Sh4EventsManifest", "Sh4EventsRecord"})
		if (!config::isTransient("research", key))
			throw FlycastException("SH-4 events research paths must be supplied as transient options");
	const IdentityManifest identity = loadIdentityManifest(researchPath(
			config::ResearchIdentityManifestPath.get()));
#if FEAT_SHREC == DYNAREC_NONE
	if (identity.runtimeConfiguration.cpuBackend == "dynarec")
		throw FlycastException("SH-4 events identity requires an unavailable dynarec backend");
#endif
	applyPreResetOverrides(identity);
	configured = true;
}

void startSh4EventsRuntime(std::function<void()> completion)
{
	const std::lock_guard<std::recursive_mutex> lock(sessionMutex);
	if (!configured)
		return;
	if (session != nullptr)
		throw FlycastException("SH-4 events runtime is already active");
	const std::filesystem::path identityPath = researchPath(
			config::ResearchIdentityManifestPath.get());
	const std::filesystem::path manifestPath = researchPath(
			config::ResearchSh4EventsManifestPath.get());
	const std::filesystem::path outputPath = researchPath(
			config::ResearchSh4EventsRecordPath.get());
	std::vector<std::pair<const char *, std::filesystem::path>> paths {
		{"identity", identityPath},
		{"SH-4 events manifest", manifestPath},
		{"SH-4 events output", outputPath},
	};
	if (!config::ResearchMapleRecordPath.get().empty())
		paths.emplace_back("Maple record", researchPath(config::ResearchMapleRecordPath.get()));
	if (!config::ResearchMapleReplayPath.get().empty())
		paths.emplace_back("Maple replay", researchPath(config::ResearchMapleReplayPath.get()));
	if (!config::ResearchMemoryRangesManifestPath.get().empty())
		paths.emplace_back("memory-ranges manifest",
				researchPath(config::ResearchMemoryRangesManifestPath.get()));
	if (!config::ResearchMemoryRangesRecordPath.get().empty())
		paths.emplace_back("memory-ranges output",
				researchPath(config::ResearchMemoryRangesRecordPath.get()));
	requireDistinctPaths(paths);

	const IdentityManifest identity = loadIdentityManifest(identityPath);
	applyDeterministicOverrides(identity);
	if (identity.initialState.available)
		authenticateInitialStateFile(identity, researchPath(
				hostfs::getSavestatePath(static_cast<int>(identity.initialState.slot),
						false)));
	const Sh4EventsManifest manifest = loadSh4EventsManifest(manifestPath);
	verifyRuntimeConfiguration(identity);
	deferredStopRequested = false;
	deferredStopClean = false;
	sessionStopRequestGeneration = stopRequestGeneration.load(std::memory_order_acquire);
	sessionDirtyStopGeneration = dirtyStopGeneration.load(std::memory_order_acquire);
	session = std::make_unique<Sh4EventsCapture>(outputPath, identity, manifest,
			static_cast<std::uint64_t>(config::ResearchSh4EventsMaxBytes.get()));
	try
	{
		sessionCompletion = std::move(completion);
		sessionBackend = backendFor(identity);
		awaitingInitialStateLoad = manifest.startAfterInitialStateLoad;
		if (!awaitingInitialStateLoad)
		{
			Sh4ObservationFilter recorderFilter;
			recorderFilter.backendMask = sh4ObservationBackendBit(sessionBackend);
			sessionSubscription = subscribeSh4Observations(recorderFilter,
					consumeSh4Observation);
		}
	}
	catch (...)
	{
		session->abandon();
		session.reset();
		sessionCompletion = {};
		throw;
	}
	sessionActive.store(!awaitingInitialStateLoad, std::memory_order_release);
	NOTICE_LOG(SH4, "%s %s SH-4 events manifest %s (%zu hooks, %zu watch ranges) to %s",
			awaitingInitialStateLoad ? "Awaiting initial-state load for" : "Armed",
			sessionBackend == Sh4ObservationBackend::Dynarec ? "dynarec" : "interpreter",
			manifest.id.c_str(), manifest.hooks.size(), manifest.watchRanges.size(),
			outputPath.string().c_str());
}

void sh4EventsInitialStateLoaded()
{
	const std::lock_guard<std::recursive_mutex> lock(sessionMutex);
	if (!awaitingInitialStateLoad)
		return;
	if (session == nullptr || sessionSubscription != 0
			|| sessionActive.load(std::memory_order_acquire))
		throw FlycastException("SH-4 events initial-state activation is inconsistent");
	Sh4ObservationFilter recorderFilter;
	recorderFilter.backendMask = sh4ObservationBackendBit(sessionBackend);
	sessionSubscription = subscribeSh4Observations(recorderFilter,
			consumeSh4Observation);
	awaitingInitialStateLoad = false;
	sessionActive.store(true, std::memory_order_release);
	NOTICE_LOG(SH4, "Armed SH-4 events capture after authenticated initial-state load");
}

void stopSh4EventsRuntime(bool clean)
{
	requestStop(clean);
	DetachedSession detached;
	{
		const std::lock_guard<std::recursive_mutex> lock(sessionMutex);
		configured = false;
		if (session == nullptr)
			return;
		if (instructionLockDepth != 0)
		{
			if (deferredStopRequested)
				deferredStopClean = deferredStopClean && clean;
			else
			{
				deferredStopRequested = true;
				deferredStopClean = clean;
			}
			return;
		}
		detached = detachSession(clean);
	}
	finishDetachedSession(std::move(detached));
}

void abortSh4EventsRuntime() noexcept
{
	requestStop(false);
	try
	{
		DetachedSession detached;
		{
			std::unique_lock<std::recursive_mutex> lock(sessionMutex);
			configured = false;
			deferredStopRequested = false;
			deferredStopClean = false;
			awaitingInitialStateLoad = false;
			detached = detachSession(false);
			while (instructionLockDepth != 0)
			{
				--instructionLockDepth;
				sessionMutex.unlock();
			}
		}
		finishDetachedSession(std::move(detached));
	}
	catch (...) { }
}

void consumeInstructionBegin(const Sh4InstructionState& state)
{
	if (!sessionActive.load(std::memory_order_acquire))
		return;
	sessionMutex.lock();
	if (!sessionActive.load(std::memory_order_acquire) || session == nullptr)
	{
		sessionMutex.unlock();
		return;
	}
	++instructionLockDepth;
	try
	{
		session->beginInstruction(state, guestMemoryReader());
	}
	catch (...)
	{
		abandonSessionForCaptureFailure();
		throw;
	}
}

void consumeInstructionEnd(const Sh4InstructionState& state)
{
	if (instructionLockDepth == 0)
		return;
	bool completed = false;
	std::function<void()> completion;
	try
	{
		if (session != nullptr)
		{
			session->endInstruction(state, guestMemoryReader());
			completed = session->completionRequested();
			if (completed)
				completion = sessionCompletion;
		}
	}
	catch (...)
	{
		abandonSessionForCaptureFailure();
		throw;
	}
	releaseInstructionLock();
	if (completed)
	{
		stopSh4EventsRuntime(true);
		if (completion)
			completion();
	}
}

void consumeInstructionAbort() noexcept
{
	if (instructionLockDepth == 0)
		return;
	const bool stopPending = stopRequestGeneration.load(std::memory_order_acquire)
			!= sessionStopRequestGeneration;
	if (deferredStopRequested || stopPending)
	{
		requestDirtyStop();
		deferredStopClean = false;
	}
	if (session != nullptr)
		session->abortInstruction();
	releaseInstructionLock();
}

void consumeMemoryAccess(std::uint32_t address, std::uint8_t width,
		Sh4MemoryAccessKind kind, std::uint64_t value)
{
	if (!sessionActive.load(std::memory_order_acquire))
		return;
	const std::lock_guard<std::recursive_mutex> lock(sessionMutex);
	try
	{
		if (session != nullptr)
			session->observeMemoryAccess(address, width, kind, value);
	}
	catch (...)
	{
		abandonSessionForCaptureFailure();
		throw;
	}
}

void consumeException(std::uint32_t exceptionPc, std::uint32_t vectorPc,
		std::uint32_t exceptionCode, std::uint64_t tick,
		const Sh4RegisterSnapshot& registers)
{
	if (!sessionActive.load(std::memory_order_acquire))
		return;
	const std::lock_guard<std::recursive_mutex> lock(sessionMutex);
	try
	{
		if (session != nullptr)
			session->observeException(exceptionPc, vectorPc, exceptionCode, tick,
					registers);
	}
	catch (...)
	{
		abandonSessionForCaptureFailure();
		throw;
	}
}

namespace
{

Sh4InstructionState observationInstructionState(const Sh4Observation& observation)
{
	Sh4InstructionState state;
	state.pc = observation.instructionPc;
	state.nextPc = observation.nextPc;
	state.opcode = observation.opcode;
	state.tick = observation.tick;
	state.registers = observation.registers;
	return state;
}

void consumeSh4Observation(const Sh4Observation& observation)
{
	if (observation.backend != sessionBackend)
		throw std::logic_error("SH-4 events received an observation from the wrong backend");
	switch (observation.type)
	{
	case Sh4ObservationType::InstructionBegin:
		consumeInstructionBegin(observationInstructionState(observation));
		break;
	case Sh4ObservationType::InstructionEnd:
		consumeInstructionEnd(observationInstructionState(observation));
		break;
	case Sh4ObservationType::InstructionAbort:
		consumeInstructionAbort();
		break;
	case Sh4ObservationType::MemoryRead:
		consumeMemoryAccess(observation.memoryAddress, observation.memoryWidth,
				Sh4MemoryAccessKind::Read, observation.memoryValue);
		break;
	case Sh4ObservationType::MemoryWrite:
		consumeMemoryAccess(observation.memoryAddress, observation.memoryWidth,
				Sh4MemoryAccessKind::Write, observation.memoryValue);
		break;
	case Sh4ObservationType::Exception:
		consumeException(observation.exceptionPc, observation.vectorPc,
				observation.exceptionCode, observation.tick, observation.registers);
		break;
	case Sh4ObservationType::Call:
	case Sh4ObservationType::Return:
		// SH-4 events v1 derives its manifest-filtered semantic records from the
		// canonical instruction observations. Discovery subscribers receive the
		// unfiltered call/return observations as an additional view.
		break;
	}
}

} // namespace

void sh4EventsInstructionBegin(std::uint32_t pc, std::uint16_t opcode,
		std::uint64_t tick, const Sh4Context& context)
{
	sh4ObservationInstructionBegin(Sh4ObservationBackend::Interpreter, pc, opcode,
			tick, context);
}

void sh4EventsInstructionEnd(std::uint32_t pc, std::uint16_t opcode,
		std::uint64_t tick, const Sh4Context& context)
{
	sh4ObservationInstructionEnd(Sh4ObservationBackend::Interpreter, pc, opcode,
			tick, context);
}

void sh4EventsInstructionAbort() noexcept
{
	sh4ObservationInstructionAbort(Sh4ObservationBackend::Interpreter);
}

void sh4EventsMemoryAccess(std::uint32_t address, std::uint8_t width,
		Sh4MemoryAccessKind kind, std::uint64_t value)
{
	sh4ObservationMemoryAccess(sh4ObservationCurrentInstructionBackend(
			Sh4ObservationBackend::Interpreter), address, width, kind, value);
}

void sh4EventsException(std::uint32_t exceptionPc, std::uint32_t vectorPc,
		std::uint32_t exceptionCode, std::uint64_t tick, const Sh4Context& context)
{
	sh4ObservationException(Sh4ObservationBackend::Interpreter, exceptionPc,
			vectorPc, exceptionCode, tick, context);
}

bool sh4EventsRuntimeActive()
{
	return sessionActive.load(std::memory_order_acquire);
}

} // namespace research

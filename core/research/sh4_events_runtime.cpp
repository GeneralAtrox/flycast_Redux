#include "research/sh4_events_runtime.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"
#include "research/identity_manifest.h"
#include "research/sh4_events_capture.h"
#include "research/sh4_events_manifest.h"
#include "types.h"

#include <atomic>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
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
std::recursive_mutex sessionMutex;
thread_local std::uint32_t instructionLockDepth = 0;
bool deferredStopRequested = false;
bool deferredStopClean = false;
std::atomic<std::uint64_t> stopRequestGeneration {0};
std::atomic<std::uint64_t> dirtyStopGeneration {0};
std::uint64_t sessionStopRequestGeneration = 0;
std::uint64_t sessionDirtyStopGeneration = 0;

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
		throw FlycastException("SH-4 events identity/runtime CPU backend mismatch");
	if (expected.threadedRendering != config::ThreadedRendering.get())
		throw FlycastException("SH-4 events identity/runtime threaded-rendering mismatch");
	if (expected.autoLoadState != config::AutoLoadState.get())
		throw FlycastException("SH-4 events identity/runtime auto-load-state mismatch");
	if (expected.autoSaveState != config::AutoSaveState.get())
		throw FlycastException("SH-4 events identity/runtime auto-save-state mismatch");
	if (expected.ggpo != config::GGPOEnable.get())
		throw FlycastException("SH-4 events identity/runtime GGPO mismatch");
}

void requireDistinctPaths(const std::vector<std::pair<const char *, std::filesystem::path>>& paths)
{
	for (std::size_t lhs = 0; lhs < paths.size(); ++lhs)
		for (std::size_t rhs = lhs + 1; rhs < paths.size(); ++rhs)
			if (pathsAlias(paths[lhs].second, paths[rhs].second))
				throw FlycastException(std::string("research paths alias: ") + paths[lhs].first
						+ " and " + paths[rhs].first);
}

Sh4RegisterSnapshot snapshotRegisters(const Sh4Context& context)
{
	Sh4RegisterSnapshot snapshot;
	for (std::size_t index = 0; index < snapshot.r.size(); ++index)
		snapshot.r[index] = context.r[index];
	snapshot.pr = context.pr;
	snapshot.gbr = context.gbr;
	snapshot.vbr = context.vbr;
	snapshot.mach = context.mac.h;
	snapshot.macl = context.mac.l;
	snapshot.sr = context.sr.getFull();
	snapshot.fpul = context.fpul;
	snapshot.fpscr = context.fpscr.full;
	return snapshot;
}

Sh4GuestMemoryReader guestMemoryReader()
{
	return [](std::uint32_t address, std::uint32_t length) -> const std::uint8_t * {
		return GetMemPtr(address, length);
	};
}

void finishSession(bool clean)
{
	if (session == nullptr)
		return;
	clean = clean && dirtyStopGeneration.load(std::memory_order_acquire)
			== sessionDirtyStopGeneration;
	sessionActive.store(false, std::memory_order_release);
	std::unique_ptr<Sh4EventsCapture> finishing = std::move(session);
	if (!clean)
	{
		finishing->abandon();
		return;
	}
	try
	{
		const Sh4EventsArtifactSummary summary = finishing->finish();
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
		finishing->abandon();
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
	if (session != nullptr)
	{
		sessionActive.store(false, std::memory_order_release);
		session->abandon();
		session.reset();
	}
}

void releaseInstructionLock() noexcept
{
	if (instructionLockDepth == 0)
		return;
	--instructionLockDepth;
	if (instructionLockDepth == 0 && deferredStopRequested)
	{
		const bool clean = deferredStopClean;
		deferredStopRequested = false;
		deferredStopClean = false;
		try
		{
			finishSession(clean);
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
	sessionMutex.unlock();
}

} // namespace

void configureSh4EventsRuntime()
{
	const std::lock_guard<std::recursive_mutex> lock(sessionMutex);
	abortSh4EventsRuntime();
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
	applyDeterministicOverrides();
	configured = true;
}

void startSh4EventsRuntime()
{
	const std::lock_guard<std::recursive_mutex> lock(sessionMutex);
	if (!configured)
		return;
	if (session != nullptr)
		throw FlycastException("SH-4 events runtime is already active");
	applyDeterministicOverrides();

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
	const Sh4EventsManifest manifest = loadSh4EventsManifest(manifestPath);
	verifyRuntimeConfiguration(identity);
	deferredStopRequested = false;
	deferredStopClean = false;
	sessionStopRequestGeneration = stopRequestGeneration.load(std::memory_order_acquire);
	sessionDirtyStopGeneration = dirtyStopGeneration.load(std::memory_order_acquire);
	session = std::make_unique<Sh4EventsCapture>(outputPath, identity, manifest,
			static_cast<std::uint64_t>(config::ResearchSh4EventsMaxBytes.get()));
	sessionActive.store(true, std::memory_order_release);
	NOTICE_LOG(SH4, "Armed SH-4 events manifest %s (%zu hooks, %zu watch ranges) to %s",
			manifest.id.c_str(), manifest.hooks.size(), manifest.watchRanges.size(),
			outputPath.string().c_str());
}

void stopSh4EventsRuntime(bool clean)
{
	requestStop(clean);
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
	finishSession(clean);
}

void abortSh4EventsRuntime() noexcept
{
	requestStop(false);
	try
	{
		const std::lock_guard<std::recursive_mutex> lock(sessionMutex);
		configured = false;
		deferredStopRequested = false;
		deferredStopClean = false;
		if (session != nullptr)
		{
			sessionActive.store(false, std::memory_order_release);
			session->abandon();
			session.reset();
		}
	}
	catch (...) { }
}

void sh4EventsInstructionBegin(std::uint32_t pc, std::uint16_t opcode,
		std::uint64_t tick, const Sh4Context& context)
{
	if (!sessionActive.load(std::memory_order_acquire))
		return;
	sessionMutex.lock();
	if (session == nullptr)
	{
		sessionMutex.unlock();
		return;
	}
	++instructionLockDepth;
	Sh4InstructionState state;
	state.pc = pc;
	state.nextPc = context.pc;
	state.opcode = opcode;
	state.tick = tick;
	state.registers = snapshotRegisters(context);
	try
	{
		session->beginInstruction(state, guestMemoryReader());
	}
	catch (...)
	{
		abandonSessionForCaptureFailure();
		releaseInstructionLock();
		throw;
	}
}

void sh4EventsInstructionEnd(std::uint32_t pc, std::uint16_t opcode,
		std::uint64_t tick, const Sh4Context& context)
{
	if (instructionLockDepth == 0)
		return;
	try
	{
		if (session != nullptr)
		{
			Sh4InstructionState state;
			state.pc = pc;
			state.nextPc = context.pc;
			state.opcode = opcode;
			state.tick = tick;
			state.registers = snapshotRegisters(context);
			session->endInstruction(state, guestMemoryReader());
		}
	}
	catch (...)
	{
		abandonSessionForCaptureFailure();
		releaseInstructionLock();
		throw;
	}
	releaseInstructionLock();
}

void sh4EventsInstructionAbort() noexcept
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

void sh4EventsMemoryAccess(std::uint32_t address, std::uint8_t width,
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
		releaseInstructionLock();
		throw;
	}
}

void sh4EventsException(std::uint32_t exceptionPc, std::uint32_t vectorPc,
		std::uint32_t exceptionCode, std::uint64_t tick, const Sh4Context& context)
{
	if (!sessionActive.load(std::memory_order_acquire))
		return;
	const std::lock_guard<std::recursive_mutex> lock(sessionMutex);
	try
	{
		if (session != nullptr)
			session->observeException(exceptionPc, vectorPc, exceptionCode, tick,
					snapshotRegisters(context));
	}
	catch (...)
	{
		abandonSessionForCaptureFailure();
		releaseInstructionLock();
		throw;
	}
}

bool sh4EventsRuntimeActive()
{
	return sessionActive.load(std::memory_order_acquire);
}

} // namespace research

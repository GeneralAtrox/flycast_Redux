#include "research/sh4_pc_checkpoint_runtime.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "hw/sh4/sh4_mem.h"
#include "log/Log.h"
#include "research/identity_manifest.h"
#include "research/maple_runtime.h"
#include "research/sh4_observation_runtime.h"
#include "types.h"

#include <array>
#include <atomic>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace research
{
namespace
{

std::mutex callbackMutex;
std::function<void()> exitCallback;
std::atomic<std::uint32_t> targetPc {0};
std::atomic<std::uint32_t> targetU32Address {0};
std::atomic<std::uint32_t> targetU32Value {0};
std::atomic<int> targetBackend {-1};
std::atomic<bool> configured {false};
std::atomic<bool> triggered {false};

bool typedRecorderConfigured()
{
	const std::array<const std::string *, 11> outputs {
		&config::ResearchMapleRecordPath.get(),
		&config::ResearchMemoryRangesRecordPath.get(),
		&config::ResearchSh4EventsRecordPath.get(),
		&config::ResearchSh4ObservationRecordPath.get(),
		&config::ResearchSh4ProfileRecordPath.get(),
		&config::ResearchPvrTaRecordPath.get(),
		&config::ResearchPvrPresentationRecordPath.get(),
		&config::ResearchPvrDrawRecordPath.get(),
		&config::ResearchGdromRecordPath.get(),
		&config::ResearchAicaRecordPath.get(),
		&config::ResearchCddaRecordPath.get(),
	};
	for (const std::string *output : outputs)
		if (!output->empty())
			return true;
	return false;
}

Sh4ObservationBackend configuredBackend()
{
	return config::DynarecEnabled.get() ? Sh4ObservationBackend::Dynarec
			: Sh4ObservationBackend::Interpreter;
}

std::uint32_t identityBoundCheckpointPc()
{
	if (config::ResearchIdentityManifestPath.get().empty())
		throw FlycastException(
				"research.Sh4PcCheckpoint requires research.IdentityManifest");
	const IdentityManifest identity = loadIdentityManifest(std::filesystem::u8path(
			config::ResearchIdentityManifestPath.get()));
	const std::uint32_t configuredPc = static_cast<std::uint32_t>(
			config::ResearchSh4PcCheckpoint.get());
	if (identity.runtimeConfiguration.sh4PcCheckpoint != configuredPc)
		throw FlycastException(
				"research identity/runtime SH-4 PC checkpoint mismatch");
	const std::uint32_t configuredAddress = static_cast<std::uint32_t>(
			config::ResearchSh4PcCheckpointU32Address.get());
	const std::uint32_t configuredValue = static_cast<std::uint32_t>(
			config::ResearchSh4PcCheckpointU32Value.get());
	if (identity.runtimeConfiguration.sh4PcCheckpointU32Address
				!= configuredAddress
			|| identity.runtimeConfiguration.sh4PcCheckpointU32Value
					!= configuredValue)
		throw FlycastException(
				"research identity/runtime SH-4 PC checkpoint U32 gate mismatch");
	return configuredPc;
}

bool checkpointU32GateMatches() noexcept
{
	const std::uint32_t address = targetU32Address.load(std::memory_order_acquire);
	if (address == 0)
		return true;
	const u8 *bytes = GetMemPtr(address, 4);
	if (bytes == nullptr)
		return false;
	const std::uint32_t value = static_cast<std::uint32_t>(bytes[0])
			| (static_cast<std::uint32_t>(bytes[1]) << 8)
			| (static_cast<std::uint32_t>(bytes[2]) << 16)
			| (static_cast<std::uint32_t>(bytes[3]) << 24);
	return value == targetU32Value.load(std::memory_order_acquire);
}

void releaseOwnership() noexcept
{
	const int backend = targetBackend.exchange(-1, std::memory_order_acq_rel);
	if (backend >= 0)
		releaseSh4InstructionOwnership(static_cast<Sh4ObservationBackend>(backend));
}

} // namespace

void configureSh4PcCheckpointRuntime()
{
	stopSh4PcCheckpointRuntime();
	const std::int64_t configuredPc = config::ResearchSh4PcCheckpoint.get();
	if (configuredPc == 0)
		return;
	if (configuredPc < 0 || configuredPc > std::numeric_limits<std::uint32_t>::max()
			|| (configuredPc & 1) != 0)
		throw FlycastException(
				"research.Sh4PcCheckpoint must be an aligned non-zero 32-bit guest PC");
	if (!config::isTransient("research", "Sh4PcCheckpoint"))
		throw FlycastException("research.Sh4PcCheckpoint must be transient");
	const std::int64_t configuredAddress =
			config::ResearchSh4PcCheckpointU32Address.get();
	const std::int64_t configuredValue =
			config::ResearchSh4PcCheckpointU32Value.get();
	if (configuredAddress == 0)
	{
		if (configuredValue != 0)
			throw FlycastException(
					"research SH-4 PC checkpoint U32 value requires an address");
	}
	else
	{
		if (configuredAddress < 0x8c000000ll
				|| configuredAddress > 0x8cfffffcll
				|| (configuredAddress & 3) != 0
				|| configuredValue < 0
				|| configuredValue > std::numeric_limits<std::uint32_t>::max())
			throw FlycastException(
					"research SH-4 PC checkpoint U32 gate is outside aligned Dreamcast system RAM/U32 bounds");
		if (!config::isTransient("research", "Sh4PcCheckpointU32Address")
				|| !config::isTransient("research", "Sh4PcCheckpointU32Value"))
			throw FlycastException(
					"research SH-4 PC checkpoint U32 gate must be transient");
	}
	if (!typedRecorderConfigured())
		throw FlycastException(
				"research.Sh4PcCheckpoint requires a configured typed recorder output");
	if (config::ThreadedRendering.get())
		throw FlycastException(
				"research.Sh4PcCheckpoint requires non-threaded rendering");
	if (config::DynarecEnabled.get() && !config::ResearchDynarecObservation.get())
		throw FlycastException(
				"research.Sh4PcCheckpoint with dynarec requires existing typed per-instruction observation");
	const std::uint32_t boundPc = identityBoundCheckpointPc();
	targetU32Address.store(static_cast<std::uint32_t>(configuredAddress),
			std::memory_order_release);
	targetU32Value.store(static_cast<std::uint32_t>(configuredValue),
			std::memory_order_release);
	targetPc.store(boundPc,
			std::memory_order_release);
	triggered.store(false, std::memory_order_release);
	configured.store(true, std::memory_order_release);
}

void startSh4PcCheckpointRuntime(std::function<void()> cleanExitCallback)
{
	if (!configured.load(std::memory_order_acquire))
		return;
	if (!cleanExitCallback)
		throw FlycastException("SH-4 PC checkpoint requires a clean-exit callback");
	if (identityBoundCheckpointPc() != targetPc.load(std::memory_order_acquire))
		throw FlycastException("SH-4 PC checkpoint binding changed before start");
	const Sh4ObservationBackend backend = configuredBackend();
	{
		const std::lock_guard<std::mutex> lock(callbackMutex);
		exitCallback = std::move(cleanExitCallback);
	}
	targetBackend.store(static_cast<int>(backend), std::memory_order_release);
	retainSh4InstructionOwnership(backend);
	NOTICE_LOG(SH4, "Armed read-only SH-4 PC checkpoint at %08x (%s)",
			targetPc.load(std::memory_order_acquire),
			backend == Sh4ObservationBackend::Dynarec ? "dynarec" : "interpreter");
}

void stopSh4PcCheckpointRuntime() noexcept
{
	targetPc.store(0, std::memory_order_release);
	targetU32Address.store(0, std::memory_order_release);
	targetU32Value.store(0, std::memory_order_release);
	configured.store(false, std::memory_order_release);
	releaseOwnership();
	const std::lock_guard<std::mutex> lock(callbackMutex);
	exitCallback = {};
}

void sh4PcCheckpointInstructionEnd(Sh4ObservationBackend backend,
		std::uint32_t pc) noexcept
{
	if (targetBackend.load(std::memory_order_acquire) != static_cast<int>(backend))
		return;
	// A deferred SH-4 observation capture changes to its authenticated precise
	// timing model only at Sh4ObservationStartDma.  Do not let a common PC hit
	// during boot terminate that capture before its native bus is active.  PC
	// terminals backed only by another typed recorder retain immediate matching.
	if (!config::ResearchSh4ObservationRecordPath.get().empty()
			&& !sh4ObservationBusActive(backend))
		return;
	// A replay-backed typed capture is publishable only after every event in the
	// authenticated input stream has been consumed. The requested PC may occur
	// while the final asynchronous DMA is still open, so defer that hit rather
	// than turning an otherwise clean PC stop into an incomplete replay.
	if (mapleReplaying() && !mapleReplayConsumed())
		return;
	if (!checkpointU32GateMatches())
		return;
	std::uint32_t expected = pc;
	if (expected == 0 || !targetPc.compare_exchange_strong(expected, 0,
			std::memory_order_acq_rel, std::memory_order_acquire))
		return;
	triggered.store(true, std::memory_order_release);
	NOTICE_LOG(SH4, "Reached read-only SH-4 PC checkpoint at %08x", pc);
	try
	{
		std::function<void()> callback;
		{
			const std::lock_guard<std::mutex> lock(callbackMutex);
			callback = exitCallback;
		}
		if (callback)
			callback();
	}
	catch (const std::exception& exception)
	{
		ERROR_LOG(SH4, "SH-4 PC checkpoint clean-exit request failed: %s",
				exception.what());
	}
	catch (...)
	{
		ERROR_LOG(SH4, "SH-4 PC checkpoint clean-exit request failed");
	}
}

bool sh4PcCheckpointRuntimeActive() noexcept
{
	return targetPc.load(std::memory_order_acquire) != 0;
}

bool sh4PcCheckpointRuntimeTriggered() noexcept
{
	return triggered.load(std::memory_order_acquire);
}

std::uint32_t sh4PcCheckpointTarget() noexcept
{
	return targetPc.load(std::memory_order_acquire);
}

} // namespace research

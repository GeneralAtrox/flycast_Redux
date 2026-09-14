#include "research/sh4_pc_checkpoint_runtime.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "hw/sh4/sh4_mem.h"
#include "log/Log.h"
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

Sh4ObservationBackend configuredBackend()
{
	return config::DynarecEnabled.get() ? Sh4ObservationBackend::Dynarec
			: Sh4ObservationBackend::Interpreter;
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

namespace
{

void validateCheckpoint(std::int64_t pc, std::int64_t gateAddress, std::int64_t gateValue)
{
	if (pc <= 0 || pc > std::numeric_limits<std::uint32_t>::max() || (pc & 1) != 0)
		throw FlycastException(
				"research.Sh4PcCheckpoint must be an aligned non-zero 32-bit guest PC");
	if (gateAddress == 0)
	{
		if (gateValue != 0)
			throw FlycastException(
					"research SH-4 PC checkpoint U32 value requires an address");
	}
	else if (gateAddress < 0x8c000000ll || gateAddress > 0x8cfffffcll
			|| (gateAddress & 3) != 0 || gateValue < 0
			|| gateValue > std::numeric_limits<std::uint32_t>::max())
	{
		throw FlycastException(
				"research SH-4 PC checkpoint U32 gate is outside aligned Dreamcast system RAM/U32 bounds");
	}
	if (config::ThreadedRendering.get())
		throw FlycastException(
				"research.Sh4PcCheckpoint requires non-threaded rendering");
	if (config::DynarecEnabled.get() && !config::ResearchDynarecObservation.get())
		throw FlycastException(
				"research.Sh4PcCheckpoint with dynarec requires research.DynarecObservation");
}

void armTargets(std::uint32_t pc, std::uint32_t gateAddress, std::uint32_t gateValue)
{
	targetU32Address.store(gateAddress, std::memory_order_release);
	targetU32Value.store(gateValue, std::memory_order_release);
	targetPc.store(pc, std::memory_order_release);
	triggered.store(false, std::memory_order_release);
	configured.store(true, std::memory_order_release);
}

} // namespace

void configureSh4PcCheckpointRuntime()
{
	stopSh4PcCheckpointRuntime();
	const std::int64_t configuredPc = config::ResearchSh4PcCheckpoint.get();
	if (configuredPc == 0)
		return;
	const std::int64_t gateAddress = config::ResearchSh4PcCheckpointU32Address.get();
	const std::int64_t gateValue = config::ResearchSh4PcCheckpointU32Value.get();
	validateCheckpoint(configuredPc, gateAddress, gateValue);
	armTargets(static_cast<std::uint32_t>(configuredPc),
			static_cast<std::uint32_t>(gateAddress), static_cast<std::uint32_t>(gateValue));
}

void armSh4PcCheckpointRuntime(std::uint32_t pc, std::uint32_t gateAddress,
		std::uint32_t gateValue, std::function<void()> callback)
{
	if (!callback)
		throw FlycastException("SH-4 PC checkpoint requires a callback");
	validateCheckpoint(pc, gateAddress, gateValue);
	stopSh4PcCheckpointRuntime();
	armTargets(pc, gateAddress, gateValue);
	startSh4PcCheckpointRuntime(std::move(callback));
}

void startSh4PcCheckpointRuntime(std::function<void()> cleanExitCallback)
{
	if (!configured.load(std::memory_order_acquire))
		return;
	if (!cleanExitCallback)
		throw FlycastException("SH-4 PC checkpoint requires a clean-exit callback");
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
	// A replay finishes cleanly only after every recorded event has been
	// consumed. The requested PC may occur while the final asynchronous DMA is
	// still open, so defer that hit rather than cutting the replay short.
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

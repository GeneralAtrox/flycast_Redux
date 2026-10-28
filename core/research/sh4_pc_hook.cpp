#include "research/sh4_pc_hook.h"

#include "log/Log.h"

#include <array>
#include <atomic>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace research
{
namespace
{

std::array<u32, SH4_PC_HOOK_MAX> hookPcs {};
std::atomic<unsigned> hookCount {0};
std::mutex callbackMutex;
std::function<void(u32, u32, u32)> hookCallback;

} // namespace

bool sh4PcHookAt(u32 pc) noexcept
{
	const unsigned count = hookCount.load(std::memory_order_acquire);
	for (unsigned i = 0; i < count; i++)
		if (hookPcs[i] == pc)
			return true;
	return false;
}

void sh4PcHookSet(const u32 *pcs, unsigned count, std::function<void(u32, u32, u32)> callback)
{
	if (count > SH4_PC_HOOK_MAX)
		throw std::invalid_argument("too many hooked PCs");
	hookCount.store(0, std::memory_order_release);
	{
		std::lock_guard<std::mutex> lock(callbackMutex);
		hookCallback = std::move(callback);
	}
	for (unsigned i = 0; i < count; i++)
		hookPcs[i] = pcs[i];
	hookCount.store(count, std::memory_order_release);
}

void sh4PcHookClear() noexcept
{
	hookCount.store(0, std::memory_order_release);
	std::lock_guard<std::mutex> lock(callbackMutex);
	hookCallback = nullptr;
}

void sh4PcHookRun(u32 pc, u32 r4, u32 r5) noexcept
{
	try
	{
		std::function<void(u32, u32, u32)> callback;
		{
			std::lock_guard<std::mutex> lock(callbackMutex);
			callback = hookCallback;
		}
		if (callback)
			callback(pc, r4, r5);
	}
	catch (const std::exception& exception)
	{
		WARN_LOG(COMMON, "SH-4 PC hook at %08x failed: %s", pc, exception.what());
	}
	catch (...)
	{
		WARN_LOG(COMMON, "SH-4 PC hook at %08x failed", pc);
	}
}

} // namespace research

#include "research/memory_ranges_runtime.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "hw/sh4/sh4_mem.h"
#include "research/identity_manifest.h"
#include "research/memory_ranges_artifact.h"
#include "research/memory_ranges_capture.h"
#include "research/memory_ranges_manifest.h"
#include "types.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace research
{
namespace
{

bool configured = false;
std::unique_ptr<MemoryRangesCapture> session;

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
		throw FlycastException("memory-ranges identity/runtime CPU backend mismatch");
	if (expected.threadedRendering != config::ThreadedRendering.get())
		throw FlycastException("memory-ranges identity/runtime threaded-rendering mismatch");
	if (expected.autoLoadState != config::AutoLoadState.get())
		throw FlycastException("memory-ranges identity/runtime auto-load-state mismatch");
	if (expected.autoSaveState != config::AutoSaveState.get())
		throw FlycastException("memory-ranges identity/runtime auto-save-state mismatch");
	if (expected.ggpo != config::GGPOEnable.get())
		throw FlycastException("memory-ranges identity/runtime GGPO mismatch");
}

void requireDistinctPaths(const std::vector<std::pair<const char *, std::filesystem::path>>& paths)
{
	for (std::size_t lhs = 0; lhs < paths.size(); ++lhs)
		for (std::size_t rhs = lhs + 1; rhs < paths.size(); ++rhs)
			if (pathsAlias(paths[lhs].second, paths[rhs].second))
				throw FlycastException(std::string("research paths alias: ") + paths[lhs].first
						+ " and " + paths[rhs].first);
}

} // namespace

void configureMemoryRangesRuntime()
{
	abortMemoryRangesRuntime();
	const bool hasManifest = !config::ResearchMemoryRangesManifestPath.get().empty();
	const bool hasOutput = !config::ResearchMemoryRangesRecordPath.get().empty();
	if (!hasManifest && !hasOutput)
		return;
	if (!hasManifest || !hasOutput)
		throw FlycastException(
				"memory-ranges capture requires both research.MemoryRangesManifest and MemoryRangesRecord");
	if (config::ResearchIdentityManifestPath.get().empty())
		throw FlycastException("memory-ranges capture requires research.IdentityManifest");
	if (config::ResearchMemoryRangesMaxBytes.get() < MemoryRangesArtifactHeaderSize)
		throw FlycastException("research.MemoryRangesMaxBytes is smaller than the artifact header");
	for (const char *key : {"IdentityManifest", "MemoryRangesManifest", "MemoryRangesRecord"})
		if (!config::isTransient("research", key))
			throw FlycastException("memory-ranges research paths must be supplied as transient options");
	applyDeterministicOverrides();
	configured = true;
}

void startMemoryRangesRuntime()
{
	if (!configured)
		return;
	if (session != nullptr)
		throw FlycastException("memory-ranges runtime is already active");
	applyDeterministicOverrides();

	const std::filesystem::path identityPath = researchPath(
			config::ResearchIdentityManifestPath.get());
	const std::filesystem::path manifestPath = researchPath(
			config::ResearchMemoryRangesManifestPath.get());
	const std::filesystem::path outputPath = researchPath(
			config::ResearchMemoryRangesRecordPath.get());
	std::vector<std::pair<const char *, std::filesystem::path>> paths {
		{"identity", identityPath},
		{"memory-ranges manifest", manifestPath},
		{"memory-ranges output", outputPath},
	};
	if (!config::ResearchMapleRecordPath.get().empty())
		paths.emplace_back("Maple record", researchPath(config::ResearchMapleRecordPath.get()));
	if (!config::ResearchMapleReplayPath.get().empty())
		paths.emplace_back("Maple replay", researchPath(config::ResearchMapleReplayPath.get()));
	requireDistinctPaths(paths);

	const IdentityManifest identity = loadIdentityManifest(identityPath);
	const MemoryRangesManifest manifest = loadMemoryRangesManifest(manifestPath);
	verifyRuntimeConfiguration(identity);
	session = std::make_unique<MemoryRangesCapture>(outputPath, identity, manifest,
			static_cast<std::uint64_t>(config::ResearchMemoryRangesMaxBytes.get()));
	NOTICE_LOG(SH4, "Armed memory-ranges capture manifest %s (%zu ranges) to %s",
			manifest.id.c_str(), manifest.ranges.size(), outputPath.string().c_str());
}

void stopMemoryRangesRuntime(bool clean)
{
	if (session == nullptr)
	{
		configured = false;
		return;
	}
	std::unique_ptr<MemoryRangesCapture> finishing = std::move(session);
	configured = false;
	if (!clean)
	{
		finishing->abandon();
		return;
	}
	try
	{
		const MemoryRangesArtifactSummary summary = finishing->finish();
		NOTICE_LOG(SH4,
				"Memory-ranges artifact complete: %llu ranges, %llu bytes, tick %llu, PC %08x",
				static_cast<unsigned long long>(summary.rangeEventCount),
				static_cast<unsigned long long>(summary.totalMemoryBytes),
				static_cast<unsigned long long>(summary.startTick), summary.triggerPc);
	}
	catch (...)
	{
		finishing->abandon();
		throw;
	}
}

void abortMemoryRangesRuntime() noexcept
{
	configured = false;
	if (session != nullptr)
	{
		session->abandon();
		session.reset();
	}
}

void memoryRangesInstructionBoundary(std::uint32_t executedPc, std::uint64_t tick)
{
	if (session == nullptr)
		return;
	const bool captured = session->observeInstruction(executedPc, tick,
			[](std::uint32_t address, std::uint32_t length) -> const std::uint8_t * {
				return GetMemPtr(address, length);
			});
	if (captured)
		NOTICE_LOG(SH4, "Captured memory ranges at interpreter PC %08x, tick %llu",
				executedPc, static_cast<unsigned long long>(tick));
}

bool memoryRangesRuntimeActive()
{
	return session != nullptr;
}

} // namespace research

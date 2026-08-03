#include "research/sh4_profile_capture_runtime.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "hw/sh4/sh4_if.h"
#include "log/Log.h"
#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/sh4_profile.h"
#include "research/sh4_profile_artifact.h"
#include "types.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace research
{
namespace
{
std::filesystem::path pathFor(const std::string& value)
{
#ifdef __cpp_char8_t
	std::u8string utf8(value.size(), u8'\0');
	std::memcpy(utf8.data(), value.data(), value.size());
	return std::filesystem::path(utf8);
#else
	return std::filesystem::u8path(value);
#endif
}

std::filesystem::path runningExecutable()
{
#ifdef _WIN32
	std::vector<wchar_t> buffer(1024);
	while (buffer.size() <= 32768)
	{
		const DWORD count = GetModuleFileNameW(nullptr, buffer.data(),
				static_cast<DWORD>(buffer.size()));
		if (count == 0)
			throw FlycastException("cannot resolve the running Flycast executable");
		if (count < buffer.size())
			return std::filesystem::path(std::wstring(buffer.data(), count));
		buffer.resize(buffer.size() * 2);
	}
	throw FlycastException("running Flycast executable path is too long");
#elif defined(__APPLE__)
	std::uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::vector<char> buffer(size);
	if (_NSGetExecutablePath(buffer.data(), &size) != 0)
		throw FlycastException("cannot resolve the running Flycast executable");
	return std::filesystem::weakly_canonical(buffer.data());
#elif defined(__linux__)
	return std::filesystem::read_symlink("/proc/self/exe");
#elif defined(__FreeBSD__)
	return std::filesystem::read_symlink("/proc/curproc/file");
#else
	throw FlycastException("running executable authentication is unsupported");
#endif
}

void authenticateExecutable(const IdentityManifest& identity)
{
	if (identity.emulatorExecutable.size == 0)
		throw FlycastException("SH-4 profile identity has no executable authority");
	const auto executable = runningExecutable();
	std::error_code error;
	if (std::filesystem::file_size(executable, error)
				!= identity.emulatorExecutable.size || error
			|| !sha256Equal(hashFileExact(executable,
					identity.emulatorExecutable.size),
					identity.emulatorExecutable.digest))
		throw FlycastException(
				"running Flycast executable differs from SH-4 profile identity");
}

struct Configuration
{
	IdentityManifest identity;
	std::filesystem::path identityPath;
	std::filesystem::path maplePath;
	std::filesystem::path outputPath;
	bool mapleRecord = false;
	Sha256Digest replayDigest {};
	std::uint64_t maximumReplayBytes = 0;
	std::uint64_t maximumArtifactBytes = 0;
	std::size_t maximumBlocks = 0;
	std::size_t maximumBranches = 0;
	std::uint64_t maximumExecutions = 0;
};

struct Session
{
	Configuration configuration;
	std::shared_ptr<Sh4DynarecProfileCollector> collector;
};

std::unique_ptr<Configuration> configured;
std::unique_ptr<Session> session;

void applyRuntimeConfiguration(const IdentityManifest& identity)
{
	config::DynarecEnabled.override(true);
	config::ResearchDynarecObservation.override(false);
	config::ResearchDreamcastRtcSeed.override(
			identity.runtimeConfiguration.dreamcastRtcSeed);
	config::ThreadedRendering.override(false);
	config::AutoLoadState.override(identity.runtimeConfiguration.autoLoadState);
	if (identity.initialState.available)
		config::SavestateSlot.override(static_cast<int>(identity.initialState.slot));
	config::AutoSaveState.override(false);
	config::GGPOEnable.override(false);
}

void reauthenticate(const Configuration& configuration)
{
	authenticateExecutable(configuration.identity);
	if (!sha256Equal(hashFileExact(configuration.identityPath,
				MaxIdentityManifestBytes), configuration.identity.digest))
		throw FlycastException("SH-4 profile immutable input changed during capture");
	if (!configuration.mapleRecord
			&& !sha256Equal(hashFileExact(configuration.maplePath,
					configuration.maximumReplayBytes), configuration.replayDigest))
		throw FlycastException("SH-4 profile immutable Maple replay changed during capture");
}
}

void configureSh4DynarecProfileCaptureRuntime()
{
	abortSh4DynarecProfileCaptureRuntime();
	if (config::ResearchSh4ProfileRecordPath.get().empty())
		return;
#if HOST_CPU != CPU_X64 || FEAT_SHREC == DYNAREC_NONE
	throw FlycastException("SH-4 production dynarec profiling is supported only by the x64 JIT");
#endif
	const bool mapleRecord = !config::ResearchMapleRecordPath.get().empty();
	const bool mapleReplay = !config::ResearchMapleReplayPath.get().empty();
	if (config::ResearchIdentityManifestPath.get().empty()
			|| mapleRecord == mapleReplay)
		throw FlycastException(
				"SH-4 profile capture requires exactly one Maple record or replay path");
	if (!config::ResearchSh4ObservationRecordPath.get().empty()
			|| !config::ResearchMemoryRangesRecordPath.get().empty()
			|| !config::ResearchSh4EventsRecordPath.get().empty()
			|| !config::ResearchPvrTaRecordPath.get().empty()
			|| !config::ResearchPvrPresentationRecordPath.get().empty()
			|| !config::ResearchPvrDrawRecordPath.get().empty()
			|| !config::ResearchGdromRecordPath.get().empty()
			|| !config::ResearchAicaRecordPath.get().empty())
		throw FlycastException(
				"SH-4 production dynarec profiling cannot be combined with another typed recorder");
	for (const char *key : {"IdentityManifest",
			mapleRecord ? "MapleRecord" : "MapleReplay", "Sh4ProfileRecord"})
		if (!config::isTransient("research", key))
			throw FlycastException("SH-4 profile capture paths must be transient options");
	if (config::ResearchSh4ProfileMaxBytes.get()
			< static_cast<std::int64_t>(Sh4DynarecProfileArtifactHeaderSize)
			|| config::ResearchSh4ProfileMaxBlocks.get() <= 0
			|| config::ResearchSh4ProfileMaxBranches.get() <= 0
			|| config::ResearchSh4ProfileMaxExecutions.get() <= 0)
		throw FlycastException("SH-4 profile capture bounds are invalid");
	auto next = std::make_unique<Configuration>();
	next->identityPath = pathFor(config::ResearchIdentityManifestPath.get());
	next->maplePath = pathFor(mapleRecord ? config::ResearchMapleRecordPath.get()
			: config::ResearchMapleReplayPath.get());
	next->mapleRecord = mapleRecord;
	next->outputPath = pathFor(config::ResearchSh4ProfileRecordPath.get());
	if (pathsAlias(next->identityPath, next->maplePath)
			|| pathsAlias(next->identityPath, next->outputPath)
			|| pathsAlias(next->maplePath, next->outputPath))
		throw FlycastException("SH-4 profile capture paths alias");
	next->identity = loadIdentityManifest(next->identityPath);
	if (mapleRecord)
		requireSh4DynarecProfileRecordIdentityV3(next->identity);
	else
		requireSh4DynarecProfileIdentityV2(next->identity);
	authenticateExecutable(next->identity);
	next->maximumReplayBytes = static_cast<std::uint64_t>(
			config::ResearchMapleTraceMaxBytes.get());
	if (!mapleRecord)
	{
		const MapleTraceSummary replay = validateProductionMapleTraceFile(
				next->maplePath, next->identity.mapleReplayIdentityDigest,
				next->maximumReplayBytes);
		if (next->identity.runtimeConfiguration.mapleDmaCheckpoint != 0
				&& replay.dmaCount
						!= next->identity.runtimeConfiguration.mapleDmaCheckpoint)
			throw FlycastException("SH-4 profile replay DMA count differs from identity");
		next->replayDigest = hashFileExact(next->maplePath,
				next->maximumReplayBytes);
	}
	next->maximumArtifactBytes = static_cast<std::uint64_t>(
			config::ResearchSh4ProfileMaxBytes.get());
	next->maximumBlocks = static_cast<std::size_t>(
			config::ResearchSh4ProfileMaxBlocks.get());
	next->maximumBranches = static_cast<std::size_t>(
			config::ResearchSh4ProfileMaxBranches.get());
	next->maximumExecutions = static_cast<std::uint64_t>(
			config::ResearchSh4ProfileMaxExecutions.get());
	applyRuntimeConfiguration(next->identity);
	configured = std::move(next);
}

void startSh4DynarecProfileCaptureRuntime()
{
	if (configured == nullptr)
		return;
	if (session != nullptr)
		throw FlycastException("SH-4 profile capture is already active");
	applyRuntimeConfiguration(configured->identity);
	reauthenticate(*configured);
	auto next = std::make_unique<Session>();
	next->configuration = *configured;
	next->collector = std::make_shared<Sh4DynarecProfileCollector>(
			configured->maximumBlocks, configured->maximumBranches,
			configured->maximumExecutions);
	activateSh4DynarecProfileCollector(next->collector);
	// A block compiled before the collector was armed has no generation hook.
	// Resetting here guarantees every subsequently executable block is profiled.
	emu.getSh4Executor()->ResetCache();
	session = std::move(next);
	NOTICE_LOG(SH4, "Armed production dynarec profile capture to %s",
			configured->outputPath.string().c_str());
}

void stopSh4DynarecProfileCaptureRuntime(bool clean)
{
	configured.reset();
	if (session == nullptr)
		return;
	auto finishing = std::move(session);
	deactivateSh4DynarecProfileCollector();
	if (!clean)
		return;
	reauthenticate(finishing->configuration);
	if (finishing->configuration.mapleRecord)
	{
		const MapleTraceSummary replay = validateProductionMapleTraceFile(
				finishing->configuration.maplePath,
				finishing->configuration.identity.digest,
				finishing->configuration.maximumReplayBytes);
		if (replay.dmaCount
				!= finishing->configuration.identity.runtimeConfiguration.mapleDmaCheckpoint)
			throw FlycastException("SH-4 profile recorded Maple DMA count differs from identity");
		finishing->configuration.replayDigest = hashFileExact(
				finishing->configuration.maplePath,
				finishing->configuration.maximumReplayBytes);
	}
	const Sh4DynarecProfileSnapshot profile = finishing->collector->snapshot();
	if (!profile.complete)
		throw FlycastException(profile.failure);
	Sh4DynarecProfileArtifactBinding binding;
	binding.identityDigest = finishing->configuration.identity.digest;
	binding.replayDigest = finishing->configuration.replayDigest;
	binding.configurationDigest =
			finishing->configuration.identity.configurationDigest;
	const auto summary = writeSh4DynarecProfileArtifact(
			finishing->configuration.outputPath, binding, profile,
			finishing->configuration.maximumArtifactBytes);
	NOTICE_LOG(SH4, "Finalized SH-4 dynarec profile (%llu blocks, %llu branches)",
			static_cast<unsigned long long>(summary.blockCount),
			static_cast<unsigned long long>(summary.branchCount));
}

void abortSh4DynarecProfileCaptureRuntime() noexcept
{
	configured.reset();
	deactivateSh4DynarecProfileCollector();
	session.reset();
}

bool sh4DynarecProfileCaptureRuntimeActive()
{
	return session != nullptr;
}

} // namespace research

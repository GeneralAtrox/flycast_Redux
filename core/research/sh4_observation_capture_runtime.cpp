#include "research/sh4_observation_capture_runtime.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "log/Log.h"
#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/maple_runtime.h"
#include "research/sh4_observation.h"
#include "research/sh4_observation_runtime.h"
#include "research/sh4_observation_trace.h"
#include "types.h"

#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <utility>

namespace research
{
namespace
{

constexpr std::uint64_t MaximumManifestSetBytes = 16ull * 1024 * 1024;

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

void applyEquivalenceOverrides(const IdentityManifest& identity)
{
	const bool dynarec = identity.runtimeConfiguration.cpuBackend == "dynarec";
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	config::ResearchDreamcastRtcSeed.override(
			identity.runtimeConfiguration.dreamcastRtcSeed);
	config::ThreadedRendering.override(false);
	config::AutoLoadState.override(identity.runtimeConfiguration.autoLoadState);
	if (identity.initialState.available)
		config::SavestateSlot.override(static_cast<int>(identity.initialState.slot));
	config::AutoSaveState.override(false);
	config::GGPOEnable.override(false);
}

void verifyRuntimeConfiguration(const IdentityManifest& identity)
{
	const bool dynarec = identity.runtimeConfiguration.cpuBackend == "dynarec";
	if (config::DynarecEnabled.get() != dynarec
			|| config::ResearchDynarecObservation.get() != dynarec
			|| config::ResearchDreamcastRtcSeed.get()
					!= identity.runtimeConfiguration.dreamcastRtcSeed
			|| static_cast<std::uint64_t>(config::ResearchSh4ObservationStartDma.get())
					!= identity.runtimeConfiguration.sh4ObservationStartDma
			|| config::ThreadedRendering.get()
			|| config::AutoLoadState.get()
					!= identity.runtimeConfiguration.autoLoadState
			|| (identity.initialState.available
					&& static_cast<std::uint32_t>(config::SavestateSlot.get())
							!= identity.initialState.slot)
			|| config::AutoSaveState.get()
			|| config::GGPOEnable.get())
		throw FlycastException("SH-4 observation identity/runtime configuration mismatch");
}

struct Configuration
{
	IdentityManifest identity;
	std::filesystem::path identityPath;
	std::filesystem::path replayPath;
	std::filesystem::path manifestSetPath;
	std::filesystem::path outputPath;
	Sha256Digest replayDigest {};
	Sha256Digest manifestSetDigest {};
	std::uint64_t maximumReplayBytes = 0;
	std::uint64_t maximumBytes = 0;
	std::uint64_t startDma = 0;
};

struct Session
{
	Session(std::unique_ptr<Sh4ObservationTraceWriter> writer,
			Configuration configuration)
		: writer(std::move(writer)), configuration(std::move(configuration))
	{
	}

	std::unique_ptr<Sh4ObservationTraceWriter> writer;
	Configuration configuration;
	Sh4ObservationSubscription subscription = 0;
	std::exception_ptr failure;
};

std::unique_ptr<Configuration> configured;
std::unique_ptr<Session> session;

void subscribeSession(Session& active)
{
	if (active.subscription != 0)
		throw FlycastException("SH-4 observation capture subscription is already active");
	const Sh4ObservationBackend backend = backendFor(active.configuration.identity);
	Sh4ObservationFilter filter;
	filter.backendMask = sh4ObservationBackendBit(backend);
	Session *raw = &active;
	raw->subscription = subscribeSh4Observations(filter,
			[raw](const Sh4Observation& observation) {
				if (raw->failure != nullptr)
					return;
				try
				{
					raw->writer->write(observation);
				}
				catch (const std::exception& exception)
				{
					raw->failure = std::current_exception();
					raw->writer->abandon();
					ERROR_LOG(SH4, "SH-4 observation writer failed: %s",
							exception.what());
				}
				catch (...)
				{
					raw->failure = std::current_exception();
					raw->writer->abandon();
					ERROR_LOG(SH4, "SH-4 observation writer failed");
				}
			});
	sh4ObservationSetPreciseTiming(backend, true);
}

void observationDmaBegin(std::uint64_t oneBasedDmaCount)
{
	if (session == nullptr || session->configuration.startDma == 0)
		return;
	if (oneBasedDmaCount < session->configuration.startDma)
		return;
	if (oneBasedDmaCount != session->configuration.startDma)
		throw FlycastException("SH-4 observation start DMA was skipped");
	subscribeSession(*session);
	setMapleDmaBeginHandler(nullptr);
	NOTICE_LOG(SH4, "Started SH-4 observation at Maple DMA %llu",
			static_cast<unsigned long long>(oneBasedDmaCount));
}

void requireDistinct(const std::filesystem::path& lhs,
		const std::filesystem::path& rhs, const char *message)
{
	if (pathsAlias(lhs, rhs))
		throw FlycastException(message);
}

void reauthenticate(const Configuration& configuration)
{
	const Sha256Digest identityDigest = hashFileExact(configuration.identityPath,
			MaxIdentityManifestBytes);
	const Sha256Digest replayDigest = hashFileExact(configuration.replayPath,
			configuration.maximumReplayBytes);
	const Sha256Digest manifestSetDigest = hashFileExact(
			configuration.manifestSetPath, MaximumManifestSetBytes);
	if (!sha256Equal(identityDigest, configuration.identity.digest)
			|| !sha256Equal(replayDigest, configuration.replayDigest)
			|| !sha256Equal(manifestSetDigest, configuration.manifestSetDigest))
		throw FlycastException("SH-4 observation immutable input changed during capture");
}

} // namespace

void configureSh4ObservationCaptureRuntime()
{
	abortSh4ObservationCaptureRuntime();
	const bool hasOutput = !config::ResearchSh4ObservationRecordPath.get().empty();
	const bool hasManifestSet =
			!config::ResearchSh4ObservationManifestSetPath.get().empty();
	if (!hasOutput && !hasManifestSet)
		return;
	if (!hasOutput || !hasManifestSet)
		throw FlycastException("SH-4 observation capture requires record and manifest-set paths");
	if (config::ResearchIdentityManifestPath.get().empty()
			|| config::ResearchMapleReplayPath.get().empty())
		throw FlycastException("SH-4 observation capture requires identity and Maple replay paths");
	if (!config::ResearchMapleRecordPath.get().empty())
		throw FlycastException("SH-4 observation capture requires replay, not Maple recording");
	if (config::ResearchSh4ObservationMaxBytes.get()
			< static_cast<std::int64_t>(Sh4ObservationTraceHeaderSize))
		throw FlycastException("research.Sh4ObservationMaxBytes is smaller than the trace header");
	if (config::ResearchMapleTraceMaxBytes.get() <= 0)
		throw FlycastException("research.MapleTraceMaxBytes must be positive");
	if (config::ResearchMapleTraceMaxBytes.get()
			< static_cast<std::int64_t>(MapleTraceHeaderSize))
		throw FlycastException("research.MapleTraceMaxBytes is smaller than the trace header");
	if (config::ResearchSh4ObservationStartDma.get() < 0
			|| static_cast<std::uint64_t>(config::ResearchSh4ObservationStartDma.get())
					> MaximumMapleDmaCheckpoint)
		throw FlycastException("research.Sh4ObservationStartDma is outside [0, 10000000]");
	for (const char *key : {"IdentityManifest", "MapleReplay",
			"Sh4ObservationRecord", "Sh4ObservationManifestSet",
			"DreamcastRtcSeed"})
		if (!config::isTransient("research", key))
			throw FlycastException("SH-4 observation paths must be supplied as transient options");
	if (config::ResearchSh4ObservationStartDma.get() != 0
			&& !config::isTransient("research", "Sh4ObservationStartDma"))
		throw FlycastException("research.Sh4ObservationStartDma must be transient");

	auto next = std::make_unique<Configuration>();
	next->identityPath = researchPath(config::ResearchIdentityManifestPath.get());
	next->replayPath = researchPath(config::ResearchMapleReplayPath.get());
	next->manifestSetPath = researchPath(
			config::ResearchSh4ObservationManifestSetPath.get());
	next->outputPath = researchPath(config::ResearchSh4ObservationRecordPath.get());
	requireDistinct(next->identityPath, next->replayPath,
			"SH-4 observation identity and replay paths alias");
	requireDistinct(next->identityPath, next->manifestSetPath,
			"SH-4 observation identity and manifest-set paths alias");
	requireDistinct(next->identityPath, next->outputPath,
			"SH-4 observation identity and output paths alias");
	requireDistinct(next->replayPath, next->manifestSetPath,
			"SH-4 observation replay and manifest-set paths alias");
	requireDistinct(next->replayPath, next->outputPath,
			"SH-4 observation replay and output paths alias");
	requireDistinct(next->manifestSetPath, next->outputPath,
			"SH-4 observation manifest-set and output paths alias");

	next->identity = loadIdentityManifest(next->identityPath);
	requireSh4EquivalenceIdentityV2(next->identity);
#if FEAT_SHREC == DYNAREC_NONE
	if (next->identity.runtimeConfiguration.cpuBackend == "dynarec")
		throw FlycastException("SH-4 equivalence identity requires an unavailable dynarec backend");
#endif
	next->maximumReplayBytes = static_cast<std::uint64_t>(
			config::ResearchMapleTraceMaxBytes.get());
	next->replayDigest = hashFileExact(next->replayPath,
			next->maximumReplayBytes);
	next->manifestSetDigest = hashFileExact(next->manifestSetPath,
			MaximumManifestSetBytes);
	next->maximumBytes = static_cast<std::uint64_t>(
			config::ResearchSh4ObservationMaxBytes.get());
	next->startDma = static_cast<std::uint64_t>(
			config::ResearchSh4ObservationStartDma.get());
	if (next->identity.runtimeConfiguration.sh4ObservationStartDma
			!= next->startDma)
		throw FlycastException("SH-4 observation identity/runtime start DMA mismatch");
	applyEquivalenceOverrides(next->identity);
	configured = std::move(next);
}

void startSh4ObservationCaptureRuntime()
{
	if (configured == nullptr)
		return;
	if (session != nullptr)
		throw FlycastException("SH-4 observation capture is already active");
	applyEquivalenceOverrides(configured->identity);
	verifyRuntimeConfiguration(configured->identity);
	reauthenticate(*configured);
	sh4ObservationResetPreciseTiming();
	sh4DynarecExecutionTimingReset();

	Sh4ObservationTraceBinding binding;
	binding.backend = backendFor(configured->identity);
	binding.identityDigest = configured->identity.digest;
	binding.replayDigest = configured->replayDigest;
	binding.manifestSetDigest = configured->manifestSetDigest;
	auto writer = std::make_unique<Sh4ObservationTraceWriter>(
			configured->outputPath, binding, configured->maximumBytes);
	auto active = std::make_unique<Session>(std::move(writer), *configured);
	session = std::move(active);
	if (configured->startDma == 0)
		subscribeSession(*session);
	else
		setMapleDmaBeginHandler(observationDmaBegin);
	NOTICE_LOG(SH4, "Recording %s SH-4 observation trace to %s",
			binding.backend == Sh4ObservationBackend::Dynarec ? "dynarec" : "interpreter",
			configured->outputPath.string().c_str());
}

void stopSh4ObservationCaptureRuntime(bool clean)
{
	if (session == nullptr)
	{
		configured.reset();
		sh4ObservationResetPreciseTiming();
		sh4DynarecExecutionTimingReset();
		return;
	}
	std::unique_ptr<Session> finishing = std::move(session);
	setMapleDmaBeginHandler(nullptr);
	sh4ObservationResetPreciseTiming();
	if (finishing->subscription != 0)
		unsubscribeSh4Observations(finishing->subscription);
	configured.reset();
	sh4DynarecExecutionTimingReset();
	if (!clean)
	{
		finishing->writer->abandon();
		return;
	}
	if (finishing->failure != nullptr)
	{
		finishing->writer->abandon();
		std::rethrow_exception(finishing->failure);
	}
	try
	{
		reauthenticate(finishing->configuration);
		const Sh4ObservationTraceSummary summary = finishing->writer->finalize();
		NOTICE_LOG(SH4, "Finalized SH-4 observation trace (%llu events)",
				static_cast<unsigned long long>(summary.eventCount));
	}
	catch (...)
	{
		finishing->writer->abandon();
		throw;
	}
}

void abortSh4ObservationCaptureRuntime() noexcept
{
	setMapleDmaBeginHandler(nullptr);
	configured.reset();
	sh4ObservationResetPreciseTiming();
	sh4DynarecExecutionTimingReset();
	if (session == nullptr)
		return;
	std::unique_ptr<Session> abandoning = std::move(session);
	if (abandoning->subscription != 0)
		unsubscribeSh4Observations(abandoning->subscription);
	abandoning->writer->abandon();
}

bool sh4ObservationCaptureRuntimeActive()
{
	return session != nullptr;
}

} // namespace research

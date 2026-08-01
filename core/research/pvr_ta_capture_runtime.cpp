#include "research/pvr_ta_capture_runtime.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "log/Log.h"
#include "research/identity_manifest.h"
#include "research/maple_runtime.h"
#include "research/maple_trace.h"
#include "research/pvr_ta_artifact.h"
#include "research/pvr_ta_manifest.h"
#include "research/pvr_ta_observation.h"
#include "research/pvr_presentation_artifact.h"
#include "research/pvr_presentation_observation.h"
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

void applyOverrides(const IdentityManifest& identity)
{
	const bool dynarec = identity.runtimeConfiguration.cpuBackend == "dynarec";
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	config::ResearchDreamcastRtcSeed.override(
			identity.runtimeConfiguration.dreamcastRtcSeed);
	config::ThreadedRendering.override(false);
	config::AutoLoadState.override(false);
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
			|| static_cast<std::uint64_t>(config::ResearchPvrTaStartDma.get())
					!= identity.runtimeConfiguration.pvrTaStartDma
			|| config::ThreadedRendering.get()
			|| config::AutoLoadState.get()
			|| config::AutoSaveState.get()
			|| config::GGPOEnable.get())
		throw FlycastException("PowerVR TA identity/runtime configuration mismatch");
}

void requireDistinct(const std::filesystem::path& lhs,
		const std::filesystem::path& rhs, const char *message)
{
	if (pathsAlias(lhs, rhs))
		throw FlycastException(message);
}

struct Configuration
{
	IdentityManifest identity;
	PvrTaManifest manifest;
	std::filesystem::path identityPath;
	std::filesystem::path replayPath;
	std::filesystem::path manifestPath;
	std::filesystem::path outputPath;
	std::filesystem::path presentationOutputPath;
	Sha256Digest replayDigest {};
	Sha256Digest manifestDigest {};
	std::uint64_t maximumReplayBytes = 0;
	std::uint64_t maximumBytes = 0;
	std::uint64_t presentationMaximumBytes = 0;
	std::uint64_t startDma = 0;
};

struct Session
{
	Session(std::unique_ptr<PvrTaArtifactWriter> writer,
			std::unique_ptr<PvrPresentationArtifactWriter> presentationWriter,
			Configuration configuration)
		: writer(std::move(writer)),
		  presentationWriter(std::move(presentationWriter)),
		  configuration(std::move(configuration)) {}

	std::unique_ptr<PvrTaArtifactWriter> writer;
	std::unique_ptr<PvrPresentationArtifactWriter> presentationWriter;
	Configuration configuration;
	PvrTaObservationSubscription subscription = 0;
	PvrPresentationObservationSubscription presentationSubscription = 0;
	std::uint64_t droppedBaseline = 0;
	std::uint64_t presentationDroppedBaseline = 0;
	std::exception_ptr failure;
};

std::unique_ptr<Configuration> configured;
std::unique_ptr<Session> session;

void reauthenticate(const Configuration& configuration)
{
	const Sha256Digest identityDigest = hashFileExact(configuration.identityPath,
			MaxIdentityManifestBytes);
	const Sha256Digest replayDigest = hashFileExact(configuration.replayPath,
			configuration.maximumReplayBytes);
	const Sha256Digest manifestDigest = hashFileExact(configuration.manifestPath,
			MaxPvrTaManifestBytes);
	if (!sha256Equal(identityDigest, configuration.identity.digest)
			|| !sha256Equal(replayDigest, configuration.replayDigest)
			|| !sha256Equal(manifestDigest, configuration.manifestDigest))
		throw FlycastException("PowerVR TA immutable input changed during capture");
}

void subscribeSession(Session& active)
{
	if (active.subscription != 0)
		throw FlycastException("PowerVR TA evidence subscription is already active");
	active.droppedBaseline = pvrTaObservationDroppedCount();
	active.presentationDroppedBaseline = pvrPresentationObservationDroppedCount();
	Session *raw = &active;
	active.subscription = subscribePvrTaEvidenceObservations(
			[raw](const PvrTaObservation& observation) {
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
					ERROR_LOG(PVR, "PowerVR TA writer failed: %s", exception.what());
				}
				catch (...)
				{
					raw->failure = std::current_exception();
					raw->writer->abandon();
					ERROR_LOG(PVR, "PowerVR TA writer failed");
				}
			});
	if (active.presentationWriter != nullptr)
	{
		active.presentationSubscription = subscribePvrPresentationEvidenceObservations(
				[raw](const PvrPresentationObservation& observation) {
					if (raw->failure != nullptr)
						return;
					try
					{
						raw->presentationWriter->write(observation);
					}
					catch (...)
					{
						raw->failure = std::current_exception();
						raw->presentationWriter->abandon();
						ERROR_LOG(PVR, "PowerVR presentation writer failed");
					}
				});
	}
}

void observationDmaBegin(std::uint64_t oneBasedDmaCount)
{
	if (session == nullptr || session->configuration.startDma == 0)
		return;
	if (oneBasedDmaCount < session->configuration.startDma)
		return;
	if (oneBasedDmaCount != session->configuration.startDma)
		throw FlycastException("PowerVR TA observation start DMA was skipped");
	subscribeSession(*session);
	setMapleDmaBeginHandler(nullptr);
	NOTICE_LOG(PVR, "Started PowerVR TA observation at Maple DMA %llu",
			static_cast<unsigned long long>(oneBasedDmaCount));
}

} // namespace

void configurePvrTaCaptureRuntime()
{
	abortPvrTaCaptureRuntime();
	const bool hasOutput = !config::ResearchPvrTaRecordPath.get().empty();
	const bool hasManifest = !config::ResearchPvrTaManifestPath.get().empty();
	const bool hasPresentation =
			!config::ResearchPvrPresentationRecordPath.get().empty();
	if (hasPresentation && (!hasOutput || !hasManifest))
		throw FlycastException(
				"PowerVR presentation capture requires the TA record and manifest");
	if (!hasOutput && !hasManifest)
		return;
	if (!hasOutput || !hasManifest)
		throw FlycastException(
				"PowerVR TA capture requires both record and manifest paths");
	if (!config::ResearchSh4ObservationRecordPath.get().empty())
		throw FlycastException(
				"PowerVR TA and SH-4 observation recorders cannot share the Maple boundary handler");
	if (config::ResearchIdentityManifestPath.get().empty()
			|| config::ResearchMapleReplayPath.get().empty())
		throw FlycastException(
				"PowerVR TA capture requires identity and Maple replay paths");
	if (!config::ResearchMapleRecordPath.get().empty())
		throw FlycastException("PowerVR TA capture requires replay, not Maple recording");
	if (config::ResearchPvrTaMaxBytes.get()
			< static_cast<std::int64_t>(PvrTaArtifactHeaderSize))
		throw FlycastException("research.PvrTaMaxBytes is smaller than the artifact header");
	if (hasPresentation && config::ResearchPvrPresentationMaxBytes.get()
			< static_cast<std::int64_t>(PvrPresentationArtifactHeaderSize))
		throw FlycastException(
				"research.PvrPresentationMaxBytes is smaller than the artifact header");
	if (config::ResearchMapleTraceMaxBytes.get()
			< static_cast<std::int64_t>(MapleTraceHeaderSize))
		throw FlycastException("research.MapleTraceMaxBytes is invalid");
	if (config::ResearchPvrTaStartDma.get() < 0
			|| static_cast<std::uint64_t>(config::ResearchPvrTaStartDma.get())
					> MaximumMapleDmaCheckpoint)
		throw FlycastException("research.PvrTaStartDma is outside [0, 10000000]");
	for (const char *key : {"IdentityManifest", "MapleReplay", "PvrTaRecord",
			"PvrTaManifest", "DreamcastRtcSeed"})
		if (!config::isTransient("research", key))
			throw FlycastException("PowerVR TA paths must be supplied as transient options");
	if (config::ResearchPvrTaStartDma.get() != 0
			&& !config::isTransient("research", "PvrTaStartDma"))
		throw FlycastException("research.PvrTaStartDma must be transient");

	auto next = std::make_unique<Configuration>();
	next->identityPath = researchPath(config::ResearchIdentityManifestPath.get());
	next->replayPath = researchPath(config::ResearchMapleReplayPath.get());
	next->manifestPath = researchPath(config::ResearchPvrTaManifestPath.get());
	next->outputPath = researchPath(config::ResearchPvrTaRecordPath.get());
	if (hasPresentation)
	{
		if (!config::isTransient("research", "PvrPresentationRecord"))
			throw FlycastException(
					"PowerVR presentation path must be supplied as a transient option");
		next->presentationOutputPath = researchPath(
				config::ResearchPvrPresentationRecordPath.get());
	}
	requireDistinct(next->identityPath, next->replayPath,
			"PowerVR TA identity and replay paths alias");
	requireDistinct(next->identityPath, next->manifestPath,
			"PowerVR TA identity and manifest paths alias");
	requireDistinct(next->identityPath, next->outputPath,
			"PowerVR TA identity and output paths alias");
	requireDistinct(next->replayPath, next->manifestPath,
			"PowerVR TA replay and manifest paths alias");
	requireDistinct(next->replayPath, next->outputPath,
			"PowerVR TA replay and output paths alias");
	requireDistinct(next->manifestPath, next->outputPath,
			"PowerVR TA manifest and output paths alias");
	if (hasPresentation)
	{
		requireDistinct(next->presentationOutputPath, next->identityPath,
				"PowerVR presentation and identity paths alias");
		requireDistinct(next->presentationOutputPath, next->replayPath,
				"PowerVR presentation and replay paths alias");
		requireDistinct(next->presentationOutputPath, next->manifestPath,
				"PowerVR presentation and manifest paths alias");
		requireDistinct(next->presentationOutputPath, next->outputPath,
				"PowerVR presentation and TA output paths alias");
	}

	next->identity = loadIdentityManifest(next->identityPath);
	requireSh4EquivalenceIdentityV2(next->identity);
#if FEAT_SHREC == DYNAREC_NONE
	if (next->identity.runtimeConfiguration.cpuBackend == "dynarec")
		throw FlycastException("PowerVR TA identity requires an unavailable dynarec backend");
#endif
	next->maximumReplayBytes = static_cast<std::uint64_t>(
			config::ResearchMapleTraceMaxBytes.get());
	validateProductionMapleTraceFile(next->replayPath,
			next->identity.mapleReplayIdentityDigest, next->maximumReplayBytes);
	next->replayDigest = hashFileExact(next->replayPath, next->maximumReplayBytes);
	next->manifest = loadPvrTaManifest(next->manifestPath);
	requirePvrTaManifestIdentity(next->manifest, next->identity);
	next->manifestDigest = next->manifest.digest;
	next->maximumBytes = static_cast<std::uint64_t>(
			config::ResearchPvrTaMaxBytes.get());
	next->presentationMaximumBytes = static_cast<std::uint64_t>(
			config::ResearchPvrPresentationMaxBytes.get());
	if (next->maximumBytes != next->manifest.maximumBytes)
		throw FlycastException("PowerVR TA runtime byte limit differs from manifest");
	next->startDma = static_cast<std::uint64_t>(
			config::ResearchPvrTaStartDma.get());
	if (next->identity.runtimeConfiguration.pvrTaStartDma != next->startDma)
		throw FlycastException("PowerVR TA identity/runtime start DMA mismatch");
	if (next->identity.runtimeConfiguration.mapleDmaCheckpoint != 0
			&& next->startDma > next->identity.runtimeConfiguration.mapleDmaCheckpoint)
		throw FlycastException("PowerVR TA start DMA follows the terminal checkpoint");
	applyOverrides(next->identity);
	configured = std::move(next);
}

void startPvrTaCaptureRuntime()
{
	if (configured == nullptr)
		return;
	if (session != nullptr)
		throw FlycastException("PowerVR TA capture is already active");
	applyOverrides(configured->identity);
	verifyRuntimeConfiguration(configured->identity);
	reauthenticate(*configured);

	PvrTaArtifactBinding binding;
	binding.backend = backendFor(configured->identity);
	binding.identityDigest = configured->identity.digest;
	binding.replayDigest = configured->replayDigest;
	binding.manifestDigest = configured->manifestDigest;
	auto writer = std::make_unique<PvrTaArtifactWriter>(configured->outputPath,
			binding, configured->maximumBytes, configured->manifest.maximumEvents);
	std::unique_ptr<PvrPresentationArtifactWriter> presentationWriter;
	if (!configured->presentationOutputPath.empty())
	{
		PvrPresentationArtifactBinding presentationBinding;
		presentationBinding.backend = binding.backend;
		presentationBinding.identityDigest = binding.identityDigest;
		presentationBinding.replayDigest = binding.replayDigest;
		presentationWriter = std::make_unique<PvrPresentationArtifactWriter>(
				configured->presentationOutputPath, presentationBinding,
				configured->presentationMaximumBytes);
	}
	session = std::make_unique<Session>(std::move(writer),
			std::move(presentationWriter), *configured);
	if (configured->startDma == 0)
		subscribeSession(*session);
	else
		setMapleDmaBeginHandler(observationDmaBegin);
	NOTICE_LOG(PVR, "Recording typed PowerVR TA artifact to %s",
			configured->outputPath.string().c_str());
}

void stopPvrTaCaptureRuntime(bool clean)
{
	if (session == nullptr)
	{
		configured.reset();
		return;
	}
	std::unique_ptr<Session> finishing = std::move(session);
	setMapleDmaBeginHandler(nullptr);
	if (finishing->subscription != 0)
		unsubscribePvrTaObservations(finishing->subscription);
	if (finishing->presentationSubscription != 0)
		unsubscribePvrPresentationObservations(
				finishing->presentationSubscription);
	configured.reset();
	if (!clean)
	{
		finishing->writer->abandon();
		if (finishing->presentationWriter != nullptr)
			finishing->presentationWriter->abandon();
		return;
	}
	if (finishing->failure != nullptr)
	{
		finishing->writer->abandon();
		if (finishing->presentationWriter != nullptr)
			finishing->presentationWriter->abandon();
		std::rethrow_exception(finishing->failure);
	}
	if (pvrTaObservationDroppedCount() != finishing->droppedBaseline)
	{
		finishing->writer->abandon();
		if (finishing->presentationWriter != nullptr)
			finishing->presentationWriter->abandon();
		throw FlycastException("PowerVR TA observations were dropped during capture");
	}
	if (pvrPresentationObservationDroppedCount()
			!= finishing->presentationDroppedBaseline)
	{
		finishing->writer->abandon();
		if (finishing->presentationWriter != nullptr)
			finishing->presentationWriter->abandon();
		throw FlycastException(
				"PowerVR presentation observations were dropped during capture");
	}
	try
	{
		reauthenticate(finishing->configuration);
		const PvrTaArtifactSummary observed = finishing->writer->getSummary();
		if (observed.typeCounts[4]
				!= finishing->configuration.manifest.renderDoneCount)
			throw FlycastException(
					"PowerVR TA render-done count differs from manifest");
		const PvrTaArtifactSummary summary = finishing->writer->finalize();
		if (finishing->presentationWriter != nullptr)
		{
			const auto presentationSummary =
					finishing->presentationWriter->finalize();
			NOTICE_LOG(PVR, "Finalized PowerVR presentation artifact (%llu events)",
					static_cast<unsigned long long>(presentationSummary.eventCount));
		}
		NOTICE_LOG(PVR, "Finalized PowerVR TA artifact (%llu events)",
				static_cast<unsigned long long>(summary.eventCount));
	}
	catch (...)
	{
		finishing->writer->abandon();
		if (finishing->presentationWriter != nullptr)
			finishing->presentationWriter->abandon();
		throw;
	}
}

void abortPvrTaCaptureRuntime() noexcept
{
	setMapleDmaBeginHandler(nullptr);
	configured.reset();
	if (session == nullptr)
		return;
	std::unique_ptr<Session> abandoning = std::move(session);
	if (abandoning->subscription != 0)
		unsubscribePvrTaObservations(abandoning->subscription);
	if (abandoning->presentationSubscription != 0)
		unsubscribePvrPresentationObservations(
				abandoning->presentationSubscription);
	abandoning->writer->abandon();
	if (abandoning->presentationWriter != nullptr)
		abandoning->presentationWriter->abandon();
}

bool pvrTaCaptureRuntimeActive()
{
	return session != nullptr;
}

} // namespace research

#include "research/pvr_ta_capture_runtime.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/sh4/sh4_sched.h"
#include "log/Log.h"
#include "oslib/oslib.h"
#include "research/identity_manifest.h"
#include "research/maple_runtime.h"
#include "research/maple_trace.h"
#include "research/pvr_ta_artifact.h"
#include "research/pvr_ta_manifest.h"
#include "research/pvr_ta_observation.h"
#include "research/pvr_presentation_artifact.h"
#include "research/pvr_presentation_observation.h"
#include "research/pvr_draw_artifact.h"
#include "research/pvr_draw_observation.h"
#include "types.h"

#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
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
	config::AutoLoadState.override(identity.runtimeConfiguration.autoLoadState);
	if (identity.initialState.available)
		config::SavestateSlot.override(static_cast<int>(identity.initialState.slot));
	config::AutoSaveState.override(false);
	config::GGPOEnable.override(false);
	const PvrDrawConfiguration& draw =
			identity.runtimeConfiguration.pvrDrawConfiguration;
	if (draw.available)
	{
		config::RendererType.override(RenderType::DirectX11);
		config::PerStripSorting.override(draw.perStripSorting);
		config::TranslucentPolygonDepthMask.override(
				draw.translucentPolygonDepthMask);
		config::ModifierVolumes.override(draw.modifierVolumes);
		config::RenderResolution.override(static_cast<int>(draw.renderResolution));
		config::EmulateFramebuffer.override(draw.emulateFramebuffer);
		config::FixUpscaleBleedingEdge.override(draw.fixUpscaleBleedingEdge);
	}
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
					!= identity.runtimeConfiguration.autoLoadState
			|| (identity.initialState.available
					&& static_cast<std::uint32_t>(config::SavestateSlot.get())
							!= identity.initialState.slot)
			|| config::AutoSaveState.get()
			|| config::GGPOEnable.get())
		throw FlycastException("PowerVR TA identity/runtime configuration mismatch");
	const PvrDrawConfiguration& draw =
			identity.runtimeConfiguration.pvrDrawConfiguration;
	if (draw.available
			&& (config::RendererType.get() != RenderType::DirectX11
					|| config::PerStripSorting.get() != draw.perStripSorting
					|| config::TranslucentPolygonDepthMask.get()
							!= draw.translucentPolygonDepthMask
					|| config::ModifierVolumes.get() != draw.modifierVolumes
					|| config::RenderResolution.get()
							!= static_cast<int>(draw.renderResolution)
					|| config::EmulateFramebuffer.get() != draw.emulateFramebuffer
					|| config::FixUpscaleBleedingEdge.get()
							!= draw.fixUpscaleBleedingEdge))
		throw FlycastException("PowerVR draw identity/runtime configuration mismatch");
}

void requireDistinct(const std::filesystem::path& lhs,
		const std::filesystem::path& rhs, const char *message)
{
	if (pathsAlias(lhs, rhs))
		throw FlycastException(message);
}

PvrDrawConfiguration currentRendererConfiguration()
{
	PvrDrawConfiguration result;
	result.available = true;
	result.renderer = config::RendererType.get() == RenderType::DirectX11
			? "directx11" : "unsupported";
	result.perStripSorting = config::PerStripSorting.get();
	result.translucentPolygonDepthMask =
			config::TranslucentPolygonDepthMask.get();
	result.modifierVolumes = config::ModifierVolumes.get();
	result.renderResolution = static_cast<std::uint32_t>(
			config::RenderResolution.get());
	result.emulateFramebuffer = config::EmulateFramebuffer.get();
	result.fixUpscaleBleedingEdge = config::FixUpscaleBleedingEdge.get();
	return result;
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
	std::filesystem::path drawOutputPath;
	Sha256Digest replayDigest {};
	Sha256Digest manifestDigest {};
	std::uint64_t maximumReplayBytes = 0;
	std::uint64_t maximumBytes = 0;
	std::uint64_t presentationMaximumBytes = 0;
	std::uint64_t drawMaximumBytes = 0;
	Sha256Digest rendererDigest {};
	std::uint64_t startDma = 0;
};

struct Session
{
	Session(std::unique_ptr<PvrTaArtifactWriter> writer,
			std::unique_ptr<PvrPresentationArtifactWriter> presentationWriter,
			std::unique_ptr<PvrDrawArtifactWriter> drawWriter,
			Configuration configuration)
		: writer(std::move(writer)),
		  presentationWriter(std::move(presentationWriter)),
		  drawWriter(std::move(drawWriter)),
		  configuration(std::move(configuration)) {}

	std::unique_ptr<PvrTaArtifactWriter> writer;
	std::unique_ptr<PvrPresentationArtifactWriter> presentationWriter;
	std::unique_ptr<PvrDrawArtifactWriter> drawWriter;
	Configuration configuration;
	PvrTaObservationSubscription subscription = 0;
	PvrPresentationObservationSubscription presentationSubscription = 0;
	PvrDrawObservationSubscription drawSubscription = 0;
	std::uint64_t droppedBaseline = 0;
	std::uint64_t presentationDroppedBaseline = 0;
	std::uint64_t drawDroppedBaseline = 0;
	bool taWindowComplete = false;
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
	if (configuration.identity.initialState.available)
		authenticateInitialStateFile(configuration.identity, researchPath(
				hostfs::getSavestatePath(static_cast<int>(
						configuration.identity.initialState.slot), false)));
}

void subscribeSession(Session& active)
{
	if (active.subscription != 0)
		throw FlycastException("PowerVR TA evidence subscription is already active");
	active.droppedBaseline = pvrTaObservationDroppedCount();
	active.presentationDroppedBaseline = pvrPresentationObservationDroppedCount();
	active.drawDroppedBaseline = pvrDrawObservationDroppedCount();
	Session *raw = &active;
	active.subscription = subscribePvrTaEvidenceObservations(
			[raw](const PvrTaObservation& observation) {
				if (raw->failure != nullptr || raw->taWindowComplete)
					return;
				try
				{
					raw->writer->write(observation);
					if (observation.type == PvrTaObservationType::RenderDone
							&& raw->writer->getSummary().typeCounts[4]
									== raw->configuration.manifest.renderDoneCount)
					{
						raw->taWindowComplete = true;
						freezePvrTaObservedRenderGenerationWindow();
					}
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
		observePvrInitialRegisterState(pvr_regs, sizeof(pvr_regs),
				pvrCurrentRenderGeneration(), sh4_sched_now64());
	}
	if (active.drawWriter != nullptr)
	{
		active.drawSubscription = subscribePvrDrawEvidenceObservations(
				[raw](const PvrDrawObservation& observation) {
					if (raw->failure != nullptr)
						return;
					try
					{
						raw->drawWriter->write(observation);
					}
					catch (...)
					{
						raw->failure = std::current_exception();
						raw->drawWriter->abandon();
						ERROR_LOG(PVR, "PowerVR draw writer failed");
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
	const bool hasDraw = !config::ResearchPvrDrawRecordPath.get().empty();
	if (hasPresentation && (!hasOutput || !hasManifest))
		throw FlycastException(
				"PowerVR presentation capture requires the TA record and manifest");
	if (hasDraw && (!hasOutput || !hasManifest || !hasPresentation))
		throw FlycastException(
				"PowerVR draw capture requires TA and presentation recording");
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
	if (hasDraw && config::ResearchPvrDrawMaxBytes.get()
			< static_cast<std::int64_t>(PvrDrawArtifactHeaderSize))
		throw FlycastException(
				"research.PvrDrawMaxBytes is smaller than the artifact header");
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
	if (hasDraw)
	{
		if (!config::isTransient("research", "PvrDrawRecord"))
			throw FlycastException(
					"PowerVR draw path must be supplied as a transient option");
		next->drawOutputPath = researchPath(config::ResearchPvrDrawRecordPath.get());
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
	if (hasDraw)
	{
		requireDistinct(next->drawOutputPath, next->identityPath,
				"PowerVR draw and identity paths alias");
		requireDistinct(next->drawOutputPath, next->replayPath,
				"PowerVR draw and replay paths alias");
		requireDistinct(next->drawOutputPath, next->manifestPath,
				"PowerVR draw and manifest paths alias");
		requireDistinct(next->drawOutputPath, next->outputPath,
				"PowerVR draw and TA output paths alias");
		requireDistinct(next->drawOutputPath, next->presentationOutputPath,
				"PowerVR draw and presentation output paths alias");
	}

	next->identity = loadIdentityManifest(next->identityPath);
	requireSh4EquivalenceIdentityV2(next->identity);
	if (hasDraw && !next->identity.runtimeConfiguration.pvrDrawConfiguration.available)
		throw FlycastException(
				"PowerVR draw capture requires identity-bound renderer configuration");
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
	next->drawMaximumBytes = static_cast<std::uint64_t>(
			config::ResearchPvrDrawMaxBytes.get());
	if (hasDraw)
		next->rendererDigest = pvrDrawConfigurationDigest(
				next->identity.runtimeConfiguration.pvrDrawConfiguration);
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
	if (!configured->drawOutputPath.empty()
			&& (config::RendererType.get() != RenderType::DirectX11
					|| !sha256Equal(pvrDrawConfigurationDigest(
							currentRendererConfiguration()),
							configured->rendererDigest)))
		throw FlycastException(
				"PowerVR draw renderer configuration changed before capture");
	reauthenticate(*configured);

	PvrTaArtifactBinding binding;
	binding.backend = backendFor(configured->identity);
	binding.identityDigest = configured->identity.digest;
	binding.replayDigest = configured->replayDigest;
	binding.manifestDigest = configured->manifestDigest;
	auto writer = std::make_unique<PvrTaArtifactWriter>(configured->outputPath,
			binding, configured->maximumBytes, configured->manifest.maximumEvents);
	std::unique_ptr<PvrPresentationArtifactWriter> presentationWriter;
	std::unique_ptr<PvrDrawArtifactWriter> drawWriter;
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
	if (!configured->drawOutputPath.empty())
	{
		PvrDrawArtifactBinding drawBinding;
		drawBinding.backend = binding.backend;
		drawBinding.identityDigest = binding.identityDigest;
		drawBinding.replayDigest = binding.replayDigest;
		drawWriter = std::make_unique<PvrDrawArtifactWriter>(
				configured->drawOutputPath, drawBinding,
				configured->drawMaximumBytes);
	}
	session = std::make_unique<Session>(std::move(writer),
			std::move(presentationWriter), std::move(drawWriter), *configured);
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
	if (finishing->drawSubscription != 0)
		unsubscribePvrDrawObservations(finishing->drawSubscription);
	configured.reset();
	const auto abandonOutputs = [&finishing]() noexcept {
		finishing->writer->abandon();
		if (finishing->presentationWriter != nullptr)
			finishing->presentationWriter->abandon();
		if (finishing->drawWriter != nullptr)
			finishing->drawWriter->abandon();
	};
	if (!clean)
	{
		abandonOutputs();
		return;
	}
	if (finishing->failure != nullptr)
	{
		abandonOutputs();
		std::rethrow_exception(finishing->failure);
	}
	if (pvrTaObservationDroppedCount() != finishing->droppedBaseline)
	{
		abandonOutputs();
		throw FlycastException("PowerVR TA observations were dropped during capture");
	}
	if (pvrPresentationObservationDroppedCount()
			!= finishing->presentationDroppedBaseline)
	{
		abandonOutputs();
		throw FlycastException(
				"PowerVR presentation observations were dropped during capture");
	}
	if (finishing->drawWriter != nullptr
			&& pvrDrawObservationDroppedCount() != finishing->drawDroppedBaseline)
	{
		abandonOutputs();
		throw FlycastException(
				"PowerVR draw observations were dropped during capture");
	}
	try
	{
		reauthenticate(finishing->configuration);
		if (finishing->drawWriter != nullptr
				&& !sha256Equal(pvrDrawConfigurationDigest(
						currentRendererConfiguration()),
						finishing->configuration.rendererDigest))
			throw FlycastException(
					"PowerVR draw renderer configuration changed during capture");
		const PvrTaArtifactSummary observed = finishing->writer->getSummary();
		if (observed.typeCounts[4]
				!= finishing->configuration.manifest.renderDoneCount)
			throw FlycastException(("PowerVR TA render-done count differs from manifest "
					"(observed " + std::to_string(observed.typeCounts[4])
					+ ", expected " + std::to_string(
							finishing->configuration.manifest.renderDoneCount)
					+ ")").c_str());
		const PvrTaArtifactSummary summary = finishing->writer->finalize();
		if (finishing->presentationWriter != nullptr)
		{
			const auto presentationSummary =
					finishing->presentationWriter->finalize();
			NOTICE_LOG(PVR, "Finalized PowerVR presentation artifact (%llu events)",
					static_cast<unsigned long long>(presentationSummary.eventCount));
		}
		if (finishing->drawWriter != nullptr)
		{
			const Sha256Digest taDigest = hashFileExact(
					finishing->configuration.outputPath,
					finishing->configuration.maximumBytes);
			const Sha256Digest presentationDigest = hashFileExact(
					finishing->configuration.presentationOutputPath,
					finishing->configuration.presentationMaximumBytes);
			finishing->drawWriter->bindLinkedArtifacts(taDigest,
					presentationDigest, finishing->configuration.rendererDigest);
			const auto drawSummary = finishing->drawWriter->finalize();
			NOTICE_LOG(PVR, "Finalized PowerVR draw artifact (%llu events)",
					static_cast<unsigned long long>(drawSummary.eventCount));
		}
		NOTICE_LOG(PVR, "Finalized PowerVR TA artifact (%llu events)",
				static_cast<unsigned long long>(summary.eventCount));
	}
	catch (...)
	{
		abandonOutputs();
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
	if (abandoning->drawSubscription != 0)
		unsubscribePvrDrawObservations(abandoning->drawSubscription);
	abandoning->writer->abandon();
	if (abandoning->presentationWriter != nullptr)
		abandoning->presentationWriter->abandon();
	if (abandoning->drawWriter != nullptr)
		abandoning->drawWriter->abandon();
}

bool pvrTaCaptureRuntimeActive()
{
	return session != nullptr;
}

bool pvrTaCaptureWindowComplete()
{
	return session != nullptr && session->taWindowComplete;
}

} // namespace research

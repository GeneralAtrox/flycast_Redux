#include "cfg/option.h"
#include "hw/sh4/sh4_if.h"
#include "json.hpp"
#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/pvr_ta_artifact.h"
#include "research/pvr_ta_capture_runtime.h"
#include "research/pvr_ta_observation.h"
#include "research/pvr_draw_artifact.h"
#include "research/pvr_draw_observation.h"
#include "research/pvr_presentation_artifact.h"
#include "research/pvr_presentation_observation.h"
#include "research/sh4_observation_runtime.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

using json = nlohmann::json;

class TemporaryDirectory
{
public:
	TemporaryDirectory()
	{
		static std::atomic<unsigned> sequence {0};
		path = std::filesystem::temp_directory_path()
				/ ("flycast-pvr-ta-runtime-test-"
						+ std::to_string(sequence.fetch_add(1)));
		std::error_code error;
		std::filesystem::remove_all(path, error);
		std::filesystem::create_directories(path);
	}

	~TemporaryDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	std::filesystem::path file(const char *name) const { return path / name; }

private:
	std::filesystem::path path;
};

class RuntimeReset
{
public:
	~RuntimeReset()
	{
		research::abortPvrTaCaptureRuntime();
		config::ResearchIdentityManifestPath.set("");
		config::ResearchMapleRecordPath.set("");
		config::ResearchMapleReplayPath.set("");
		config::ResearchSh4ObservationRecordPath.set("");
		config::ResearchPvrTaRecordPath.set("");
		config::ResearchPvrTaManifestPath.set("");
		config::ResearchPvrTaMaxBytes.set(512ll * 1024 * 1024);
		config::ResearchPvrTaStartDma.set(0);
		config::ResearchPvrPresentationRecordPath.set("");
		config::ResearchPvrPresentationMaxBytes.set(512ll * 1024 * 1024);
		config::ResearchPvrDrawRecordPath.set("");
		config::ResearchPvrDrawMaxBytes.set(512ll * 1024 * 1024);
		config::ResearchMapleTraceMaxBytes.set(512ll * 1024 * 1024);
		config::ResearchDreamcastRtcSeed.set(-1);
		config::ResearchDynarecObservation.set(false);
		config::DynarecEnabled.set(false);
		config::RendererType.set(RenderType::DirectX11);
	}
};

void writeText(const std::filesystem::path& path, const std::string& text)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(output.good());
	output.write(text.data(), static_cast<std::streamsize>(text.size()));
	ASSERT_TRUE(output.good());
}

std::string digestHex(const std::string& text)
{
	return research::sha256ToHex(research::sha256(text.data(), text.size()));
}

json identityV2(const research::Sha256Digest& replayIdentity)
{
	json values {
		{"cpu_backend", "interpreter"},
		{"dynarec_observation", false},
		{"dreamcast_rtc_seed", 0x90000000u},
		{"threaded_rendering", false},
		{"autoload_state", false},
		{"autosave_state", false},
		{"ggpo", false},
		{"pvr_draw_configuration", {
			{"renderer", "directx11"},
			{"per_strip_sorting", false},
			{"translucent_polygon_depth_mask", false},
			{"modifier_volumes", true},
			{"render_resolution", 480},
			{"emulate_framebuffer", false},
			{"fix_upscale_bleeding_edge", true},
		}},
	};
	const json blob {
		{"path", "descriptive-only.bin"},
		{"size", 0},
		{"sha256", std::string(64, '0')},
	};
	json boot = blob;
	boot["name"] = "fixture.elf";
	return json {
		{"schema", "flycast-research-identity"},
		{"schema_version", 2},
		{"media", {
			{"kind", "elf"},
			{"source", blob},
			{"ip_bin", blob},
			{"boot_executable", boot},
		}},
		{"firmware", {
			{"mode", "hle"},
			{"hle_identity", "fixture-hle"},
			{"flash_initial", blob},
		}},
		{"persistent_devices", json::array()},
		{"emulator", {
			{"git_commit", std::string(40, '0')},
			{"executable", blob},
		}},
		{"configuration", {
			{"values", values},
			{"sha256", digestHex(values.dump())},
		}},
		{"static_analysis", {
			{"program_sha256", std::string(64, '0')},
			{"export_sha256", std::string(64, '1')},
			{"image_base", "0x8c010000"},
		}},
		{"equivalence", {
			{"maple_replay_identity_sha256",
					research::sha256ToHex(replayIdentity)},
		}},
	};
}

std::vector<std::uint8_t> words(std::initializer_list<std::uint32_t> values)
{
	std::vector<std::uint8_t> bytes;
	for (const std::uint32_t value : values)
	{
		bytes.push_back(static_cast<std::uint8_t>(value));
		bytes.push_back(static_cast<std::uint8_t>(value >> 8));
		bytes.push_back(static_cast<std::uint8_t>(value >> 16));
		bytes.push_back(static_cast<std::uint8_t>(value >> 24));
	}
	return bytes;
}

void writeReplay(const std::filesystem::path& path,
		const research::Sha256Digest& identityDigest)
{
	research::MapleTraceWriter writer(path, identityDigest);
	research::MapleDmaBeginEvent begin;
	begin.tick = 10;
	begin.descriptorAddress = 0x0c001000;
	begin.mden = 1;
	begin.mdst = 1;
	begin.mmsel = 1;
	begin.trigger = research::MapleDmaTrigger::Software;
	writer.beginDma(begin);
	research::MapleTransactionEvent transaction;
	transaction.dmaOrdinal = 0;
	transaction.tick = 10;
	transaction.descriptorAddress = 0x0c001000;
	transaction.destinationAddress = 0x0c002000;
	transaction.descriptorHeader1 = 0x80000001;
	transaction.descriptorHeader2 = 0x0c002000;
	transaction.deviceType = 0;
	transaction.bus = 0;
	transaction.port = 5;
	transaction.command = 0x09;
	transaction.flags = research::MapleTransactionDevicePresent;
	transaction.request = words({0x01002009, 0x01000000});
	transaction.response = words({0x02002008, 0x01000000, 0xffff0000});
	writer.writeTransaction(transaction);
	research::MapleDmaScheduleEvent schedule;
	schedule.dmaOrdinal = 0;
	schedule.tick = 10;
	schedule.inputWireBytes = 11;
	schedule.outputWireBytes = 15;
	schedule.scheduledCycles = 1000;
	schedule.responseCount = 1;
	writer.scheduleDma(schedule);
	research::MapleDmaCommitEvent commit;
	commit.dmaOrdinal = 0;
	commit.tick = 1010;
	commit.callbackCycles = 1000;
	commit.responseCount = 1;
	commit.flags = research::MapleCommitInterruptRaised;
	writer.commitDma(commit);
	writer.finalize();
}

research::PvrTaRenderSelectionTranscript transcript()
{
	research::PvrTaRenderSelectionTranscript result;
	result.initialized = true;
	result.regionBase = 0x00200000;
	result.fpuParamCfg = 0;
	result.record(0x00200010, 0);
	result.record(0x00200000, 0x80000000);
	result.record(0x00200000, 0x80000000);
	result.record(0x00200000, 0x80000000);
	result.record(0x00200004, 0x00300000);
	result.record(0x00300000, 0x00100000);
	return result;
}

struct CompleteSlice
{
	std::uint64_t renderGeneration = 0;
	std::array<research::PvrTaBlockProvenance, 2> blocks;
};

CompleteSlice emitCompleteSlice()
{
	CompleteSlice result;
	Sh4Context context {};
	context.pc = 0x8c010102;
	context.pr = 0x8c020000;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter,
			0x8c010100, 0x2102, 100, context);
	research::observePvrTaListBoundary(false, 0x00100000, 0, 101);
	std::array<std::uint8_t, 32> block {};
	result.blocks[0] = research::observePvrTaAcceptedBlock(
			research::PvrTaInputSource::StoreQueue,
			0xe0000020, 0x10000020, block.data(), 0x00100000, 0,
			7, 0, 0, 1, 102);
	result.blocks[1] = research::observePvrTaAcceptedBlock(
			research::PvrTaInputSource::StoreQueue,
			0xe0000040, 0x10000040, block.data(), 0x00100000, 0,
			7, 1, 0, 2, 103);
	const std::uint32_t selected[] {0x00100000};
	const bool available[] {true};
	const auto reads = transcript();
	result.renderGeneration = research::observePvrTaStartRender(
			selected, available, 1, &reads, 104);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter,
			0x8c010100, 0x2102, 105, context);
	research::observePvrTaRenderDone(200);
	return result;
}

void emitCompleteDrawSlice(const CompleteSlice& slice)
{
	using namespace research;
	PvrDrawObservation primitive;
	primitive.type = PvrDrawObservationType::PrimitiveDecoded;
	primitive.tick = 201;
	primitive.renderGeneration = slice.renderGeneration;
	primitive.primitiveGeneration = allocatePvrPrimitiveGeneration();
	primitive.contextAddress = slice.blocks[0].contextAddress;
	primitive.contextGeneration = slice.blocks[0].contextGeneration;
	primitive.renderPass = slice.blocks[0].renderPass;
	primitive.listType = 0;
	primitive.primitiveKind = PvrPrimitiveKind::PolygonStrip;
	primitive.ownerClass = PvrPrimitiveOwnerClass::Exact;
	primitive.count = 3;
	primitive.vertices.resize(primitive.count);
	primitive.bounds.available = true;
	primitive.bounds.maximumX = 10;
	primitive.bounds.maximumY = 10;
	primitive.bounds.maximumZ = 1;
	primitive.parameterBlocks = {slice.blocks[0]};
	primitive.vertexBlocks = {slice.blocks[1]};
	const std::uint64_t primitiveGeneration = primitive.primitiveGeneration;
	observePvrPrimitiveDecoded(std::move(primitive));
	observePvrDrawConsumed(slice.renderGeneration, {primitiveGeneration},
			PvrDrawBackend::DirectX11, PvrDrawPass::Color, 0, 3, true, 202);
	observePvrDrawRenderCompleted(slice.renderGeneration, true, 203);
}

void emitCompletePresentationSlice()
{
	using namespace research;
	Sh4Context context {};
	context.pc = 0x8c010202;
	sh4ObservationInstructionBegin(Sh4ObservationBackend::Interpreter,
			0x8c010200, 0x2102, 300, context);
	observePvrRegisterWrite(0x005f8050, 0x50, 0x20, 0, 0x20,
			PvrRegisterWriteDisposition::Stored, 0, 300);
	const std::array<std::uint8_t, 2> write {{0xe0, 0x07}};
	observePvrVramWrite(PvrVramWriteSource::Sh4Area1Direct,
			0xa4000020, 0x20, write.data(), write.size(), 0, 300);
	sh4ObservationInstructionEnd(Sh4ObservationBackend::Interpreter,
			0x8c010200, 0x2102, 301, context);
	PvrFramebufferConfig framebufferConfig;
	framebufferConfig.fbReadControl = 1u << 2;
	const std::uint64_t framebufferGeneration = observePvrFramebufferCaptured(
			PvrFramebufferKind::DreamcastVram, 0, framebufferConfig, 1, 1, 2,
			write.data(), write.size(), 302);
	observePvrRenderQueued(framebufferGeneration, PvrRenderKind::DirectFramebuffer,
			0x20, 303);
	observePvrRenderCompleted(framebufferGeneration,
			PvrRenderKind::DirectFramebuffer, true, 304);
	observePvrPresentation(PvrPresentationSource::Framebuffer,
			framebufferGeneration, true, 305);
}

struct FixturePaths
{
	std::filesystem::path identity;
	std::filesystem::path replay;
	std::filesystem::path manifest;
	std::filesystem::path output;
	std::filesystem::path presentation;
	std::filesystem::path draw;
	std::string manifestBytes;
};

FixturePaths configureFixture(const TemporaryDirectory& directory,
		std::int64_t maximumBytes = 1024 * 1024,
		std::uint64_t renderDoneCount = 1)
{
	FixturePaths paths {
		directory.file("identity.json"),
		directory.file("replay.fcmt"),
		directory.file("pvr-manifest.json"),
		directory.file("capture.fcpvr"),
		directory.file("presentation.fcpvrp"),
		directory.file("draw.fcpvrd"),
		"",
	};
	paths.manifestBytes = json {
		{"schema", "flycast-research-pvr-ta-capture-manifest"},
		{"schema_version", 1},
		{"manifest_id", "runtime-fixture"},
		{"capture", {{"start_dma", 0},
				{"render_done_count", renderDoneCount}}},
		{"limits", {{"maximum_bytes", maximumBytes}, {"maximum_events", 100}}},
		{"static_analysis", {
			{"export_sha256", std::string(64, '1')},
			{"executable_sha256", std::string(64, '0')},
		}},
	}.dump();
	const research::Sha256Digest replayIdentity = research::sha256(
			"runtime-maple-identity", 22);
	writeReplay(paths.replay, replayIdentity);
	writeText(paths.manifest, paths.manifestBytes);
	writeText(paths.identity, identityV2(replayIdentity).dump());
	config::ResearchIdentityManifestPath.set(paths.identity.u8string());
	config::ResearchMapleReplayPath.set(paths.replay.u8string());
	config::ResearchPvrTaManifestPath.set(paths.manifest.u8string());
	config::ResearchPvrTaRecordPath.set(paths.output.u8string());
	config::ResearchPvrTaMaxBytes.set(maximumBytes);
	config::ResearchMapleTraceMaxBytes.set(1024 * 1024);
	return paths;
}

research::PvrTaArtifactBinding bindingFor(const FixturePaths& paths)
{
	research::PvrTaArtifactBinding result;
	result.backend = research::Sh4ObservationBackend::Interpreter;
	result.identityDigest = research::loadIdentityManifest(paths.identity).digest;
	result.replayDigest = research::hashFileExact(paths.replay,
			std::filesystem::file_size(paths.replay));
	result.manifestDigest = research::sha256(paths.manifestBytes.data(),
			paths.manifestBytes.size());
	return result;
}

research::PvrPresentationArtifactBinding presentationBindingFor(
		const FixturePaths& paths)
{
	research::PvrPresentationArtifactBinding result;
	result.backend = research::Sh4ObservationBackend::Interpreter;
	result.identityDigest = research::loadIdentityManifest(paths.identity).digest;
	result.replayDigest = research::hashFileExact(paths.replay,
			std::filesystem::file_size(paths.replay));
	return result;
}

research::PvrDrawArtifactBinding drawBindingFor(const FixturePaths& paths)
{
	research::PvrDrawArtifactBinding result;
	result.backend = research::Sh4ObservationBackend::Interpreter;
	result.identityDigest = research::loadIdentityManifest(paths.identity).digest;
	result.replayDigest = research::hashFileExact(paths.replay,
			std::filesystem::file_size(paths.replay));
	result.taArtifactDigest = research::hashFileExact(paths.output,
			std::filesystem::file_size(paths.output));
	result.presentationArtifactDigest = research::hashFileExact(
			paths.presentation, std::filesystem::file_size(paths.presentation));
	result.rendererConfigurationDigest = research::pvrDrawConfigurationDigest(
			research::loadIdentityManifest(paths.identity).runtimeConfiguration
					.pvrDrawConfiguration);
	return result;
}

} // namespace

TEST(ResearchPvrTaCaptureRuntime, IdentityAllowsExplicitFullSessionStart)
{
	TemporaryDirectory directory;
	const research::Sha256Digest replayIdentity = research::sha256(
			"runtime-maple-identity", 22);
	json identity = identityV2(replayIdentity);
	json& values = identity["configuration"]["values"];
	values["pvr_ta_start_dma"] = 0;
	identity["configuration"]["sha256"] = digestHex(values.dump());
	const auto path = directory.file("identity.json");
	writeText(path, identity.dump());

	const research::IdentityManifest parsed =
			research::loadIdentityManifest(path);
	EXPECT_EQ(0u, parsed.runtimeConfiguration.pvrTaStartDma);
}

TEST(ResearchPvrTaCaptureRuntime, FinalizesAuthenticatedCompleteSlice)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const FixturePaths paths = configureFixture(directory);
	research::configurePvrTaCaptureRuntime();
	research::startPvrTaCaptureRuntime();
	ASSERT_TRUE(research::pvrTaCaptureRuntimeActive());
	emitCompleteSlice();
	research::stopPvrTaCaptureRuntime(true);
	EXPECT_FALSE(research::pvrTaCaptureRuntimeActive());
	const auto summary = research::validatePvrTaArtifactFile(paths.output,
			bindingFor(paths), 1024 * 1024, 100);
	EXPECT_EQ(5u, summary.eventCount);
}

TEST(ResearchPvrTaCaptureRuntime, FinalizesSameRunPresentationArtifact)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const FixturePaths paths = configureFixture(directory);
	config::ResearchPvrPresentationRecordPath.set(paths.presentation.u8string());
	config::ResearchPvrPresentationMaxBytes.set(1024 * 1024);
	research::configurePvrTaCaptureRuntime();
	research::startPvrTaCaptureRuntime();
	emitCompleteSlice();
	emitCompletePresentationSlice();
	research::stopPvrTaCaptureRuntime(true);
	const auto presentation = research::validatePvrPresentationArtifactFile(
			paths.presentation, presentationBindingFor(paths), 1024 * 1024, 100);
	EXPECT_TRUE(presentation.completeVerticalSlice);
}

TEST(ResearchPvrTaCaptureRuntime,
		FreezesCompletedTaWindowWhilePresentationContinues)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const FixturePaths paths = configureFixture(directory);
	config::RendererType.set(RenderType::DirectX11);
	config::ResearchPvrPresentationRecordPath.set(paths.presentation.u8string());
	config::ResearchPvrPresentationMaxBytes.set(1024 * 1024);
	config::ResearchPvrDrawRecordPath.set(paths.draw.u8string());
	config::ResearchPvrDrawMaxBytes.set(1024 * 1024);
	research::configurePvrTaCaptureRuntime();
	research::startPvrTaCaptureRuntime();

	const CompleteSlice included = emitCompleteSlice();
	emitCompleteDrawSlice(included);
	// A later producer generation is outside the manifest's exact one-render
	// TA window. Its TA and draw events must both be excluded while completion
	// for the already-included generation remains admissible.
	const CompleteSlice excluded = emitCompleteSlice();
	emitCompleteDrawSlice(excluded);
	emitCompletePresentationSlice();
	research::stopPvrTaCaptureRuntime(true);

	const auto ta = research::validatePvrTaArtifactFile(paths.output,
			bindingFor(paths), 1024 * 1024, 100);
	EXPECT_EQ(5u, ta.eventCount);
	EXPECT_EQ(1u, ta.typeCounts[4]);
	const auto presentation = research::validatePvrPresentationArtifactFile(
			paths.presentation, presentationBindingFor(paths), 1024 * 1024, 100);
	EXPECT_TRUE(presentation.completeVerticalSlice);
	const auto draw = research::validatePvrDrawArtifactFile(
			paths.draw, drawBindingFor(paths), 1024 * 1024, 100);
	EXPECT_TRUE(draw.completeVerticalSlice);
	EXPECT_EQ(3u, draw.eventCount);
	EXPECT_EQ(1u, draw.typeCounts[0]);
	EXPECT_EQ(1u, draw.typeCounts[1]);
	EXPECT_EQ(1u, draw.typeCounts[2]);
}

TEST(ResearchPvrTaCaptureRuntime, FinalizesDrawArtifactBoundToSameRun)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const FixturePaths paths = configureFixture(directory);
	config::RendererType.set(RenderType::DirectX11);
	config::ResearchPvrPresentationRecordPath.set(paths.presentation.u8string());
	config::ResearchPvrPresentationMaxBytes.set(1024 * 1024);
	config::ResearchPvrDrawRecordPath.set(paths.draw.u8string());
	config::ResearchPvrDrawMaxBytes.set(1024 * 1024);
	research::configurePvrTaCaptureRuntime();
	research::startPvrTaCaptureRuntime();
	const CompleteSlice slice = emitCompleteSlice();
	emitCompleteDrawSlice(slice);
	emitCompletePresentationSlice();
	research::stopPvrTaCaptureRuntime(true);
	const auto draw = research::validatePvrDrawArtifactFile(paths.draw,
			drawBindingFor(paths), 1024 * 1024, 100);
	EXPECT_TRUE(draw.completeVerticalSlice);
}

TEST(ResearchPvrTaCaptureRuntime, IgnoresOnlyPreSubscriptionRenderCompletion)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const FixturePaths paths = configureFixture(directory);
	config::RendererType.set(RenderType::DirectX11);
	config::ResearchPvrPresentationRecordPath.set(paths.presentation.u8string());
	config::ResearchPvrPresentationMaxBytes.set(1024 * 1024);
	config::ResearchPvrDrawRecordPath.set(paths.draw.u8string());
	config::ResearchPvrDrawMaxBytes.set(1024 * 1024);
	research::configurePvrTaCaptureRuntime();
	research::startPvrTaCaptureRuntime();

	// A delayed subscription can observe completion of the render that began
	// before its authenticated Maple boundary. That frame is outside the slice.
	research::observePvrTaRenderDone(90);
	research::observePvrRenderCompleted(999,
			research::PvrRenderKind::Screen, true, 91);
	research::observePvrPresentation(research::PvrPresentationSource::Render,
			999, true, 92);
	research::observePvrDrawRenderCompleted(999, true, 93);

	const CompleteSlice slice = emitCompleteSlice();
	emitCompleteDrawSlice(slice);
	emitCompletePresentationSlice();
	research::stopPvrTaCaptureRuntime(true);

	const auto ta = research::validatePvrTaArtifactFile(paths.output,
			bindingFor(paths), 1024 * 1024, 100);
	EXPECT_EQ(5u, ta.eventCount);
	const auto presentation = research::validatePvrPresentationArtifactFile(
			paths.presentation, presentationBindingFor(paths), 1024 * 1024, 100);
	EXPECT_TRUE(presentation.completeVerticalSlice);
	const auto draw = research::validatePvrDrawArtifactFile(paths.draw,
			drawBindingFor(paths), 1024 * 1024, 100);
	EXPECT_TRUE(draw.completeVerticalSlice);
}

TEST(ResearchPvrTaCaptureRuntime, RejectsDrawWithoutIdentityRendererConfiguration)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const FixturePaths paths = configureFixture(directory);
	json identity = json::parse(std::ifstream(paths.identity));
	json& values = identity["configuration"]["values"];
	values.erase("pvr_draw_configuration");
	identity["configuration"]["sha256"] = digestHex(values.dump());
	writeText(paths.identity, identity.dump());
	config::ResearchPvrPresentationRecordPath.set(paths.presentation.u8string());
	config::ResearchPvrDrawRecordPath.set(paths.draw.u8string());
	EXPECT_THROW(research::configurePvrTaCaptureRuntime(), std::runtime_error);
}

TEST(ResearchPvrTaCaptureRuntime, RendererMutationAbandonsDrawSlice)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const FixturePaths paths = configureFixture(directory);
	config::ResearchPvrPresentationRecordPath.set(paths.presentation.u8string());
	config::ResearchPvrDrawRecordPath.set(paths.draw.u8string());
	research::configurePvrTaCaptureRuntime();
	research::startPvrTaCaptureRuntime();
	const CompleteSlice slice = emitCompleteSlice();
	emitCompleteDrawSlice(slice);
	emitCompletePresentationSlice();
	config::ModifierVolumes.set(false);
	EXPECT_THROW(research::stopPvrTaCaptureRuntime(true), std::runtime_error);
	EXPECT_THROW(research::validatePvrDrawArtifactFile(paths.draw,
			drawBindingFor(paths), 1024 * 1024, 100), std::runtime_error);
}

TEST(ResearchPvrTaCaptureRuntime, ChangedManifestLeavesCandidateIncomplete)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const FixturePaths paths = configureFixture(directory);
	research::configurePvrTaCaptureRuntime();
	research::startPvrTaCaptureRuntime();
	emitCompleteSlice();
	writeText(paths.manifest, "changed manifest");
	EXPECT_THROW(research::stopPvrTaCaptureRuntime(true), std::runtime_error);
	EXPECT_THROW(research::validatePvrTaArtifactFile(paths.output,
			bindingFor(paths), 1024 * 1024, 100), std::runtime_error);
}

TEST(ResearchPvrTaCaptureRuntime, WriterFailureIsLatchedAndIncomplete)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const FixturePaths paths = configureFixture(directory,
			research::PvrTaArtifactHeaderSize + 80);
	research::configurePvrTaCaptureRuntime();
	research::startPvrTaCaptureRuntime();
	EXPECT_NO_THROW(emitCompleteSlice());
	EXPECT_THROW(research::stopPvrTaCaptureRuntime(true), std::runtime_error);
	EXPECT_THROW(research::validatePvrTaArtifactFile(paths.output,
			bindingFor(paths), 1024 * 1024, 100), std::runtime_error);
}

TEST(ResearchPvrTaCaptureRuntime, RenderCountMismatchLeavesCandidateIncomplete)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const FixturePaths paths = configureFixture(directory, 1024 * 1024, 2);
	research::configurePvrTaCaptureRuntime();
	research::startPvrTaCaptureRuntime();
	emitCompleteSlice();
	EXPECT_THROW(research::stopPvrTaCaptureRuntime(true), std::runtime_error);
	EXPECT_THROW(research::validatePvrTaArtifactFile(paths.output,
			bindingFor(paths), 1024 * 1024, 100), std::runtime_error);
}

TEST(ResearchPvrTaCaptureRuntime, AbortLeavesCandidateIncomplete)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const FixturePaths paths = configureFixture(directory);
	research::configurePvrTaCaptureRuntime();
	research::startPvrTaCaptureRuntime();
	emitCompleteSlice();
	research::abortPvrTaCaptureRuntime();
	EXPECT_FALSE(research::pvrTaCaptureRuntimeActive());
	EXPECT_THROW(research::validatePvrTaArtifactFile(paths.output,
			bindingFor(paths), 1024 * 1024, 100), std::runtime_error);
}

TEST(ResearchPvrTaCaptureRuntime, DirtyStopLeavesCandidateIncomplete)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const FixturePaths paths = configureFixture(directory);
	research::configurePvrTaCaptureRuntime();
	research::startPvrTaCaptureRuntime();
	emitCompleteSlice();
	research::stopPvrTaCaptureRuntime(false);
	EXPECT_FALSE(research::pvrTaCaptureRuntimeActive());
	EXPECT_THROW(research::validatePvrTaArtifactFile(paths.output,
			bindingFor(paths), 1024 * 1024, 100), std::runtime_error);
}

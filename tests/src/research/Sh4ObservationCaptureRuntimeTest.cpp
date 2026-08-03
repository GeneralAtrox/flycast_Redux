#include "cfg/option.h"
#include "research/identity_manifest.h"
#include "research/maple_runtime.h"
#include "research/maple_trace.h"
#include "research/sh4_observation_capture_runtime.h"
#include "research/sh4_observation_runtime.h"
#include "research/sh4_observation_trace.h"
#include "hw/sh4/sh4_if.h"
#include "json.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

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
				/ ("flycast-sh4-observation-capture-test-"
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
		research::abortSh4ObservationCaptureRuntime();
		research::abortRuntime();
		research::setMapleCheckpointHandler(nullptr);
		research::setMapleDmaBeginHandler(nullptr);
		config::ResearchIdentityManifestPath.set("");
		config::ResearchMapleRecordPath.set("");
		config::ResearchMapleReplayPath.set("");
		config::ResearchSh4ObservationRecordPath.set("");
		config::ResearchSh4ObservationManifestSetPath.set("");
		config::ResearchSh4ObservationMaxBytes.set(512ll * 1024 * 1024);
		config::ResearchMapleTraceMaxBytes.set(512ll * 1024 * 1024);
		config::ResearchMapleDmaCheckpoint.set(0);
		config::ResearchSh4ObservationStartDma.set(0);
		config::ResearchDreamcastRtcSeed.set(-1);
		config::ResearchDynarecObservation.set(false);
		config::DynarecEnabled.set(false);
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

json identityV2(const char *backend, bool observation,
		const std::string& replayIdentityDigest, std::uint64_t dmaCheckpoint = 0,
		std::uint64_t observationStartDma = 0)
{
	json values {
		{"cpu_backend", backend},
		{"dynarec_observation", observation},
		{"dreamcast_rtc_seed", 0x90000000u},
		{"threaded_rendering", false},
		{"autoload_state", false},
		{"autosave_state", false},
		{"ggpo", false},
	};
	if (dmaCheckpoint != 0)
		values["maple_dma_checkpoint"] = dmaCheckpoint;
	if (observationStartDma != 0)
		values["sh4_observation_start_dma"] = observationStartDma;
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
		{"equivalence", {
			{"maple_replay_identity_sha256", replayIdentityDigest},
		}},
	};
}

void ignoreMapleCheckpoint()
{
}

} // namespace

TEST(ResearchSh4ObservationCaptureRuntime,
		V2IdentityCapturesExactReplayAndManifestBindings)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const auto identityPath = directory.file("identity-v2.json");
	const auto replayPath = directory.file("replay.fcmt");
	const auto manifestSetPath = directory.file("manifest-set.json");
	const auto outputPath = directory.file("interpreter.fcso");
	const std::string replayBytes = "typed replay fixture";
	const std::string manifestSetBytes = "{\"fixture\":true}";
	writeText(identityPath, identityV2("interpreter", false,
			std::string(64, '1')).dump());
	writeText(replayPath, replayBytes);
	writeText(manifestSetPath, manifestSetBytes);

	config::ResearchIdentityManifestPath.set(identityPath.u8string());
	config::ResearchMapleReplayPath.set(replayPath.u8string());
	config::ResearchSh4ObservationManifestSetPath.set(
			manifestSetPath.u8string());
	config::ResearchSh4ObservationRecordPath.set(outputPath.u8string());
	config::ResearchSh4ObservationMaxBytes.set(1024 * 1024);
	config::ResearchMapleTraceMaxBytes.set(1024 * 1024);
	config::DynarecEnabled.set(true);
	config::ResearchDynarecObservation.set(true);

	research::configureSh4ObservationCaptureRuntime();
	EXPECT_FALSE(config::DynarecEnabled.get());
	EXPECT_FALSE(config::ResearchDynarecObservation.get());
	EXPECT_EQ(0x90000000ll, config::ResearchDreamcastRtcSeed.get());
	research::startSh4ObservationCaptureRuntime();
	ASSERT_TRUE(research::sh4ObservationCaptureRuntimeActive());

	Sh4Context context {};
	context.pc = 0x8c010002;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter,
			0x8c010000, 0x0009, 10, context);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter,
			0x8c010000, 0x0009, 11, context);
	research::stopSh4ObservationCaptureRuntime(true);
	EXPECT_FALSE(research::sh4ObservationCaptureRuntimeActive());

	const research::IdentityManifest identity =
			research::loadIdentityManifest(identityPath);
	research::Sh4ObservationTraceBinding binding;
	binding.backend = research::Sh4ObservationBackend::Interpreter;
	binding.identityDigest = identity.digest;
	binding.replayDigest = research::sha256(replayBytes.data(), replayBytes.size());
	binding.manifestSetDigest = research::sha256(manifestSetBytes.data(),
			manifestSetBytes.size());
	const auto summary = research::validateSh4ObservationTraceFile(outputPath,
			binding, 1024 * 1024, 100);
	EXPECT_EQ(2u, summary.eventCount);
}

TEST(ResearchIdentity, V2BackendAndObservationMustAgree)
{
	TemporaryDirectory directory;
	const auto path = directory.file("identity-v2-invalid.json");
	writeText(path, identityV2("interpreter", true,
			std::string(64, '1')).dump());
	EXPECT_THROW(research::loadIdentityManifest(path), std::runtime_error);
}

TEST(ResearchIdentity, V2RequiresBoundedDreamcastRtcSeed)
{
	TemporaryDirectory directory;
	const auto missingPath = directory.file("identity-v2-missing-rtc.json");
	json missing = identityV2("interpreter", false, std::string(64, '1'));
	missing["configuration"]["values"].erase("dreamcast_rtc_seed");
	missing["configuration"]["sha256"] = digestHex(
			missing["configuration"]["values"].dump());
	writeText(missingPath, missing.dump());
	EXPECT_THROW(research::loadIdentityManifest(missingPath), std::runtime_error);

	const auto oversizedPath = directory.file("identity-v2-oversized-rtc.json");
	json oversized = identityV2("interpreter", false, std::string(64, '1'));
	oversized["configuration"]["values"]["dreamcast_rtc_seed"] =
			std::uint64_t {0x1'0000'0000ull};
	oversized["configuration"]["sha256"] = digestHex(
			oversized["configuration"]["values"].dump());
	writeText(oversizedPath, oversized.dump());
	EXPECT_THROW(research::loadIdentityManifest(oversizedPath), std::runtime_error);
}

TEST(ResearchIdentity, V2AuthenticatesExactAicaConfiguration)
{
	TemporaryDirectory directory;
	json identity = identityV2("interpreter", false, std::string(64, '1'));
	identity["configuration"]["values"]["aica_configuration"] = {
		{"dsp_enabled", false},
		{"vmu_sound", false},
		{"sample_rate", 44100},
		{"sample_format", "signed-pcm16-le-stereo"},
		{"output_stage", "pre-backend-pre-user-volume"},
	};
	identity["configuration"]["sha256"] = digestHex(
			identity["configuration"]["values"].dump());
	const auto path = directory.file("identity-v2-aica.json");
	writeText(path, identity.dump());
	const auto parsed = research::loadIdentityManifest(path);
	EXPECT_TRUE(parsed.runtimeConfiguration.aicaConfiguration.available);
	EXPECT_FALSE(parsed.runtimeConfiguration.aicaConfiguration.dspEnabled);
	EXPECT_FALSE(parsed.runtimeConfiguration.aicaConfiguration.vmuSound);
	EXPECT_NE(research::Sha256Digest {}, research::aicaConfigurationDigest(
			parsed.runtimeConfiguration.aicaConfiguration));

	identity["configuration"]["values"]["aica_configuration"]["vmu_sound"] = true;
	identity["configuration"]["sha256"] = digestHex(
			identity["configuration"]["values"].dump());
	writeText(path, identity.dump());
	EXPECT_THROW(research::loadIdentityManifest(path), std::runtime_error);
}

TEST(ResearchIdentity, V1RejectsV2OnlyDynarecObservationField)
{
	TemporaryDirectory directory;
	const auto path = directory.file("identity-v1-with-v2-field.json");
	json identity = identityV2("interpreter", false, std::string(64, '1'));
	identity["schema_version"] = 1;
	identity.erase("equivalence");
	writeText(path, identity.dump());
	EXPECT_THROW(research::loadIdentityManifest(path), std::runtime_error);
}

TEST(ResearchSh4ObservationCaptureRuntime,
		DefersSubscriptionUntilAuthenticatedMapleDmaBegin)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const auto identityPath = directory.file("identity-v2-deferred.json");
	const auto replayPath = directory.file("replay.fcmt");
	const auto manifestSetPath = directory.file("manifest-set.json");
	const auto outputPath = directory.file("interpreter-deferred.fcso");
	research::Sha256Digest replayIdentity {};
	replayIdentity.fill(0x11);
	writeText(identityPath, identityV2("interpreter", false,
			research::sha256ToHex(replayIdentity), 1, 1).dump());
	writeText(manifestSetPath, "{\"fixture\":true}");

	research::MapleDmaBeginEvent begin;
	begin.tick = 100;
	begin.descriptorAddress = 0x0c001000;
	begin.mden = 1;
	begin.mdst = 1;
	begin.mmsel = 1;
	begin.trigger = research::MapleDmaTrigger::Software;
	research::MapleTransactionEvent transaction;
	transaction.dmaOrdinal = 0;
	transaction.tick = 100;
	transaction.descriptorAddress = 0x0c001000;
	transaction.destinationAddress = 0x0c002000;
	transaction.descriptorHeader1 = 0x80000001;
	transaction.descriptorHeader2 = 0x0c002000;
	transaction.deviceType = 0;
	transaction.bus = 0;
	transaction.port = 5;
	transaction.command = 0x09;
	transaction.flags = research::MapleTransactionDevicePresent;
	transaction.request = {0x09, 0x20, 0x00, 0x01,
			0x00, 0x00, 0x00, 0x01};
	transaction.response = {0x08, 0x20, 0x00, 0x02,
			0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0xff, 0xff};
	research::MapleDmaScheduleEvent schedule;
	schedule.dmaOrdinal = 0;
	schedule.tick = 100;
	schedule.inputWireBytes = 11;
	schedule.outputWireBytes = 15;
	schedule.scheduledCycles = 1000;
	schedule.responseCount = 1;
	research::MapleDmaCommitEvent commit;
	commit.dmaOrdinal = 0;
	commit.tick = 1100;
	commit.callbackCycles = 1000;
	commit.responseCount = 1;
	commit.flags = research::MapleCommitInterruptRaised;
	{
		research::MapleTraceWriter writer(replayPath, replayIdentity);
		writer.beginDma(begin);
		writer.writeTransaction(transaction);
		writer.scheduleDma(schedule);
		writer.commitDma(commit);
		writer.finalize();
	}

	config::ResearchIdentityManifestPath.set(identityPath.u8string());
	config::ResearchMapleReplayPath.set(replayPath.u8string());
	config::ResearchMapleDmaCheckpoint.set(1);
	config::ResearchSh4ObservationManifestSetPath.set(manifestSetPath.u8string());
	config::ResearchSh4ObservationRecordPath.set(outputPath.u8string());
	config::ResearchSh4ObservationStartDma.set(1);
	config::ResearchSh4ObservationMaxBytes.set(1024 * 1024);
	config::ResearchMapleTraceMaxBytes.set(1024 * 1024);
	research::setMapleCheckpointHandler(ignoreMapleCheckpoint);
	research::configureRuntime();
	research::configureSh4ObservationCaptureRuntime();
	research::startRuntime();
	research::startSh4ObservationCaptureRuntime();
	EXPECT_FALSE(research::sh4ObservationBusActive(
			research::Sh4ObservationBackend::Interpreter));
	EXPECT_FALSE(research::sh4ObservationPreciseTimingActive(
			research::Sh4ObservationBackend::Interpreter));

	Sh4Context context {};
	context.pc = 0x8c010002;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter,
			0x8c010000, 0x0009, 10, context);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter,
			0x8c010000, 0x0009, 11, context);
	EXPECT_EQ(0u, research::mapleBeginDma(begin));
	EXPECT_TRUE(research::sh4ObservationBusActive(
			research::Sh4ObservationBackend::Interpreter));
	EXPECT_TRUE(research::sh4ObservationPreciseTimingActive(
			research::Sh4ObservationBackend::Interpreter));
	context.pc = 0x8c020002;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter,
			0x8c020000, 0x0009, 20, context);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter,
			0x8c020000, 0x0009, 21, context);
	research::mapleTransaction(transaction);
	research::mapleScheduleDma(schedule);
	research::mapleCommitDma(commit);
	research::stopSh4ObservationCaptureRuntime(true);
	EXPECT_FALSE(research::sh4ObservationPreciseTimingActive(
			research::Sh4ObservationBackend::Interpreter));
	research::stopRuntime(true);

	const research::IdentityManifest identity =
			research::loadIdentityManifest(identityPath);
	research::Sh4ObservationTraceBinding binding;
	binding.backend = research::Sh4ObservationBackend::Interpreter;
	binding.identityDigest = identity.digest;
	binding.replayDigest = research::hashFileExact(replayPath, 1024 * 1024);
	const std::string manifestSetBytes = "{\"fixture\":true}";
	binding.manifestSetDigest = research::sha256(manifestSetBytes.data(),
			manifestSetBytes.size());
	const auto summary = research::validateSh4ObservationTraceFile(outputPath,
			binding, 1024 * 1024, 100);
	EXPECT_EQ(2u, summary.eventCount);
}

TEST(ResearchSh4ObservationCaptureRuntime,
		V2DynarecIdentitySelectsObservedDynarecBeforeExecution)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const auto identityPath = directory.file("identity-v2.json");
	const auto replayPath = directory.file("replay.fcmt");
	const auto manifestSetPath = directory.file("manifest-set.json");
	const auto outputPath = directory.file("dynarec.fcso");
	writeText(identityPath, identityV2("dynarec", true,
			std::string(64, '1')).dump());
	writeText(replayPath, "typed replay fixture");
	writeText(manifestSetPath, "{\"fixture\":true}");

	config::ResearchIdentityManifestPath.set(identityPath.u8string());
	config::ResearchMapleReplayPath.set(replayPath.u8string());
	config::ResearchSh4ObservationManifestSetPath.set(
			manifestSetPath.u8string());
	config::ResearchSh4ObservationRecordPath.set(outputPath.u8string());
	config::ResearchSh4ObservationMaxBytes.set(1024 * 1024);
	config::ResearchMapleTraceMaxBytes.set(1024 * 1024);
	config::DynarecEnabled.set(false);
	config::ResearchDynarecObservation.set(false);

	research::configureSh4ObservationCaptureRuntime();
	EXPECT_TRUE(config::DynarecEnabled.get());
	EXPECT_TRUE(config::ResearchDynarecObservation.get());
	research::startSh4ObservationCaptureRuntime();
	EXPECT_TRUE(research::sh4ObservationCaptureRuntimeActive());
	Sh4Context context {};
	context.pc = 0x8c020002;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Dynarec,
			0x8c020000, 0x0009, 20, context);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Dynarec,
			0x8c020000, 0x0009, 21, context);
	research::stopSh4ObservationCaptureRuntime(true);

	const research::IdentityManifest identity =
			research::loadIdentityManifest(identityPath);
	research::Sh4ObservationTraceBinding binding;
	binding.backend = research::Sh4ObservationBackend::Dynarec;
	binding.identityDigest = identity.digest;
	const std::string replayBytes = "typed replay fixture";
	const std::string manifestSetBytes = "{\"fixture\":true}";
	binding.replayDigest = research::sha256(replayBytes.data(), replayBytes.size());
	binding.manifestSetDigest = research::sha256(manifestSetBytes.data(),
			manifestSetBytes.size());
	const auto summary = research::validateSh4ObservationTraceFile(outputPath,
			binding, 1024 * 1024, 100);
	EXPECT_EQ(2u, summary.eventCount);
}

TEST(ResearchSh4ObservationCaptureRuntime,
		ChangedImmutableReplayRejectsCleanPublication)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const auto identityPath = directory.file("identity-v2.json");
	const auto replayPath = directory.file("replay.fcmt");
	const auto manifestSetPath = directory.file("manifest-set.json");
	const auto outputPath = directory.file("interpreter.fcso");
	writeText(identityPath, identityV2("interpreter", false,
			std::string(64, '1')).dump());
	writeText(replayPath, "typed replay fixture");
	writeText(manifestSetPath, "{\"fixture\":true}");

	config::ResearchIdentityManifestPath.set(identityPath.u8string());
	config::ResearchMapleReplayPath.set(replayPath.u8string());
	config::ResearchSh4ObservationManifestSetPath.set(
			manifestSetPath.u8string());
	config::ResearchSh4ObservationRecordPath.set(outputPath.u8string());
	config::ResearchSh4ObservationMaxBytes.set(1024 * 1024);
	config::ResearchMapleTraceMaxBytes.set(1024 * 1024);

	research::configureSh4ObservationCaptureRuntime();
	research::startSh4ObservationCaptureRuntime();
	writeText(replayPath, "changed replay fixture");
	EXPECT_THROW(research::stopSh4ObservationCaptureRuntime(true),
			std::runtime_error);
	EXPECT_FALSE(research::sh4ObservationCaptureRuntimeActive());

	research::Sh4ObservationTraceBinding binding;
	binding.backend = research::Sh4ObservationBackend::Interpreter;
	EXPECT_THROW(research::validateSh4ObservationTraceFile(outputPath,
			binding, 1024 * 1024, 100), std::runtime_error);
}

TEST(ResearchSh4ObservationCaptureRuntime,
		WriterFailureIsLatchedUntilCleanStopAndLeavesIncompleteTrace)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const auto identityPath = directory.file("identity-v2.json");
	const auto replayPath = directory.file("replay.fcmt");
	const auto manifestSetPath = directory.file("manifest-set.json");
	const auto outputPath = directory.file("interpreter.fcso");
	writeText(identityPath, identityV2("interpreter", false,
			std::string(64, '1')).dump());
	writeText(replayPath, "typed replay fixture");
	writeText(manifestSetPath, "{\"fixture\":true}");

	config::ResearchIdentityManifestPath.set(identityPath.u8string());
	config::ResearchMapleReplayPath.set(replayPath.u8string());
	config::ResearchSh4ObservationManifestSetPath.set(
			manifestSetPath.u8string());
	config::ResearchSh4ObservationRecordPath.set(outputPath.u8string());
	config::ResearchSh4ObservationMaxBytes.set(
			research::Sh4ObservationTraceHeaderSize
			+ research::Sh4ObservationTraceEventSize);
	config::ResearchMapleTraceMaxBytes.set(1024 * 1024);

	research::configureSh4ObservationCaptureRuntime();
	research::startSh4ObservationCaptureRuntime();
	Sh4Context context {};
	context.pc = 0x8c030002;
	EXPECT_NO_THROW(research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter,
			0x8c030000, 0x0009, 30, context));
	EXPECT_NO_THROW(research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter,
			0x8c030000, 0x0009, 31, context));
	EXPECT_THROW(research::stopSh4ObservationCaptureRuntime(true),
			std::runtime_error);
	EXPECT_FALSE(research::sh4ObservationCaptureRuntimeActive());

	research::Sh4ObservationTraceBinding binding;
	binding.backend = research::Sh4ObservationBackend::Interpreter;
	EXPECT_THROW(research::validateSh4ObservationTraceFile(outputPath,
			binding, 1024 * 1024, 100), std::runtime_error);
}

TEST(ResearchSh4ObservationCaptureRuntime,
		AbortLeavesIncompleteTrace)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const auto identityPath = directory.file("identity-v2.json");
	const auto replayPath = directory.file("replay.fcmt");
	const auto manifestSetPath = directory.file("manifest-set.json");
	const auto outputPath = directory.file("interpreter.fcso");
	writeText(identityPath, identityV2("interpreter", false,
			std::string(64, '1')).dump());
	writeText(replayPath, "typed replay fixture");
	writeText(manifestSetPath, "{\"fixture\":true}");

	config::ResearchIdentityManifestPath.set(identityPath.u8string());
	config::ResearchMapleReplayPath.set(replayPath.u8string());
	config::ResearchSh4ObservationManifestSetPath.set(
			manifestSetPath.u8string());
	config::ResearchSh4ObservationRecordPath.set(outputPath.u8string());
	config::ResearchSh4ObservationMaxBytes.set(1024 * 1024);
	config::ResearchMapleTraceMaxBytes.set(1024 * 1024);

	research::configureSh4ObservationCaptureRuntime();
	research::startSh4ObservationCaptureRuntime();
	Sh4Context context {};
	context.pc = 0x8c040002;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter,
			0x8c040000, 0x0009, 40, context);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter,
			0x8c040000, 0x0009, 41, context);
	research::abortSh4ObservationCaptureRuntime();
	EXPECT_FALSE(research::sh4ObservationCaptureRuntimeActive());

	research::Sh4ObservationTraceBinding binding;
	binding.backend = research::Sh4ObservationBackend::Interpreter;
	EXPECT_THROW(research::validateSh4ObservationTraceFile(outputPath,
			binding, 1024 * 1024, 100), std::runtime_error);
}

TEST(ResearchSh4ObservationCaptureRuntime,
		RejectsNegativeReplayByteLimitLocally)
{
	RuntimeReset reset;
	TemporaryDirectory directory;
	const auto identityPath = directory.file("identity-v2.json");
	const auto replayPath = directory.file("replay.fcmt");
	const auto manifestSetPath = directory.file("manifest-set.json");
	const auto outputPath = directory.file("interpreter.fcso");
	writeText(identityPath, identityV2("interpreter", false,
			std::string(64, '1')).dump());
	writeText(replayPath, "typed replay fixture");
	writeText(manifestSetPath, "{\"fixture\":true}");

	config::ResearchIdentityManifestPath.set(identityPath.u8string());
	config::ResearchMapleReplayPath.set(replayPath.u8string());
	config::ResearchSh4ObservationManifestSetPath.set(
			manifestSetPath.u8string());
	config::ResearchSh4ObservationRecordPath.set(outputPath.u8string());
	config::ResearchSh4ObservationMaxBytes.set(1024 * 1024);
	config::ResearchMapleTraceMaxBytes.set(-1);

	EXPECT_THROW(research::configureSh4ObservationCaptureRuntime(),
			std::runtime_error);
	EXPECT_FALSE(std::filesystem::exists(outputPath));
}

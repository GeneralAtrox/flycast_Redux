#include "cfg/option.h"
#include "hw/sh4/sh4_if.h"
#include "json.hpp"
#include "research/sha256.h"
#include "research/sh4_observation_runtime.h"
#include "research/sh4_pc_checkpoint_runtime.h"
#include "ResearchRuntimeStubs.h"
#include "types.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{

using json = nlohmann::json;

std::string zeroDigest()
{
	return std::string(64, '0');
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(output.good());
	output.write(text.data(), static_cast<std::streamsize>(text.size()));
	ASSERT_TRUE(output.good());
}

class ResearchSh4PcCheckpointRuntime : public testing::Test
{
protected:
	void bindIdentity(std::uint32_t pc, bool dynarec = false,
			std::uint32_t gateAddress = 0, std::uint32_t gateValue = 0)
	{
		json values {
			{"cpu_backend", dynarec ? "dynarec" : "interpreter"},
			{"dynarec_observation", dynarec},
			{"dreamcast_rtc_seed", 0},
			{"threaded_rendering", false},
			{"autoload_state", false},
			{"autosave_state", false},
			{"ggpo", false},
			{"maple_dma_checkpoint", 0},
			{"sh4_pc_checkpoint", pc},
		};
		if (gateAddress != 0)
		{
			values["sh4_pc_checkpoint_u32_address"] = gateAddress;
			values["sh4_pc_checkpoint_u32_value"] = gateValue;
		}
		const std::string canonicalValues = values.dump();
		const json blob {
			{"path", "descriptive-only.bin"},
			{"size", 0},
			{"sha256", zeroDigest()},
		};
		json track = blob;
		track["track"] = 1;
		track["start_fad"] = 150;
		track["sector_size"] = 2048;
		track["offset"] = 0;
		const json root {
			{"schema", "flycast-research-identity"},
			{"schema_version", 2},
			{"media", {
				{"kind", "gdi"},
				{"source", blob},
				{"tracks", json::array({track})},
				{"ip_bin", blob},
				{"boot_executable", {
					{"name", "1ST_READ.BIN"},
					{"path", "1ST_READ.BIN"},
					{"size", 0},
					{"sha256", zeroDigest()},
				}},
			}},
			{"firmware", {
				{"mode", "hle"},
				{"bios", nullptr},
				{"hle_identity", "reios:test"},
				{"flash_initial", blob},
			}},
			{"persistent_devices", json::array()},
			{"emulator", {
				{"git_commit", std::string(40, '0')},
				{"executable", blob},
			}},
			{"configuration", {
				{"values", values},
				{"sha256", research::sha256ToHex(research::sha256(
						canonicalValues.data(), canonicalValues.size()))},
			}},
			{"equivalence", {{"maple_replay_identity_sha256", zeroDigest()}}},
		};
		const std::filesystem::path path = directory / "identity.json";
		writeText(path, root.dump(2));
		config::ResearchIdentityManifestPath.set(path.string());
	}

	void clearRecorderOutputs()
	{
		config::ResearchMapleRecordPath.set("");
		config::ResearchMemoryRangesRecordPath.set("");
		config::ResearchSh4EventsRecordPath.set("");
		config::ResearchSh4ObservationRecordPath.set("");
		config::ResearchSh4ProfileRecordPath.set("");
		config::ResearchPvrTaRecordPath.set("");
		config::ResearchPvrPresentationRecordPath.set("");
		config::ResearchPvrDrawRecordPath.set("");
		config::ResearchGdromRecordPath.set("");
		config::ResearchAicaRecordPath.set("");
		config::ResearchCddaRecordPath.set("");
	}

	void SetUp() override
	{
		static std::atomic<unsigned> sequence {0};
		directory = std::filesystem::temp_directory_path()
				/ ("flycast-sh4-pc-checkpoint-test-"
						+ std::to_string(std::chrono::steady_clock::now()
								.time_since_epoch().count()) + "-"
						+ std::to_string(sequence++));
		std::filesystem::create_directory(directory);
		research::stopSh4PcCheckpointRuntime();
		config::ResearchSh4PcCheckpoint.set(0);
		config::ResearchSh4PcCheckpointU32Address.set(0);
		config::ResearchSh4PcCheckpointU32Value.set(0);
		clearRecorderOutputs();
		config::DynarecEnabled.set(false);
		config::ResearchDynarecObservation.set(false);
		config::ThreadedRendering.set(false);
		bindIdentity(0x8c010000);
	}

	void TearDown() override
	{
		research::stopSh4PcCheckpointRuntime();
		config::ResearchSh4PcCheckpoint.set(0);
		config::ResearchSh4PcCheckpointU32Address.set(0);
		config::ResearchSh4PcCheckpointU32Value.set(0);
		clearRecorderOutputs();
		config::DynarecEnabled.set(true);
		config::ResearchDynarecObservation.set(false);
		config::ThreadedRendering.set(true);
		config::ResearchIdentityManifestPath.set("");
		std::error_code error;
		std::filesystem::remove_all(directory, error);
	}

	std::filesystem::path directory;
};

TEST_F(ResearchSh4PcCheckpointRuntime, RequiresTypedRecorderAndAlignedGuestPc)
{
	config::ResearchSh4PcCheckpoint.set(0x8c010001);
	EXPECT_THROW(research::configureSh4PcCheckpointRuntime(), FlycastException);

	config::ResearchSh4PcCheckpoint.set(0x8c010000);
	EXPECT_THROW(research::configureSh4PcCheckpointRuntime(), FlycastException);

	config::ResearchMapleRecordPath.set("capture.fcmt");
	EXPECT_NO_THROW(research::configureSh4PcCheckpointRuntime());

	config::ThreadedRendering.set(true);
	EXPECT_THROW(research::configureSh4PcCheckpointRuntime(), FlycastException);
}

TEST_F(ResearchSh4PcCheckpointRuntime, DynarecRequiresExistingTypedMarkers)
{
	config::ResearchSh4PcCheckpoint.set(0x8c010000);
	config::ResearchSh4ObservationRecordPath.set("capture.fcsh4obs");
	config::DynarecEnabled.set(true);
	EXPECT_THROW(research::configureSh4PcCheckpointRuntime(), FlycastException);

	config::ResearchDynarecObservation.set(true);
	bindIdentity(0x8c010000, true);
	EXPECT_NO_THROW(research::configureSh4PcCheckpointRuntime());
}

TEST_F(ResearchSh4PcCheckpointRuntime, RejectsIdentityRuntimePcMismatch)
{
	config::ResearchSh4PcCheckpoint.set(0x8c010100);
	config::ResearchMapleRecordPath.set("capture.fcmt");
	EXPECT_THROW(research::configureSh4PcCheckpointRuntime(), FlycastException);
}

TEST_F(ResearchSh4PcCheckpointRuntime, FiresOnceAfterMatchingTopLevelInstruction)
{
	config::ResearchSh4PcCheckpoint.set(0x8c010100);
	config::ResearchMapleRecordPath.set("capture.fcmt");
	bindIdentity(0x8c010100);
	research::configureSh4PcCheckpointRuntime();
	unsigned calls = 0;
	research::startSh4PcCheckpointRuntime([&calls] { ++calls; });

	Sh4Context context {};
	context.pc = 0x8c010002;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter, 0x8c010000, 0x0009,
			1, context);
	context.pc = 0x8c010102;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter, 0x8c010100, 0x0009,
			2, context);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter, 0x8c010100, 0x0009,
			3, context);
	EXPECT_EQ(0u, calls);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter, 0x8c010000, 0x0009,
			4, context);
	EXPECT_EQ(0u, calls);

	context.pc = 0x8c010102;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter, 0x8c010100, 0x0009,
			5, context);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter, 0x8c010100, 0x0009,
			6, context);
	EXPECT_EQ(1u, calls);
	EXPECT_TRUE(research::sh4PcCheckpointRuntimeTriggered());
	EXPECT_FALSE(research::sh4PcCheckpointRuntimeActive());

	research::sh4PcCheckpointInstructionEnd(
			research::Sh4ObservationBackend::Interpreter, 0x8c010100);
	EXPECT_EQ(1u, calls);
}

TEST_F(ResearchSh4PcCheckpointRuntime,
		DeferredSh4RecorderCannotTerminateBeforeItsObservationBusStarts)
{
	using Backend = research::Sh4ObservationBackend;
	config::ResearchSh4PcCheckpoint.set(0x8c010100);
	config::ResearchSh4ObservationRecordPath.set("capture.fcso");
	bindIdentity(0x8c010100);
	research::configureSh4PcCheckpointRuntime();
	unsigned calls = 0;
	research::startSh4PcCheckpointRuntime([&calls] { ++calls; });

	Sh4Context context {};
	context.pc = 0x8c010102;
	research::sh4ObservationInstructionBegin(Backend::Interpreter,
			0x8c010100, 0x0009, 1, context);
	research::sh4ObservationInstructionEnd(Backend::Interpreter,
			0x8c010100, 0x0009, 2, context);
	EXPECT_EQ(0u, calls);
	EXPECT_TRUE(research::sh4PcCheckpointRuntimeActive());

	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Interpreter);
	const research::Sh4ObservationSubscription subscription =
			research::subscribeSh4Observations(filter,
					[](const research::Sh4Observation&) {});
	context.pc = 0x8c010102;
	research::sh4ObservationInstructionBegin(Backend::Interpreter,
			0x8c010100, 0x0009, 3, context);
	research::sh4ObservationInstructionEnd(Backend::Interpreter,
			0x8c010100, 0x0009, 4, context);
	research::unsubscribeSh4Observations(subscription);

	EXPECT_EQ(1u, calls);
	EXPECT_TRUE(research::sh4PcCheckpointRuntimeTriggered());
	EXPECT_FALSE(research::sh4PcCheckpointRuntimeActive());
}

TEST_F(ResearchSh4PcCheckpointRuntime,
		AuthenticatedGuestU32GateDefersTheMatchingPcUntilGuestStateMatches)
{
	constexpr std::uint32_t pc = 0x8c010100;
	constexpr std::uint32_t gateAddress = 0x8c001000;
	constexpr std::uint32_t gateValue = 1;
	config::ResearchSh4PcCheckpoint.set(pc);
	config::ResearchSh4PcCheckpointU32Address.set(gateAddress);
	config::ResearchSh4PcCheckpointU32Value.set(gateValue);
	config::ResearchMapleRecordPath.set("capture.fcmt");
	bindIdentity(pc, false, gateAddress, gateValue);
	research_test::clearGuestRam();
	research::configureSh4PcCheckpointRuntime();
	unsigned calls = 0;
	research::startSh4PcCheckpointRuntime([&calls] { ++calls; });

	research::sh4PcCheckpointInstructionEnd(
			research::Sh4ObservationBackend::Interpreter, pc);
	EXPECT_EQ(0u, calls);
	EXPECT_TRUE(research::sh4PcCheckpointRuntimeActive());

	ASSERT_TRUE(research_test::writeGuestRam(gateAddress, {1, 0, 0, 0}));
	research::sh4PcCheckpointInstructionEnd(
			research::Sh4ObservationBackend::Interpreter, pc);
	EXPECT_EQ(1u, calls);
	EXPECT_TRUE(research::sh4PcCheckpointRuntimeTriggered());
	EXPECT_FALSE(research::sh4PcCheckpointRuntimeActive());
}

} // namespace

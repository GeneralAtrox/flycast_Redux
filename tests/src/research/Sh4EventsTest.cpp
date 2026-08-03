#include <gtest/gtest.h>

#include "json.hpp"
#include "cfg/option.h"
#include "hw/sh4/sh4_if.h"
#include "research/identity_manifest.h"
#include "research/sh4_events_artifact.h"
#include "research/sh4_events_capture.h"
#include "research/sh4_events_manifest.h"
#include "research/sh4_events_runtime.h"
#include "research/sh4_observation.h"
#include "research/sha256.h"
#include "ResearchRuntimeStubs.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <string>
#include <vector>

namespace
{

using json = nlohmann::json;

class Sh4EventsTemporaryDirectory
{
public:
	Sh4EventsTemporaryDirectory()
	{
		static std::atomic<unsigned> sequence {0};
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path = std::filesystem::temp_directory_path()
				/ ("flycast-research-sh4-events-test-" + std::to_string(stamp) + "-"
						+ std::to_string(sequence++));
		std::filesystem::create_directory(path);
		clearOptions();
	}

	~Sh4EventsTemporaryDirectory()
	{
		research::abortSh4EventsRuntime();
		clearOptions();
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	std::filesystem::path file(const char *name) const { return path / name; }

private:
	static void clearOptions()
	{
		config::ResearchIdentityManifestPath = "";
		config::ResearchMapleRecordPath = "";
		config::ResearchMapleReplayPath = "";
		config::ResearchMemoryRangesManifestPath = "";
		config::ResearchMemoryRangesRecordPath = "";
		config::ResearchSh4EventsManifestPath = "";
		config::ResearchSh4EventsRecordPath = "";
		config::AutoLoadState.override(false);
		config::SavestateSlot.override(0);
	}

	std::filesystem::path path;
};

void writeBytes(const std::filesystem::path& path,
		const std::vector<std::uint8_t>& bytes)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(output.good());
	output.write(reinterpret_cast<const char *>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
	ASSERT_TRUE(output.good());
}

void writeText(const std::filesystem::path& path, const std::string& text)
{
	writeBytes(path, std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::vector<std::uint8_t> readBytes(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input)
		return {};
	return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input), {});
}

std::string digestOf(const std::string& value)
{
	return research::sha256ToHex(research::sha256(value.data(), value.size()));
}

std::string digestOf(const std::vector<std::uint8_t>& value)
{
	return research::sha256ToHex(research::sha256(value.data(), value.size()));
}

struct Sh4FixtureData
{
	std::vector<std::uint8_t> callBytes {0xde, 0xad, 0xbe, 0xef};
	std::vector<std::uint8_t> returnBytes {0x10, 0x32, 0x54, 0x76, 0x98};
	std::string executableDigest = digestOf("SH-4 fixture executable");
	std::string staticAnalysisDigest = digestOf("SH-4 fixture static analysis");
	std::string hookManifestDigest = digestOf("SH-4 fixture hook manifest");
};

json identityJson(const Sh4FixtureData& fixture)
{
	json values {
		{"cpu_backend", "interpreter"},
		{"threaded_rendering", false},
		{"autoload_state", false},
		{"autosave_state", false},
		{"ggpo", false},
	};
	const std::string canonicalValues = values.dump();
	const std::string valuesDigest = digestOf(canonicalValues);
	const json blob {
		{"path", "descriptive-only.bin"},
		{"size", 0},
		{"sha256", std::string(64, '0')},
	};
	json root {
		{"schema", "flycast-research-identity"},
		{"schema_version", 1},
		{"media", {
			{"kind", "gdi"},
			{"source", blob},
			{"ip_bin", blob},
			{"boot_executable", {
				{"name", "1ST_READ.BIN"},
				{"path", "1ST_READ.BIN"},
				{"size", 1234},
				{"sha256", fixture.executableDigest},
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
			{"sha256", valuesDigest},
		}},
		{"static_analysis", {
			{"program_sha256", fixture.executableDigest},
			{"image_base", "0x8c010000"},
			{"export_sha256", fixture.staticAnalysisDigest},
			{"hook_manifest_sha256", fixture.hookManifestDigest},
		}},
	};
	json track = blob;
	track["track"] = 1;
	track["start_fad"] = 150;
	track["sector_size"] = 2048;
	track["offset"] = 0;
	root["media"]["tracks"] = json::array({track});
	return root;
}

json stateIdentityJson(const Sh4FixtureData& fixture,
		const std::filesystem::path& statePath,
		const std::vector<std::uint8_t>& stateBytes, std::uint32_t slot)
{
	json root = identityJson(fixture);
	root["schema_version"] = 2;
	root["initial_state"] = {
		{"kind", "savestate"},
		{"slot", slot},
		{"blob", {
			{"path", statePath.u8string()},
			{"size", stateBytes.size()},
			{"sha256", digestOf(stateBytes)},
		}},
	};
	root["equivalence"] = {
		{"maple_replay_identity_sha256", std::string(64, '1')},
	};
	json& values = root["configuration"]["values"];
	values["dynarec_observation"] = false;
	values["dreamcast_rtc_seed"] = 0;
	values["autoload_state"] = true;
	values["savestate_slot"] = slot;
	root["configuration"]["sha256"] = digestOf(values.dump());
	return root;
}

json manifestJson(const Sh4FixtureData& fixture)
{
	return json {
		{"schema", "flycast-research-sh4-events-manifest"},
		{"schema_version", 1},
		{"manifest_id", "FIXTURE-SH4-EVENTS-V1"},
		{"address_space", "flycast-sh4-virtual"},
		{"bindings", {
			{"executable_sha256", fixture.executableDigest},
			{"static_analysis_id", "STATIC-FIXTURE-V1"},
			{"static_analysis_sha256", fixture.staticAnalysisDigest},
			{"hook_manifest_id", "HOOKS-FIXTURE-V1"},
			{"hook_manifest_sha256", fixture.hookManifestDigest},
		}},
		{"hooks", json::array({{
			{"hook_id", "HOOK-MAIN"},
			{"entry_pc", "0x8c010100"},
			{"end_address_exclusive", "0x8c010120"},
			{"snapshots", json::array({
				{
					{"snapshot_id", "CALL-ABSOLUTE"},
					{"phase", "call"},
					{"source", {{"kind", "absolute"}, {"address", "0x8c001000"}}},
					{"length", fixture.callBytes.size()},
					{"required", true},
				},
				{
					{"snapshot_id", "CALL-OPTIONAL-INVALID"},
					{"phase", "call"},
					{"source", {{"kind", "register-relative"},
							{"register_index", 15}, {"offset", -1}}},
					{"length", 3},
					{"required", false},
				},
				{
					{"snapshot_id", "RETURN-R4"},
					{"phase", "return"},
					{"source", {{"kind", "register-relative"},
							{"register_index", 4}, {"offset", 4}}},
					{"length", fixture.returnBytes.size()},
					{"required", true},
				},
			})},
		}})},
		{"watch_ranges", json::array({{
			{"watch_id", "WATCH-MAIN"},
			{"address", "0x8c002000"},
			{"length", 16},
			{"access", json::array({"read", "write"})},
		}})},
		{"limits", {
			{"maximum_events", 32},
			{"maximum_snapshot_bytes_per_event", 64},
			{"maximum_total_snapshot_bytes", 128},
			{"maximum_open_invocations", 8},
		}},
		{"acceptance", {
			{"backend", "interpreter"},
			{"minimum_call_events", 1},
			{"minimum_watch_events", 3},
			{"require_balanced_calls", true},
			{"zero_dropped_events", true},
			{"natural_exit", true},
		}},
	};
}

std::filesystem::path writeIdentity(const Sh4EventsTemporaryDirectory& directory,
		const Sh4FixtureData& fixture, const char *name = "identity.json")
{
	const std::filesystem::path path = directory.file(name);
	writeText(path, identityJson(fixture).dump(2));
	return path;
}

std::filesystem::path writeManifest(const Sh4EventsTemporaryDirectory& directory,
		const Sh4FixtureData& fixture, const char *name = "manifest.json")
{
	const std::filesystem::path path = directory.file(name);
	writeText(path, manifestJson(fixture).dump(2));
	return path;
}

using FixtureMemory = std::map<std::uint32_t, std::vector<std::uint8_t>>;

FixtureMemory fixtureMemory(const Sh4FixtureData& fixture)
{
	return {
		{0x8c001000, fixture.callBytes},
		{0x8c001100, fixture.returnBytes},
	};
}

research::Sh4GuestMemoryReader mapReader(const FixtureMemory& memory)
{
	return [&memory](std::uint32_t address, std::uint32_t length)
			-> const std::uint8_t * {
		const auto found = memory.find(address);
		if (found == memory.end() || found->second.size() != length)
			return nullptr;
		return found->second.data();
	};
}

research::Sh4InstructionState instruction(std::uint32_t pc, std::uint16_t opcode,
		std::uint64_t tick)
{
	research::Sh4InstructionState state;
	state.pc = pc;
	state.nextPc = pc + 2;
	state.opcode = opcode;
	state.tick = tick;
	for (std::size_t index = 0; index < state.registers.r.size(); ++index)
		state.registers.r[index] = 0x10000000u + static_cast<std::uint32_t>(index);
	state.registers.pr = 0x8c0f0000;
	state.registers.gbr = 0x8c000100;
	state.registers.vbr = 0x8c000000;
	state.registers.mach = 0x11111111;
	state.registers.macl = 0x22222222;
	state.registers.sr = 0x40000001;
	state.registers.fpul = 0x33333333;
	state.registers.fpscr = 0x00040001;
	return state;
}

research::Sh4EventsArtifactSummary recordFixture(
		const std::filesystem::path& artifactPath,
		const research::IdentityManifest& identity,
		const research::Sh4EventsManifest& manifest,
		const Sh4FixtureData& fixture)
{
	const FixtureMemory memory = fixtureMemory(fixture);
	research::Sh4EventsCapture capture(artifactPath, identity, manifest);

	auto call = instruction(0x8c020000, 0x410b, 100); // JSR @R1
	call.registers.r[1] = 0x8c010100;
	call.registers.r[15] = 0;
	capture.beginInstruction(call, mapReader(memory));
	auto delay = instruction(0x8c020002, 0x0009, 101);
	capture.beginInstruction(delay, mapReader(memory));
	capture.observeMemoryAccess(0x8c00200e, 4,
			research::Sh4MemoryAccessKind::Read, 0x12345678);
	delay.nextPc = 0x8c020004;
	delay.tick = 102;
	capture.endInstruction(delay, mapReader(memory));
	call.nextPc = 0x8c010100;
	call.tick = 103;
	capture.endInstruction(call, mapReader(memory));

	auto body = instruction(0x8c010100, 0x6012, 110);
	capture.beginInstruction(body, mapReader(memory));
	capture.observeMemoryAccess(0x8c002000, 8,
			research::Sh4MemoryAccessKind::Read, 0x0123456789abcdefull);
	capture.observeMemoryAccess(0x8c002008, 2,
			research::Sh4MemoryAccessKind::Write, 0xbeef);
	body.nextPc = 0x8c010102;
	body.tick = 111;
	capture.endInstruction(body, mapReader(memory));

	auto faultOwner = instruction(0x8c010102, 0xa000, 112);
	capture.beginInstruction(faultOwner, mapReader(memory));
	auto fault = instruction(0x8c010104, 0xffff, 113);
	capture.beginInstruction(fault, mapReader(memory));
	capture.observeException(fault.pc, fault.registers.vbr + 0x100, 0x180, 114,
			fault.registers);
	capture.abortInstruction();
	capture.abortInstruction();

	auto returned = instruction(0x8c010110, 0x000b, 120);
	returned.registers.r[4] = 0x8c0010fc;
	capture.beginInstruction(returned, mapReader(memory));
	returned.nextPc = 0x8c020004;
	returned.registers.r[0] = 0xcafebabe;
	returned.tick = 124;
	capture.endInstruction(returned, mapReader(memory));
	return capture.finish();
}

void recordRuntimeFixture(Sh4Context& context)
{
	context.r[1] = 0x8c010100;
	context.r[15] = 0;
	context.vbr = 0x8c000000;
	context.pc = 0x8c020002;
	research::sh4EventsInstructionBegin(0x8c020000, 0x410b, 100, context);
	research::sh4EventsInstructionBegin(0x8c020002, 0x0009, 101, context);
	research::sh4EventsMemoryAccess(0x8c00200e, 4,
			research::Sh4MemoryAccessKind::Read, 0x12345678);
	context.pc = 0x8c020004;
	research::sh4EventsInstructionEnd(0x8c020002, 0x0009, 102, context);
	context.pc = 0x8c010100;
	research::sh4EventsInstructionEnd(0x8c020000, 0x410b, 103, context);

	research::sh4EventsInstructionBegin(0x8c010100, 0x6012, 110, context);
	research::sh4EventsMemoryAccess(0x8c002000, 8,
			research::Sh4MemoryAccessKind::Read, 0x0123456789abcdefull);
	research::sh4EventsMemoryAccess(0x8c002008, 2,
			research::Sh4MemoryAccessKind::Write, 0xbeef);
	context.pc = 0x8c010102;
	research::sh4EventsInstructionEnd(0x8c010100, 0x6012, 111, context);

	research::sh4EventsInstructionBegin(0x8c010102, 0xa000, 112, context);
	research::sh4EventsInstructionBegin(0x8c010104, 0xffff, 113, context);
	research::sh4EventsException(0x8c010104, context.vbr + 0x100, 0x180, 114,
			context);
	research::sh4EventsInstructionAbort();
	research::sh4EventsInstructionAbort();

	context.r[4] = 0x8c0010fc;
	research::sh4EventsInstructionBegin(0x8c010110, 0x000b, 120, context);
	context.pc = 0x8c020004;
	context.r[0] = 0xcafebabe;
	research::sh4EventsInstructionEnd(0x8c010110, 0x000b, 124, context);
}

} // namespace

TEST(ResearchSh4Events, ManifestValidatesSemanticsAndExactIdentityBindings)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	const auto manifestPath = writeManifest(directory, fixture);
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const research::Sh4EventsManifest manifest =
			research::loadSh4EventsManifest(manifestPath);

	EXPECT_EQ("FIXTURE-SH4-EVENTS-V1", manifest.id);
	ASSERT_EQ(1u, manifest.hooks.size());
	EXPECT_EQ(0x8c010100u, manifest.hooks[0].entryPc);
	EXPECT_EQ(3u, manifest.hooks[0].snapshots.size());
	ASSERT_EQ(1u, manifest.watchRanges.size());
	EXPECT_EQ(research::Sh4WatchRead | research::Sh4WatchWrite,
			manifest.watchRanges[0].access);
	EXPECT_NO_THROW(research::requireSh4EventsIdentity(manifest, identity));

	Sh4FixtureData wrongFixture = fixture;
	wrongFixture.executableDigest = digestOf("different executable");
	const research::IdentityManifest wrongIdentity = research::loadIdentityManifest(
			writeIdentity(directory, wrongFixture, "wrong-identity.json"));
	EXPECT_THROW(research::requireSh4EventsIdentity(manifest, wrongIdentity),
			std::runtime_error);
}

TEST(ResearchSh4Events, ManifestRejectsDuplicateUnknownOverlapAndImpossibleLimits)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const json valid = manifestJson(fixture);
	const auto invalidPath = directory.file("invalid.json");

	std::string duplicate = valid.dump();
	duplicate.insert(1, "\"schema\":\"flycast-research-sh4-events-manifest\",");
	writeText(invalidPath, duplicate);
	EXPECT_THROW(research::loadSh4EventsManifest(invalidPath), std::runtime_error);

	json changed = valid;
	changed["unexpected"] = true;
	writeText(invalidPath, changed.dump());
	EXPECT_THROW(research::loadSh4EventsManifest(invalidPath), std::runtime_error);

	changed = valid;
	json overlappingHook = changed["hooks"][0];
	overlappingHook["hook_id"] = "HOOK-OVERLAP";
	overlappingHook["entry_pc"] = "0x8c010110";
	overlappingHook["end_address_exclusive"] = "0x8c010130";
	overlappingHook["snapshots"] = json::array();
	changed["hooks"].push_back(overlappingHook);
	writeText(invalidPath, changed.dump());
	EXPECT_THROW(research::loadSh4EventsManifest(invalidPath), std::runtime_error);

	changed = valid;
	json overlappingWatch = changed["watch_ranges"][0];
	overlappingWatch["watch_id"] = "WATCH-OVERLAP";
	overlappingWatch["address"] = "0x8c002008";
	changed["watch_ranges"].push_back(overlappingWatch);
	writeText(invalidPath, changed.dump());
	EXPECT_THROW(research::loadSh4EventsManifest(invalidPath), std::runtime_error);

	changed = valid;
	changed["watch_ranges"][0]["access"] = json::array({"read", "read"});
	writeText(invalidPath, changed.dump());
	EXPECT_THROW(research::loadSh4EventsManifest(invalidPath), std::runtime_error);

	changed = valid;
	changed["limits"]["maximum_events"] = 4;
	writeText(invalidPath, changed.dump());
	EXPECT_THROW(research::loadSh4EventsManifest(invalidPath), std::runtime_error);

	changed = valid;
	changed["limits"]["maximum_snapshot_bytes_per_event"] = 3;
	writeText(invalidPath, changed.dump());
	EXPECT_THROW(research::loadSh4EventsManifest(invalidPath), std::runtime_error);
}

TEST(ResearchSh4Events, CapturesDelaySlotWatchesCallsReturnsSnapshotsAndException)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const research::IdentityManifest identity = research::loadIdentityManifest(
			writeIdentity(directory, fixture));
	const research::Sh4EventsManifest manifest = research::loadSh4EventsManifest(
			writeManifest(directory, fixture));
	const auto artifactPath = directory.file("events.fcsh4");
	const research::Sh4EventsArtifactSummary written =
			recordFixture(artifactPath, identity, manifest, fixture);

	EXPECT_EQ(6u, written.eventCount);
	EXPECT_EQ(1u, written.callCount);
	EXPECT_EQ(1u, written.returnCount);
	EXPECT_EQ(2u, written.watchReadCount);
	EXPECT_EQ(1u, written.watchWriteCount);
	EXPECT_EQ(3u, written.snapshotCount);
	EXPECT_EQ(9u, written.snapshotBytes);
	EXPECT_EQ(1u, written.exceptionCount);
	EXPECT_EQ(100u, written.startTick);
	EXPECT_EQ(124u, written.endTick);
	EXPECT_EQ(1u, written.maximumOpenInvocations);
	EXPECT_EQ(0u, written.droppedEvents);

	const research::Sh4EventsArtifactSummary validated =
			research::validateProductionSh4EventsArtifactFile(
					artifactPath, identity, manifest);
	EXPECT_TRUE(research::sha256Equal(written.payloadDigest, validated.payloadDigest));
	EXPECT_EQ(written.eventCount, validated.eventCount);
}

TEST(ResearchSh4Events, BinaryContractRemainsByteExact)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const research::IdentityManifest identity = research::loadIdentityManifest(
			writeIdentity(directory, fixture));
	const research::Sh4EventsManifest manifest = research::loadSh4EventsManifest(
			writeManifest(directory, fixture));
	const auto artifactPath = directory.file("locked.fcsh4");
	recordFixture(artifactPath, identity, manifest, fixture);
	EXPECT_EQ("f3513e69e6634983114574ea555f64693c3f0e0d4fef70780eba02e08a72cff0",
			digestOf(readBytes(artifactPath)));
}

TEST(ResearchSh4Events, DecodesBsrAndBsrfWithoutGuestPatching)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	json values = manifestJson(fixture);
	values["hooks"] = json::array({
		{
			{"hook_id", "HOOK-BSR"},
			{"entry_pc", "0x8c040010"},
			{"end_address_exclusive", "0x8c040020"},
			{"snapshots", json::array()},
		},
		{
			{"hook_id", "HOOK-BSRF"},
			{"entry_pc", "0x8c050104"},
			{"end_address_exclusive", "0x8c050120"},
			{"snapshots", json::array()},
		},
	});
	values["watch_ranges"] = json::array();
	values["limits"]["maximum_snapshot_bytes_per_event"] = 0;
	values["limits"]["maximum_total_snapshot_bytes"] = 0;
	values["acceptance"]["minimum_call_events"] = 2;
	values["acceptance"]["minimum_watch_events"] = 0;
	const auto manifestPath = directory.file("call-forms.json");
	writeText(manifestPath, values.dump(2));
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const research::Sh4EventsManifest manifest =
			research::loadSh4EventsManifest(manifestPath);
	const auto artifactPath = directory.file("call-forms.fcsh4");
	const FixtureMemory memory;
	{
		research::Sh4EventsCapture capture(artifactPath, identity, manifest);
		auto bsr = instruction(0x8c040000, 0xb006, 10);
		capture.beginInstruction(bsr, mapReader(memory));
		bsr.nextPc = 0x8c040010;
		bsr.tick = 11;
		capture.endInstruction(bsr, mapReader(memory));
		auto bsrReturn = instruction(0x8c040018, 0x000b, 12);
		capture.beginInstruction(bsrReturn, mapReader(memory));
		bsrReturn.nextPc = 0x8c040004;
		bsrReturn.tick = 13;
		capture.endInstruction(bsrReturn, mapReader(memory));

		auto bsrf = instruction(0x8c050000, 0x0203, 20);
		bsrf.registers.r[2] = 0x100;
		capture.beginInstruction(bsrf, mapReader(memory));
		bsrf.nextPc = 0x8c050104;
		bsrf.tick = 21;
		capture.endInstruction(bsrf, mapReader(memory));
		auto bsrfReturn = instruction(0x8c050110, 0x000b, 22);
		capture.beginInstruction(bsrfReturn, mapReader(memory));
		bsrfReturn.nextPc = 0x8c050004;
		bsrfReturn.tick = 23;
		capture.endInstruction(bsrfReturn, mapReader(memory));

		const auto summary = capture.finish();
		EXPECT_EQ(2u, summary.callCount);
		EXPECT_EQ(2u, summary.returnCount);
	}
	EXPECT_NO_THROW(research::validateProductionSh4EventsArtifactFile(
			artifactPath, identity, manifest));
}

TEST(ResearchSh4Events, ValidatorRejectsMutationIncompleteAndWrongBindings)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	const auto manifestPath = writeManifest(directory, fixture);
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const research::Sh4EventsManifest manifest =
			research::loadSh4EventsManifest(manifestPath);
	const auto artifactPath = directory.file("valid.fcsh4");
	recordFixture(artifactPath, identity, manifest, fixture);

	std::vector<std::uint8_t> mutated = readBytes(artifactPath);
	ASSERT_GT(mutated.size(), research::Sh4EventsArtifactHeaderSize);
	mutated.back() ^= 0x80;
	const auto mutatedPath = directory.file("mutated.fcsh4");
	writeBytes(mutatedPath, mutated);
	EXPECT_THROW(research::validateProductionSh4EventsArtifactFile(
			mutatedPath, identity, manifest), std::runtime_error);

	const auto incompletePath = directory.file("incomplete.fcsh4");
	{
		research::Sh4EventsCapture capture(incompletePath, identity, manifest);
		capture.abandon();
	}
	EXPECT_THROW(research::validateProductionSh4EventsArtifactFile(
			incompletePath, identity, manifest), std::runtime_error);

	Sh4FixtureData wrongFixture = fixture;
	wrongFixture.hookManifestDigest = digestOf("wrong hook manifest");
	const research::IdentityManifest wrongIdentity = research::loadIdentityManifest(
			writeIdentity(directory, wrongFixture, "wrong.json"));
	EXPECT_THROW(research::validateProductionSh4EventsArtifactFile(
			artifactPath, wrongIdentity, manifest), std::runtime_error);
	EXPECT_THROW(research::Sh4EventsCapture(artifactPath, identity, manifest),
			std::exception);
}

TEST(ResearchSh4Events, LimitsAndRequiredMemoryFailClosed)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const research::IdentityManifest identity = research::loadIdentityManifest(
			writeIdentity(directory, fixture));
	const research::Sh4EventsManifest manifest = research::loadSh4EventsManifest(
			writeManifest(directory, fixture));
	const auto missingPath = directory.file("missing.fcsh4");
	{
		research::Sh4EventsCapture capture(missingPath, identity, manifest);
		auto call = instruction(0x8c020000, 0x410b, 1);
		call.registers.r[1] = 0x8c010100;
		call.registers.r[15] = 0;
		EXPECT_THROW(capture.beginInstruction(call,
				[](std::uint32_t, std::uint32_t) -> const std::uint8_t * {
					return nullptr;
				}), std::runtime_error);
		capture.abortInstruction();
		EXPECT_THROW(capture.finish(), std::logic_error);
		capture.abandon();
	}
	EXPECT_THROW(research::validateProductionSh4EventsArtifactFile(
			missingPath, identity, manifest), std::runtime_error);

	json boundedJson = manifestJson(fixture);
	boundedJson["acceptance"]["minimum_watch_events"] = 1;
	boundedJson["limits"]["maximum_events"] = 4;
	const auto boundedManifestPath = directory.file("bounded.json");
	writeText(boundedManifestPath, boundedJson.dump(2));
	const research::Sh4EventsManifest boundedManifest =
			research::loadSh4EventsManifest(boundedManifestPath);
	const FixtureMemory memory = fixtureMemory(fixture);
	const auto boundedPath = directory.file("bounded.fcsh4");
	{
		research::Sh4EventsCapture capture(boundedPath, identity, boundedManifest);
		auto call = instruction(0x8c020000, 0x410b, 10);
		call.registers.r[1] = 0x8c010100;
		call.registers.r[15] = 0;
		capture.beginInstruction(call, mapReader(memory));
		call.nextPc = 0x8c010100;
		call.tick = 11;
		capture.endInstruction(call, mapReader(memory));
		auto body = instruction(0x8c010100, 0x0009, 12);
		capture.beginInstruction(body, mapReader(memory));
		capture.observeMemoryAccess(0x8c002000, 1,
				research::Sh4MemoryAccessKind::Read, 1);
		body.tick = 13;
		capture.endInstruction(body, mapReader(memory));
		auto returned = instruction(0x8c010110, 0x000b, 14);
		returned.registers.r[4] = 0x8c0010fc;
		capture.beginInstruction(returned, mapReader(memory));
		returned.nextPc = 0x8c020004;
		returned.tick = 15;
		capture.endInstruction(returned, mapReader(memory));
		auto after = instruction(0x8c020004, 0x0009, 16);
		capture.beginInstruction(after, mapReader(memory));
		capture.observeMemoryAccess(0x8c002001, 1,
				research::Sh4MemoryAccessKind::Read, 2);
		EXPECT_THROW(capture.observeMemoryAccess(0x8c002002, 1,
				research::Sh4MemoryAccessKind::Read, 3), std::runtime_error);
		capture.abortInstruction();
		capture.abandon();
	}
	EXPECT_THROW(research::validateProductionSh4EventsArtifactFile(
			boundedPath, identity, boundedManifest), std::runtime_error);

	json depthJson = manifestJson(fixture);
	depthJson["limits"]["maximum_open_invocations"] = 1;
	const auto depthManifestPath = directory.file("depth.json");
	writeText(depthManifestPath, depthJson.dump(2));
	const research::Sh4EventsManifest depthManifest =
			research::loadSh4EventsManifest(depthManifestPath);
	const auto depthPath = directory.file("depth.fcsh4");
	{
		research::Sh4EventsCapture capture(depthPath, identity, depthManifest);
		auto first = instruction(0x8c020000, 0x410b, 20);
		first.registers.r[1] = 0x8c010100;
		first.registers.r[15] = 0;
		capture.beginInstruction(first, mapReader(memory));
		first.nextPc = 0x8c010100;
		first.tick = 21;
		capture.endInstruction(first, mapReader(memory));
		auto nested = instruction(0x8c010104, 0x410b, 22);
		nested.registers.r[1] = 0x8c010100;
		nested.registers.r[15] = 0;
		EXPECT_THROW(capture.beginInstruction(nested, mapReader(memory)),
				std::runtime_error);
		capture.abortInstruction();
		capture.abandon();
	}
	EXPECT_THROW(research::validateProductionSh4EventsArtifactFile(
			depthPath, identity, depthManifest), std::runtime_error);
}

TEST(ResearchSh4Events, RuntimeRejectsAliasedResearchPathsBeforeOpeningOutput)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	const auto manifestPath = writeManifest(directory, fixture);
	const auto outputPath = directory.file("aliased.fcsh4");
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchSh4EventsManifestPath = manifestPath.string();
	config::ResearchSh4EventsRecordPath = outputPath.string();
	config::ResearchMemoryRangesRecordPath = outputPath.string();

	research::configureSh4EventsRuntime();
	EXPECT_THROW(research::startSh4EventsRuntime(), std::runtime_error);
	EXPECT_FALSE(std::filesystem::exists(outputPath));
	EXPECT_FALSE(research::sh4EventsRuntimeActive());
}

TEST(ResearchSh4Events, RuntimeUsesAuthenticatedIdentityBoundInitialState)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	constexpr std::uint32_t slot = 9;
	const std::filesystem::path loadedStatePath =
			"research-test-state-" + std::to_string(slot) + ".state";
	ASSERT_FALSE(std::filesystem::exists(loadedStatePath));
	struct RemoveStateFile
	{
		explicit RemoveStateFile(std::filesystem::path path) : path(std::move(path)) {}
		~RemoveStateFile()
		{
			std::error_code error;
			std::filesystem::remove(path, error);
		}
		std::filesystem::path path;
	} removeStateFile(loadedStatePath);
	const std::vector<std::uint8_t> stateBytes {0x46, 0x4c, 0x59, 0x53, 7, 8, 9};
	writeBytes(loadedStatePath, stateBytes);
	const auto identityPath = directory.file("state-identity.json");
	writeText(identityPath,
			stateIdentityJson(fixture, loadedStatePath, stateBytes, slot).dump(2));
	const auto manifestPath = writeManifest(directory, fixture);
	const auto outputPath = directory.file("state-runtime.fcsh4");
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchSh4EventsManifestPath = manifestPath.string();
	config::ResearchSh4EventsRecordPath = outputPath.string();

	research::configureSh4EventsRuntime();
	EXPECT_FALSE(config::AutoLoadState.get());
	research::startSh4EventsRuntime();
	EXPECT_TRUE(config::AutoLoadState.get());
	EXPECT_EQ(static_cast<int>(slot), config::SavestateSlot.get());
	research::abortSh4EventsRuntime();

	writeBytes(loadedStatePath, {0x46, 0x4c, 0x59, 0x53, 7, 8, 0});
	config::ResearchSh4EventsRecordPath = directory.file("mismatch.fcsh4").string();
	research::configureSh4EventsRuntime();
	EXPECT_THROW(research::startSh4EventsRuntime(), std::runtime_error);
	EXPECT_FALSE(research::sh4EventsRuntimeActive());
}

TEST(ResearchSh4Events, RuntimeArmsCapturesAndFinalizesOnlyOnCleanExit)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	const auto manifestPath = writeManifest(directory, fixture);
	const auto artifactPath = directory.file("runtime.fcsh4");
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const research::Sh4EventsManifest manifest =
			research::loadSh4EventsManifest(manifestPath);

	research_test::clearGuestRam();
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001000, fixture.callBytes));
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001100, fixture.returnBytes));
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchSh4EventsManifestPath = manifestPath.string();
	config::ResearchSh4EventsRecordPath = artifactPath.string();
	config::ResearchSh4EventsMaxBytes = research::DefaultMaximumSh4EventsArtifactBytes;

	research::configureSh4EventsRuntime();
	research::startSh4EventsRuntime();
	ASSERT_TRUE(research::sh4EventsRuntimeActive());
	Sh4Context context {};
	recordRuntimeFixture(context);
	research::stopSh4EventsRuntime(true);
	EXPECT_FALSE(research::sh4EventsRuntimeActive());

	const auto summary = research::validateProductionSh4EventsArtifactFile(
			artifactPath, identity, manifest);
	EXPECT_EQ(6u, summary.eventCount);
	EXPECT_EQ(0u, summary.droppedEvents);
}

TEST(ResearchSh4Events, AuxiliaryCallFailureCannotStrandRecorderScope)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	const auto manifestPath = writeManifest(directory, fixture);
	const auto artifactPath = directory.file("call-subscriber-failure.fcsh4");
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchSh4EventsManifestPath = manifestPath.string();
	config::ResearchSh4EventsRecordPath = artifactPath.string();
	config::ResearchSh4EventsMaxBytes = research::DefaultMaximumSh4EventsArtifactBytes;
	research::Sh4ObservationFilter callFilter;
	callFilter.typeMask = research::sh4ObservationTypeBit(
			research::Sh4ObservationType::Call);
	const auto failingSubscription = research::subscribeSh4Observations(callFilter,
			[](const research::Sh4Observation&) {
				throw std::runtime_error("fixture call subscriber failure");
			});

	research_test::clearGuestRam();
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001000, fixture.callBytes));
	research::configureSh4EventsRuntime();
	research::startSh4EventsRuntime();
	Sh4Context context {};
	context.r[1] = 0x8c010100;
	context.pc = 0x8c020002;
	EXPECT_THROW(research::sh4EventsInstructionBegin(
			0x8c020000, 0x410b, 100, context), std::runtime_error);
	EXPECT_TRUE(research::unsubscribeSh4Observations(failingSubscription));

	auto stop = std::async(std::launch::async, [] {
		research::stopSh4EventsRuntime(false);
	});
	ASSERT_EQ(std::future_status::ready, stop.wait_for(std::chrono::seconds(2)));
	EXPECT_NO_THROW(stop.get());
	EXPECT_FALSE(research::sh4EventsRuntimeActive());
}

TEST(ResearchSh4Events, RuntimeFinalizationWaitsForActiveInstruction)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	const auto manifestPath = writeManifest(directory, fixture);
	const auto artifactPath = directory.file("instruction-boundary.fcsh4");
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const research::Sh4EventsManifest manifest =
			research::loadSh4EventsManifest(manifestPath);

	research_test::clearGuestRam();
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001000, fixture.callBytes));
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001100, fixture.returnBytes));
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchSh4EventsManifestPath = manifestPath.string();
	config::ResearchSh4EventsRecordPath = artifactPath.string();
	config::ResearchSh4EventsMaxBytes = research::DefaultMaximumSh4EventsArtifactBytes;

	research::configureSh4EventsRuntime();
	research::startSh4EventsRuntime();
	Sh4Context context {};
	recordRuntimeFixture(context);
	context.pc = 0x8c030002;
	research::sh4EventsInstructionBegin(0x8c030000, 0x0009, 130, context);

	auto finalization = std::async(std::launch::async, [] {
		research::stopSh4EventsRuntime(true);
	});
	EXPECT_EQ(std::future_status::timeout, finalization.wait_for(std::chrono::milliseconds(20)));
	context.pc = 0x8c030002;
	research::sh4EventsInstructionEnd(0x8c030000, 0x0009, 131, context);
	EXPECT_EQ(std::future_status::ready, finalization.wait_for(std::chrono::seconds(2)));
	EXPECT_NO_THROW(finalization.get());

	const auto summary = research::validateProductionSh4EventsArtifactFile(
			artifactPath, identity, manifest);
	EXPECT_EQ(6u, summary.eventCount);
	EXPECT_EQ(0u, summary.droppedEvents);
}

TEST(ResearchSh4Events, RuntimeDefersReentrantFinalizationToInstructionEnd)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	const auto manifestPath = writeManifest(directory, fixture);
	const auto artifactPath = directory.file("reentrant-boundary.fcsh4");
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const research::Sh4EventsManifest manifest =
			research::loadSh4EventsManifest(manifestPath);

	research_test::clearGuestRam();
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001000, fixture.callBytes));
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001100, fixture.returnBytes));
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchSh4EventsManifestPath = manifestPath.string();
	config::ResearchSh4EventsRecordPath = artifactPath.string();
	config::ResearchSh4EventsMaxBytes = research::DefaultMaximumSh4EventsArtifactBytes;

	research::configureSh4EventsRuntime();
	research::startSh4EventsRuntime();
	Sh4Context context {};
	recordRuntimeFixture(context);
	context.pc = 0x8c030002;
	research::sh4EventsInstructionBegin(0x8c030000, 0x0009, 130, context);
	research::stopSh4EventsRuntime(true);
	EXPECT_TRUE(research::sh4EventsRuntimeActive());
	context.pc = 0x8c030002;
	research::sh4EventsInstructionEnd(0x8c030000, 0x0009, 131, context);
	EXPECT_FALSE(research::sh4EventsRuntimeActive());

	const auto summary = research::validateProductionSh4EventsArtifactFile(
			artifactPath, identity, manifest);
	EXPECT_EQ(6u, summary.eventCount);
	EXPECT_EQ(0u, summary.droppedEvents);
}

TEST(ResearchSh4Events, RuntimeRejectsDeferredCleanStopAfterInstructionAbort)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	const auto manifestPath = writeManifest(directory, fixture);
	const auto artifactPath = directory.file("aborted-boundary.fcsh4");
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const research::Sh4EventsManifest manifest =
			research::loadSh4EventsManifest(manifestPath);

	research_test::clearGuestRam();
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001000, fixture.callBytes));
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001100, fixture.returnBytes));
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchSh4EventsManifestPath = manifestPath.string();
	config::ResearchSh4EventsRecordPath = artifactPath.string();
	config::ResearchSh4EventsMaxBytes = research::DefaultMaximumSh4EventsArtifactBytes;

	research::configureSh4EventsRuntime();
	research::startSh4EventsRuntime();
	Sh4Context context {};
	recordRuntimeFixture(context);
	context.pc = 0x8c030002;
	research::sh4EventsInstructionBegin(0x8c030000, 0x0009, 130, context);
	research::stopSh4EventsRuntime(true);
	research::sh4EventsInstructionAbort();
	EXPECT_FALSE(research::sh4EventsRuntimeActive());
	EXPECT_THROW(research::validateProductionSh4EventsArtifactFile(
			artifactPath, identity, manifest), std::runtime_error);
}

TEST(ResearchSh4Events, RuntimeRejectsCrossThreadCleanStopAfterInstructionAbort)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	const auto manifestPath = writeManifest(directory, fixture);
	const auto artifactPath = directory.file("cross-thread-abort.fcsh4");
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const research::Sh4EventsManifest manifest =
			research::loadSh4EventsManifest(manifestPath);

	research_test::clearGuestRam();
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001000, fixture.callBytes));
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001100, fixture.returnBytes));
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchSh4EventsManifestPath = manifestPath.string();
	config::ResearchSh4EventsRecordPath = artifactPath.string();
	config::ResearchSh4EventsMaxBytes = research::DefaultMaximumSh4EventsArtifactBytes;

	research::configureSh4EventsRuntime();
	research::startSh4EventsRuntime();
	Sh4Context context {};
	recordRuntimeFixture(context);
	context.pc = 0x8c030002;
	research::sh4EventsInstructionBegin(0x8c030000, 0x0009, 130, context);

	std::promise<void> cleanStopInvoked;
	auto cleanStopStarted = cleanStopInvoked.get_future();
	auto cleanStop = std::async(std::launch::async, [&cleanStopInvoked] {
		cleanStopInvoked.set_value();
		research::stopSh4EventsRuntime(true);
	});
	ASSERT_EQ(std::future_status::ready,
			cleanStopStarted.wait_for(std::chrono::seconds(2)));
	EXPECT_EQ(std::future_status::timeout,
			cleanStop.wait_for(std::chrono::milliseconds(100)));
	research::sh4EventsInstructionAbort();
	ASSERT_EQ(std::future_status::ready, cleanStop.wait_for(std::chrono::seconds(2)));
	EXPECT_NO_THROW(cleanStop.get());

	EXPECT_THROW(research::validateProductionSh4EventsArtifactFile(
			artifactPath, identity, manifest), std::runtime_error);
}

TEST(ResearchSh4Events, RuntimeDirtyStopPreemptsDeferredCleanPublication)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	const auto manifestPath = writeManifest(directory, fixture);
	const auto artifactPath = directory.file("dirty-preemption.fcsh4");
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const research::Sh4EventsManifest manifest =
			research::loadSh4EventsManifest(manifestPath);

	research_test::clearGuestRam();
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001000, fixture.callBytes));
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001100, fixture.returnBytes));
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchSh4EventsManifestPath = manifestPath.string();
	config::ResearchSh4EventsRecordPath = artifactPath.string();
	config::ResearchSh4EventsMaxBytes = research::DefaultMaximumSh4EventsArtifactBytes;

	research::configureSh4EventsRuntime();
	research::startSh4EventsRuntime();
	Sh4Context context {};
	recordRuntimeFixture(context);
	context.pc = 0x8c030002;
	research::sh4EventsInstructionBegin(0x8c030000, 0x0009, 130, context);
	research::stopSh4EventsRuntime(true);

	std::promise<void> dirtyStopInvoked;
	auto dirtyStopStarted = dirtyStopInvoked.get_future();
	auto dirtyStop = std::async(std::launch::async, [&dirtyStopInvoked] {
		dirtyStopInvoked.set_value();
		research::stopSh4EventsRuntime(false);
	});
	ASSERT_EQ(std::future_status::ready,
			dirtyStopStarted.wait_for(std::chrono::seconds(2)));
	EXPECT_EQ(std::future_status::timeout,
			dirtyStop.wait_for(std::chrono::milliseconds(100)));
	context.pc = 0x8c030002;
	research::sh4EventsInstructionEnd(0x8c030000, 0x0009, 131, context);
	ASSERT_EQ(std::future_status::ready, dirtyStop.wait_for(std::chrono::seconds(2)));
	EXPECT_NO_THROW(dirtyStop.get());

	EXPECT_THROW(research::validateProductionSh4EventsArtifactFile(
			artifactPath, identity, manifest), std::runtime_error);
}

TEST(ResearchSh4Events, RuntimeDeferredFinalizationFailureDoesNotEscapeInstructionHook)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	const auto manifestPath = writeManifest(directory, fixture);
	const auto artifactPath = directory.file("deferred-failure.fcsh4");
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const research::Sh4EventsManifest manifest =
			research::loadSh4EventsManifest(manifestPath);

	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchSh4EventsManifestPath = manifestPath.string();
	config::ResearchSh4EventsRecordPath = artifactPath.string();
	config::ResearchSh4EventsMaxBytes = research::DefaultMaximumSh4EventsArtifactBytes;

	research::configureSh4EventsRuntime();
	research::startSh4EventsRuntime();
	Sh4Context context {};
	context.pc = 0x8c030002;
	research::sh4EventsInstructionBegin(0x8c030000, 0x0009, 130, context);
	research::stopSh4EventsRuntime(true);
	EXPECT_NO_THROW(research::sh4EventsInstructionEnd(
			0x8c030000, 0x0009, 131, context));
	EXPECT_FALSE(research::sh4EventsRuntimeActive());
	EXPECT_THROW(research::validateProductionSh4EventsArtifactFile(
			artifactPath, identity, manifest), std::runtime_error);
}

TEST(ResearchSh4Events, RuntimeCaptureFailureOwnsInstructionScopeCleanup)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	json limitedManifestJson = manifestJson(fixture);
	limitedManifestJson["limits"]["maximum_open_invocations"] = 1;
	const auto manifestPath = directory.file("limited-manifest.json");
	writeText(manifestPath, limitedManifestJson.dump(2));
	const auto artifactPath = directory.file("capture-failure.fcsh4");

	research_test::clearGuestRam();
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001000, fixture.callBytes));
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchSh4EventsManifestPath = manifestPath.string();
	config::ResearchSh4EventsRecordPath = artifactPath.string();
	config::ResearchSh4EventsMaxBytes = research::DefaultMaximumSh4EventsArtifactBytes;

	research::configureSh4EventsRuntime();
	research::startSh4EventsRuntime();
	Sh4Context context {};
	context.r[1] = 0x8c010100;
	context.r[15] = 0;
	context.pc = 0x8c020002;
	research::sh4EventsInstructionBegin(0x8c020000, 0x410b, 100, context);
	context.pc = 0x8c010100;
	research::sh4EventsInstructionEnd(0x8c020000, 0x410b, 101, context);
	context.pc = 0x8c010106;
	EXPECT_THROW(research::sh4EventsInstructionBegin(
			0x8c010104, 0x410b, 102, context), std::runtime_error);
	EXPECT_FALSE(research::sh4EventsRuntimeActive());
}

TEST(ResearchSh4Events, RuntimeExceptionFailureOwnsInstructionScopeCleanup)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	json limitedManifestJson = manifestJson(fixture);
	limitedManifestJson["limits"]["maximum_events"] = 1;
	limitedManifestJson["acceptance"]["minimum_call_events"] = 0;
	limitedManifestJson["acceptance"]["minimum_watch_events"] = 1;
	const auto manifestPath = directory.file("exception-limit-manifest.json");
	writeText(manifestPath, limitedManifestJson.dump(2));
	const auto artifactPath = directory.file("exception-failure.fcsh4");

	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchSh4EventsManifestPath = manifestPath.string();
	config::ResearchSh4EventsRecordPath = artifactPath.string();
	config::ResearchSh4EventsMaxBytes = research::DefaultMaximumSh4EventsArtifactBytes;

	research::configureSh4EventsRuntime();
	research::startSh4EventsRuntime();
	Sh4Context context {};
	context.vbr = 0x8c000000;
	context.pc = 0x8c010102;
	research::sh4EventsInstructionBegin(0x8c010100, 0x0009, 100, context);
	research::sh4EventsMemoryAccess(0x8c002000, 1,
			research::Sh4MemoryAccessKind::Read, 0x12);
	EXPECT_THROW(research::sh4EventsException(
			0x8c010100, context.vbr + 0x100, 0x180, 101, context),
			std::runtime_error);
	EXPECT_FALSE(research::sh4EventsRuntimeActive());
	research::sh4EventsInstructionAbort();
}

TEST(ResearchSh4Events, NestedCaptureFailureReleasesEveryRecorderScope)
{
	Sh4EventsTemporaryDirectory directory;
	Sh4FixtureData fixture;
	const auto identityPath = writeIdentity(directory, fixture);
	json limitedManifestJson = manifestJson(fixture);
	limitedManifestJson["limits"]["maximum_events"] = 1;
	limitedManifestJson["acceptance"]["minimum_call_events"] = 0;
	limitedManifestJson["acceptance"]["minimum_watch_events"] = 1;
	const auto manifestPath = directory.file("nested-failure-manifest.json");
	writeText(manifestPath, limitedManifestJson.dump(2));
	const auto artifactPath = directory.file("nested-failure.fcsh4");
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchSh4EventsManifestPath = manifestPath.string();
	config::ResearchSh4EventsRecordPath = artifactPath.string();
	config::ResearchSh4EventsMaxBytes = research::DefaultMaximumSh4EventsArtifactBytes;

	research::configureSh4EventsRuntime();
	research::startSh4EventsRuntime();
	Sh4Context context {};
	context.vbr = 0x8c000000;
	context.pc = 0x8c010102;
	research::sh4EventsInstructionBegin(0x8c010100, 0xa000, 100, context);
	context.pc = 0x8c010104;
	research::sh4EventsInstructionBegin(0x8c010102, 0x0009, 101, context);
	research::sh4EventsMemoryAccess(0x8c002000, 1,
			research::Sh4MemoryAccessKind::Read, 0x12);
	EXPECT_THROW(research::sh4EventsException(
			0x8c010102, context.vbr + 0x100, 0x180, 102, context),
			std::runtime_error);
	EXPECT_FALSE(research::sh4EventsRuntimeActive());

	auto abort = std::async(std::launch::async, [] {
		research::abortSh4EventsRuntime();
	});
	ASSERT_EQ(std::future_status::ready, abort.wait_for(std::chrono::seconds(2)));
	EXPECT_NO_THROW(abort.get());
	// Direct hook tests own the emitter-side frames that an interpreter catch
	// would normally abort while unwinding the nested delay slot.
	research::sh4EventsInstructionAbort();
	research::sh4EventsInstructionAbort();
}

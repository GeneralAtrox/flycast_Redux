#include "research/sh4_equivalence.h"

#include "json.hpp"
#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/sh4_observation_trace.h"
#include "research/sha256.h"

#include <gtest/gtest.h>

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
				/ ("flycast-sh4-equivalence-test-"
						+ std::to_string(sequence.fetch_add(1)));
		std::error_code ignored;
		std::filesystem::remove_all(path, ignored);
		std::filesystem::create_directories(path);
	}

	~TemporaryDirectory()
	{
		std::error_code ignored;
		std::filesystem::remove_all(path, ignored);
	}

	std::filesystem::path file(const char *name) const { return path / name; }

private:
	std::filesystem::path path;
};

void writeText(const std::filesystem::path& path, const std::string& text)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(output.good());
	output.write(text.data(), static_cast<std::streamsize>(text.size()));
	ASSERT_TRUE(output.good());
}

std::string digest(const std::string& text)
{
	return research::sha256ToHex(research::sha256(text.data(), text.size()));
}

json blob(const std::filesystem::path& path)
{
	const std::uint64_t size = std::filesystem::file_size(path);
	return {{"path", path.u8string()}, {"size", size},
			{"sha256", research::sha256ToHex(research::hashFileExact(path, size))}};
}

json identity(const char *backend, const json& emulator,
		const std::string& mapleIdentity)
{
	json values {
		{"cpu_backend", backend},
		{"dynarec_observation", std::string(backend) == "dynarec"},
		{"dreamcast_rtc_seed", 0x90000000u},
		{"threaded_rendering", false},
		{"autoload_state", false},
		{"autosave_state", false},
		{"ggpo", false},
	};
	const json descriptiveBlob {
		{"path", "descriptive-only.bin"},
		{"size", 0},
		{"sha256", std::string(64, '0')},
	};
	json boot = descriptiveBlob;
	boot["name"] = "fixture.elf";
	return {
		{"schema", "flycast-research-identity"},
		{"schema_version", 2},
		{"media", {{"kind", "elf"}, {"source", descriptiveBlob},
				{"ip_bin", descriptiveBlob}, {"boot_executable", boot}}},
		{"firmware", {{"mode", "hle"}, {"hle_identity", "fixture-hle"},
				{"flash_initial", descriptiveBlob}}},
		{"persistent_devices", json::array()},
		{"emulator", {{"git_commit", std::string(40, 'a')},
				{"executable", emulator}}},
		{"configuration", {{"values", values},
				{"sha256", digest(values.dump())}}},
		{"equivalence", {{"maple_replay_identity_sha256", mapleIdentity}}},
	};
}

std::vector<std::uint8_t> littleEndianWords(
		std::initializer_list<std::uint32_t> words)
{
	std::vector<std::uint8_t> bytes;
	for (std::uint32_t word : words)
	{
		bytes.push_back(static_cast<std::uint8_t>(word));
		bytes.push_back(static_cast<std::uint8_t>(word >> 8));
		bytes.push_back(static_cast<std::uint8_t>(word >> 16));
		bytes.push_back(static_cast<std::uint8_t>(word >> 24));
	}
	return bytes;
}

void writeReplay(const std::filesystem::path& path,
		const research::Sha256Digest& identityDigest)
{
	research::MapleTraceWriter writer(path, identityDigest);
	research::MapleDmaBeginEvent begin;
	begin.tick = 100;
	begin.descriptorAddress = 0x0c001000;
	begin.mden = 1;
	begin.mdst = 1;
	begin.mmsel = 1;
	begin.trigger = research::MapleDmaTrigger::Software;
	EXPECT_EQ(0u, writer.beginDma(begin));
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
	transaction.request = littleEndianWords({0x01002009, 0x01000000});
	transaction.response = littleEndianWords(
			{0x02002008, 0x01000000, 0xffff0000});
	EXPECT_EQ(0u, writer.writeTransaction(transaction));
	research::MapleDmaScheduleEvent schedule;
	schedule.dmaOrdinal = 0;
	schedule.tick = 100;
	schedule.inputWireBytes = 11;
	schedule.outputWireBytes = 15;
	schedule.scheduledCycles = 1000;
	schedule.responseCount = 1;
	writer.scheduleDma(schedule);
	research::MapleDmaCommitEvent commit;
	commit.dmaOrdinal = 0;
	commit.tick = 1100;
	commit.callbackCycles = 1000;
	commit.responseCount = 1;
	commit.flags = research::MapleCommitInterruptRaised;
	writer.commitDma(commit);
	writer.finalize();
}

research::Sh4Observation instruction(research::Sh4ObservationBackend backend,
		research::Sh4ObservationType type, std::uint64_t tick,
		std::uint32_t registerZero)
{
	research::Sh4Observation event;
	event.backend = backend;
	event.type = type;
	event.tick = tick;
	event.instructionPc = 0x8c010000;
	event.nextPc = 0x8c010002;
	event.opcode = 0x0009;
	event.availableFields = research::Sh4Observation::HasNextPc
			| research::Sh4Observation::HasRegisters;
	event.registers.r[0] = registerZero;
	return event;
}

void writeObservationTrace(const std::filesystem::path& path,
		research::Sh4ObservationBackend backend,
		const research::Sha256Digest& identityDigest,
		const research::Sha256Digest& replayDigest,
		const research::Sha256Digest& manifestSetDigest,
		std::uint32_t registerZero)
{
	research::Sh4ObservationTraceBinding binding;
	binding.backend = backend;
	binding.identityDigest = identityDigest;
	binding.replayDigest = replayDigest;
	binding.manifestSetDigest = manifestSetDigest;
	research::Sh4ObservationTraceWriter writer(path, binding);
	writer.write(instruction(backend,
			research::Sh4ObservationType::InstructionBegin, 10, registerZero));
	writer.write(instruction(backend,
			research::Sh4ObservationType::InstructionEnd, 11, registerZero));
	writer.finalize();
}

struct Fixture
{
	explicit Fixture(TemporaryDirectory& directory, bool divergent = false)
	{
		emulator = directory.file("flycast-fixture.exe");
		comparator = directory.file("comparator-fixture.exe");
		manifest = directory.file("hooks.json");
		manifestSet = directory.file("manifest-set.json");
		replay = directory.file("replay.fcmt");
		interpreterIdentity = directory.file("interpreter-identity.json");
		dynarecIdentity = directory.file("dynarec-identity.json");
		interpreterTrace = directory.file("interpreter.fcso");
		dynarecTrace = directory.file("dynarec.fcso");
		job = directory.file("job.json");
		report = directory.file("report.json");
		writeText(emulator, "same emulator executable bytes");
		writeText(comparator, "independent comparator executable bytes");
		writeText(manifest, "{\"hooks\":[]}");
		const json set {
			{"schema", "flycast-research-sh4-equivalence-manifest-set"},
			{"schema_version", 1},
			{"manifests", json::array({{
				{"kind", "hook-manifest"},
				{"name", "hooks.json"},
				{"size", std::filesystem::file_size(manifest)},
				{"sha256", research::sha256ToHex(research::hashFileExact(
						manifest, std::filesystem::file_size(manifest)))},
			}})},
		};
		writeText(manifestSet, set.dump());
		const auto mapleIdentityDigest = research::sha256("maple-v1-identity", 17);
		const std::string mapleIdentity = research::sha256ToHex(mapleIdentityDigest);
		writeReplay(replay, mapleIdentityDigest);
		writeText(interpreterIdentity,
				identity("interpreter", blob(emulator), mapleIdentity).dump());
		writeText(dynarecIdentity,
				identity("dynarec", blob(emulator), mapleIdentity).dump());
		const auto interpreterIdentityDigest = research::hashFileExact(
				interpreterIdentity, std::filesystem::file_size(interpreterIdentity));
		const auto dynarecIdentityDigest = research::hashFileExact(
				dynarecIdentity, std::filesystem::file_size(dynarecIdentity));
		const auto replayDigest = research::hashFileExact(replay,
				std::filesystem::file_size(replay));
		const auto manifestSetDigest = research::hashFileExact(manifestSet,
				std::filesystem::file_size(manifestSet));
		writeObservationTrace(interpreterTrace,
				research::Sh4ObservationBackend::Interpreter,
				interpreterIdentityDigest, replayDigest, manifestSetDigest, 7);
		writeObservationTrace(dynarecTrace,
				research::Sh4ObservationBackend::Dynarec,
				dynarecIdentityDigest, replayDigest, manifestSetDigest,
				divergent ? 8 : 7);
		jobJson = {
			{"schema", "flycast-research-sh4-equivalence-job"},
			{"schema_version", 1},
			{"job_id", "12345678-1234-1234-1234-123456789abc"},
			{"interpreter", {{"identity", blob(interpreterIdentity)},
					{"trace", blob(interpreterTrace)}}},
			{"dynarec", {{"identity", blob(dynarecIdentity)},
					{"trace", blob(dynarecTrace)}}},
			{"emulator", blob(emulator)},
			{"replay", blob(replay)},
			{"manifest_set", blob(manifestSet)},
			{"manifests", json::array({{
				{"kind", "hook-manifest"},
				{"name", "hooks.json"},
				{"source", blob(manifest)},
			}})},
			{"comparator", {{"executable", blob(comparator)}}},
			{"limits", {{"maximum_replay_bytes", 1024 * 1024},
					{"maximum_trace_bytes", 1024 * 1024},
					{"maximum_events", 1000}}},
			{"metadata", json::object()},
		};
		writeText(job, jobJson.dump());
	}

	void rewriteJob() { writeText(job, jobJson.dump()); }

	std::filesystem::path emulator;
	std::filesystem::path comparator;
	std::filesystem::path manifest;
	std::filesystem::path manifestSet;
	std::filesystem::path replay;
	std::filesystem::path interpreterIdentity;
	std::filesystem::path dynarecIdentity;
	std::filesystem::path interpreterTrace;
	std::filesystem::path dynarecTrace;
	std::filesystem::path job;
	std::filesystem::path report;
	json jobJson;
};

} // namespace

TEST(ResearchSh4Equivalence, ProfileIdentitySelectsNormalDynarecWithoutMarkers)
{
	TemporaryDirectory directory;
	const auto emulator = directory.file("flycast-fixture.exe");
	const auto identityPath = directory.file("profile-identity.json");
	writeText(emulator, "profile emulator executable bytes");
	auto root = identity("dynarec", blob(emulator), std::string(64, '1'));
	root["configuration"]["values"]["dynarec_observation"] = false;
	root["configuration"]["values"]["dynarec_profile"] = true;
	root["configuration"]["sha256"] =
			digest(root["configuration"]["values"].dump());
	writeText(identityPath, root.dump());
	const auto profileIdentity = research::loadIdentityManifest(identityPath);
	EXPECT_NO_THROW(research::requireSh4DynarecProfileIdentityV2(profileIdentity));
	EXPECT_THROW(research::requireSh4EquivalenceIdentityV2(profileIdentity),
			std::runtime_error);

	root["configuration"]["values"]["dynarec_observation"] = true;
	root["configuration"]["sha256"] =
			digest(root["configuration"]["values"].dump());
	writeText(identityPath, root.dump());
	EXPECT_THROW(research::loadIdentityManifest(identityPath), std::runtime_error);
}

TEST(ResearchSh4Equivalence, DiagnosticIdentityCannotBecomeEquivalenceEvidence)
{
	TemporaryDirectory directory;
	const auto emulator = directory.file("flycast-fixture.exe");
	const auto identityPath = directory.file("diagnostic-identity.json");
	writeText(emulator, "diagnostic emulator executable bytes");
	auto root = identity("dynarec", blob(emulator), std::string(64, '1'));
	root["configuration"]["values"]["dynarec_observation"] = false;
	root["configuration"]["values"]["dynarec_replay_diagnostic"] = true;
	root["configuration"]["sha256"] =
			digest(root["configuration"]["values"].dump());
	writeText(identityPath, root.dump());
	const auto diagnosticIdentity = research::loadIdentityManifest(identityPath);
	EXPECT_NO_THROW(research::requireSh4DynarecReplayDiagnosticIdentityV2(
			diagnosticIdentity));
	EXPECT_THROW(research::requireSh4EquivalenceIdentityV2(diagnosticIdentity),
			std::runtime_error);
	EXPECT_THROW(research::requireSh4DynarecProfileIdentityV2(diagnosticIdentity),
			std::runtime_error);

	root["configuration"]["values"]["dynarec_observation"] = true;
	root["configuration"]["sha256"] =
			digest(root["configuration"]["values"].dump());
	writeText(identityPath, root.dump());
	const auto observedDiagnosticIdentity =
			research::loadIdentityManifest(identityPath);
	EXPECT_NO_THROW(research::requireSh4DynarecReplayDiagnosticIdentityV2(
			observedDiagnosticIdentity));
	EXPECT_THROW(research::requireSh4EquivalenceIdentityV2(
			observedDiagnosticIdentity), std::runtime_error);

	root["configuration"]["values"]["dynarec_profile"] = true;
	root["configuration"]["sha256"] =
			digest(root["configuration"]["values"].dump());
	writeText(identityPath, root.dump());
	EXPECT_THROW(research::loadIdentityManifest(identityPath), std::runtime_error);
}

TEST(ResearchSh4Equivalence, IssuesAndRevalidatesEquivalentTypedReport)
{
	TemporaryDirectory directory;
	Fixture fixture(directory);
	const auto issued = research::issueSh4EquivalenceReport(fixture.job,
			fixture.report, fixture.comparator);
	EXPECT_TRUE(issued.equivalent);
	EXPECT_EQ(2u, issued.matchedEventCount);
	EXPECT_FALSE(issued.firstDivergence.has_value());
	const auto validated = research::validateSh4EquivalenceReport(fixture.job,
			fixture.report, fixture.comparator);
	EXPECT_TRUE(validated.equivalent);
	const json report = json::parse(std::ifstream(fixture.report));
	EXPECT_EQ("equivalent", report.at("status"));
	EXPECT_TRUE(report.at("first_divergence").is_null());
	EXPECT_EQ(1u, report.at("manifest_set").at("manifest_count"));
}

TEST(ResearchSh4Equivalence, ValidDivergenceReportsFirstExactField)
{
	TemporaryDirectory directory;
	Fixture fixture(directory, true);
	const auto summary = research::issueSh4EquivalenceReport(fixture.job,
			fixture.report, fixture.comparator);
	ASSERT_FALSE(summary.equivalent);
	ASSERT_TRUE(summary.firstDivergence.has_value());
	EXPECT_EQ(0u, summary.firstDivergence->ordinal);
	EXPECT_EQ("registers.r[0]", summary.firstDivergence->field);
	const json report = json::parse(std::ifstream(fixture.report));
	EXPECT_EQ("divergent", report.at("status"));
	EXPECT_EQ("registers.r[0]",
			report.at("first_divergence").at("field"));
}

TEST(ResearchSh4Equivalence, RejectsConfigurationDifferenceOutsideBackendKeys)
{
	TemporaryDirectory directory;
	Fixture fixture(directory);
	json dynarec = json::parse(std::ifstream(fixture.dynarecIdentity));
	dynarec["configuration"]["values"]["extra_setting"] = 1;
	dynarec["configuration"]["sha256"] = digest(
			dynarec["configuration"]["values"].dump());
	writeText(fixture.dynarecIdentity, dynarec.dump());
	fixture.jobJson["dynarec"]["identity"] = blob(fixture.dynarecIdentity);
	fixture.rewriteJob();
	EXPECT_THROW(research::issueSh4EquivalenceReport(fixture.job,
			fixture.report, fixture.comparator), std::runtime_error);
	EXPECT_FALSE(std::filesystem::exists(fixture.report));
}

TEST(ResearchSh4Equivalence, RejectsWrongRunningComparatorAndExistingReport)
{
	TemporaryDirectory directory;
	Fixture fixture(directory);
	const auto other = directory.file("other-comparator.exe");
	writeText(other, "other comparator");
	EXPECT_THROW(research::issueSh4EquivalenceReport(fixture.job,
			fixture.report, other), std::runtime_error);
	EXPECT_FALSE(std::filesystem::exists(fixture.report));
	research::issueSh4EquivalenceReport(fixture.job, fixture.report,
			fixture.comparator);
	EXPECT_THROW(research::issueSh4EquivalenceReport(fixture.job,
			fixture.report, fixture.comparator), std::runtime_error);
}

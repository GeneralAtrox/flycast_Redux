#include "json.hpp"
#include "research/ghidra_export.h"
#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/sh4_ghidra_join.h"
#include "research/sh4_ghidra_package.h"
#include "research/sh4_profile.h"
#include "research/sh4_profile_artifact.h"
#include "research/sha256.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <tuple>
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
				/ ("flycast-sh4-ghidra-join-test-"
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

std::vector<std::uint8_t> words(std::initializer_list<std::uint32_t> values)
{
	std::vector<std::uint8_t> bytes;
	for (std::uint32_t value : values)
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
	transaction.command = 9;
	transaction.flags = research::MapleTransactionDevicePresent;
	transaction.request = words({0x01002009, 0x01000000});
	transaction.response = words({0x02002008, 0x01000000, 0xffff0000});
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

research::Sh4DynarecBlockDefinition block(std::uint32_t virtualAddress,
		std::initializer_list<std::uint8_t> bytes,
		research::Sh4DynarecBranchKind kind =
			research::Sh4DynarecBranchKind::None)
{
	research::Sh4DynarecBlockDefinition result;
	result.virtualAddress = virtualAddress;
	result.physicalAddress = virtualAddress & 0x1fffffff;
	result.guestCodeSize = static_cast<std::uint32_t>(bytes.size());
	result.guestCycles = 2;
	result.guestOpcodes = static_cast<std::uint32_t>(bytes.size() / 2);
	result.guestBytes.assign(bytes);
	result.byteStatus = research::Sh4DynarecProfileByteStatus::Complete;
	result.branchKind = kind;
	if (kind != research::Sh4DynarecBranchKind::None)
	{
		result.branchSource = virtualAddress;
		result.branchOpcode = static_cast<std::uint16_t>(result.guestBytes[0]
				| (std::uint16_t(result.guestBytes[1]) << 8));
	}
	return result;
}

struct Fixture
{
	explicit Fixture(TemporaryDirectory& directory)
	{
		program = directory.file("fixture.bin");
		script = directory.file("ExportFlycastResearch.java");
		exportPath = directory.file("ghidra-export.json");
		identityPath = directory.file("identity.json");
		replay = directory.file("maple.fcmt");
		profile = directory.file("profile.fcsh4profile");
		join = directory.file("join.json");
		emulator = directory.file("flycast.exe");

		programBytes = {
			0x01, 0x89, 0x09, 0x00,
			0x09, 0x00, 0x09, 0x00,
			0x09, 0x00, 0x09, 0x00,
			0x2b, 0x40, 0x09, 0x00,
			0x09, 0x00, 0x09, 0x00,
			0x09, 0x00, 0x09, 0x00,
			0x09, 0x00, 0x09, 0x00,
			0x09, 0x00, 0x09, 0x00,
		};
		writeBytes(program, programBytes);
		writeText(script, "fixture exporter script\n");
		writeText(emulator, "fixture emulator\n");
		const std::string programDigest = research::sha256ToHex(
				research::sha256(programBytes.data(), programBytes.size()));
		const std::string scriptDigest = research::sha256ToHex(
				research::hashFileExact(script, 1024));

		json exportJson {
			{"schema", "flycast-research-ghidra-export"},
			{"schema_version", 1},
			{"export_id", "STATIC-JOIN-FIXTURE-V1"},
			{"evidence_class", "static-analysis"},
			{"producer", {{"name", "Ghidra"}, {"version", "12.1.2"},
					{"exporter_id", "flycast-ghidra-export-v1"},
					{"exporter_sha256", scriptDigest}}},
			{"program", {{"name", "fixture.bin"},
					{"executable_sha256", programDigest},
					{"executable_size", programBytes.size()},
					{"language_id", "SuperH4:LE:32:default"},
					{"compiler_spec_id", "default"}, {"endian", "little"},
					{"address_size", 32}, {"address_space", "ram"},
					{"ghidra_image_base", "0x8c010000"},
					{"image_base", "0x8c010000"},
					{"image_base_source", "ghidra-program"},
					{"minimum_address", "0x8c010000"},
					{"maximum_address", "0x8c01001f"}}},
			{"memory_blocks", json::array({{{"name", "main"},
					{"start_address", "0x8c010000"}, {"length", 32},
					{"read", true}, {"write", true}, {"execute", true},
					{"initialized", true}}})},
			{"functions", json::array({
				{{"entry_address", "0x8c010000"}, {"name", "function_a"},
					{"namespace", "Global"}, {"calling_convention", "default"},
					{"return_type", "/void"}, {"parameter_types", json::array()},
					{"body_ranges", json::array({{{"start_address", "0x8c010000"},
							{"length", 8}}})}, {"thunk", false}, {"no_return", false}},
				{{"entry_address", "0x8c01000c"}, {"name", "function_b"},
					{"namespace", "Global"}, {"calling_convention", "default"},
					{"return_type", "/void"}, {"parameter_types", json::array()},
					{"body_ranges", json::array({{{"start_address", "0x8c01000c"},
							{"length", 4}}})}, {"thunk", false}, {"no_return", false}},
			})},
			{"symbols", json::array({
				{{"address", "0x8c010000"}, {"name", "function_a"},
					{"namespace", "Global"}, {"kind", "function"},
					{"source", "user_defined"}, {"primary", true}},
				{{"address", "0x8c010000"}, {"name", "sdk_alias_a"},
					{"namespace", "SDK"}, {"kind", "label"},
					{"source", "imported"}, {"primary", false}},
				{{"address", "0x8c01000c"}, {"name", "function_b"},
					{"namespace", "Global"}, {"kind", "function"},
					{"source", "analysis"}, {"primary", true}},
			})},
			{"data_types", json::array({{{"path", "/byte"}, {"kind", "integer"},
					{"length", 1}, {"dynamic", false}, {"definition", "byte"}}})},
			{"counts", {{"memory_blocks", 1}, {"functions", 2},
					{"symbols", 3}, {"data_types", 1}}},
		};
		writeText(exportPath, exportJson.dump(2) + "\n");
		const std::string exportDigest = research::sha256ToHex(
				research::hashFileExact(exportPath, 1024 * 1024));

		mapleIdentity.fill(0x44);
		json values {{"cpu_backend", "dynarec"},
			{"dynarec_observation", false}, {"dynarec_profile", true},
			{"dreamcast_rtc_seed", 0x90000000u},
			{"threaded_rendering", false}, {"autoload_state", false},
			{"autosave_state", false}, {"ggpo", false}};
		const json descriptiveBlob {{"path", "descriptive-only.bin"}, {"size", 0},
				{"sha256", std::string(64, '0')}};
		json boot = blob(program);
		boot["name"] = "fixture.bin";
		json identityJson {
			{"schema", "flycast-research-identity"}, {"schema_version", 2},
			{"media", {{"kind", "elf"}, {"source", descriptiveBlob},
					{"ip_bin", descriptiveBlob}, {"boot_executable", boot}}},
			{"firmware", {{"mode", "hle"}, {"hle_identity", "fixture-hle"},
					{"flash_initial", descriptiveBlob}}},
			{"persistent_devices", json::array()},
			{"emulator", {{"git_commit", std::string(40, 'a')},
					{"executable", blob(emulator)}}},
			{"configuration", {{"values", values}, {"sha256", digest(values.dump())}}},
			{"static_analysis", {{"program_sha256", programDigest},
					{"image_base", "0x8c010000"}, {"export_sha256", exportDigest}}},
			{"equivalence", {{"maple_replay_identity_sha256",
					research::sha256ToHex(mapleIdentity)}}},
		};
		writeText(identityPath, identityJson.dump(2) + "\n");
		identity = research::loadIdentityManifest(identityPath);
		writeReplay(replay, mapleIdentity);

		research::Sh4DynarecProfileCollector collector(16, 16, 64);
		auto conditional = block(0x8c010000, {0x01, 0x89, 0x09, 0x00},
				research::Sh4DynarecBranchKind::Conditional);
		conditional.branchTarget = 0x8c010008;
		conditional.fallthroughTarget = 0x8c010004;
		const auto first = collector.registerBlock(conditional);
		collector.enter(first, 10); collector.exit(first, 0x8c010004, 12);
		collector.enter(first, 20); collector.exit(first, 0x8c010008, 22);
		const auto interior = collector.registerBlock(
				block(0x8c010004, {0x09, 0x00, 0x09, 0x00}));
		collector.enter(interior, 30); collector.exit(interior, 0x8c010008, 32);
		const auto unassigned = collector.registerBlock(
				block(0x8c010008, {0x09, 0x00, 0x09, 0x00}));
		collector.enter(unassigned, 40); collector.exit(unassigned, 0x8c01000c, 42);
		auto jump = block(0x8c01000c, {0x2b, 0x40, 0x09, 0x00},
				research::Sh4DynarecBranchKind::Jump);
		const auto indirect = collector.registerBlock(jump);
		collector.enter(indirect, 50); collector.exit(indirect, 0x8c010000, 52);
		collector.enter(indirect, 60); collector.exit(indirect, 0x8c010004, 62);
		const auto repeated = collector.registerBlock(conditional);
		collector.enter(repeated, 70); collector.exit(repeated, 0x8c010008, 72);
		const auto runtime = collector.registerBlock(
				block(0x8c00fa00, {0x09, 0x00}));
		collector.enter(runtime, 80); collector.exit(runtime, 0x8c00fa02, 82);

		binding.identityDigest = identity.digest;
		binding.replayDigest = research::hashFileExact(replay,
				research::DefaultMaximumMapleTraceBytes);
		binding.configurationDigest = identity.configurationDigest;
		research::writeSh4DynarecProfileArtifact(profile, binding,
				collector.snapshot());
	}

	std::filesystem::path program, script, exportPath, identityPath, replay, profile,
			join, emulator;
	std::vector<std::uint8_t> programBytes;
	research::Sha256Digest mapleIdentity {};
	research::IdentityManifest identity;
	research::Sh4DynarecProfileArtifactBinding binding;
};

std::filesystem::path buildPackageCandidate(TemporaryDirectory& directory,
		Fixture& fixture, const std::filesystem::path& accepted,
		const std::filesystem::path& runningValidator)
{
	const std::string packageId = "12345678-1234-1234-1234-123456789abc";
	const auto publisher = directory.file("publisher-source.ps1");
	const auto semanticValidator = directory.file("semantic-validator-source.exe");
	const auto sourceJobPath = directory.file("source-package-job.json");
	writeText(publisher, "fixture publisher\n");
	writeText(semanticValidator, "fixture semantic validator\n");
	writeText(runningValidator, "fixture package validator\n");
	research::writeSh4GhidraSemanticJoin(fixture.join, fixture.profile,
			fixture.identityPath, fixture.replay, fixture.exportPath,
			fixture.program, fixture.script);
	json sourceJob {
		{"schema", "flycast-research-sh4-ghidra-package-job"},
		{"schema_version", 1}, {"package_id", packageId},
		{"output", {{"accepted_directory", accepted.u8string()}}},
		{"identity", blob(fixture.identityPath)},
		{"emulator", blob(fixture.emulator)},
		{"maple_replay", blob(fixture.replay)},
		{"profile", {{"candidate", blob(fixture.profile)}}},
		{"ghidra_export", blob(fixture.exportPath)},
		{"program", blob(fixture.program)},
		{"exporter_script", blob(fixture.script)},
		{"semantic_join", {{"candidate", blob(fixture.join)},
				{"maximum_bytes",
						research::DefaultMaximumSh4GhidraSemanticJoinBytes}}},
		{"publisher", {{"script", blob(publisher)}}},
		{"semantic_validator", {{"executable", blob(semanticValidator)}}},
		{"package_validator", {{"executable", blob(runningValidator)}}},
		{"limits", {{"validator_timeout_seconds", 60}}},
		{"metadata", json::object()},
	};
	writeText(sourceJobPath, sourceJob.dump(2) + "\n");
	const auto candidate = accepted.parent_path()
			/ (".flycast-research-sh4-ghidra-candidate-" + packageId);
	std::filesystem::create_directory(candidate);
	for (const auto& item : std::vector<std::tuple<std::filesystem::path,
			const char *>> {
		{sourceJobPath, "source-job.json"},
		{fixture.identityPath, "identity.json"},
		{fixture.emulator, "emulator.bin"},
		{fixture.replay, "maple-replay.fcmt"},
		{fixture.profile, "sh4-profile.fcsh4profile"},
		{fixture.exportPath, "ghidra-export.json"},
		{fixture.program, "program.bin"},
		{fixture.script, "exporter-script.java"},
		{fixture.join, "sh4-ghidra-join.json"},
		{publisher, "publisher.ps1"},
		{semanticValidator, "semantic-validator.exe"},
		{runningValidator, "package-validator.exe"},
	})
		std::filesystem::copy_file(std::get<0>(item), candidate / std::get<1>(item));
	json localized = sourceJob;
	localized["identity"]["path"] = "identity.json";
	localized["emulator"]["path"] = "emulator.bin";
	localized["maple_replay"]["path"] = "maple-replay.fcmt";
	localized["profile"]["candidate"]["path"] = "sh4-profile.fcsh4profile";
	localized["ghidra_export"]["path"] = "ghidra-export.json";
	localized["program"]["path"] = "program.bin";
	localized["exporter_script"]["path"] = "exporter-script.java";
	localized["semantic_join"]["candidate"]["path"] = "sh4-ghidra-join.json";
	localized["publisher"]["script"]["path"] = "publisher.ps1";
	localized["semantic_validator"]["executable"]["path"] =
			"semantic-validator.exe";
	localized["package_validator"]["executable"]["path"] =
			"package-validator.exe";
	writeText(candidate / "job.json", localized.dump(2) + "\n");
	std::vector<std::filesystem::path> entries;
	for (const auto& entry : std::filesystem::directory_iterator(candidate))
		entries.push_back(entry.path());
	std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
		return left.filename().u8string() < right.filename().u8string();
	});
	json locked = json::array();
	for (const auto& entry : entries)
	{
		json value = blob(entry);
		value["path"] = entry.filename().u8string();
		locked.push_back(std::move(value));
	}
	writeText(candidate / "package.json", json {
		{"schema", "flycast-research-sh4-ghidra-package"},
		{"schema_version", 1}, {"package_id", packageId},
		{"locked_entries", locked},
	}.dump(2) + "\n");
	return candidate;
}

} // namespace

TEST(ResearchSh4GhidraJoin, PreservesExactFunctionSymbolsGenerationsAndEdges)
{
	TemporaryDirectory directory;
	Fixture fixture(directory);
	const auto ghidra = research::loadGhidraExport(fixture.exportPath);
	ASSERT_EQ(2u, ghidra.functions.size());
	ASSERT_EQ(3u, ghidra.symbols.size());
	EXPECT_EQ("sdk_alias_a", ghidra.symbols[1].name);
	const auto decoded = research::validateSh4DynarecProfileArtifact(
			fixture.profile, fixture.binding);
	ASSERT_EQ(6u, decoded.blocks.size());
	ASSERT_EQ(5u, decoded.branches.size());
	EXPECT_EQ(0x8c010000u, decoded.blocks.front().definition.virtualAddress);

	const auto created = research::writeSh4GhidraSemanticJoin(fixture.join,
			fixture.profile, fixture.identityPath, fixture.replay, fixture.exportPath,
			fixture.program, fixture.script);
	EXPECT_EQ(6u, created.blockCount);
	EXPECT_EQ(5u, created.edgeCount);
	EXPECT_EQ(5u, created.authenticatedProgramBlocks);
	EXPECT_EQ(4u, created.functionOwnedBlocks);
	EXPECT_EQ(1u, created.staticExecutableUnassignedBlocks);
	EXPECT_EQ(1u, created.runtimeOnlyBlocks);
	EXPECT_NO_THROW(research::validateSh4GhidraSemanticJoin(fixture.join,
			fixture.profile, fixture.identityPath, fixture.replay, fixture.exportPath,
			fixture.program, fixture.script));

	const json artifact = json::parse(std::ifstream(fixture.join));
	EXPECT_EQ("function-entry", artifact["blocks"][0]["mapping"]["class"]);
	EXPECT_EQ("function-interior", artifact["blocks"][1]["mapping"]["class"]);
	EXPECT_EQ("static-executable-unassigned",
			artifact["blocks"][2]["mapping"]["class"]);
	EXPECT_EQ("runtime-only", artifact["blocks"][5]["mapping"]["class"]);
	EXPECT_EQ(2u, artifact["functions"].size());
	EXPECT_EQ(2u, artifact["symbol_addresses"][0]["symbols"].size());
	EXPECT_EQ(1u, artifact["blocks"][0]["generation"]);
	EXPECT_EQ(5u, artifact["blocks"][4]["generation"]);
	EXPECT_EQ(2u, artifact["coverage"]["edge_destination_classes"]
			["function-interior"]);
	EXPECT_THROW(research::writeSh4GhidraSemanticJoin(fixture.join,
			fixture.profile, fixture.identityPath, fixture.replay, fixture.exportPath,
			fixture.program, fixture.script), std::runtime_error);
}

TEST(ResearchSh4GhidraJoin, RejectsDerivedMutationAndExecutableByteMismatch)
{
	TemporaryDirectory directory;
	Fixture fixture(directory);
	research::writeSh4GhidraSemanticJoin(fixture.join, fixture.profile,
			fixture.identityPath, fixture.replay, fixture.exportPath,
			fixture.program, fixture.script);
	json artifact = json::parse(std::ifstream(fixture.join));
	artifact["blocks"][0]["mapping"]["class"] = "runtime-only";
	writeText(fixture.join, artifact.dump(2) + "\n");
	EXPECT_THROW(research::validateSh4GhidraSemanticJoin(fixture.join,
			fixture.profile, fixture.identityPath, fixture.replay, fixture.exportPath,
			fixture.program, fixture.script), std::runtime_error);

	const auto mismatchPath = directory.file("mismatch.fcsh4profile");
	research::Sh4DynarecProfileCollector mismatch(2, 2, 2);
	const auto generation = mismatch.registerBlock(
			block(0x8c010004, {0x08, 0x00, 0x09, 0x00}));
	mismatch.enter(generation, 1);
	mismatch.exit(generation, 0x8c010008, 3);
	research::writeSh4DynarecProfileArtifact(mismatchPath, fixture.binding,
			mismatch.snapshot());
	const auto mismatchJoin = directory.file("mismatch.json");
	EXPECT_THROW(research::writeSh4GhidraSemanticJoin(mismatchJoin, mismatchPath,
			fixture.identityPath, fixture.replay, fixture.exportPath,
			fixture.program, fixture.script), std::runtime_error);
}

TEST(ResearchSh4GhidraPackage, IssuesReceiptRevalidatesWithoutSourcesAndRejectsMutation)
{
	TemporaryDirectory directory;
	Fixture fixture(directory);
	const auto accepted = directory.file("accepted-sh4-ghidra-package");
	const auto validator = directory.file("package-validator-source.exe");
	const auto candidate = buildPackageCandidate(directory, fixture, accepted, validator);
	const auto issued = research::issueSh4GhidraPackageV1Receipt(candidate,
			candidate / "package-validation.json", validator);
	EXPECT_EQ("12345678-1234-1234-1234-123456789abc", issued.packageId);
	EXPECT_EQ(13u, issued.lockedEntryCount);
	std::filesystem::rename(candidate, accepted);

	for (const auto& source : {fixture.identityPath, fixture.emulator, fixture.replay,
			fixture.profile, fixture.exportPath, fixture.program, fixture.script,
			fixture.join, validator, directory.file("publisher-source.ps1"),
			directory.file("semantic-validator-source.exe"),
			directory.file("source-package-job.json")})
	{
		std::error_code ignored;
		std::filesystem::remove(source, ignored);
	}
	const auto revalidated = research::validateSh4GhidraPackageV1ReadOnly(accepted);
	EXPECT_EQ(issued.packageId, revalidated.packageId);
	EXPECT_EQ(issued.lockedEntryCount, revalidated.lockedEntryCount);

	std::ofstream mutation(accepted / "sh4-ghidra-join.json",
			std::ios::binary | std::ios::app);
	mutation << 'x';
	mutation.close();
	EXPECT_THROW(research::validateSh4GhidraPackageV1ReadOnly(accepted),
			std::runtime_error);
}

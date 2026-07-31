#include <gtest/gtest.h>

#include "json.hpp"
#include "research/ghidra_export.h"
#include "research/identity_manifest.h"
#include "research/sh4_events_manifest.h"
#include "research/sha256.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

using json = nlohmann::json;

class GhidraTemporaryDirectory
{
public:
	GhidraTemporaryDirectory()
	{
		static std::atomic<unsigned> sequence {0};
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path = std::filesystem::temp_directory_path()
				/ ("flycast-research-ghidra-test-" + std::to_string(stamp) + "-"
						+ std::to_string(sequence++));
		std::filesystem::create_directory(path);
	}

	~GhidraTemporaryDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
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

std::string digestOf(const std::string& value)
{
	return research::sha256ToHex(research::sha256(value.data(), value.size()));
}

json exportJson(const std::string& programDigest, std::uint64_t programSize,
		const std::string& scriptDigest)
{
	return json {
		{"schema", "flycast-research-ghidra-export"},
		{"schema_version", 1},
		{"export_id", "STATIC-FIXTURE-V1"},
		{"evidence_class", "static-analysis"},
		{"producer", {
			{"name", "Ghidra"},
			{"version", "12.1.2"},
			{"exporter_id", "flycast-ghidra-export-v1"},
			{"exporter_sha256", scriptDigest},
		}},
		{"program", {
			{"name", "fixture.bin"},
			{"executable_sha256", programDigest},
			{"executable_size", programSize},
			{"language_id", "SuperH4:LE:32:default"},
			{"compiler_spec_id", "default"},
			{"endian", "little"},
			{"address_size", 32},
			{"address_space", "ram"},
			{"ghidra_image_base", "0x8c010000"},
			{"image_base", "0x8c010000"},
			{"image_base_source", "ghidra-program"},
			{"minimum_address", "0x8c010000"},
			{"maximum_address", "0x8c010fff"},
		}},
		{"memory_blocks", json::array({{
			{"name", "main"},
			{"start_address", "0x8c010000"},
			{"length", 4096},
			{"read", true},
			{"write", true},
			{"execute", true},
			{"initialized", true},
		}})},
		{"functions", json::array({{
			{"entry_address", "0x8c010100"},
			{"name", "fixture_function"},
			{"namespace", "Global"},
			{"calling_convention", "default"},
			{"return_type", "/void"},
			{"parameter_types", json::array({"/uint32_t"})},
			{"body_ranges", json::array({{
				{"start_address", "0x8c010100"},
				{"length", 32},
			}})},
			{"thunk", false},
			{"no_return", false},
		}})},
		{"symbols", json::array({{
			{"address", "0x8c010100"},
			{"name", "fixture_function"},
			{"namespace", "Global"},
			{"kind", "function"},
			{"source", "user_defined"},
			{"primary", true},
		}})},
		{"data_types", json::array({{
			{"path", "/Fixture"},
			{"kind", "composite"},
			{"length", 4},
			{"dynamic", false},
			{"definition", "struct Fixture { uint32_t value; }"},
		}})},
		{"counts", {
			{"memory_blocks", 1},
			{"functions", 1},
			{"symbols", 1},
			{"data_types", 1},
		}},
	};
}

json identityJson(const std::string& programDigest, std::uint64_t programSize,
		const std::string& exportDigest, const std::string& hookDigest)
{
	json values {
		{"cpu_backend", "interpreter"},
		{"threaded_rendering", false},
		{"autoload_state", false},
		{"autosave_state", false},
		{"ggpo", false},
	};
	const json emptyBlob {
		{"path", "descriptive-only.bin"},
		{"size", 0},
		{"sha256", std::string(64, '0')},
	};
	json root {
		{"schema", "flycast-research-identity"},
		{"schema_version", 1},
		{"media", {
			{"kind", "gdi"},
			{"source", emptyBlob},
			{"ip_bin", emptyBlob},
			{"boot_executable", {
				{"name", "1ST_READ.BIN"},
				{"path", "fixture.bin"},
				{"size", programSize},
				{"sha256", programDigest},
			}},
		}},
		{"firmware", {
			{"mode", "hle"},
			{"bios", nullptr},
			{"hle_identity", "reios:test"},
			{"flash_initial", emptyBlob},
		}},
		{"persistent_devices", json::array()},
		{"emulator", {
			{"git_commit", std::string(40, '0')},
			{"executable", emptyBlob},
		}},
		{"configuration", {
			{"values", values},
			{"sha256", digestOf(values.dump())},
		}},
		{"static_analysis", {
			{"program_sha256", programDigest},
			{"image_base", "0x8c010000"},
			{"export_sha256", exportDigest},
			{"hook_manifest_sha256", hookDigest},
		}},
	};
	json track = emptyBlob;
	track["track"] = 1;
	track["start_fad"] = 150;
	track["sector_size"] = 2048;
	track["offset"] = 0;
	root["media"]["tracks"] = json::array({track});
	return root;
}

json sh4ManifestJson(const std::string& programDigest, const std::string& exportDigest,
		const std::string& hookDigest)
{
	return json {
		{"schema", "flycast-research-sh4-events-manifest"},
		{"schema_version", 1},
		{"manifest_id", "SH4-FIXTURE-V1"},
		{"address_space", "flycast-sh4-virtual"},
		{"bindings", {
			{"executable_sha256", programDigest},
			{"static_analysis_id", "STATIC-FIXTURE-V1"},
			{"static_analysis_sha256", exportDigest},
			{"hook_manifest_id", "HOOKS-FIXTURE-V1"},
			{"hook_manifest_sha256", hookDigest},
		}},
		{"hooks", json::array({{
			{"hook_id", "HOOK-FIXTURE"},
			{"entry_pc", "0x8c010100"},
			{"end_address_exclusive", "0x8c010120"},
			{"snapshots", json::array()},
		}})},
		{"watch_ranges", json::array()},
		{"limits", {
			{"maximum_events", 2},
			{"maximum_snapshot_bytes_per_event", 0},
			{"maximum_total_snapshot_bytes", 0},
			{"maximum_open_invocations", 1},
		}},
		{"acceptance", {
			{"backend", "interpreter"},
			{"minimum_call_events", 1},
			{"minimum_watch_events", 0},
			{"require_balanced_calls", true},
			{"zero_dropped_events", true},
			{"natural_exit", true},
		}},
	};
}

struct Fixture
{
	explicit Fixture(const GhidraTemporaryDirectory& directory)
	{
		programPath = directory.file("fixture.bin");
		scriptPath = directory.file("ExportFlycastResearch.java");
		exportPath = directory.file("ghidra-export.json");
		identityPath = directory.file("identity.json");
		manifestPath = directory.file("sh4-events.json");
		const std::string programBytes = "fixture SH-4 executable bytes";
		const std::string scriptBytes = "fixture exporter script bytes";
		writeText(programPath, programBytes);
		writeText(scriptPath, scriptBytes);
		programDigest = digestOf(programBytes);
		scriptDigest = digestOf(scriptBytes);
		const std::string exportBytes = exportJson(programDigest, programBytes.size(),
				scriptDigest).dump(2) + "\n";
		writeText(exportPath, exportBytes);
		exportDigest = digestOf(exportBytes);
		hookDigest = digestOf("fixture hook manifest");
		writeText(identityPath, identityJson(programDigest, programBytes.size(), exportDigest,
				hookDigest).dump(2) + "\n");
		writeText(manifestPath, sh4ManifestJson(programDigest, exportDigest, hookDigest).dump(2)
				+ "\n");
	}

	std::filesystem::path programPath;
	std::filesystem::path scriptPath;
	std::filesystem::path exportPath;
	std::filesystem::path identityPath;
	std::filesystem::path manifestPath;
	std::string programDigest;
	std::string scriptDigest;
	std::string exportDigest;
	std::string hookDigest;
};

} // namespace

TEST(ResearchGhidraExport, ValidatesExactIdentityAndSh4Join)
{
	GhidraTemporaryDirectory directory;
	Fixture fixture(directory);
	const research::GhidraExport exportArtifact = research::loadGhidraExport(fixture.exportPath);
	const research::IdentityManifest identity = research::loadIdentityManifest(fixture.identityPath);
	const research::Sh4EventsManifest manifest =
			research::loadSh4EventsManifest(fixture.manifestPath);
	EXPECT_EQ("STATIC-FIXTURE-V1", exportArtifact.exportId);
	EXPECT_EQ(0x8c010000u, exportArtifact.imageBase);
	EXPECT_EQ(1u, exportArtifact.memoryBlockCount);
	EXPECT_EQ(1u, exportArtifact.functionCount);
	EXPECT_EQ(1u, exportArtifact.symbolCount);
	EXPECT_EQ(1u, exportArtifact.dataTypeCount);
	EXPECT_NO_THROW(research::requireGhidraExportIdentity(exportArtifact, identity,
			fixture.programPath, fixture.scriptPath));
	EXPECT_NO_THROW(research::requireSh4EventsIdentity(manifest, identity));
	EXPECT_NO_THROW(research::requireGhidraExportSh4Join(exportArtifact, manifest));
}

TEST(ResearchGhidraExport, RejectsMalformedOrderingRangesAndCounts)
{
	GhidraTemporaryDirectory directory;
	Fixture fixture(directory);
	json base = exportJson(fixture.programDigest,
			std::filesystem::file_size(fixture.programPath), fixture.scriptDigest);

	json badCount = base;
	badCount["counts"]["functions"] = 2;
	const std::filesystem::path badCountPath = directory.file("bad-count.json");
	writeText(badCountPath, badCount.dump());
	EXPECT_THROW(research::loadGhidraExport(badCountPath), std::runtime_error);

	json outsideFunction = base;
	outsideFunction["functions"][0]["body_ranges"][0]["start_address"] = "0x8c020000";
	const std::filesystem::path outsidePath = directory.file("outside.json");
	writeText(outsidePath, outsideFunction.dump());
	EXPECT_THROW(research::loadGhidraExport(outsidePath), std::runtime_error);

	json unorderedTypes = base;
	unorderedTypes["data_types"].push_back({
		{"path", "/Before"}, {"kind", "other"}, {"length", 1},
		{"dynamic", false}, {"definition", "byte"},
	});
	unorderedTypes["counts"]["data_types"] = 2;
	const std::filesystem::path unorderedPath = directory.file("unordered.json");
	writeText(unorderedPath, unorderedTypes.dump());
	EXPECT_THROW(research::loadGhidraExport(unorderedPath), std::runtime_error);

	json unknownField = base;
	unknownField["unexpected"] = true;
	const std::filesystem::path unknownFieldPath = directory.file("unknown-field.json");
	writeText(unknownFieldPath, unknownField.dump());
	EXPECT_THROW(research::loadGhidraExport(unknownFieldPath), std::runtime_error);

	std::string duplicateKeyBytes = base.dump();
	const std::size_t schemaPosition = duplicateKeyBytes.find("\"schema\":");
	ASSERT_NE(std::string::npos, schemaPosition);
	duplicateKeyBytes.insert(schemaPosition,
			"\"schema\":\"flycast-research-ghidra-export\",");
	const std::filesystem::path duplicateKeyPath = directory.file("duplicate-key.json");
	writeText(duplicateKeyPath, duplicateKeyBytes);
	EXPECT_THROW(research::loadGhidraExport(duplicateKeyPath), std::runtime_error);

	json overlappingBlocks = base;
	overlappingBlocks["memory_blocks"].push_back({
		{"name", "overlap"}, {"start_address", "0x8c010800"}, {"length", 4096},
		{"read", true}, {"write", true}, {"execute", true}, {"initialized", true},
	});
	overlappingBlocks["counts"]["memory_blocks"] = 2;
	const std::filesystem::path overlappingBlocksPath =
			directory.file("overlapping-blocks.json");
	writeText(overlappingBlocksPath, overlappingBlocks.dump());
	EXPECT_THROW(research::loadGhidraExport(overlappingBlocksPath), std::runtime_error);

	json outsideSymbol = base;
	outsideSymbol["symbols"][0]["address"] = "0x8c020000";
	const std::filesystem::path outsideSymbolPath = directory.file("outside-symbol.json");
	writeText(outsideSymbolPath, outsideSymbol.dump());
	EXPECT_THROW(research::loadGhidraExport(outsideSymbolPath), std::runtime_error);

	json overlappingFunctions = base;
	json secondFunction = overlappingFunctions["functions"][0];
	secondFunction["entry_address"] = "0x8c010110";
	secondFunction["name"] = "overlapping_function";
	secondFunction["body_ranges"][0]["start_address"] = "0x8c010110";
	overlappingFunctions["functions"].push_back(secondFunction);
	overlappingFunctions["counts"]["functions"] = 2;
	const std::filesystem::path overlappingFunctionsPath =
			directory.file("overlapping-functions.json");
	writeText(overlappingFunctionsPath, overlappingFunctions.dump());
	EXPECT_THROW(research::loadGhidraExport(overlappingFunctionsPath), std::runtime_error);

	json oddFunction = base;
	oddFunction["functions"][0]["entry_address"] = "0x8c010101";
	const std::filesystem::path oddFunctionPath = directory.file("odd-function.json");
	writeText(oddFunctionPath, oddFunction.dump());
	EXPECT_THROW(research::loadGhidraExport(oddFunctionPath), std::runtime_error);

	json wrongLanguage = base;
	wrongLanguage["program"]["language_id"] = "SuperH4:BE:32:default";
	const std::filesystem::path wrongLanguagePath = directory.file("wrong-language.json");
	writeText(wrongLanguagePath, wrongLanguage.dump());
	EXPECT_THROW(research::loadGhidraExport(wrongLanguagePath), std::runtime_error);

	json derivedImageBase = base;
	derivedImageBase["program"]["ghidra_image_base"] = "0x00000000";
	derivedImageBase["program"]["image_base_source"] =
			"minimum-initialized-executable-block";
	const std::filesystem::path derivedPath = directory.file("derived-image-base.json");
	writeText(derivedPath, derivedImageBase.dump());
	EXPECT_EQ(0x8c010000u, research::loadGhidraExport(derivedPath).imageBase);

	json invalidDerivedImageBase = derivedImageBase;
	invalidDerivedImageBase["program"]["image_base"] = "0x8c010100";
	const std::filesystem::path invalidDerivedPath =
			directory.file("invalid-derived-image-base.json");
	writeText(invalidDerivedPath, invalidDerivedImageBase.dump());
	EXPECT_THROW(research::loadGhidraExport(invalidDerivedPath), std::runtime_error);

	json falseDerivedImageBase = base;
	falseDerivedImageBase["program"]["image_base_source"] =
			"minimum-initialized-executable-block";
	const std::filesystem::path falseDerivedPath =
			directory.file("false-derived-image-base.json");
	writeText(falseDerivedPath, falseDerivedImageBase.dump());
	EXPECT_THROW(research::loadGhidraExport(falseDerivedPath), std::runtime_error);

	json invalidIdentifier = base;
	invalidIdentifier["symbols"][0]["kind"] = "_label";
	const std::filesystem::path invalidIdentifierPath =
			directory.file("invalid-identifier.json");
	writeText(invalidIdentifierPath, invalidIdentifier.dump());
	EXPECT_THROW(research::loadGhidraExport(invalidIdentifierPath), std::runtime_error);

	json oversizedDataType = base;
	oversizedDataType["data_types"][0]["length"] = 4294967297ULL;
	const std::filesystem::path oversizedDataTypePath =
			directory.file("oversized-data-type.json");
	writeText(oversizedDataTypePath, oversizedDataType.dump());
	EXPECT_THROW(research::loadGhidraExport(oversizedDataTypePath), std::runtime_error);
}

TEST(ResearchGhidraExport, RejectsProgramScriptIdentityAndStaticJoinMismatch)
{
	GhidraTemporaryDirectory directory;
	Fixture fixture(directory);
	const research::GhidraExport exportArtifact = research::loadGhidraExport(fixture.exportPath);
	const research::IdentityManifest identity = research::loadIdentityManifest(fixture.identityPath);

	const std::filesystem::path wrongProgram = directory.file("wrong-program.bin");
	writeText(wrongProgram, "wrong program");
	EXPECT_THROW(research::requireGhidraExportIdentity(exportArtifact, identity, wrongProgram,
			fixture.scriptPath), std::runtime_error);

	const std::filesystem::path wrongScript = directory.file("wrong-script.java");
	writeText(wrongScript, "wrong script");
	EXPECT_THROW(research::requireGhidraExportIdentity(exportArtifact, identity,
			fixture.programPath, wrongScript), std::runtime_error);

	json wrongManifest = sh4ManifestJson(fixture.programDigest, fixture.exportDigest,
			fixture.hookDigest);
	wrongManifest["bindings"]["static_analysis_id"] = "OTHER-EXPORT";
	const std::filesystem::path wrongManifestPath = directory.file("wrong-manifest.json");
	writeText(wrongManifestPath, wrongManifest.dump());
	const research::Sh4EventsManifest manifest =
			research::loadSh4EventsManifest(wrongManifestPath);
	EXPECT_THROW(research::requireGhidraExportSh4Join(exportArtifact, manifest),
			std::runtime_error);
}

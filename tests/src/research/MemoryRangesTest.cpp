#include <gtest/gtest.h>

#include "json.hpp"
#include "research/identity_manifest.h"
#include "research/memory_ranges_artifact.h"
#include "research/memory_ranges_capture.h"
#include "research/memory_ranges_manifest.h"
#include "research/memory_ranges_runtime.h"
#include "research/sha256.h"
#include "cfg/option.h"
#include "ResearchRuntimeStubs.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace
{

using json = nlohmann::json;

class MemoryRangesTemporaryDirectory
{
public:
	MemoryRangesTemporaryDirectory()
	{
		static std::atomic<unsigned> sequence {0};
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path = std::filesystem::temp_directory_path()
				/ ("flycast-research-memory-ranges-test-" + std::to_string(stamp) + "-"
						+ std::to_string(sequence++));
		std::filesystem::create_directory(path);
	}

	~MemoryRangesTemporaryDirectory()
	{
		research::abortMemoryRangesRuntime();
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	std::filesystem::path file(const char *name) const { return path / name; }

private:
	std::filesystem::path path;
};

void writeMemoryRangesBytes(const std::filesystem::path& path,
		const std::vector<std::uint8_t>& bytes)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(output.good());
	output.write(reinterpret_cast<const char *>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
	ASSERT_TRUE(output.good());
}

void writeMemoryRangesText(const std::filesystem::path& path, const std::string& text)
{
	writeMemoryRangesBytes(path, std::vector<std::uint8_t>(text.begin(), text.end()));
}

std::string digestOf(const std::string& value)
{
	return research::sha256ToHex(research::sha256(value.data(), value.size()));
}

std::string digestOf(const std::vector<std::uint8_t>& value)
{
	return research::sha256ToHex(research::sha256(value.data(), value.size()));
}

struct MemoryRangesFixtureData
{
	std::vector<std::uint8_t> first {0x01, 0x23, 0x45, 0x67};
	std::vector<std::uint8_t> second {
		0x89, 0xab, 0xcd, 0xef, 0x10, 0x32, 0x54, 0x76,
		0x98, 0xba, 0xdc, 0xfe, 0x55, 0xaa, 0x00, 0xff, 0x42,
	};
	std::string executableDigest = digestOf("fixture executable");
	std::string staticAnalysisDigest = digestOf("fixture static analysis export");
	std::string hookManifestDigest = digestOf("fixture hook manifest");
};

json identityJson(const MemoryRangesFixtureData& fixture)
{
	json values {
		{"cpu_backend", "interpreter"},
		{"threaded_rendering", false},
		{"autoload_state", false},
		{"autosave_state", false},
		{"ggpo", false},
	};
	const std::string canonicalValues = values.dump();
	const std::string valuesDigest = research::sha256ToHex(
			research::sha256(canonicalValues.data(), canonicalValues.size()));
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

json manifestJson(const MemoryRangesFixtureData& fixture)
{
	return json {
		{"schema", "flycast-research-memory-ranges-manifest"},
		{"schema_version", 1},
		{"manifest_id", "FIXTURE-MEMORY-RANGES-V1"},
		{"address_space", "flycast-sh4-virtual"},
		{"bindings", {
			{"executable_sha256", fixture.executableDigest},
			{"static_analysis_id", "STATIC-FIXTURE-V1"},
			{"static_analysis_sha256", fixture.staticAnalysisDigest},
			{"hook_manifest_id", "HOOKS-FIXTURE-V1"},
			{"hook_manifest_sha256", fixture.hookManifestDigest},
		}},
		{"trigger", {
			{"kind", "guest-pc-enters-range"},
			{"start_address", "0x8c010000"},
			{"end_address_exclusive", "0x8c020000"},
		}},
		{"ranges", json::array({
			{
				{"range_id", "RANGE-FIRST"},
				{"address", "0x8c001000"},
				{"length", fixture.first.size()},
				{"expected_sha256", digestOf(fixture.first)},
			},
			{
				{"range_id", "RANGE-SECOND"},
				{"address", "0x8c002000"},
				{"length", fixture.second.size()},
				{"expected_sha256", digestOf(fixture.second)},
			},
		})},
		{"limits", {
			{"maximum_total_bytes", fixture.first.size() + fixture.second.size()},
		}},
		{"acceptance", {
			{"snapshot_consistency", "single-interpreter-boundary"},
			{"expected_trigger_count", 1},
			{"expected_event_count", 2},
			{"zero_dropped_events", true},
			{"natural_exit", true},
		}},
	};
}

std::filesystem::path writeMemoryRangesIdentity(const MemoryRangesTemporaryDirectory& directory,
		const MemoryRangesFixtureData& fixture, const char *name = "identity.json")
{
	const std::filesystem::path path = directory.file(name);
	writeMemoryRangesText(path, identityJson(fixture).dump(2));
	return path;
}

std::filesystem::path writeMemoryRangesManifest(const MemoryRangesTemporaryDirectory& directory,
		const MemoryRangesFixtureData& fixture, const char *name = "manifest.json")
{
	const std::filesystem::path path = directory.file(name);
	writeMemoryRangesText(path, manifestJson(fixture).dump(2));
	return path;
}

std::map<std::uint32_t, std::vector<std::uint8_t>> fixtureMemory(
		const MemoryRangesFixtureData& fixture)
{
	return {
		{0x8c001000, fixture.first},
		{0x8c002000, fixture.second},
	};
}

research::GuestMemoryReader mapReader(
		const std::map<std::uint32_t, std::vector<std::uint8_t>>& memory)
{
	return [&memory](std::uint32_t address, std::uint32_t length) -> const std::uint8_t * {
		const auto found = memory.find(address);
		if (found == memory.end() || found->second.size() != length)
			return nullptr;
		return found->second.data();
	};
}

} // namespace

TEST(ResearchMemoryRanges, ManifestValidatesSemanticsAndIdentityBindings)
{
	MemoryRangesTemporaryDirectory directory;
	MemoryRangesFixtureData fixture;
	const research::IdentityManifest identity = research::loadIdentityManifest(
			writeMemoryRangesIdentity(directory, fixture));
	const research::MemoryRangesManifest manifest = research::loadMemoryRangesManifest(
			writeMemoryRangesManifest(directory, fixture));
	EXPECT_EQ("FIXTURE-MEMORY-RANGES-V1", manifest.id);
	ASSERT_EQ(2u, manifest.ranges.size());
	EXPECT_EQ(0x8c010000u, manifest.triggerStart);
	EXPECT_EQ(0x8c020000u, manifest.triggerEndExclusive);
	EXPECT_EQ(fixture.first.size() + fixture.second.size(), manifest.maximumTotalBytes);
	EXPECT_NO_THROW(research::requireMemoryRangesIdentity(manifest, identity));

	MemoryRangesFixtureData wrongFixture = fixture;
	wrongFixture.staticAnalysisDigest = digestOf("different export");
	const research::IdentityManifest wrongIdentity = research::loadIdentityManifest(
			writeMemoryRangesIdentity(directory, wrongFixture, "wrong-identity.json"));
	EXPECT_THROW(research::requireMemoryRangesIdentity(manifest, wrongIdentity), std::runtime_error);
}

TEST(ResearchMemoryRanges, ManifestRejectsDuplicateKeysOverlapAndCountMismatch)
{
	MemoryRangesTemporaryDirectory directory;
	MemoryRangesFixtureData fixture;
	json root = manifestJson(fixture);

	std::string duplicate = root.dump(2);
	const std::string needle = "\"schema_version\": 1,";
	const std::size_t position = duplicate.find(needle);
	ASSERT_NE(std::string::npos, position);
	duplicate.insert(position + needle.size(), "\n  \"schema_version\": 1,");
	writeMemoryRangesText(directory.file("duplicate.json"), duplicate);
	EXPECT_THROW(research::loadMemoryRangesManifest(directory.file("duplicate.json")),
			std::runtime_error);

	root = manifestJson(fixture);
	root["ranges"][1]["address"] = "0x8c001002";
	writeMemoryRangesText(directory.file("overlap.json"), root.dump(2));
	EXPECT_THROW(research::loadMemoryRangesManifest(directory.file("overlap.json")),
			std::runtime_error);

	root = manifestJson(fixture);
	root["acceptance"]["expected_event_count"] = 1;
	writeMemoryRangesText(directory.file("count.json"), root.dump(2));
	EXPECT_THROW(research::loadMemoryRangesManifest(directory.file("count.json")),
			std::runtime_error);
}

TEST(ResearchMemoryRanges, CapturesExactlyOneInterpreterBoundaryAndValidatesRawBytes)
{
	MemoryRangesTemporaryDirectory directory;
	MemoryRangesFixtureData fixture;
	const research::IdentityManifest identity = research::loadIdentityManifest(
			writeMemoryRangesIdentity(directory, fixture));
	const research::MemoryRangesManifest manifest = research::loadMemoryRangesManifest(
			writeMemoryRangesManifest(directory, fixture));
	const auto memory = fixtureMemory(fixture);
	const std::filesystem::path artifactPath = directory.file("memory-ranges.fcmr");
	research::MemoryRangesCapture capture(artifactPath, identity, manifest);
	EXPECT_FALSE(capture.observeInstruction(0x8c00ffff, 100, mapReader(memory)));
	EXPECT_TRUE(capture.observeInstruction(0x8c010000, 1234, mapReader(memory)));
	EXPECT_FALSE(capture.observeInstruction(0x8c010002, 1235, mapReader(memory)));
	const research::MemoryRangesArtifactSummary written = capture.finish();
	EXPECT_EQ(3u, written.eventCount);
	EXPECT_EQ(1u, written.triggerCount);
	EXPECT_EQ(2u, written.rangeEventCount);
	EXPECT_EQ(21u, written.totalMemoryBytes);
	EXPECT_EQ(1234u, written.startTick);
	EXPECT_EQ(1234u, written.endTick);
	EXPECT_EQ(0u, written.droppedEvents);

	const research::MemoryRangesArtifactSummary validated =
			research::validateProductionMemoryRangesArtifactFile(
					artifactPath, identity, manifest);
	EXPECT_EQ(written.payloadDigest, validated.payloadDigest);
	EXPECT_EQ(0x8c010000u, validated.triggerPc);
	EXPECT_EQ(21u, validated.totalMemoryBytes);
	EXPECT_EQ("8006b60d75bbcb20666b294f0dac7560e3061e6c78cadd8c995d5d4037a81b40",
			research::sha256ToHex(research::hashFileExact(
					artifactPath, research::DefaultMaximumMemoryRangesArtifactBytes)));
}

TEST(ResearchMemoryRanges, IndependentValidatorRejectsWrongBytesMutationAndIncompleteOutput)
{
	MemoryRangesTemporaryDirectory directory;
	MemoryRangesFixtureData fixture;
	const research::IdentityManifest identity = research::loadIdentityManifest(
			writeMemoryRangesIdentity(directory, fixture));
	const research::MemoryRangesManifest manifest = research::loadMemoryRangesManifest(
			writeMemoryRangesManifest(directory, fixture));

	auto wrongMemory = fixtureMemory(fixture);
	wrongMemory.at(0x8c002000).back() ^= 1;
	const std::filesystem::path wrongPath = directory.file("wrong-bytes.fcmr");
	{
		research::MemoryRangesCapture capture(wrongPath, identity, manifest);
		ASSERT_TRUE(capture.observeInstruction(0x8c010100, 77, mapReader(wrongMemory)));
		capture.finish();
	}
	EXPECT_THROW(research::validateProductionMemoryRangesArtifactFile(
			wrongPath, identity, manifest), std::runtime_error);

	const auto memory = fixtureMemory(fixture);
	const std::filesystem::path validPath = directory.file("valid.fcmr");
	{
		research::MemoryRangesCapture capture(validPath, identity, manifest);
		ASSERT_TRUE(capture.observeInstruction(0x8c010100, 77, mapReader(memory)));
		capture.finish();
	}
	std::vector<std::uint8_t> mutated = research::readFileExact(
			validPath, research::DefaultMaximumMemoryRangesArtifactBytes);
	mutated.back() ^= 1;
	const std::filesystem::path mutatedPath = directory.file("mutated.fcmr");
	writeMemoryRangesBytes(mutatedPath, mutated);
	EXPECT_THROW(research::validateProductionMemoryRangesArtifactFile(
			mutatedPath, identity, manifest), std::runtime_error);

	const std::filesystem::path incompletePath = directory.file("incomplete.fcmr");
	{
		research::MemoryRangesCapture capture(incompletePath, identity, manifest);
		ASSERT_TRUE(capture.observeInstruction(0x8c010100, 77, mapReader(memory)));
		capture.abandon();
	}
	EXPECT_THROW(research::validateProductionMemoryRangesArtifactFile(
			incompletePath, identity, manifest), std::runtime_error);
}

TEST(ResearchMemoryRanges, WriterFailsClosedForInvalidLifecycleAndUnsupportedMemory)
{
	MemoryRangesTemporaryDirectory directory;
	MemoryRangesFixtureData fixture;
	const research::IdentityManifest identity = research::loadIdentityManifest(
			writeMemoryRangesIdentity(directory, fixture));
	const research::MemoryRangesManifest manifest = research::loadMemoryRangesManifest(
			writeMemoryRangesManifest(directory, fixture));
	const std::filesystem::path boundedPath = directory.file("bounded.fcmr");
	EXPECT_THROW(research::MemoryRangesCapture(boundedPath, identity, manifest,
			research::MemoryRangesArtifactHeaderSize), std::runtime_error);
	EXPECT_FALSE(std::filesystem::exists(boundedPath));

	const std::filesystem::path missingPath = directory.file("missing-memory.fcmr");
	{
		research::MemoryRangesCapture capture(missingPath, identity, manifest);
		EXPECT_THROW(capture.observeInstruction(0x8c010100, 88,
				[](std::uint32_t, std::uint32_t) -> const std::uint8_t * { return nullptr; }),
				std::runtime_error);
		EXPECT_THROW(capture.finish(), std::logic_error);
		capture.abandon();
	}

	const auto memory = fixtureMemory(fixture);
	const std::filesystem::path existingPath = directory.file("existing.fcmr");
	{
		research::MemoryRangesCapture capture(existingPath, identity, manifest);
		ASSERT_TRUE(capture.observeInstruction(0x8c010100, 88, mapReader(memory)));
		capture.finish();
	}
	EXPECT_THROW(research::MemoryRangesCapture(existingPath, identity, manifest),
			std::exception);
}

TEST(ResearchMemoryRanges, RuntimeArmsFiltersCapturesAndFinalizesAtCleanExit)
{
	MemoryRangesTemporaryDirectory directory;
	MemoryRangesFixtureData fixture;
	const std::filesystem::path identityPath = writeMemoryRangesIdentity(directory, fixture);
	const std::filesystem::path manifestPath = writeMemoryRangesManifest(directory, fixture);
	const std::filesystem::path artifactPath = directory.file("runtime.fcmr");
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const research::MemoryRangesManifest manifest = research::loadMemoryRangesManifest(manifestPath);

	research_test::clearGuestRam();
	ASSERT_TRUE(research_test::writeGuestRam(0x8c001000, fixture.first));
	ASSERT_TRUE(research_test::writeGuestRam(0x8c002000, fixture.second));
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchMemoryRangesManifestPath = manifestPath.string();
	config::ResearchMemoryRangesRecordPath = artifactPath.string();
	config::ResearchMemoryRangesMaxBytes = research::DefaultMaximumMemoryRangesArtifactBytes;
	config::ResearchMapleRecordPath = "";
	config::ResearchMapleReplayPath = "";

	research::configureMemoryRangesRuntime();
	research::startMemoryRangesRuntime();
	ASSERT_TRUE(research::memoryRangesRuntimeActive());
	research::memoryRangesInstructionBoundary(0x8c00fffe, 900);
	research::memoryRangesInstructionBoundary(0x8c010000, 901);
	research::memoryRangesInstructionBoundary(0x8c010002, 902);
	research::stopMemoryRangesRuntime(true);
	EXPECT_FALSE(research::memoryRangesRuntimeActive());

	const research::MemoryRangesArtifactSummary summary =
			research::validateProductionMemoryRangesArtifactFile(
					artifactPath, identity, manifest);
	EXPECT_EQ(0x8c010000u, summary.triggerPc);
	EXPECT_EQ(901u, summary.startTick);
	EXPECT_EQ(2u, summary.rangeEventCount);
	EXPECT_EQ(0u, summary.droppedEvents);
}

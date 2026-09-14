#include <gtest/gtest.h>

#include "json.hpp"
#include "research/identity_manifest.h"
#include "research/maple_observation.h"
#include "research/maple_trace.h"
#include "research/sha256.h"
#ifndef RESEARCH_FORMAT_ONLY
#include "cfg/option.h"
#include "hw/flashrom/nvmem.h"
#include "research/maple_runtime.h"
#include "ResearchRuntimeStubs.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#if !defined(RESEARCH_FORMAT_ONLY) \
		&& !defined(FLYCAST_RESEARCH_STANDALONE_TESTS)
namespace research_test
{
void setInitialFlashData(const std::vector<std::uint8_t>& bytes)
{
	nvmem::setInitialFlashDataForTesting(bytes.data(), bytes.size());
}
}
#endif

namespace
{

using json = nlohmann::json;

class TemporaryDirectory
{
public:
	TemporaryDirectory()
	{
		static std::atomic<unsigned> sequence {0};
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		path = std::filesystem::temp_directory_path()
				/ ("flycast-research-maple-test-" + std::to_string(stamp) + "-"
						+ std::to_string(sequence++));
		std::filesystem::create_directory(path);
	}

	~TemporaryDirectory()
	{
#ifndef RESEARCH_FORMAT_ONLY
		research::abortRuntime();
#endif
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	std::filesystem::path file(const char *name) const { return path / name; }

private:
	std::filesystem::path path;
};

void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes)
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

std::string zeroDigest()
{
	return std::string(64, '0');
}

std::filesystem::path writeIdentity(const TemporaryDirectory& directory,
		const char *name = "identity.json", std::uint64_t dmaCheckpoint = 0)
{
	json values {
		{"cpu_backend", "interpreter"},
		{"threaded_rendering", false},
		{"autoload_state", false},
		{"autosave_state", false},
		{"ggpo", false},
	};
	if (dmaCheckpoint != 0)
		values["maple_dma_checkpoint"] = dmaCheckpoint;
	const std::string canonicalValues = values.dump();
	const std::string valuesDigest = research::sha256ToHex(
			research::sha256(canonicalValues.data(), canonicalValues.size()));
	const json blob {
		{"path", "descriptive-only.bin"},
		{"size", 0},
		{"sha256", zeroDigest()},
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
			{"sha256", valuesDigest},
		}},
	};
	json track = blob;
	track["track"] = 1;
	track["start_fad"] = 150;
	track["sector_size"] = 2048;
	track["offset"] = 0;
	root["media"]["tracks"] = json::array({track});
	const std::filesystem::path path = directory.file(name);
	writeText(path, root.dump(2));
	return path;
}

std::filesystem::path writeObservedDynarecDiagnosticIdentity(
		const TemporaryDirectory& directory,
		const std::filesystem::path& recordIdentityPath)
{
	const research::IdentityManifest recordIdentity =
			research::loadIdentityManifest(recordIdentityPath);
	json root = json::parse(std::ifstream(recordIdentityPath));
	root["schema_version"] = 2;
	json& values = root["configuration"]["values"];
	values["cpu_backend"] = "dynarec";
	values["dynarec_observation"] = true;
	values["dynarec_replay_diagnostic"] = true;
	values["dreamcast_rtc_seed"] = 0;
	const std::string canonicalValues = values.dump();
	root["configuration"]["sha256"] = research::sha256ToHex(
			research::sha256(canonicalValues.data(), canonicalValues.size()));
	root["equivalence"] = {
		{"maple_replay_identity_sha256",
			research::sha256ToHex(recordIdentity.digest)},
	};
	const std::filesystem::path path =
			directory.file("observed-dynarec-diagnostic-identity.json");
	writeText(path, root.dump(2));
	return path;
}

std::vector<std::uint8_t> littleEndianWords(std::initializer_list<std::uint32_t> words)
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

struct FixtureEvents
{
	FixtureEvents()
	{
		begin.tick = 100;
		begin.descriptorAddress = 0x0c001000;
		begin.mden = 1;
		begin.mdst = 1;
		begin.mmsel = 1;
		begin.trigger = research::MapleDmaTrigger::Software;
		begin.swapMsb = false;

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
		transaction.response = littleEndianWords({0x02002008, 0x01000000, 0xffff0000});

		schedule.dmaOrdinal = 0;
		schedule.tick = 100;
		schedule.inputWireBytes = 11;
		schedule.outputWireBytes = 15;
		schedule.scheduledCycles = 1000;
		schedule.responseCount = 1;

		commit.dmaOrdinal = 0;
		commit.tick = 1100;
		commit.callbackCycles = 1000;
		commit.jitter = 0;
		commit.responseCount = 1;
		commit.flags = research::MapleCommitInterruptRaised;
	}

	research::MapleDmaBeginEvent begin;
	research::MapleTransactionEvent transaction;
	research::MapleDmaScheduleEvent schedule;
	research::MapleDmaCommitEvent commit;
};

std::filesystem::path writeTrace(const TemporaryDirectory& directory,
		const research::Sha256Digest& identity, const char *name = "maple.fcmr")
{
	FixtureEvents events;
	const std::filesystem::path path = directory.file(name);
	research::MapleTraceWriter writer(path, identity);
	EXPECT_EQ(0u, writer.beginDma(events.begin));
	EXPECT_EQ(0u, writer.writeTransaction(events.transaction));
	writer.scheduleDma(events.schedule);
	writer.commitDma(events.commit);
	const research::MapleTraceSummary summary = writer.finalize();
	EXPECT_EQ(1u, summary.dmaCount);
	EXPECT_EQ(1u, summary.transactionCount);
	EXPECT_EQ(4u, summary.eventCount);
	return path;
}

} // namespace

TEST(ResearchSha256, KnownVectorsAndIncrementalInput)
{
	EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
			research::sha256ToHex(research::sha256(nullptr, 0)));
	constexpr char Abc[] = "abc";
	EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
			research::sha256ToHex(research::sha256(Abc, 3)));
	research::Sha256 incremental;
	incremental.update(Abc, 1);
	incremental.update(Abc + 1, 2);
	EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
			research::sha256ToHex(incremental.finalize()));
}

TEST(ResearchIdentity, ValidatesSchemaAndConfigurationDigest)
{
	TemporaryDirectory directory;
	const std::filesystem::path identityPath = writeIdentity(directory);
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	EXPECT_FALSE(identity.bytes.empty());
	EXPECT_EQ("interpreter", identity.runtimeConfiguration.cpuBackend);
	EXPECT_FALSE(identity.runtimeConfiguration.threadedRendering);
	EXPECT_EQ("gdi", identity.mediaKind);
	EXPECT_EQ(1u, identity.mediaTrackCount);
	EXPECT_NO_THROW(research::requireCaptureV1Identity(identity));
	EXPECT_EQ(identity.digest, research::hashFileExact(identityPath,
			research::MaxIdentityManifestBytes));

	std::string text(identity.bytes.begin(), identity.bytes.end());
	json root = json::parse(text);
	root["configuration"]["sha256"] = zeroDigest();
	const std::filesystem::path invalidPath = directory.file("identity-invalid.json");
	writeText(invalidPath, root.dump());
	EXPECT_THROW(research::loadIdentityManifest(invalidPath), std::runtime_error);

	root["configuration"]["values"]["threaded_rendering"] = true;
	const std::string changedValues = root["configuration"]["values"].dump();
	root["configuration"]["sha256"] = research::sha256ToHex(
			research::sha256(changedValues.data(), changedValues.size()));
	writeText(invalidPath, root.dump());
	EXPECT_THROW(research::loadIdentityManifest(invalidPath), std::runtime_error);
}

TEST(ResearchIdentity, BindsMutuallyExclusiveSh4PcTerminalForV2Replay)
{
	TemporaryDirectory directory;
	const research::IdentityManifest base = research::loadIdentityManifest(
			writeIdentity(directory));
	json root = json::parse(std::string(base.bytes.begin(), base.bytes.end()));
	root["schema_version"] = 2;
	root["equivalence"] = {
		{"maple_replay_identity_sha256", zeroDigest()},
	};
	json& values = root["configuration"]["values"];
	values["dynarec_observation"] = false;
	values["dreamcast_rtc_seed"] = 0;

	auto writeCandidate = [&](const char *name) {
		const std::string canonical = values.dump();
		root["configuration"]["sha256"] = research::sha256ToHex(
				research::sha256(canonical.data(), canonical.size()));
		const std::filesystem::path path = directory.file(name);
		writeText(path, root.dump(2));
		return path;
	};

	const auto legacyPath = writeCandidate("identity-v2-legacy.json");
	const auto legacy = research::loadIdentityManifest(legacyPath);
	EXPECT_EQ(0u, legacy.runtimeConfiguration.mapleDmaCheckpoint);
	EXPECT_EQ(0u, legacy.runtimeConfiguration.sh4PcCheckpoint);

	values["maple_dma_checkpoint"] = 0;
	values["sh4_pc_checkpoint"] = 0x8c097a7e;
	const auto pcPath = writeCandidate("identity-v2-pc.json");
	const auto pc = research::loadIdentityManifest(pcPath);
	EXPECT_EQ(0u, pc.runtimeConfiguration.mapleDmaCheckpoint);
	EXPECT_EQ(0x8c097a7eu, pc.runtimeConfiguration.sh4PcCheckpoint);

	values.erase("sh4_pc_checkpoint");
	EXPECT_THROW(research::loadIdentityManifest(
			writeCandidate("identity-v2-zero-maple-only.json")), std::runtime_error);

	values["sh4_pc_checkpoint"] = 0x8c097a7f;
	EXPECT_THROW(research::loadIdentityManifest(
			writeCandidate("identity-v2-odd-pc.json")), std::runtime_error);

	values["sh4_pc_checkpoint"] = 0x8c097a7e;
	values["maple_dma_checkpoint"] = 1;
	EXPECT_THROW(research::loadIdentityManifest(
			writeCandidate("identity-v2-two-terminals.json")), std::runtime_error);
}

TEST(ResearchIdentity, AuthenticatesRealDreamcastFirmwareAndLoadedBytes)
{
	TemporaryDirectory directory;
	const research::IdentityManifest hle = research::loadIdentityManifest(
			writeIdentity(directory));
	json root = json::parse(std::string(hle.bytes.begin(), hle.bytes.end()));
	std::vector<std::uint8_t> bios(research::DreamcastBiosBytes);
	for (std::size_t index = 0; index < bios.size(); ++index)
		bios[index] = static_cast<std::uint8_t>((index * 29u + 7u) & 0xffu);
	std::vector<std::uint8_t> flash(research::DreamcastFlashBytes);
	for (std::size_t index = 0; index < flash.size(); ++index)
		flash[index] = static_cast<std::uint8_t>((index * 11u + 3u) & 0xffu);
	const auto biosPath = directory.file("dc_boot.bin");
	const auto flashPath = directory.file("dc_nvmem.bin");
	writeBytes(biosPath, bios);
	writeBytes(flashPath, flash);
	root["firmware"] = {
		{"mode", "real"},
		{"bios", {
			{"path", biosPath.u8string()},
			{"size", bios.size()},
			{"sha256", research::sha256ToHex(research::sha256(
					bios.data(), bios.size()))},
		}},
		{"flash_initial", {
			{"path", flashPath.u8string()},
			{"size", flash.size()},
			{"sha256", research::sha256ToHex(research::sha256(
					flash.data(), flash.size()))},
		}},
	};
	const auto realIdentityPath = directory.file("identity-real.json");
	writeText(realIdentityPath, root.dump(2));
	const auto real = research::loadIdentityManifest(realIdentityPath);
	EXPECT_EQ(research::FirmwareMode::Real, real.firmware.mode);
	EXPECT_EQ(research::DreamcastBiosBytes, real.firmware.bios.size);
	EXPECT_NO_THROW(research::authenticateFirmwareFiles(real));
	EXPECT_NO_THROW(research::authenticateLoadedDreamcastFirmware(real, false,
			bios.data(), bios.size()));
	EXPECT_NO_THROW(research::authenticateLoadedDreamcastFlash(real,
			flash.data(), flash.size()));
	EXPECT_THROW(research::authenticateLoadedDreamcastFirmware(real, true,
			bios.data(), bios.size()), std::runtime_error);

	bios[12345] ^= 0x80;
	EXPECT_THROW(research::authenticateLoadedDreamcastFirmware(real, false,
			bios.data(), bios.size()), std::runtime_error);
	writeBytes(biosPath, bios);
	EXPECT_THROW(research::authenticateFirmwareFiles(real), std::runtime_error);
	flash[91] ^= 1;
	EXPECT_THROW(research::authenticateLoadedDreamcastFlash(real,
			flash.data(), flash.size()), std::runtime_error);
	EXPECT_NO_THROW(research::authenticateLoadedDreamcastFirmware(hle, true,
			nullptr, 0));
	EXPECT_THROW(research::authenticateLoadedDreamcastFirmware(hle, false,
			nullptr, 0), std::runtime_error);
}

TEST(ResearchIdentity, RejectsUnboundOrWrongSizedRealBios)
{
	TemporaryDirectory directory;
	const research::IdentityManifest hle = research::loadIdentityManifest(
			writeIdentity(directory));
	json root = json::parse(std::string(hle.bytes.begin(), hle.bytes.end()));
	root["firmware"]["mode"] = "real";
	root["firmware"].erase("hle_identity");
	root["firmware"]["flash_initial"]["size"] = research::DreamcastFlashBytes;
	root["firmware"]["bios"] = {
		{"path", ""}, {"size", research::DreamcastBiosBytes},
		{"sha256", zeroDigest()},
	};
	const auto path = directory.file("identity-real-invalid.json");
	writeText(path, root.dump());
	EXPECT_THROW(research::loadIdentityManifest(path), std::runtime_error);
	root["firmware"]["bios"]["path"] = "dc_boot.bin";
	root["firmware"]["bios"]["size"] = research::DreamcastBiosBytes - 1;
	writeText(path, root.dump());
	const auto wrongSize = research::loadIdentityManifest(path);
	std::vector<std::uint8_t> bytes(research::DreamcastBiosBytes - 1, 0);
	EXPECT_THROW(research::authenticateLoadedDreamcastFirmware(wrongSize, false,
			bytes.data(), bytes.size()), std::runtime_error);
}

TEST(ResearchIdentity, RejectsWrongSizedRealInitialFlash)
{
	TemporaryDirectory directory;
	const research::IdentityManifest hle = research::loadIdentityManifest(
			writeIdentity(directory));
	json root = json::parse(std::string(hle.bytes.begin(), hle.bytes.end()));
	std::vector<std::uint8_t> bios(research::DreamcastBiosBytes);
	const auto biosPath = directory.file("dc_boot.bin");
	const auto flashPath = directory.file("dc_nvmem-empty.bin");
	writeBytes(biosPath, bios);
	writeBytes(flashPath, {});
	root["firmware"] = {
		{"mode", "real"},
		{"bios", {{"path", biosPath.u8string()}, {"size", bios.size()},
			{"sha256", research::sha256ToHex(research::sha256(
					bios.data(), bios.size()))}}},
		{"flash_initial", {{"path", flashPath.u8string()}, {"size", 0},
			{"sha256", research::sha256ToHex(research::sha256(nullptr, 0))}}},
	};
	const auto path = directory.file("identity-real-empty-flash.json");
	writeText(path, root.dump(2));
	EXPECT_THROW(research::loadIdentityManifest(path), std::runtime_error);
}

TEST(ResearchIdentity, AuthenticatesBoundInitialSavestate)
{
	TemporaryDirectory directory;
	const std::filesystem::path basePath = writeIdentity(directory);
	const research::IdentityManifest base = research::loadIdentityManifest(basePath);
	json root = json::parse(std::string(base.bytes.begin(), base.bytes.end()));
	root["initial_state"] = json::object();
	const std::filesystem::path invalidV1 = directory.file("state-v1-invalid.json");
	writeText(invalidV1, root.dump(2));
	EXPECT_THROW(research::loadIdentityManifest(invalidV1), std::runtime_error);
	root = json::parse(std::string(base.bytes.begin(), base.bytes.end()));
	const std::filesystem::path statePath = directory.file("game.state");
	const std::vector<std::uint8_t> stateBytes {0x46, 0x4c, 0x59, 0x53, 1, 2, 3};
	writeBytes(statePath, stateBytes);
	root["initial_state"] = {
		{"kind", "savestate"},
		{"slot", 1},
		{"blob", {
			{"path", statePath.u8string()},
			{"size", stateBytes.size()},
			{"sha256", research::sha256ToHex(research::sha256(
					stateBytes.data(), stateBytes.size()))},
		}},
	};
	root["schema_version"] = 3;
	root["configuration"]["values"]["dynarec_observation"] = false;
	root["configuration"]["values"]["dreamcast_rtc_seed"] = 123456789u;
	root["configuration"]["values"]["autoload_state"] = true;
	root["configuration"]["values"]["savestate_slot"] = 1;
	root["configuration"]["values"]["maple_dma_checkpoint"] = 120;
	const std::string canonical = root["configuration"]["values"].dump();
	root["configuration"]["sha256"] = research::sha256ToHex(
			research::sha256(canonical.data(), canonical.size()));
	const std::filesystem::path stateIdentity = directory.file("state-identity.json");
	writeText(stateIdentity, root.dump(2));

	const research::IdentityManifest identity =
			research::loadIdentityManifest(stateIdentity);
	ASSERT_TRUE(identity.initialState.available);
	EXPECT_TRUE(identity.runtimeConfiguration.autoLoadState);
	EXPECT_EQ(1u, identity.initialState.slot);
	EXPECT_EQ(1u, identity.runtimeConfiguration.savestateSlot);
	EXPECT_EQ(120u, identity.runtimeConfiguration.mapleDmaCheckpoint);
	EXPECT_NO_THROW(research::requireMapleRecordIdentityV3(identity));
	EXPECT_NO_THROW(research::authenticateInitialStateFile(identity, statePath));

	json profileRoot = json::parse(std::string(identity.bytes.begin(),
			identity.bytes.end()));
	profileRoot["configuration"]["values"]["cpu_backend"] = "dynarec";
	profileRoot["configuration"]["values"]["dynarec_observation"] = false;
	profileRoot["configuration"]["values"]["dynarec_profile"] = true;
	const std::string profileCanonical =
			profileRoot["configuration"]["values"].dump();
	profileRoot["configuration"]["sha256"] = research::sha256ToHex(
			research::sha256(profileCanonical.data(), profileCanonical.size()));
	const std::filesystem::path profileIdentityPath =
			directory.file("state-profile-identity.json");
	writeText(profileIdentityPath, profileRoot.dump(2));
	const auto profileIdentity = research::loadIdentityManifest(profileIdentityPath);
	EXPECT_NO_THROW(research::requireMapleRecordIdentityV3(profileIdentity));
	EXPECT_NO_THROW(
			research::requireSh4DynarecProfileRecordIdentityV3(profileIdentity));

	writeBytes(statePath, {0x46, 0x4c, 0x59, 0x53, 1, 2, 4});
	EXPECT_THROW(research::authenticateInitialStateFile(identity, statePath),
			std::runtime_error);

	root["configuration"]["values"]["autoload_state"] = false;
	const std::string mismatch = root["configuration"]["values"].dump();
	root["configuration"]["sha256"] = research::sha256ToHex(
			research::sha256(mismatch.data(), mismatch.size()));
	writeText(stateIdentity, root.dump(2));
	EXPECT_THROW(research::loadIdentityManifest(stateIdentity), std::runtime_error);
}

TEST(ResearchIdentity, CaptureV1RequiresGdiWithTracks)
{
	TemporaryDirectory directory;
	const research::IdentityManifest identity = research::loadIdentityManifest(writeIdentity(directory));
	std::string text(identity.bytes.begin(), identity.bytes.end());
	json root = json::parse(text);

	root["media"]["kind"] = "cue";
	const std::filesystem::path cuePath = directory.file("identity-cue.json");
	writeText(cuePath, root.dump());
	const research::IdentityManifest cue = research::loadIdentityManifest(cuePath);
	EXPECT_THROW(research::requireCaptureV1Identity(cue), std::runtime_error);

	root["media"]["kind"] = "gdi";
	root["media"]["tracks"] = json::array();
	const std::filesystem::path emptyTracksPath = directory.file("identity-no-tracks.json");
	writeText(emptyTracksPath, root.dump());
	const research::IdentityManifest emptyTracks = research::loadIdentityManifest(emptyTracksPath);
	EXPECT_THROW(research::requireCaptureV1Identity(emptyTracks), std::runtime_error);
}

TEST(ResearchIdentity, HashesFilesInMultipleWindows)
{
	TemporaryDirectory directory;
	const std::filesystem::path path = directory.file("large-blob.bin");
	constexpr std::size_t ChunkBytes = 64 * 1024;
	constexpr std::uint64_t TotalBytes = 3ull * 1024 * 1024 + 137;
	std::vector<std::uint8_t> chunk(ChunkBytes);
	for (std::size_t i = 0; i < chunk.size(); ++i)
		chunk[i] = static_cast<std::uint8_t>((i * 131u + 17u) & 0xffu);
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(output.good());
	research::Sha256 expectedHasher;
	std::uint64_t remaining = TotalBytes;
	while (remaining != 0)
	{
		const std::size_t count = static_cast<std::size_t>(
				std::min<std::uint64_t>(remaining, chunk.size()));
		output.write(reinterpret_cast<const char *>(chunk.data()),
				static_cast<std::streamsize>(count));
		expectedHasher.update(chunk.data(), count);
		remaining -= count;
	}
	output.close();
	ASSERT_TRUE(output.good());
	EXPECT_EQ(expectedHasher.finalize(), research::hashFileExact(path, TotalBytes));
	EXPECT_THROW(research::hashFileExact(path, TotalBytes - 1), std::runtime_error);
}

TEST(ResearchMapleTrace, RoundTripsProductionTrace)
{
	TemporaryDirectory directory;
	const research::IdentityManifest identity = research::loadIdentityManifest(writeIdentity(directory));
	const std::filesystem::path tracePath = writeTrace(directory, identity.digest);
	const research::MapleTrace trace = research::loadProductionMapleTrace(tracePath, identity.digest);
	const research::MapleTraceSummary streamed = research::validateProductionMapleTraceFile(
			tracePath, identity.digest);
	ASSERT_EQ(4u, trace.events.size());
	EXPECT_EQ(1u, trace.summary.dmaCount);
	EXPECT_EQ(1u, trace.summary.transactionCount);
	EXPECT_EQ(100u, trace.summary.startTick);
	EXPECT_EQ(1100u, trace.summary.endTick);
	EXPECT_EQ(trace.summary.payloadDigest, streamed.payloadDigest);
	EXPECT_EQ(trace.summary.eventCount, streamed.eventCount);
	const auto& transaction = std::get<research::MapleTransactionEvent>(trace.events[1].data);
	EXPECT_EQ(FixtureEvents().transaction.request, transaction.request);
	EXPECT_EQ(FixtureEvents().transaction.response, transaction.response);
}

TEST(ResearchMapleTrace, V1BinaryContractRemainsByteExact)
{
	TemporaryDirectory directory;
	const research::IdentityManifest identity = research::loadIdentityManifest(writeIdentity(directory));
	const std::filesystem::path tracePath = writeTrace(directory, identity.digest);
	EXPECT_EQ("08f5d03624936e03c601ea3697db182f09390489fe489102720adb743526e4b8",
			research::sha256ToHex(research::hashFileExact(
					tracePath, research::DefaultMaximumMapleTraceBytes)));
}

TEST(ResearchMapleTrace, V2RoundTripsTerminalNopAndPreservesDescriptorChain)
{
	TemporaryDirectory directory;
	const research::IdentityManifest identity = research::loadIdentityManifest(
			writeIdentity(directory));
	const std::filesystem::path tracePath = directory.file("maple-v2.fcmt");
	FixtureEvents events;
	events.transaction.descriptorHeader1 &= 0x7fffffffu;

	research::MapleControlDescriptorEvent control;
	control.dmaOrdinal = 0;
	control.tick = events.transaction.tick;
	control.descriptorAddress = events.transaction.descriptorAddress + 8
			+ static_cast<std::uint32_t>(events.transaction.request.size());
	control.descriptorHeader = 0x80000700;
	control.operation = research::MapleControlOperation::Nop;
	control.last = true;

	research::MapleTraceWriter writer(tracePath, identity.digest,
			research::DefaultMaximumMapleTraceBytes,
			research::MapleTraceSchemaVersionV2);
	writer.beginDma(events.begin);
	writer.writeTransaction(events.transaction);
	EXPECT_EQ(0u, writer.writeControlDescriptor(control));
	writer.scheduleDma(events.schedule);
	writer.commitDma(events.commit);
	const research::MapleTraceSummary written = writer.finalize();
	EXPECT_EQ(research::MapleTraceSchemaVersionV2, written.schemaVersion);
	EXPECT_EQ(1u, written.controlDescriptorCount);

	const research::MapleTrace trace = research::loadProductionMapleTrace(
			tracePath, identity.digest);
	const research::MapleTraceSummary streamed =
			research::validateProductionMapleTraceFile(tracePath, identity.digest);
	ASSERT_EQ(5u, trace.events.size());
	EXPECT_EQ(research::MapleTraceSchemaVersionV2, streamed.schemaVersion);
	EXPECT_EQ(1u, streamed.controlDescriptorCount);
	const auto& decoded = std::get<research::MapleControlDescriptorEvent>(
			trace.events[2].data);
	EXPECT_EQ(control.descriptorAddress, decoded.descriptorAddress);
	EXPECT_EQ(control.descriptorHeader, decoded.descriptorHeader);
	EXPECT_EQ(research::MapleControlOperation::Nop, decoded.operation);
	EXPECT_TRUE(decoded.last);
}

TEST(ResearchMapleTrace, V2RejectsBrokenControlDescriptorContinuity)
{
	TemporaryDirectory directory;
	const research::IdentityManifest identity = research::loadIdentityManifest(
			writeIdentity(directory));
	FixtureEvents events;
	events.transaction.descriptorHeader1 &= 0x7fffffffu;
	research::MapleTraceWriter writer(directory.file("bad-control.fcmt"),
			identity.digest, research::DefaultMaximumMapleTraceBytes,
			research::MapleTraceSchemaVersionV2);
	writer.beginDma(events.begin);
	writer.writeTransaction(events.transaction);
	research::MapleControlDescriptorEvent control;
	control.dmaOrdinal = 0;
	control.tick = events.transaction.tick;
	control.descriptorAddress = events.transaction.descriptorAddress + 4;
	control.descriptorHeader = 0x80000700;
	control.operation = research::MapleControlOperation::Nop;
	control.last = true;
	EXPECT_THROW(writer.writeControlDescriptor(control), std::invalid_argument);
	writer.abandon();
}

TEST(ResearchMapleTrace, StreamsTraceLargerThanOneMebibyte)
{
	TemporaryDirectory directory;
	const research::IdentityManifest identity = research::loadIdentityManifest(writeIdentity(directory));
	const std::filesystem::path tracePath = directory.file("large-maple.fcmr");
	FixtureEvents events;
	events.transaction.descriptorHeader1 = 0x800000ff;
	events.transaction.request.assign(1024, 0);
	events.transaction.request[0] = events.transaction.command;
	events.transaction.request[3] = 255;
	events.transaction.response.assign(1024, 0);
	events.transaction.response[3] = 255;
	events.schedule.inputWireBytes = 1027;
	events.schedule.outputWireBytes = 1027;
	constexpr std::uint64_t DmaCount = 480;
	{
		research::MapleTraceWriter writer(tracePath, identity.digest);
		for (std::uint64_t index = 0; index < DmaCount; ++index)
		{
			const std::uint64_t tick = 100 + index * 2000;
			events.begin.tick = tick;
			const std::uint64_t dma = writer.beginDma(events.begin);
			events.transaction.dmaOrdinal = dma;
			events.transaction.tick = tick;
			writer.writeTransaction(events.transaction);
			events.schedule.dmaOrdinal = dma;
			events.schedule.tick = tick;
			writer.scheduleDma(events.schedule);
			events.commit.dmaOrdinal = dma;
			events.commit.tick = tick + 1000;
			writer.commitDma(events.commit);
		}
		writer.finalize();
	}
	ASSERT_GT(std::filesystem::file_size(tracePath), 1024u * 1024u);
	const research::MapleTraceSummary streamed = research::validateProductionMapleTraceFile(
			tracePath, identity.digest);
	const research::MapleTrace retained = research::loadProductionMapleTrace(
			tracePath, identity.digest);
	EXPECT_EQ(DmaCount, streamed.dmaCount);
	EXPECT_EQ(DmaCount, streamed.transactionCount);
	EXPECT_EQ(DmaCount * 4, streamed.eventCount);
	EXPECT_EQ(retained.summary.payloadDigest, streamed.payloadDigest);
	EXPECT_EQ(retained.summary.endTick, streamed.endTick);
}

TEST(ResearchMapleTrace, RejectsIncompleteMutationIdentityAndExistingOutput)
{
	TemporaryDirectory directory;
	const research::IdentityManifest identity = research::loadIdentityManifest(writeIdentity(directory));
	const std::filesystem::path tracePath = writeTrace(directory, identity.digest);

	research::Sha256Digest otherIdentity = identity.digest;
	otherIdentity[0] ^= 0xff;
	EXPECT_THROW(research::loadProductionMapleTrace(tracePath, otherIdentity), std::runtime_error);
	EXPECT_THROW(research::validateProductionMapleTraceFile(tracePath, otherIdentity),
			std::runtime_error);
	EXPECT_THROW(research::MapleTraceWriter(tracePath, identity.digest), std::exception);

	const std::filesystem::path mutatedPath = directory.file("mutated.fcmr");
	std::filesystem::copy_file(tracePath, mutatedPath);
	std::vector<std::uint8_t> mutated = research::readFileExact(mutatedPath,
			research::DefaultMaximumMapleTraceBytes);
	mutated.back() ^= 0x01;
	writeBytes(mutatedPath, mutated);
	EXPECT_THROW(research::loadProductionMapleTrace(mutatedPath, identity.digest),
			std::runtime_error);
	EXPECT_THROW(research::validateProductionMapleTraceFile(mutatedPath, identity.digest),
			std::runtime_error);

	const std::filesystem::path incompletePath = directory.file("incomplete.fcmr");
	{
		FixtureEvents events;
		research::MapleTraceWriter writer(incompletePath, identity.digest);
		writer.beginDma(events.begin);
		writer.writeTransaction(events.transaction);
		writer.scheduleDma(events.schedule);
		writer.commitDma(events.commit);
		writer.abandon();
	}
	EXPECT_THROW(research::loadProductionMapleTrace(incompletePath, identity.digest),
			std::runtime_error);
	EXPECT_THROW(research::validateProductionMapleTraceFile(incompletePath, identity.digest),
			std::runtime_error);
}

TEST(ResearchMapleTrace, WriterRejectsImpossibleLifecycle)
{
	TemporaryDirectory directory;
	const research::IdentityManifest identity = research::loadIdentityManifest(writeIdentity(directory));
	FixtureEvents events;
	research::MapleTraceWriter writer(directory.file("lifecycle.fcmr"), identity.digest);
	const std::uint64_t dma = writer.beginDma(events.begin);
	EXPECT_THROW(writer.beginDma(events.begin), std::logic_error);
	events.transaction.dmaOrdinal = dma;
	writer.writeTransaction(events.transaction);
	events.schedule.dmaOrdinal = dma;
	events.schedule.responseCount = 2;
	EXPECT_THROW(writer.scheduleDma(events.schedule), std::invalid_argument);
	EXPECT_THROW(writer.finalize(), std::logic_error);
	writer.abandon();

	research::MapleTraceWriter boundedWriter(directory.file("bounded.fcmr"), identity.digest,
			research::MapleTraceHeaderSize + 51);
	EXPECT_THROW(boundedWriter.beginDma(events.begin), std::runtime_error);
	boundedWriter.abandon();
}

TEST(ResearchMapleTrace, IndependentValidationRejectsAbortAndWireMismatch)
{
	TemporaryDirectory directory;
	const research::IdentityManifest identity = research::loadIdentityManifest(writeIdentity(directory));
	FixtureEvents events;

	const std::filesystem::path abortedPath = directory.file("aborted.fcmr");
	{
		research::MapleTraceWriter writer(abortedPath, identity.digest);
		writer.beginDma(events.begin);
		writer.writeTransaction(events.transaction);
		research::MapleDmaAbortEvent abort;
		abort.dmaOrdinal = 0;
		abort.tick = 101;
		abort.reason = research::MapleDmaAbortReason::Shutdown;
		abort.stage = 3;
		writer.abortDma(abort);
		writer.finalize();
	}
	EXPECT_THROW(research::loadProductionMapleTrace(abortedPath, identity.digest),
			std::runtime_error);
	EXPECT_THROW(research::validateProductionMapleTraceFile(abortedPath, identity.digest),
			std::runtime_error);

	const std::filesystem::path mismatchPath = directory.file("wire-mismatch.fcmr");
	{
		research::MapleTraceWriter writer(mismatchPath, identity.digest);
		writer.beginDma(events.begin);
		writer.writeTransaction(events.transaction);
		++events.schedule.inputWireBytes;
		writer.scheduleDma(events.schedule);
		writer.commitDma(events.commit);
		writer.finalize();
	}
	EXPECT_THROW(research::loadProductionMapleTrace(mismatchPath, identity.digest),
			std::runtime_error);
	EXPECT_THROW(research::validateProductionMapleTraceFile(mismatchPath, identity.digest),
			std::runtime_error);
}

#ifndef RESEARCH_FORMAT_ONLY
unsigned checkpointCalls = 0;

class DiagnosticConfigurationResetGuard
{
public:
	~DiagnosticConfigurationResetGuard()
	{
		research::abortRuntime();
		config::DynarecEnabled.reset();
		config::ResearchDynarecObservation.reset();
		config::ResearchDreamcastRtcSeed.reset();
		config::ThreadedRendering.reset();
		config::UseReios.reset();
		config::AutoLoadState.reset();
		config::AutoSaveState.reset();
		config::GGPOEnable.reset();
		config::ResearchIdentityManifestPath.reset();
		config::ResearchMapleRecordPath.reset();
		config::ResearchMapleReplayPath.reset();
		config::ResearchMapleDmaCheckpoint.reset();
		config::ResearchMapleTraceMaxBytes.reset();
		config::ResearchSh4ObservationRecordPath.reset();
		config::ResearchSh4EventsRecordPath.reset();
		config::ResearchSh4ProfileRecordPath.reset();
		config::ResearchPvrTaRecordPath.reset();
		config::ResearchGdromRecordPath.reset();
	}
};

class MapleObservationSubscriptionGuard
{
public:
	explicit MapleObservationSubscriptionGuard(
			research::MapleObservationSubscription token)
		: token(token)
	{
	}

	~MapleObservationSubscriptionGuard()
	{
		research::unsubscribeMapleObservations(token);
	}

private:
	research::MapleObservationSubscription token;
};

void countCheckpoint()
{
	++checkpointCalls;
}

TEST(ResearchMapleReplay, RejectsFirstRequestDivergence)
{
	TemporaryDirectory directory;
	const std::filesystem::path identityPath = writeIdentity(directory);
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const std::filesystem::path tracePath = writeTrace(directory, identity.digest);

	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchMapleRecordPath = "";
	config::ResearchMapleReplayPath = tracePath.string();
	config::ResearchMapleTraceMaxBytes = research::DefaultMaximumMapleTraceBytes;
	config::setTransient("research", "IdentityManifest", identityPath.string());
	config::setTransient("research", "MapleReplay", tracePath.string());
	research::configureRuntime();
	research::startRuntime();

	FixtureEvents events;
	const std::uint64_t dma = research::mapleBeginDma(events.begin);
	ASSERT_EQ(0u, dma);
	events.transaction.dmaOrdinal = dma;
	events.transaction.request.back() ^= 1;
	EXPECT_THROW(research::mapleTransaction(events.transaction), FlycastException);
	research::abortRuntime();
}

TEST(ResearchMapleReplay,
		DiagnosticIdentityEnablesTimingInstrumentationWithoutTraceWriter)
{
	DiagnosticConfigurationResetGuard resetConfiguration;
	TemporaryDirectory directory;
	const std::filesystem::path recordIdentityPath = writeIdentity(directory);
	const research::IdentityManifest recordIdentity =
			research::loadIdentityManifest(recordIdentityPath);
	const std::filesystem::path tracePath =
			writeTrace(directory, recordIdentity.digest);
	const std::filesystem::path identityPath =
			writeObservedDynarecDiagnosticIdentity(directory, recordIdentityPath);

	config::DynarecEnabled.set(false);
	config::ResearchDynarecObservation.set(false);
	config::ResearchSh4ObservationRecordPath.reset();
	config::ResearchSh4EventsRecordPath.reset();
	config::ResearchSh4ProfileRecordPath.reset();
	config::ResearchPvrTaRecordPath.reset();
	config::ResearchGdromRecordPath.reset();
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchMapleRecordPath = "";
	config::ResearchMapleReplayPath = tracePath.string();
	config::ResearchMapleDmaCheckpoint = 0;
	config::ResearchMapleTraceMaxBytes = research::DefaultMaximumMapleTraceBytes;
	config::setTransient("research", "IdentityManifest", identityPath.string());
	config::setTransient("research", "MapleReplay", tracePath.string());

	EXPECT_NO_THROW(research::configureRuntime());
	EXPECT_TRUE(config::DynarecEnabled.get());
	EXPECT_TRUE(config::ResearchDynarecObservation.get());
	EXPECT_NO_THROW(research::startRuntime());
	FixtureEvents events;
	const std::uint64_t dma = research::mapleBeginDma(events.begin);
	ASSERT_EQ(0u, dma);
	events.transaction.dmaOrdinal = dma;
	research::mapleTransaction(events.transaction);
	research::mapleScheduleDma(events.schedule);
	research::mapleCommitDma(events.commit);
	EXPECT_TRUE(research::mapleReplayConsumed());
	EXPECT_NO_THROW(research::stopRuntime(true));
	EXPECT_FALSE(research::mapleReplayConsumed());
}

TEST(ResearchMapleReplay, RealFirmwareReplayAuthenticatesLoadedBiosAndFlash)
{
	TemporaryDirectory directory;
	const auto hlePath = writeIdentity(directory);
	const auto hle = research::loadIdentityManifest(hlePath);
	json root = json::parse(std::string(hle.bytes.begin(), hle.bytes.end()));
	std::vector<std::uint8_t> bios(research::DreamcastBiosBytes);
	std::vector<std::uint8_t> flash(research::DreamcastFlashBytes);
	for (std::size_t index = 0; index < bios.size(); ++index)
		bios[index] = static_cast<std::uint8_t>(index * 7u + 1u);
	for (std::size_t index = 0; index < flash.size(); ++index)
		flash[index] = static_cast<std::uint8_t>(index * 19u + 3u);
	const auto biosPath = directory.file("dc_boot.bin");
	const auto flashPath = directory.file("dc_nvmem.bin");
	writeBytes(biosPath, bios); writeBytes(flashPath, flash);
	root["firmware"] = {
		{"mode", "real"},
		{"bios", {{"path", biosPath.u8string()}, {"size", bios.size()},
			{"sha256", research::sha256ToHex(research::sha256(
				bios.data(), bios.size()))}}},
		{"flash_initial", {{"path", flashPath.u8string()},
			{"size", flash.size()}, {"sha256", research::sha256ToHex(
				research::sha256(flash.data(), flash.size()))}}},
	};
	const auto identityPath = directory.file("identity-real-replay.json");
	writeText(identityPath, root.dump(2));
	const auto identity = research::loadIdentityManifest(identityPath);
	const auto tracePath = writeTrace(directory, identity.digest, "real-replay.fcmt");
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchMapleRecordPath = "";
	config::ResearchMapleReplayPath = tracePath.string();
	config::ResearchMapleDmaCheckpoint = 0;
	config::ResearchMapleTraceMaxBytes = research::DefaultMaximumMapleTraceBytes;
	config::setTransient("research", "IdentityManifest", identityPath.string());
	config::setTransient("research", "MapleReplay", tracePath.string());

	research::configureRuntime();
	EXPECT_THROW(research::startRuntime(), std::runtime_error);
	research::abortRuntime();
	std::copy(bios.begin(), bios.end(), nvmem::getBiosData());
	research_test::setInitialFlashData(flash);
	research::configureRuntime();
	EXPECT_NO_THROW(research::startRuntime());
	research::abortRuntime();
	// RTC, partition repair and other emulated writes affect the live chip after
	// load. They must not change which initial file supplied the session.
	nvmem::getFlashData()[123] ^= 1;
	research::configureRuntime();
	EXPECT_NO_THROW(research::startRuntime());
	research::abortRuntime();
	flash[123] ^= 1;
	research_test::setInitialFlashData(flash);
	research::configureRuntime();
	EXPECT_THROW(research::startRuntime(), std::runtime_error);
	research::abortRuntime();
}

TEST(ResearchMapleReplay, HleReplayAuthenticatesDeclaredLoadedFlash)
{
	TemporaryDirectory directory;
	const auto sourcePath = writeIdentity(directory);
	const auto source = research::loadIdentityManifest(sourcePath);
	json root = json::parse(std::string(source.bytes.begin(), source.bytes.end()));
	std::vector<std::uint8_t> flash(research::DreamcastFlashBytes);
	for (std::size_t index = 0; index < flash.size(); ++index)
		flash[index] = static_cast<std::uint8_t>(index * 23u + 5u);
	const auto flashPath = directory.file("dc_nvmem.bin");
	writeBytes(flashPath, flash);
	root["firmware"]["flash_initial"] = {
		{"path", flashPath.u8string()},
		{"size", flash.size()},
		{"sha256", research::sha256ToHex(research::sha256(
				flash.data(), flash.size()))},
	};
	const auto identityPath = directory.file("identity-hle-flash-replay.json");
	writeText(identityPath, root.dump(2));
	const auto identity = research::loadIdentityManifest(identityPath);
	const auto tracePath = writeTrace(directory, identity.digest,
			"hle-flash-replay.fcmt");
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchMapleRecordPath = "";
	config::ResearchMapleReplayPath = tracePath.string();
	config::ResearchMapleDmaCheckpoint = 0;
	config::ResearchMapleTraceMaxBytes = research::DefaultMaximumMapleTraceBytes;
	config::setTransient("research", "IdentityManifest", identityPath.string());
	config::setTransient("research", "MapleReplay", tracePath.string());

	research_test::setInitialFlashData(flash);
	research::configureRuntime();
	EXPECT_NO_THROW(research::startRuntime());
	research::abortRuntime();
	flash[321] ^= 1;
	research_test::setInitialFlashData(flash);
	research::configureRuntime();
	EXPECT_THROW(research::startRuntime(), std::runtime_error);
	research::abortRuntime();
}

TEST(ResearchMapleReplay, HleReplayRejectsNonzeroWrongSizedFlashDeclaration)
{
	TemporaryDirectory directory;
	const auto sourcePath = writeIdentity(directory);
	const auto source = research::loadIdentityManifest(sourcePath);
	json root = json::parse(std::string(source.bytes.begin(), source.bytes.end()));
	std::vector<std::uint8_t> flash(research::DreamcastFlashBytes);
	for (std::size_t index = 0; index < flash.size(); ++index)
		flash[index] = static_cast<std::uint8_t>(index * 31u + 9u);
	const auto flashPath = directory.file("dc_nvmem-short.bin");
	writeBytes(flashPath, flash);
	root["firmware"]["flash_initial"] = {
		{"path", flashPath.u8string()},
		{"size", research::DreamcastFlashBytes - 1},
		{"sha256", research::sha256ToHex(research::sha256(
				flash.data(), flash.size()))},
	};
	const auto identityPath = directory.file("identity-hle-short-flash.json");
	writeText(identityPath, root.dump(2));
	const auto identity = research::loadIdentityManifest(identityPath);
	const auto tracePath = writeTrace(directory, identity.digest,
			"hle-short-flash-replay.fcmt");
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchMapleRecordPath = "";
	config::ResearchMapleReplayPath = tracePath.string();
	config::ResearchMapleDmaCheckpoint = 0;
	config::ResearchMapleTraceMaxBytes = research::DefaultMaximumMapleTraceBytes;
	config::setTransient("research", "IdentityManifest", identityPath.string());
	config::setTransient("research", "MapleReplay", tracePath.string());

	research_test::setInitialFlashData(flash);
	research::configureRuntime();
	EXPECT_THROW(research::startRuntime(), std::runtime_error);
	research::abortRuntime();
}

TEST(ResearchMapleReplay, ReplaysTypedNopControlDescriptor)
{
	TemporaryDirectory directory;
	const std::filesystem::path identityPath = writeIdentity(directory);
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const std::filesystem::path tracePath = directory.file("nop-replay.fcmt");
	FixtureEvents events;
	events.transaction.descriptorHeader1 &= 0x7fffffffu;
	research::MapleControlDescriptorEvent control;
	control.dmaOrdinal = 0;
	control.tick = events.transaction.tick;
	control.descriptorAddress = events.transaction.descriptorAddress + 8
			+ static_cast<std::uint32_t>(events.transaction.request.size());
	control.descriptorHeader = 0x80000700;
	control.operation = research::MapleControlOperation::Nop;
	control.last = true;
	{
		research::MapleTraceWriter writer(tracePath, identity.digest,
				research::DefaultMaximumMapleTraceBytes,
				research::MapleTraceSchemaVersionV2);
		writer.beginDma(events.begin);
		writer.writeTransaction(events.transaction);
		writer.writeControlDescriptor(control);
		writer.scheduleDma(events.schedule);
		writer.commitDma(events.commit);
		writer.finalize();
	}

	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchMapleRecordPath = "";
	config::ResearchMapleReplayPath = tracePath.string();
	config::ResearchMapleDmaCheckpoint = 0;
	config::ResearchMapleTraceMaxBytes = research::DefaultMaximumMapleTraceBytes;
	config::setTransient("research", "IdentityManifest", identityPath.string());
	config::setTransient("research", "MapleReplay", tracePath.string());
	research::configureRuntime();
	research::startRuntime();
	EXPECT_FALSE(research::mapleReplayConsumed());
	EXPECT_EQ(0u, research::mapleBeginDma(events.begin));
	EXPECT_FALSE(research::mapleReplayConsumed());
	research::mapleTransaction(events.transaction);
	EXPECT_NO_THROW(research::mapleControlDescriptor(control));
	research::mapleScheduleDma(events.schedule);
	EXPECT_FALSE(research::mapleReplayConsumed());
	research::mapleCommitDma(events.commit);
	EXPECT_TRUE(research::mapleReplayConsumed());
	EXPECT_NO_THROW(research::stopRuntime(true));
	EXPECT_FALSE(research::mapleReplayConsumed());
}

TEST(ResearchMapleReplay, LuaObservationUsesAuthenticatedReplayResponse)
{
	TemporaryDirectory directory;
	const std::filesystem::path identityPath = writeIdentity(directory);
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const std::filesystem::path tracePath = writeTrace(directory, identity.digest);
	FixtureEvents events;
	const std::vector<std::uint8_t> recordedResponse = events.transaction.response;
	std::vector<std::uint8_t> observedResponse;
	research::MapleObservationFilter filter;
	filter.typeMask = research::mapleObservationTypeBit(
			research::MapleObservationType::Response);
	MapleObservationSubscriptionGuard subscription(
			research::subscribeMapleObservations(filter,
					[&observedResponse](const research::MapleObservation& observation) {
						observedResponse = observation.payload;
					}));

	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchMapleRecordPath = "";
	config::ResearchMapleReplayPath = tracePath.string();
	config::ResearchMapleTraceMaxBytes = research::DefaultMaximumMapleTraceBytes;
	config::setTransient("research", "IdentityManifest", identityPath.string());
	config::setTransient("research", "MapleReplay", tracePath.string());
	research::configureRuntime();
	research::startRuntime();

	const std::uint64_t observationDma = research::beginMapleObservationDma();
	const std::uint64_t dma = research::mapleBeginDma(events.begin);
	events.transaction.dmaOrdinal = dma;
	events.transaction.response[0] ^= 0xff;
	const std::vector<std::uint8_t> selected = research::mapleTransaction(
			events.transaction);
	ASSERT_EQ(recordedResponse, selected);
	ASSERT_TRUE(research::publishMapleTransactionObservations(observationDma,
			events.transaction, selected));
	research::mapleScheduleDma(events.schedule);
	research::mapleCommitDma(events.commit);
	EXPECT_NO_THROW(research::stopRuntime(true));
	EXPECT_EQ(recordedResponse, observedResponse);
	EXPECT_NE(events.transaction.response, observedResponse);
}

TEST(ResearchMapleReplay, DmaCheckpointStopsRecordAndReplayAtTerminalCommit)
{
	TemporaryDirectory directory;
	const std::filesystem::path identityPath = writeIdentity(
			directory, "checkpoint-identity.json", 1);
	const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
	const std::filesystem::path tracePath = directory.file("checkpoint.fcmt");
	FixtureEvents events;

	checkpointCalls = 0;
	research::setMapleCheckpointHandler(countCheckpoint);
	config::ResearchIdentityManifestPath = identityPath.string();
	config::ResearchMapleRecordPath = tracePath.string();
	config::ResearchMapleReplayPath = "";
	config::ResearchMapleDmaCheckpoint = 1;
	config::ResearchMapleTraceMaxBytes = research::DefaultMaximumMapleTraceBytes;
	config::setTransient("research", "IdentityManifest", identityPath.string());
	config::setTransient("research", "MapleRecord", tracePath.string());
	config::setTransient("research", "MapleDmaCheckpoint", "1");
	research::configureRuntime();
	research::startRuntime();
	EXPECT_EQ(0u, research::mapleBeginDma(events.begin));
	research::mapleTransaction(events.transaction);
	research::mapleScheduleDma(events.schedule);
	EXPECT_EQ(0u, checkpointCalls);
	research::mapleCommitDma(events.commit);
	EXPECT_EQ(1u, checkpointCalls);
	EXPECT_NO_THROW(research::stopRuntime(true));
	const research::MapleTraceSummary recorded = research::validateProductionMapleTraceFile(
			tracePath, identity.digest);
	EXPECT_EQ(1u, recorded.dmaCount);
	EXPECT_EQ(research::MapleTraceSchemaVersionV2, recorded.schemaVersion);

	checkpointCalls = 0;
	config::ResearchMapleRecordPath = "";
	config::ResearchMapleReplayPath = tracePath.string();
	config::setTransient("research", "MapleReplay", tracePath.string());
	research::configureRuntime();
	research::startRuntime();
	EXPECT_EQ(0u, research::mapleBeginDma(events.begin));
	research::mapleTransaction(events.transaction);
	research::mapleScheduleDma(events.schedule);
	research::mapleCommitDma(events.commit);
	EXPECT_EQ(1u, checkpointCalls);
	EXPECT_NO_THROW(research::stopRuntime(true));

	config::ResearchMapleDmaCheckpoint = 0;
	research::setMapleCheckpointHandler(nullptr);
}
#endif

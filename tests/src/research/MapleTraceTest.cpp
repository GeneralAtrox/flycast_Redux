#include <gtest/gtest.h>

#include "json.hpp"
#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/sha256.h"
#ifndef RESEARCH_FORMAT_ONLY
#include "cfg/option.h"
#include "research/maple_runtime.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
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
	EXPECT_EQ(1u, research::validateProductionMapleTraceFile(
			tracePath, identity.digest).dmaCount);

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

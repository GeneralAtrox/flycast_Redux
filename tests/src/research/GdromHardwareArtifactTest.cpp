#include "research/gdrom_hardware_artifact.h"

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>

namespace
{
class TemporaryDirectory
{
public:
	TemporaryDirectory()
	{
		static std::atomic<unsigned> sequence {0};
		path = std::filesystem::temp_directory_path()
				/ ("flycast-gdrom-hardware-test-" + std::to_string(sequence++));
		std::filesystem::create_directories(path);
	}
	~TemporaryDirectory()
	{
		std::error_code error; std::filesystem::remove_all(path, error);
	}
	std::filesystem::path file(const char *name) const { return path / name; }
private:
	std::filesystem::path path;
};

void writeBytes(const std::filesystem::path& path,
		const std::vector<std::uint8_t>& bytes)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.write(reinterpret_cast<const char *>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
}
void writeText(const std::filesystem::path& path, const std::string& text)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc); output << text;
}
research::Sh4InstructionOwnerToken owner(std::uint64_t generation,
		std::uint64_t tick, std::uint32_t pc)
{
	research::Sh4InstructionOwnerToken value;
	value.valid = true; value.backend = research::Sh4ObservationBackend::Interpreter;
	value.generation = generation; value.tick = tick;
	value.pc = pc; value.opcode = 0x2102; value.pr = 0x8c020000;
	return value;
}

std::filesystem::path writeReplay(const TemporaryDirectory& directory,
		const research::Sha256Digest& identity)
{
	const auto path = directory.file("maple-replay.fcmt");
	research::MapleTraceWriter writer(path, identity);
	research::MapleDmaBeginEvent begin;
	begin.tick = 100; begin.descriptorAddress = 0x0c001000;
	begin.mden = 1; begin.mdst = 1; begin.mmsel = 1;
	begin.trigger = research::MapleDmaTrigger::Software;
	EXPECT_EQ(0u, writer.beginDma(begin));
	research::MapleTransactionEvent transaction;
	transaction.dmaOrdinal = 0; transaction.tick = 100;
	transaction.descriptorAddress = 0x0c001000;
	transaction.destinationAddress = 0x0c002000;
	transaction.descriptorHeader1 = 0x80000001;
	transaction.descriptorHeader2 = 0x0c002000;
	transaction.deviceType = 0; transaction.bus = 0; transaction.port = 5;
	transaction.command = 0x09;
	transaction.flags = research::MapleTransactionDevicePresent;
	transaction.request = {0x09, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01};
	transaction.response = {0x08, 0x20, 0x00, 0x02, 0x00, 0x00,
			0x00, 0x01, 0x00, 0x00, 0xff, 0xff};
	EXPECT_EQ(0u, writer.writeTransaction(transaction));
	research::MapleDmaScheduleEvent schedule;
	schedule.dmaOrdinal = 0; schedule.tick = 100;
	schedule.inputWireBytes = 11; schedule.outputWireBytes = 15;
	schedule.scheduledCycles = 1000; schedule.responseCount = 1;
	writer.scheduleDma(schedule);
	research::MapleDmaCommitEvent commit;
	commit.dmaOrdinal = 0; commit.tick = 1100; commit.callbackCycles = 1000;
	commit.responseCount = 1; commit.flags = research::MapleCommitInterruptRaised;
	writer.commitDma(commit); writer.finalize();
	return path;
}

struct Fixture
{
	TemporaryDirectory directory;
	std::filesystem::path descriptor = directory.file("disc.gdi");
	std::filesystem::path artifact = directory.file("capture.fcgd-hw");
	research::IdentityManifest identity;
	research::Sha256Digest replay = research::sha256("real-replay", 11);
	std::vector<std::uint8_t> raw;

	Fixture()
	{
		const auto track1 = directory.file("track01.bin");
		const auto track2 = directory.file("track02.raw");
		const auto track3 = directory.file("track03.bin");
		std::vector<std::uint8_t> first(2352, 0x11), audio(2352, 0x22);
		raw.assign(2 * 2352, 0);
		for (std::size_t index = 0; index < 2048; ++index)
		{
			raw[16 + index] = static_cast<std::uint8_t>(index * 17u + 3u);
			raw[2352 + 24 + index] = static_cast<std::uint8_t>(index * 29u + 5u);
		}
		raw[15] = 1; raw[2352 + 15] = 2;
		writeBytes(track1, first); writeBytes(track2, audio); writeBytes(track3, raw);
		writeText(descriptor,
				"3\n1 0 4 2352 \"track01.bin\" 0\n"
				"2 300 0 2352 \"track02.raw\" 0\n"
				"3 45000 4 2352 \"track03.bin\" 0\n");
		identity.schemaVersion = 2; identity.mediaKind = "gdi";
		identity.mediaSourcePath = descriptor;
		identity.mediaSourceSize = std::filesystem::file_size(descriptor);
		identity.mediaSourceDigest = research::hashFileExact(descriptor,
				identity.mediaSourceSize);
		for (const auto& [number, fad, path] : {
				std::tuple<std::uint32_t, std::uint32_t, std::filesystem::path>(1, 150, track1),
				std::tuple<std::uint32_t, std::uint32_t, std::filesystem::path>(2, 450, track2),
				std::tuple<std::uint32_t, std::uint32_t, std::filesystem::path>(3, 45150, track3)})
		{
			research::MediaTrackIdentity track;
			track.path = path; track.size = std::filesystem::file_size(path);
			track.digest = research::hashFileExact(path, track.size);
			track.track = number; track.startFad = fad; track.sectorSize = 2352;
			identity.mediaTracks.push_back(track);
		}
		identity.mediaTrackCount = identity.mediaTracks.size();
		identity.digest = research::sha256("real-identity", 13);
		identity.firmware.mode = research::FirmwareMode::Real;
		std::vector<std::uint8_t> bios(research::DreamcastBiosBytes, 0x5a);
		std::vector<std::uint8_t> flash(research::DreamcastFlashBytes);
		for (std::size_t index = 0; index < flash.size(); ++index)
			flash[index] = static_cast<std::uint8_t>(index * 13u + 1u);
		identity.firmware.bios.available = true;
		identity.firmware.bios.path = directory.file("dc_boot.bin");
		identity.firmware.bios.size = bios.size();
		identity.firmware.bios.digest = research::sha256(bios.data(), bios.size());
		writeBytes(identity.firmware.bios.path, bios);
		identity.firmware.initialFlash.available = true;
		identity.firmware.initialFlash.path = directory.file("dc_nvmem.bin");
		identity.firmware.initialFlash.size = flash.size();
		identity.firmware.initialFlash.digest = research::sha256(
				flash.data(), flash.size());
		writeBytes(identity.firmware.initialFlash.path, flash);
	}

	std::vector<std::uint8_t> expected(std::uint32_t fad,
			std::uint32_t sectors, std::uint32_t sectorSize) const
	{
		std::vector<std::uint8_t> bytes;
		for (std::uint32_t index = 0; index < sectors; ++index)
		{
			const std::uint32_t current = fad + index;
			if (current < 45150 || current >= 45152)
			{
				bytes.insert(bytes.end(), sectorSize, 0); continue;
			}
			const auto begin = raw.begin() + std::size_t(current - 45150) * 2352;
			std::size_t offset = 0;
			if (sectorSize == 2048) offset = *(begin + 15) == 1 ? 16 : 24;
			else if (sectorSize == 2340) offset = 12;
			bytes.insert(bytes.end(), begin + offset, begin + offset + sectorSize);
		}
		return bytes;
	}

	void capture(std::uint32_t fad, std::uint32_t sectors,
			std::uint32_t sectorSize, bool corrupt = false,
			bool wrongDestination = false, bool omitDmaInterrupt = false,
			bool omitStatusAcknowledgement = false)
	{
		research::GdromHardwareArtifactBinding binding;
		binding.identityDigest = identity.digest; binding.replayDigest = replay;
		binding.biosDigest = identity.firmware.bios.digest;
		binding.flashDigest = identity.firmware.initialFlash.digest;
		research::GdromHardwareArtifactWriter writer(artifact, binding);
		std::uint64_t emission = 0, tick = 10;
		research::GdromHardwareObservation packet;
		packet.type = research::GdromHardwareObservationType::PacketAccepted;
		packet.emissionOrdinal = emission++; packet.tick = tick++;
		packet.commandGeneration = 1; packet.ataOwner = owner(1, 5, 0x8c010100);
		packet.packetOwner = owner(2, 9, 0x8c010200);
		packet.packet[0] = 0x30;
		packet.packet[1] = sectorSize == 2048 ? 0x20
				: sectorSize == 2340 ? 0xe6 : 0x10;
		packet.packet[2] = static_cast<std::uint8_t>(fad >> 16);
		packet.packet[3] = static_cast<std::uint8_t>(fad >> 8);
		packet.packet[4] = static_cast<std::uint8_t>(fad);
		packet.packet[8] = static_cast<std::uint8_t>(sectors >> 16);
		packet.packet[9] = static_cast<std::uint8_t>(sectors >> 8);
		packet.packet[10] = static_cast<std::uint8_t>(sectors);
		packet.features = 1; packet.startFad = fad;
		packet.sectorCount = sectors; packet.sectorBytes = sectorSize;
		packet.delivery = research::GdromHardwareDelivery::Dma;
		writer.write(packet);

		const auto bytes = expected(fad, sectors, sectorSize);
		std::uint64_t produced = 0;
		auto fillCache = [&]
		{
			const auto remainingSectors = static_cast<std::uint32_t>(
					(bytes.size() - produced) / sectorSize);
			const auto count = (std::min)(16u, remainingSectors);
			research::GdromHardwareObservation fill;
			fill.type = research::GdromHardwareObservationType::BufferFill;
			fill.emissionOrdinal = emission++; fill.tick = tick++;
			fill.commandGeneration = 1;
			fill.streamOffset = produced;
			fill.startFad = fad + static_cast<std::uint32_t>(produced / sectorSize);
			fill.sectorCount = count; fill.sectorBytes = sectorSize;
			fill.readSuccessful = true; writer.write(fill);
			produced += std::uint64_t(count) * sectorSize;
		};
		fillCache();
		std::uint64_t offset = 0;
		std::uint64_t dmaGeneration = 0;
		while (offset < bytes.size())
		{
			const std::uint64_t sessionBytes = (std::min<std::uint64_t>)(
					bytes.size() - offset, 20480);
			++dmaGeneration;
			research::GdromHardwareObservation begin;
			begin.type = research::GdromHardwareObservationType::DmaBegin;
			begin.emissionOrdinal = emission++; begin.tick = tick++;
			begin.commandGeneration = 1; begin.dmaGeneration = dmaGeneration;
			begin.packetOwner = owner(2 + dmaGeneration, begin.tick,
					0x8c010300 + static_cast<std::uint32_t>(dmaGeneration * 2));
			begin.streamOffset = offset;
			begin.dmaStar = 0x0c100000 + static_cast<std::uint32_t>(offset);
			begin.dmaLength = static_cast<std::uint32_t>(sessionBytes);
			begin.dmaDirection = 1; begin.dmaEnabled = 1; writer.write(begin);
			std::uint64_t sessionTransferred = 0;
			while (sessionTransferred < sessionBytes)
			{
				if (offset == produced) fillCache();
				const std::size_t count = static_cast<std::size_t>(
						(std::min<std::uint64_t>)(10240,
							(std::min)(sessionBytes - sessionTransferred,
									produced - offset)));
				research::GdromHardwareObservation transfer;
				transfer.type = research::GdromHardwareObservationType::DmaTransfer;
				transfer.emissionOrdinal = emission++; transfer.tick = tick++;
				transfer.commandGeneration = 1;
				transfer.dmaGeneration = dmaGeneration;
				transfer.streamOffset = offset;
				transfer.destination = begin.dmaStar
						+ static_cast<std::uint32_t>(sessionTransferred);
				if (wrongDestination && offset == 0) transfer.destination += 32;
				transfer.bytes.assign(bytes.begin() + offset,
						bytes.begin() + offset + count);
				if (corrupt && offset == 0) transfer.bytes[7] ^= 1;
				offset += count; sessionTransferred += count;
				transfer.transferredBytes = offset; writer.write(transfer);
			}
			if (!omitDmaInterrupt)
			{
				research::GdromHardwareObservation dmaInterrupt;
				dmaInterrupt.type = research::GdromHardwareObservationType::DmaInterrupt;
				dmaInterrupt.emissionOrdinal = emission++; dmaInterrupt.tick = tick++;
				dmaInterrupt.commandGeneration = 1;
				dmaInterrupt.dmaGeneration = dmaGeneration;
				dmaInterrupt.transferredBytes = offset; writer.write(dmaInterrupt);
			}
		}
		research::GdromHardwareObservation commandInterrupt;
		commandInterrupt.type = research::GdromHardwareObservationType::CommandInterrupt;
		commandInterrupt.emissionOrdinal = emission++; commandInterrupt.tick = tick++;
		commandInterrupt.commandGeneration = 1;
		commandInterrupt.transferredBytes = bytes.size(); writer.write(commandInterrupt);
		research::GdromHardwareObservation complete;
		complete.type = research::GdromHardwareObservationType::Complete;
		complete.emissionOrdinal = emission++; complete.tick = tick++;
		complete.commandGeneration = 1; complete.dmaGeneration = dmaGeneration;
		complete.transferredBytes = bytes.size(); writer.write(complete);
		if (!omitStatusAcknowledgement)
		{
			research::GdromHardwareObservation status;
			status.type = research::GdromHardwareObservationType::StatusAcknowledged;
			status.emissionOrdinal = emission++; status.tick = tick++;
			status.commandGeneration = 1;
			status.packetOwner = owner(90, status.tick, 0x8c010900);
			status.statusRegister = 0x40; status.transferredBytes = bytes.size();
			writer.write(status);
		}
		writer.finalize();
	}

	void capturePio(std::uint32_t fad)
	{
		research::GdromHardwareArtifactBinding binding;
		binding.identityDigest = identity.digest; binding.replayDigest = replay;
		binding.biosDigest = identity.firmware.bios.digest;
		binding.flashDigest = identity.firmware.initialFlash.digest;
		research::GdromHardwareArtifactWriter writer(artifact, binding);
		std::uint64_t emission = 0, tick = 10;
		research::GdromHardwareObservation packet;
		packet.type = research::GdromHardwareObservationType::PacketAccepted;
		packet.emissionOrdinal = emission++; packet.tick = tick++;
		packet.commandGeneration = 1; packet.ataOwner = owner(1, 5, 0x8c010100);
		packet.packetOwner = owner(2, 9, 0x8c010200);
		packet.packet[0] = 0x30; packet.packet[1] = 0x20;
		packet.packet[2] = static_cast<std::uint8_t>(fad >> 16);
		packet.packet[3] = static_cast<std::uint8_t>(fad >> 8);
		packet.packet[4] = static_cast<std::uint8_t>(fad);
		packet.packet[10] = 1; packet.startFad = fad;
		packet.sectorCount = 1; packet.sectorBytes = 2048;
		packet.delivery = research::GdromHardwareDelivery::Pio;
		writer.write(packet);
		research::GdromHardwareObservation ready;
		ready.type = research::GdromHardwareObservationType::PioReady;
		ready.emissionOrdinal = emission++; ready.tick = tick++;
		ready.commandGeneration = 1; ready.startFad = fad;
		ready.sectorCount = 1; ready.sectorBytes = 2048;
		ready.readSuccessful = true; writer.write(ready);
		research::GdromHardwareObservation interrupt;
		interrupt.type = research::GdromHardwareObservationType::CommandInterrupt;
		interrupt.emissionOrdinal = emission++; interrupt.tick = tick++;
		interrupt.commandGeneration = 1; writer.write(interrupt);
		const auto bytes = expected(fad, 1, 2048);
		for (std::size_t offset = 0; offset < bytes.size(); offset += 2)
		{
			research::GdromHardwareObservation word;
			word.type = research::GdromHardwareObservationType::PioWord;
			word.emissionOrdinal = emission++; word.tick = tick++;
			word.commandGeneration = 1; word.streamOffset = offset;
			word.packetOwner = owner(100 + offset / 2, word.tick,
					0x8c011000 + static_cast<std::uint32_t>(offset));
			word.pioWord = bytes[offset] | (std::uint32_t(bytes[offset + 1]) << 8);
			word.transferredBytes = offset + 2; writer.write(word);
		}
		interrupt.emissionOrdinal = emission++; interrupt.tick = tick++;
		interrupt.transferredBytes = bytes.size(); writer.write(interrupt);
		research::GdromHardwareObservation complete;
		complete.type = research::GdromHardwareObservationType::Complete;
		complete.emissionOrdinal = emission++; complete.tick = tick++;
		complete.commandGeneration = 1; complete.transferredBytes = bytes.size();
		writer.write(complete);
		research::GdromHardwareObservation status;
		status.type = research::GdromHardwareObservationType::StatusAcknowledged;
		status.emissionOrdinal = emission++; status.tick = tick++;
		status.commandGeneration = 1;
		status.packetOwner = owner(2000, status.tick, 0x8c012000);
		status.statusRegister = 0x40; status.transferredBytes = bytes.size();
		writer.write(status); writer.finalize();
	}
};
} // namespace

TEST(ResearchGdromHardwareArtifact, ValidatesMode1AndMode2UserData)
{
	Fixture fixture; fixture.capture(45150, 2, 2048);
	const auto summary = research::validateGdromHardwareArtifactFile(
			fixture.artifact, fixture.identity, fixture.replay);
	EXPECT_EQ(1u, summary.completedCommands);
	EXPECT_EQ(1u, summary.typeCounts[3]);
}

TEST(ResearchGdromHardwareArtifact, ValidatesRawAndGapZeroFill)
{
	Fixture raw; raw.capture(45150, 2, 2352);
	EXPECT_NO_THROW(research::validateGdromHardwareArtifactFile(
			raw.artifact, raw.identity, raw.replay));
	Fixture gap; gap.capture(45152, 2, 2352);
	EXPECT_NO_THROW(research::validateGdromHardwareArtifactFile(
			gap.artifact, gap.identity, gap.replay));
}

TEST(ResearchGdromHardwareArtifact, Validates2340AndRejectsMutatedTransfer)
{
	Fixture exact; exact.capture(45150, 8, 2340);
	EXPECT_NO_THROW(research::validateGdromHardwareArtifactFile(
			exact.artifact, exact.identity, exact.replay));
	Fixture corrupt; corrupt.capture(45150, 2, 2048, true);
	EXPECT_THROW(research::validateGdromHardwareArtifactFile(
			corrupt.artifact, corrupt.identity, corrupt.replay), std::runtime_error);
}

TEST(ResearchGdromHardwareArtifact, ValidatesCacheBoundaryAndMultipleDmaSessions)
{
	Fixture fixture; fixture.capture(45150, 17, 2048);
	const auto summary = research::validateGdromHardwareArtifactFile(
			fixture.artifact, fixture.identity, fixture.replay);
	EXPECT_EQ(2u, summary.typeCounts[1]);
	EXPECT_EQ(2u, summary.typeCounts[2]);
	EXPECT_EQ(5u, summary.typeCounts[3]);
	EXPECT_EQ(2u, summary.typeCounts[6]);
}

TEST(ResearchGdromHardwareArtifact, ValidatesExactPioWordOwnersAndOrder)
{
	Fixture fixture; fixture.capturePio(45150);
	const auto summary = research::validateGdromHardwareArtifactFile(
			fixture.artifact, fixture.identity, fixture.replay);
	EXPECT_EQ(1024u, summary.typeCounts[5]);
	EXPECT_EQ(2u, summary.typeCounts[7]);
}

TEST(ResearchGdromHardwareArtifact, RefusesIncompleteOrAbortedCandidate)
{
	Fixture fixture;
	research::GdromHardwareArtifactBinding binding;
	binding.identityDigest = fixture.identity.digest; binding.replayDigest = fixture.replay;
	binding.biosDigest = fixture.identity.firmware.bios.digest;
	binding.flashDigest = fixture.identity.firmware.initialFlash.digest;
	research::GdromHardwareArtifactWriter writer(fixture.artifact, binding);
	research::GdromHardwareObservation packet;
	packet.type = research::GdromHardwareObservationType::PacketAccepted;
	packet.commandGeneration = 1; packet.tick = 10;
	packet.sectorCount = 1; packet.sectorBytes = 2048;
	writer.write(packet);
	research::GdromHardwareObservation abort;
	abort.type = research::GdromHardwareObservationType::Abort;
	abort.commandGeneration = 1; abort.tick = 11; writer.write(abort);
	EXPECT_THROW(writer.finalize(), std::logic_error);
}

TEST(ResearchGdromHardwareArtifact, RejectsWrongDmaDestinationAndMissingInterrupt)
{
	Fixture destination; destination.capture(45150, 2, 2048, false, true);
	EXPECT_THROW(research::validateGdromHardwareArtifactFile(destination.artifact,
			destination.identity, destination.replay), std::runtime_error);
	Fixture interrupt; interrupt.capture(45150, 2, 2048, false, false, true);
	EXPECT_THROW(research::validateGdromHardwareArtifactFile(interrupt.artifact,
			interrupt.identity, interrupt.replay), std::runtime_error);
}

TEST(ResearchGdromHardwareArtifact, RefusesCompletionWithoutGuestStatusAcknowledgement)
{
	Fixture fixture;
	EXPECT_THROW(fixture.capture(45150, 2, 2048, false, false, false, true),
			std::logic_error);
}

TEST(ResearchGdromHardwareArtifact,
		IndependentlyAuthenticatesRealMapleReplayProvenance)
{
	Fixture fixture;
	research::IdentityManifest record = fixture.identity;
	record.schemaVersion = 1;
	record.digest = research::sha256("maple-record-identity", 21);
	record.hasMapleReplayIdentityDigest = false;
	record.runtimeConfiguration.cpuBackend = "interpreter";
	record.runtimeConfiguration.dynarecObservation = false;
	record.runtimeConfiguration.dynarecProfile = false;
	research::IdentityManifest capture = fixture.identity;
	capture.schemaVersion = 2;
	capture.hasMapleReplayIdentityDigest = true;
	capture.mapleReplayIdentityDigest = record.digest;
	capture.runtimeConfiguration.cpuBackend = "interpreter";
	capture.runtimeConfiguration.dynarecObservation = false;
	capture.runtimeConfiguration.mapleDmaCheckpoint = 1;
	const auto replayPath = writeReplay(fixture.directory, record.digest);
	const auto summary = research::validateGdromHardwareReplayFile(
			replayPath, capture, record);
	EXPECT_EQ(1u, summary.dmaCount);

	auto hle = record; hle.firmware.mode = research::FirmwareMode::Hle;
	EXPECT_THROW(research::validateGdromHardwareReplayFile(
			replayPath, capture, hle), std::runtime_error);
	auto wrongBios = record; wrongBios.firmware.bios.digest[0] ^= 1;
	EXPECT_THROW(research::validateGdromHardwareReplayFile(
			replayPath, capture, wrongBios), std::runtime_error);
	auto wrongLink = capture; wrongLink.mapleReplayIdentityDigest[0] ^= 1;
	EXPECT_THROW(research::validateGdromHardwareReplayFile(
			replayPath, wrongLink, record), std::runtime_error);
	auto wrongCheckpoint = capture;
	wrongCheckpoint.runtimeConfiguration.mapleDmaCheckpoint = 2;
	EXPECT_THROW(research::validateGdromHardwareReplayFile(
			replayPath, wrongCheckpoint, record), std::runtime_error);

	research::GdromHardwareArtifactSummary artifactSummary;
	artifactSummary.endTick = summary.endTick;
	EXPECT_NO_THROW(research::requireGdromHardwareReplayTimeline(
			artifactSummary, summary));
	artifactSummary.endTick = summary.endTick + 1;
	EXPECT_THROW(research::requireGdromHardwareReplayTimeline(
			artifactSummary, summary), std::runtime_error);

	auto replayBytes = research::readFileExact(replayPath,
			research::DefaultMaximumMapleTraceBytes);
	replayBytes.back() ^= 1; writeBytes(replayPath, replayBytes);
	EXPECT_THROW(research::validateGdromHardwareReplayFile(
			replayPath, capture, record), std::runtime_error);
}

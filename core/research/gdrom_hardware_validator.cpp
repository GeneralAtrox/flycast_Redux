#include "research/gdrom_hardware_artifact.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace research
{
namespace
{
constexpr std::array<std::uint8_t, 8> Magic {'F','C','G','D','H','W','0','1'};
constexpr std::uint32_t EventHeaderSize = 32;
constexpr std::uint32_t FixedEventPayloadSize = 164;

[[noreturn]] void invalid(const std::string& message)
{ throw std::runtime_error("invalid GD-ROM hardware artifact: " + message); }
void require(bool value, const std::string& message)
{ if (!value) invalid(message); }

class Reader
{
public:
	Reader(const std::uint8_t* data, std::size_t size) : data(data), size(size) {}
	std::uint8_t byte() { need(1); return data[position++]; }
	std::uint32_t u32()
	{
		need(4); std::uint32_t value = 0;
		for (unsigned i = 0; i < 4; ++i)
			value |= std::uint32_t(data[position++]) << (8 * i);
		return value;
	}
	std::uint64_t u64()
	{
		need(8); std::uint64_t value = 0;
		for (unsigned i = 0; i < 8; ++i)
			value |= std::uint64_t(data[position++]) << (8 * i);
		return value;
	}
	std::vector<std::uint8_t> bytes(std::size_t count)
	{
		need(count); std::vector<std::uint8_t> value(data + position,
				data + position + count); position += count; return value;
	}
	void skip(std::size_t count) { need(count); position += count; }
	std::size_t tell() const { return position; }
	std::size_t remaining() const { return size - position; }
private:
	void need(std::size_t count)
	{ if (count > size - position) invalid("binary input is truncated"); }
	const std::uint8_t* data;
	std::size_t size;
	std::size_t position = 0;
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
	std::uint32_t crc = 0xffffffffu;
	for (std::size_t i = 0; i < size; ++i)
	{
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

Sh4InstructionOwnerToken readOwner(Reader& reader)
{
	Sh4InstructionOwnerToken owner;
	owner.valid = reader.byte() != 0;
	owner.backend = static_cast<Sh4ObservationBackend>(reader.byte());
	owner.delaySlotDepth = reader.byte();
	owner.delaySlotDepth |= std::uint16_t(reader.byte()) << 8;
	owner.opcode = reader.byte();
	owner.opcode |= std::uint16_t(reader.byte()) << 8;
	reader.skip(2);
	owner.generation = reader.u64(); owner.tick = reader.u64();
	owner.pc = reader.u32(); owner.pr = reader.u32();
	return owner;
}

void requireOwner(const Sh4InstructionOwnerToken& owner,
		Sh4ObservationBackend backend, std::uint64_t eventTick,
		const char *description)
{
	require(owner.valid && owner.backend == backend && owner.generation != 0
			&& owner.tick <= eventTick && (owner.pc & 1) == 0,
			std::string(description) + " owner is invalid");
}

struct Track
{
	std::uint32_t number = 0;
	std::uint32_t startFad = 0;
	std::uint32_t type = 0;
	std::uint32_t sectorSize = 0;
	std::uint64_t offset = 0;
	std::uint64_t sectors = 0;
	std::filesystem::path path;
};

std::vector<Track> authenticateGdi(const IdentityManifest& identity)
{
	require(identity.mediaKind == "gdi" && identity.mediaTracks.size() >= 3,
			"identity is not a complete GDI");
	require(!identity.mediaSourcePath.empty(), "identity has no GDI descriptor path");
	require(std::filesystem::file_size(identity.mediaSourcePath)
			== identity.mediaSourceSize, "GDI descriptor size differs from identity");
	require(sha256Equal(hashFileExact(identity.mediaSourcePath,
			identity.mediaSourceSize), identity.mediaSourceDigest),
			"GDI descriptor digest differs from identity");
	std::ifstream input(identity.mediaSourcePath);
	require(static_cast<bool>(input), "cannot open GDI descriptor");
	std::size_t count = 0; input >> count;
	require(count == identity.mediaTracks.size() && count >= 3 && count <= 99,
			"GDI track count differs from identity");
	std::string line; std::getline(input, line);
	std::vector<Track> tracks;
	for (std::size_t index = 0; index < count; ++index)
	{
		std::getline(input, line);
		require(!line.empty(), "GDI track line is missing");
		std::istringstream fields(line);
		std::uint32_t lba = 0; std::string file;
		Track track;
		fields >> track.number >> lba >> track.type >> track.sectorSize
				>> std::quoted(file) >> track.offset;
		require(!fields.fail(), "GDI track line is malformed");
		fields >> std::ws;
		require(fields.eof(), "GDI track line has trailing fields");
		track.startFad = lba + 150;
		const auto& expected = identity.mediaTracks[index];
		require(!expected.path.empty(), "identity has a track without a path");
		require(track.number == expected.track
				&& track.startFad == expected.startFad
				&& track.sectorSize == expected.sectorSize
				&& track.offset == expected.offset
				&& std::filesystem::path(file).filename()
						== expected.path.filename(),
				"GDI track mapping differs from identity");
		require(track.type == 0 || track.type == 4,
				"GDI track type is unsupported");
		require(track.sectorSize == 2048 || track.sectorSize == 2352,
				"GDI source sector size is unsupported");
		require(std::filesystem::file_size(expected.path) == expected.size,
				"track size differs from identity");
		require(sha256Equal(hashFileExact(expected.path, expected.size),
				expected.digest), "track digest differs from identity");
		require(expected.size >= expected.offset
				&& (expected.size - expected.offset) % track.sectorSize == 0,
				"track length is not an exact sector sequence");
		track.sectors = (expected.size - expected.offset) / track.sectorSize;
		track.path = expected.path;
		tracks.push_back(std::move(track));
	}
	require(tracks[0].number == 1 && tracks[0].type == 4
			&& tracks[1].number == 2 && tracks[1].type == 0
			&& tracks[2].number == 3 && tracks[2].type == 4
			&& tracks[2].startFad == 45150,
			"GDI session layout is invalid");
	return tracks;
}

std::vector<std::uint8_t> readSector(const std::vector<Track>& tracks,
		std::uint32_t fad, std::uint32_t outputSize)
{
	const Track* selected = nullptr;
	for (auto iterator = tracks.rbegin(); iterator != tracks.rend(); ++iterator)
	{
		if (iterator->startFad <= fad
				&& std::uint64_t(fad - iterator->startFad) < iterator->sectors)
		{
			selected = &*iterator; break;
		}
	}
	std::vector<std::uint8_t> source(2352, 0);
	std::uint32_t sourceSize = 2352;
	if (selected != nullptr)
	{
		sourceSize = selected->sectorSize;
		source.resize(sourceSize);
		std::ifstream input(selected->path, std::ios::binary);
		require(static_cast<bool>(input), "cannot open authenticated GDI track");
		const std::uint64_t sector = fad - selected->startFad;
		const std::uint64_t offset = selected->offset
				+ sector * selected->sectorSize;
		input.seekg(static_cast<std::streamoff>(offset));
		input.read(reinterpret_cast<char *>(source.data()), source.size());
		require(input.gcount() == static_cast<std::streamsize>(source.size()),
				"authenticated GDI sector is truncated");
	}
	if (sourceSize == outputSize) return source;
	if (sourceSize == 2048)
	{
		require(outputSize == 2048,
				"Flycast's partial 2048-to-raw conversion is not evidentiary");
		return source;
	}
	require(sourceSize == 2352, "source sector size is unsupported");
	std::size_t offset = 0;
	if (outputSize == 2048) offset = source[15] == 1 ? 16 : 24;
	else if (outputSize == 2340) offset = 12;
	else require(outputSize == 2352, "output sector size is unsupported");
	return std::vector<std::uint8_t>(source.begin() + offset,
			source.begin() + offset + outputSize);
}

std::vector<std::uint8_t> streamSlice(const std::vector<Track>& tracks,
		std::uint32_t startFad, std::uint32_t sectorSize,
		std::uint64_t offset, std::size_t count)
{
	std::vector<std::uint8_t> result;
	result.reserve(count);
	while (count != 0)
	{
		const std::uint64_t sectorIndex = offset / sectorSize;
		const std::size_t inSector = static_cast<std::size_t>(offset % sectorSize);
		const auto sector = readSector(tracks,
				startFad + static_cast<std::uint32_t>(sectorIndex), sectorSize);
		const std::size_t take = (std::min)(count, sector.size() - inSector);
		result.insert(result.end(), sector.begin() + inSector,
				sector.begin() + inSector + take);
		offset += take; count -= take;
	}
	return result;
}

struct ParsedEvent
{
	GdromHardwareObservation event;
	std::uint32_t byteCount = 0;
};

ParsedEvent parseEvent(Reader& reader, std::uint32_t typeValue,
		std::uint64_t emission, std::uint64_t tick)
{
	ParsedEvent parsed;
	auto& event = parsed.event;
	event.type = static_cast<GdromHardwareObservationType>(typeValue);
	event.emissionOrdinal = emission; event.tick = tick;
	event.commandGeneration = reader.u64(); event.dmaGeneration = reader.u64();
	event.ataOwner = readOwner(reader); event.packetOwner = readOwner(reader);
	for (auto& byte : event.packet) byte = reader.byte();
	require(reader.u32() == 0, "event reserved field is nonzero");
	event.features = reader.u32(); event.byteCountRegister = reader.u32();
	event.driveState = reader.u32(); event.startFad = reader.u32();
	event.sectorCount = reader.u32(); event.sectorBytes = reader.u32();
	event.delivery = static_cast<GdromHardwareDelivery>(reader.u32());
	event.readSuccessful = reader.u32() != 0;
	event.destination = reader.u32(); event.dmaStar = reader.u32();
	event.dmaLength = reader.u32(); event.dmaDirection = reader.u32();
	event.dmaEnabled = reader.u32(); event.pioWord = reader.u32();
	event.statusRegister = reader.u32();
	event.abortReason = static_cast<GdromHardwareAbortReason>(reader.u32());
	parsed.byteCount = reader.u32();
	event.streamOffset = reader.u64(); event.transferredBytes = reader.u64();
	event.bytes = reader.bytes(parsed.byteCount);
	return parsed;
}

struct Command
{
	bool active = false;
	std::uint64_t generation = 0;
	std::uint64_t expected = 0;
	std::uint64_t produced = 0;
	std::uint64_t transferred = 0;
	std::uint64_t dmaGeneration = 0;
	std::uint64_t dmaTransferred = 0;
	std::uint64_t dmaLimit = 0;
	std::uint32_t dmaDestination = 0;
	std::uint32_t startFad = 0;
	std::uint32_t sectorCount = 0;
	std::uint32_t sectorSize = 0;
	GdromHardwareDelivery delivery = GdromHardwareDelivery::Pio;
	bool dmaActive = false;
	std::uint64_t commandInterrupts = 0;
};

bool sameBlob(const BlobIdentity& left, const BlobIdentity& right)
{
	return left.available == right.available
			&& left.size == right.size
			&& sha256Equal(left.digest, right.digest);
}

bool sameInitialState(const InitialStateIdentity& left,
		const InitialStateIdentity& right)
{
	return left.available == right.available
			&& (!left.available || (left.slot == right.slot
					&& left.size == right.size
					&& sha256Equal(left.digest, right.digest)));
}

bool sameTrack(const MediaTrackIdentity& left,
		const MediaTrackIdentity& right)
{
	return left.track == right.track && left.startFad == right.startFad
			&& left.sectorSize == right.sectorSize && left.offset == right.offset
			&& left.size == right.size && sha256Equal(left.digest, right.digest);
}

bool samePersistentDevice(const PersistentDeviceIdentity& left,
		const PersistentDeviceIdentity& right)
{
	return left.kind == right.kind && left.bus == right.bus
			&& left.port == right.port && sameBlob(left.blob, right.blob);
}

void decodePacket(const std::array<std::uint8_t, 12>& packet,
		std::uint32_t& startFad, std::uint32_t& count,
		std::uint32_t& sectorSize)
{
	require(packet[0] == 0x30 || packet[0] == 0x31,
			"SPI packet is not CD_READ or CD_READ2");
	const bool prmtype = (packet[1] & 1) != 0;
	const std::uint32_t expdtype = (packet[1] >> 1) & 7;
	const bool other = (packet[1] & 0x10) != 0;
	const bool data = (packet[1] & 0x20) != 0;
	const bool subh = (packet[1] & 0x40) != 0;
	const bool head = (packet[1] & 0x80) != 0;
	sectorSize = 2048;
	if (head && subh && data && expdtype == 3 && !other) sectorSize = 2340;
	else if (other || expdtype == 1) sectorSize = 2352;
	if (prmtype)
		startFad = packet[2] * 60u * 75u + packet[3] * 75u + packet[4];
	else
		startFad = (std::uint32_t(packet[2]) << 16)
				| (std::uint32_t(packet[3]) << 8) | packet[4];
	count = packet[0] == 0x30
			? (std::uint32_t(packet[8]) << 16)
				| (std::uint32_t(packet[9]) << 8) | packet[10]
			: (std::uint32_t(packet[6]) << 8) | packet[7];
	require(count != 0, "SPI packet has a zero sector count");
}
} // namespace

GdromHardwareArtifactSummary validateGdromHardwareArtifactFile(
		const std::filesystem::path& artifact,
		const IdentityManifest& identity, const Sha256Digest& replayDigest,
		std::uint64_t maximumBytes, std::uint64_t maximumEvents)
{
	require(identity.firmware.mode == FirmwareMode::Real,
			"hardware artifact identity does not select real firmware");
	require(identity.firmware.bios.size == DreamcastBiosBytes
			&& identity.firmware.initialFlash.size == DreamcastFlashBytes,
			"hardware artifact firmware sizes are not Dreamcast BIOS/flash sizes");
	authenticateFirmwareFiles(identity);
	const auto tracks = authenticateGdi(identity);
	const auto file = readFileExact(artifact, maximumBytes);
	require(file.size() >= GdromHardwareArtifactHeaderSize,
			"file is smaller than its header");
	require(std::equal(Magic.begin(), Magic.end(), file.begin()), "magic is invalid");
	Reader header(file.data() + 8, GdromHardwareArtifactHeaderSize - 8);
	require(header.u32() == GdromHardwareArtifactSchemaVersion
			&& header.u32() == GdromHardwareArtifactHeaderSize,
			"schema/header size is invalid");
	require(header.u32() == 0x01020304 && header.u32() == 1,
			"artifact is incomplete");
	GdromHardwareArtifactSummary summary;
	const auto backendValue = header.u32(); require(header.u32() == 0,
			"header reserved field is nonzero");
	require(backendValue == 1 || backendValue == 2, "backend is invalid");
	summary.binding.backend = static_cast<Sh4ObservationBackend>(backendValue);
	summary.eventCount = header.u64(); summary.payloadBytes = header.u64();
	summary.droppedEvents = header.u64(); summary.startTick = header.u64();
	summary.endTick = header.u64(); summary.completedCommands = header.u64();
	for (auto& byte : summary.binding.identityDigest) byte = header.byte();
	for (auto& byte : summary.binding.replayDigest) byte = header.byte();
	for (auto& byte : summary.binding.biosDigest) byte = header.byte();
	for (auto& byte : summary.binding.flashDigest) byte = header.byte();
	for (auto& byte : summary.payloadDigest) byte = header.byte();
	for (auto& count : summary.typeCounts) count = header.u64();
	require(summary.eventCount != 0 && summary.eventCount <= maximumEvents
			&& summary.completedCommands != 0 && summary.droppedEvents == 0,
			"event/completion/dropped count is invalid");
	require(summary.payloadBytes == file.size() - GdromHardwareArtifactHeaderSize,
			"payload size is inconsistent");
	require(sha256Equal(summary.binding.identityDigest, identity.digest)
			&& sha256Equal(summary.binding.replayDigest, replayDigest)
			&& sha256Equal(summary.binding.biosDigest, identity.firmware.bios.digest)
			&& sha256Equal(summary.binding.flashDigest,
					identity.firmware.initialFlash.digest),
			"artifact binding is incorrect");
	require(sha256Equal(summary.payloadDigest, sha256(
			file.data() + GdromHardwareArtifactHeaderSize, summary.payloadBytes)),
			"payload digest is incorrect");
	std::uint32_t storedCrc = 0;
	for (unsigned i = 0; i < 4; ++i)
		storedCrc |= std::uint32_t(file[GdromHardwareArtifactHeaderSize - 4 + i])
				<< (8 * i);
	require(storedCrc == crc32(file.data(), GdromHardwareArtifactHeaderSize - 4),
			"header CRC is incorrect");

	Reader reader(file.data() + GdromHardwareArtifactHeaderSize,
			summary.payloadBytes);
	std::array<std::uint64_t, 13> counts {};
	std::uint64_t previousEmission = 0, previousTick = 0, completed = 0;
	std::uint64_t completedAwaitingStatus = 0;
	std::uint64_t completedTransferred = 0;
	Command command;
	for (std::uint64_t ordinal = 0; ordinal < summary.eventCount; ++ordinal)
	{
		const std::size_t start = reader.tell();
		const auto typeValue = reader.u32(); const auto eventSize = reader.u32();
		require(typeValue >= 1 && typeValue <= 13
				&& eventSize >= EventHeaderSize + FixedEventPayloadSize
				&& eventSize <= reader.remaining() + 8,
				"event header is invalid");
		require(reader.u64() == ordinal, "artifact event ordinal is not contiguous");
		const auto emission = reader.u64(); const auto tick = reader.u64();
		if (ordinal != 0)
		{
			require(emission == previousEmission + 1,
					"source emission ordinal is not contiguous");
			require(tick >= previousTick, "event tick moved backwards");
		}
		previousEmission = emission; previousTick = tick;
		const auto parsed = parseEvent(reader, typeValue, emission, tick);
		const auto& event = parsed.event;
		++counts[typeValue - 1];
		const auto type = event.type;
		if (type == GdromHardwareObservationType::PacketAccepted)
		{
			require(!command.active && completedAwaitingStatus == 0
					&& event.commandGeneration != 0,
					"packet overlaps an active command");
			requireOwner(event.ataOwner, summary.binding.backend, tick, "ATA PACKET");
			requireOwner(event.packetOwner, summary.binding.backend, tick, "SPI packet");
			require(event.ataOwner.generation != event.packetOwner.generation,
					"ATA and SPI boundaries do not have distinct instruction owners");
			std::uint32_t fad = 0, sectors = 0, sectorSize = 0;
			decodePacket(event.packet, fad, sectors, sectorSize);
			require(event.startFad == fad && event.sectorCount == sectors
					&& event.sectorBytes == sectorSize,
					"decoded SPI fields differ from packet bytes");
			const auto delivery = (event.features & 1) != 0
					? GdromHardwareDelivery::Dma : GdromHardwareDelivery::Pio;
			require(event.delivery == delivery, "delivery mode differs from ATA features");
			command = {}; command.active = true;
			command.generation = event.commandGeneration;
			command.startFad = fad; command.sectorCount = sectors;
			command.sectorSize = sectorSize; command.delivery = delivery;
			command.expected = std::uint64_t(sectors) * sectorSize;
			require(command.expected <= maximumBytes,
					"command byte count exceeds artifact bound");
		}
		else if (type == GdromHardwareObservationType::Reset
				|| type == GdromHardwareObservationType::LoadState)
		{
			require(!command.active && completedAwaitingStatus == 0
					&& event.commandGeneration == 0,
					"state boundary invalidates an observed command");
		}
		else if (type == GdromHardwareObservationType::StatusAcknowledged)
		{
			requireOwner(event.packetOwner, summary.binding.backend, tick,
					"GD_STATUS read");
			require(event.statusRegister <= 0xff,
					"GD_STATUS acknowledgement value is invalid");
			if (command.active)
				require(event.commandGeneration == command.generation
						&& event.transferredBytes == command.transferred,
						"intermediate GD_STATUS acknowledgement is unrelated");
			else
			{
				require(completedAwaitingStatus != 0
						&& event.commandGeneration == completedAwaitingStatus
						&& event.transferredBytes == completedTransferred,
						"terminal GD_STATUS acknowledgement is unrelated");
				completedAwaitingStatus = 0; completedTransferred = 0;
			}
		}
		else
		{
			require(command.active
					&& event.commandGeneration == command.generation,
					"event has no matching command");
			if (type == GdromHardwareObservationType::BufferFill
					|| type == GdromHardwareObservationType::PioReady)
			{
				const bool pio = type == GdromHardwareObservationType::PioReady;
				require((pio ? GdromHardwareDelivery::Pio
						: GdromHardwareDelivery::Dma) == command.delivery,
						"buffer event has the wrong delivery path");
				const std::uint64_t remainingBytes = command.expected - command.produced;
				const std::uint32_t remainingSectors = static_cast<std::uint32_t>(
						remainingBytes / command.sectorSize);
				const std::uint32_t maximumSectors = pio
						? (65536u - 1) / command.sectorSize : 16u;
				const std::uint32_t expectedCount = (std::min)(remainingSectors,
						maximumSectors);
				require(event.readSuccessful && event.streamOffset == command.produced
						&& event.startFad == command.startFad
								+ command.produced / command.sectorSize
						&& event.sectorCount == expectedCount
						&& event.sectorBytes == command.sectorSize,
						"buffer-fill geometry/result is inconsistent");
				command.produced += std::uint64_t(expectedCount)
						* command.sectorSize;
			}
			else if (type == GdromHardwareObservationType::DmaBegin)
			{
				require(command.delivery == GdromHardwareDelivery::Dma
						&& !command.dmaActive, "DMA session overlap or wrong path");
				requireOwner(event.packetOwner, summary.binding.backend, tick,
						"GD-DMA start");
				require(event.dmaDirection == 1 && event.dmaEnabled == 1
						&& (event.dmaLength & 0x1f) == 0
						&& event.dmaStar >= 0x0c000000u
						&& event.dmaStar < 0x0d000000u
						&& event.streamOffset == command.transferred,
						"GD-DMA start registers are invalid");
				command.dmaActive = true;
				command.dmaGeneration = event.dmaGeneration;
				command.dmaDestination = event.dmaStar;
				command.dmaTransferred = 0;
				command.dmaLimit = event.dmaLength == 0 ? 0x02000000u
						: event.dmaLength;
			}
			else if (type == GdromHardwareObservationType::DmaTransfer)
			{
				require(command.delivery == GdromHardwareDelivery::Dma
						&& command.dmaActive
						&& event.dmaGeneration == command.dmaGeneration
						&& event.streamOffset == command.transferred
						&& event.destination == command.dmaDestination
								+ command.dmaTransferred
						&& !event.bytes.empty() && event.bytes.size() <= 10240
						&& event.bytes.size() <= command.expected - command.transferred
						&& event.bytes.size() <= command.dmaLimit - command.dmaTransferred,
						"GD-DMA transfer geometry is invalid");
				const auto expected = streamSlice(tracks, command.startFad,
						command.sectorSize, command.transferred,
						event.bytes.size());
				require(event.bytes == expected,
						"GD-DMA bytes differ from independent GDI reconstruction");
				command.transferred += event.bytes.size();
				command.dmaTransferred += event.bytes.size();
				require(event.transferredBytes == command.transferred,
						"GD-DMA post-transfer byte count is invalid");
			}
			else if (type == GdromHardwareObservationType::PioWord)
			{
				require(command.delivery == GdromHardwareDelivery::Pio
						&& event.streamOffset == command.transferred
						&& command.transferred + 2 <= command.produced,
						"PIO word geometry is invalid");
				requireOwner(event.packetOwner, summary.binding.backend, tick,
						"GD_DATA read");
				const auto expected = streamSlice(tracks, command.startFad,
						command.sectorSize, command.transferred, 2);
				require(event.pioWord == (std::uint32_t(expected[0])
						| (std::uint32_t(expected[1]) << 8)),
						"PIO word differs from independent GDI reconstruction");
				command.transferred += 2;
				require(event.transferredBytes == command.transferred,
						"PIO post-read byte count is invalid");
			}
			else if (type == GdromHardwareObservationType::DmaInterrupt)
			{
				require(command.dmaActive
						&& event.dmaGeneration == command.dmaGeneration
						&& command.dmaTransferred == command.dmaLimit
						&& event.transferredBytes == command.transferred,
						"GD-ROM DMA interrupt is not tied to a completed DMA session");
				command.dmaActive = false;
			}
			else if (type == GdromHardwareObservationType::CommandInterrupt)
			{
				require(event.transferredBytes == command.transferred,
						"GD-ROM command interrupt byte count is invalid");
				++command.commandInterrupts;
			}
			else if (type == GdromHardwareObservationType::Complete)
			{
				require(command.produced == command.expected
						&& command.transferred == command.expected
						&& event.transferredBytes == command.expected
						&& !(command.dmaActive
								&& command.dmaTransferred == command.dmaLimit)
						&& command.commandInterrupts != 0,
						"command completion is incomplete");
				command = {}; ++completed;
				completedAwaitingStatus = event.commandGeneration;
				completedTransferred = event.transferredBytes;
			}
			else if (type == GdromHardwareObservationType::Abort)
				invalid("accepted artifact contains an aborted command");
		}
		require(reader.tell() == start + eventSize,
				"event size does not match its payload");
	}
	require(reader.remaining() == 0 && !command.active
			&& completedAwaitingStatus == 0,
			"payload has trailing bytes or an incomplete command");
	require(counts == summary.typeCounts, "header event counts differ from payload");
	require(completed == summary.completedCommands,
			"header completed-command count differs from payload");
	require(summary.startTick <= summary.endTick, "header tick range is invalid");
	return summary;
}

MapleTraceSummary validateGdromHardwareReplayFile(
		const std::filesystem::path& replay,
		const IdentityManifest& captureIdentity,
		const IdentityManifest& recordIdentity,
		std::uint64_t maximumBytes)
{
	requireSh4EquivalenceIdentityV2(captureIdentity);
	if (recordIdentity.schemaVersion == 1)
		requireCaptureV1Identity(recordIdentity);
	else if (recordIdentity.schemaVersion == 3)
		requireMapleRecordIdentityV3(recordIdentity);
	else
		invalid("Maple record identity must use schema version 1 or 3");
	require(captureIdentity.firmware.mode == FirmwareMode::Real
			&& recordIdentity.firmware.mode == FirmwareMode::Real,
			"capture and Maple record identities must both select real firmware");
	require(captureIdentity.hasMapleReplayIdentityDigest
			&& sha256Equal(captureIdentity.mapleReplayIdentityDigest,
					recordIdentity.digest),
			"Maple record identity digest is not the capture replay authority");
	require(captureIdentity.firmware.bios.size == DreamcastBiosBytes
			&& recordIdentity.firmware.bios.size == DreamcastBiosBytes
			&& sameBlob(captureIdentity.firmware.bios,
					recordIdentity.firmware.bios),
			"Maple record BIOS authority differs from the capture identity");
	require(captureIdentity.firmware.initialFlash.size == DreamcastFlashBytes
			&& recordIdentity.firmware.initialFlash.size == DreamcastFlashBytes
			&& sameBlob(captureIdentity.firmware.initialFlash,
					recordIdentity.firmware.initialFlash),
			"Maple record initial-flash authority differs from the capture identity");
	require(captureIdentity.mediaKind == recordIdentity.mediaKind
			&& captureIdentity.mediaSourceSize == recordIdentity.mediaSourceSize
			&& sha256Equal(captureIdentity.mediaSourceDigest,
					recordIdentity.mediaSourceDigest)
			&& sha256Equal(captureIdentity.bootExecutableDigest,
					recordIdentity.bootExecutableDigest)
			&& captureIdentity.mediaTracks.size()
					== recordIdentity.mediaTracks.size(),
			"Maple record media authority differs from the capture identity");
	for (std::size_t index = 0; index < captureIdentity.mediaTracks.size(); ++index)
		require(sameTrack(captureIdentity.mediaTracks[index],
				recordIdentity.mediaTracks[index]),
				"Maple record track authority differs from the capture identity");
	require(captureIdentity.emulatorExecutable.size
				== recordIdentity.emulatorExecutable.size
			&& sha256Equal(captureIdentity.emulatorExecutable.digest,
					recordIdentity.emulatorExecutable.digest),
			"Maple record emulator authority differs from the capture identity");
	require(sameInitialState(captureIdentity.initialState,
			recordIdentity.initialState),
			"Maple record initial-state authority differs from the capture identity");
	require(captureIdentity.persistentDevices.size()
				== recordIdentity.persistentDevices.size(),
			"Maple record persistent-device inventory differs from the capture identity");
	for (std::size_t index = 0;
			index < captureIdentity.persistentDevices.size(); ++index)
		require(samePersistentDevice(captureIdentity.persistentDevices[index],
				recordIdentity.persistentDevices[index]),
				"Maple record persistent-device authority differs from the capture identity");
	require(captureIdentity.runtimeConfiguration.dreamcastRtcSeed
				== recordIdentity.runtimeConfiguration.dreamcastRtcSeed,
			"Maple record RTC seed differs from the capture identity");
	require(captureIdentity.runtimeConfiguration.mapleDmaCheckpoint != 0,
			"capture identity has no terminal Maple DMA checkpoint");
	const MapleTraceSummary summary = validateProductionMapleTraceFile(replay,
			recordIdentity.digest, maximumBytes);
	require(summary.dmaCount
				== captureIdentity.runtimeConfiguration.mapleDmaCheckpoint,
			"Maple replay DMA count differs from the capture checkpoint");
	return summary;
}

void requireGdromHardwareReplayTimeline(
		const GdromHardwareArtifactSummary& artifact,
		const MapleTraceSummary& replay)
{
	require(artifact.endTick <= replay.endTick,
			"GD-ROM artifact extends beyond the authenticated Maple replay boundary");
}

} // namespace research

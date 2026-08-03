#include "research/gdrom_hardware_artifact.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#endif

namespace research
{
namespace
{
constexpr std::array<std::uint8_t, 8> Magic {'F','C','G','D','H','W','0','1'};
constexpr std::uint32_t Complete = 1;
constexpr std::uint32_t EventHeaderSize = 32;

void require(bool value, const char *message)
{
	if (!value)
		throw std::runtime_error(std::string("invalid GD-ROM hardware artifact: ")
				+ message);
}
void u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
void u64(std::vector<std::uint8_t>& out, std::uint64_t value)
{ for (unsigned i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i))); }
void put32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value)
{ for (unsigned i = 0; i < 4; ++i) out[offset + i] = static_cast<std::uint8_t>(value >> (8 * i)); }
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
void owner(std::vector<std::uint8_t>& out,
		const Sh4InstructionOwnerToken& token)
{
	out.push_back(token.valid ? 1 : 0);
	out.push_back(static_cast<std::uint8_t>(token.backend));
	out.push_back(static_cast<std::uint8_t>(token.delaySlotDepth));
	out.push_back(static_cast<std::uint8_t>(token.delaySlotDepth >> 8));
	out.push_back(static_cast<std::uint8_t>(token.opcode));
	out.push_back(static_cast<std::uint8_t>(token.opcode >> 8));
	out.push_back(0); out.push_back(0);
	u64(out, token.generation); u64(out, token.tick);
	u32(out, token.pc); u32(out, token.pr);
}
std::vector<std::uint8_t> header(const GdromHardwareArtifactSummary& summary,
		bool complete)
{
	std::vector<std::uint8_t> out;
	out.insert(out.end(), Magic.begin(), Magic.end());
	u32(out, GdromHardwareArtifactSchemaVersion);
	u32(out, GdromHardwareArtifactHeaderSize);
	u32(out, 0x01020304); u32(out, complete ? Complete : 0);
	u32(out, static_cast<std::uint32_t>(summary.binding.backend)); u32(out, 0);
	u64(out, summary.eventCount); u64(out, summary.payloadBytes);
	u64(out, summary.droppedEvents); u64(out, summary.startTick);
	u64(out, summary.endTick); u64(out, summary.completedCommands);
	out.insert(out.end(), summary.binding.identityDigest.begin(),
			summary.binding.identityDigest.end());
	out.insert(out.end(), summary.binding.replayDigest.begin(),
			summary.binding.replayDigest.end());
	out.insert(out.end(), summary.binding.biosDigest.begin(),
			summary.binding.biosDigest.end());
	out.insert(out.end(), summary.binding.flashDigest.begin(),
			summary.binding.flashDigest.end());
	out.insert(out.end(), summary.payloadDigest.begin(), summary.payloadDigest.end());
	for (const auto count : summary.typeCounts) u64(out, count);
	out.resize(GdromHardwareArtifactHeaderSize - 4, 0);
	u32(out, 0);
	put32(out, GdromHardwareArtifactHeaderSize - 4,
			crc32(out.data(), GdromHardwareArtifactHeaderSize - 4));
	return out;
}
std::vector<std::uint8_t> eventBytes(const GdromHardwareObservation& event,
		std::uint64_t ordinal)
{
	require(event.bytes.size() <= std::numeric_limits<std::uint32_t>::max(),
			"event byte payload is too large");
	std::vector<std::uint8_t> payload;
	u64(payload, event.commandGeneration); u64(payload, event.dmaGeneration);
	owner(payload, event.ataOwner); owner(payload, event.packetOwner);
	payload.insert(payload.end(), event.packet.begin(), event.packet.end());
	u32(payload, 0);
	for (const auto value : {
		event.features, event.byteCountRegister, event.driveState,
		event.startFad, event.sectorCount, event.sectorBytes,
		static_cast<std::uint32_t>(event.delivery),
		event.readSuccessful ? 1u : 0u, event.destination,
		event.dmaStar, event.dmaLength, event.dmaDirection,
		event.dmaEnabled, event.pioWord,
		event.statusRegister,
		static_cast<std::uint32_t>(event.abortReason),
		static_cast<std::uint32_t>(event.bytes.size())})
		u32(payload, value);
	u64(payload, event.streamOffset); u64(payload, event.transferredBytes);
	payload.insert(payload.end(), event.bytes.begin(), event.bytes.end());
	require(payload.size() <= std::numeric_limits<std::uint32_t>::max()
			- EventHeaderSize, "event is too large");
	std::vector<std::uint8_t> out;
	u32(out, static_cast<std::uint32_t>(event.type));
	u32(out, static_cast<std::uint32_t>(EventHeaderSize + payload.size()));
	u64(out, ordinal); u64(out, event.emissionOrdinal); u64(out, event.tick);
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}
} // namespace

class GdromHardwareArtifactWriter::OutputFile
{
public:
	explicit OutputFile(const std::filesystem::path& path)
	{
		std::error_code error;
		if (path.parent_path().empty()
				|| !std::filesystem::is_directory(path.parent_path(), error) || error)
			throw std::runtime_error("GD-ROM hardware output parent is not a directory");
#ifdef _WIN32
		handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			throw std::system_error(static_cast<int>(GetLastError()),
					std::system_category(), "cannot exclusively create GD-ROM hardware output");
#else
		fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0) throw std::system_error(errno, std::generic_category(),
				"cannot exclusively create GD-ROM hardware output");
#endif
	}
	~OutputFile()
	{
#ifdef _WIN32
		if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
		if (fd >= 0) ::close(fd);
#endif
	}
	void write(const std::vector<std::uint8_t>& bytes)
	{
		const std::uint8_t* data = bytes.data(); std::size_t size = bytes.size();
		while (size != 0)
		{
#ifdef _WIN32
			const DWORD chunk = static_cast<DWORD>((std::min<std::size_t>)(
					size, (std::numeric_limits<DWORD>::max)()));
			DWORD written = 0;
			if (!WriteFile(handle, data, chunk, &written, nullptr) || written == 0)
				throw std::system_error(static_cast<int>(GetLastError()),
						std::system_category(), "cannot write GD-ROM hardware output");
#else
			const ssize_t written = ::write(fd, data, size);
			if (written < 0 && errno == EINTR) continue;
			if (written <= 0) throw std::system_error(errno, std::generic_category(),
					"cannot write GD-ROM hardware output");
#endif
			data += written; size -= static_cast<std::size_t>(written);
		}
	}
	void seek(std::uint64_t offset)
	{
#ifdef _WIN32
		LARGE_INTEGER position; position.QuadPart = static_cast<LONGLONG>(offset);
		if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN))
			throw std::system_error(static_cast<int>(GetLastError()),
					std::system_category(), "cannot seek GD-ROM hardware output");
#else
		if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot seek GD-ROM hardware output");
#endif
	}
	void flush()
	{
#ifdef _WIN32
		if (!FlushFileBuffers(handle)) throw std::system_error(
				static_cast<int>(GetLastError()), std::system_category(),
				"cannot flush GD-ROM hardware output");
#else
		if (::fsync(fd) != 0) throw std::system_error(errno,
				std::generic_category(), "cannot flush GD-ROM hardware output");
#endif
	}
private:
#ifdef _WIN32
	HANDLE handle = INVALID_HANDLE_VALUE;
#else
	int fd = -1;
#endif
};

GdromHardwareArtifactWriter::GdromHardwareArtifactWriter(
		const std::filesystem::path& path,
		const GdromHardwareArtifactBinding& binding,
		std::uint64_t maximumBytes, std::uint64_t maximumEvents)
	: path(path), maximumBytes(maximumBytes), maximumEvents(maximumEvents)
{
	if (maximumBytes < GdromHardwareArtifactHeaderSize || maximumEvents == 0)
		throw std::invalid_argument("GD-ROM hardware artifact limits are invalid");
	summary.binding = binding;
	output = std::make_unique<OutputFile>(path);
	output->write(header(summary, false)); output->flush();
}
GdromHardwareArtifactWriter::~GdromHardwareArtifactWriter()
{ if (!finalized) abandon(); }

void GdromHardwareArtifactWriter::write(
		const GdromHardwareObservation& observation)
{
	if (finalized || abandoned)
		throw std::logic_error("GD-ROM hardware writer is not writable");
	if (observation.schemaVersion != GdromHardwareObservationSchemaVersion)
		throw std::runtime_error("GD-ROM hardware observation schema mismatch");
	if (summary.eventCount >= maximumEvents)
		throw std::runtime_error("GD-ROM hardware event limit exceeded");
	if (hasEvents && observation.tick < summary.endTick)
		throw std::runtime_error("GD-ROM hardware tick moved backwards");
	const auto type = observation.type;
	if (type == GdromHardwareObservationType::PacketAccepted)
	{
		if (commandOpen || awaitingStatusAck || observation.commandGeneration == 0)
			throw std::runtime_error("GD-ROM hardware command overlap");
		commandOpen = true; openGeneration = observation.commandGeneration;
		expectedBytes = std::uint64_t(observation.sectorCount)
				* observation.sectorBytes;
	}
	else if (type == GdromHardwareObservationType::Reset
			|| type == GdromHardwareObservationType::LoadState)
	{
		if (commandOpen) invalidCandidate = true;
		if (awaitingStatusAck) invalidCandidate = true;
		commandOpen = false; awaitingStatusAck = false;
	}
	else if (type == GdromHardwareObservationType::Abort)
	{
		if (commandOpen && observation.commandGeneration == openGeneration)
			invalidCandidate = true;
		commandOpen = false;
	}
	else if (type == GdromHardwareObservationType::StatusAcknowledged)
	{
		if (commandOpen)
		{
			if (observation.commandGeneration != openGeneration)
				throw std::runtime_error("GD-ROM status acknowledgement has no matching command");
		}
		else if (!awaitingStatusAck
				|| observation.commandGeneration != completedGeneration)
			throw std::runtime_error("GD-ROM terminal status acknowledgement is unrelated");
		else awaitingStatusAck = false;
	}
	else
	{
		if (!commandOpen || observation.commandGeneration != openGeneration)
			throw std::runtime_error("GD-ROM hardware event has no matching command");
		if (type == GdromHardwareObservationType::Complete)
		{
			if (observation.transferredBytes != expectedBytes)
				invalidCandidate = true;
			commandOpen = false; ++summary.completedCommands;
			awaitingStatusAck = true;
			completedGeneration = observation.commandGeneration;
		}
	}
	const auto bytes = eventBytes(observation, summary.eventCount);
	if (summary.payloadBytes > maximumBytes - GdromHardwareArtifactHeaderSize
			|| bytes.size() > maximumBytes - GdromHardwareArtifactHeaderSize
					- summary.payloadBytes)
		throw std::runtime_error("GD-ROM hardware byte limit exceeded");
	output->write(bytes); payloadHasher.update(bytes.data(), bytes.size());
	if (!hasEvents) summary.startTick = observation.tick;
	summary.endTick = observation.tick;
	summary.payloadBytes += bytes.size();
	++summary.typeCounts[static_cast<unsigned>(type) - 1];
	++summary.eventCount; hasEvents = true;
}

GdromHardwareArtifactSummary GdromHardwareArtifactWriter::finalize(
		std::uint64_t droppedEvents)
{
	if (finalized || abandoned)
		throw std::logic_error("GD-ROM hardware writer cannot finalize");
	if (!hasEvents || commandOpen || awaitingStatusAck || invalidCandidate
			|| summary.completedCommands == 0)
		throw std::logic_error("GD-ROM hardware artifact has no complete clean command");
	summary.droppedEvents = droppedEvents;
	if (droppedEvents != 0)
		throw std::runtime_error("GD-ROM hardware observations were dropped");
	summary.payloadDigest = payloadHasher.finalize();
	output->seek(0); output->write(header(summary, true)); output->flush();
	output.reset(); finalized = true;
	return summary;
}

void GdromHardwareArtifactWriter::abandon() noexcept
{ abandoned = true; output.reset(); }

} // namespace research

#include "research/gdrom_artifact.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
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

constexpr std::array<std::uint8_t, 8> Magic {'F','C','G','D','R','O','M','1'};
constexpr std::uint32_t Complete = 1;
constexpr std::uint32_t EventHeaderSize = 32;

void require(bool value, const char *message)
{
	if (!value) throw std::runtime_error(std::string("invalid GD-ROM artifact: ") + message);
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
	for (std::size_t i = 0; i < size; ++i) {
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}
void owner(std::vector<std::uint8_t>& out, const Sh4InstructionOwnerToken& token,
		Sh4ObservationBackend backend, std::uint64_t tick)
{
	const std::size_t start = out.size();
	require(token.valid, "command has no SH-4 owner");
	require(token.backend == backend && token.generation != 0 && token.tick <= tick,
			"command owner is inconsistent");
	out.push_back(1); out.push_back(static_cast<std::uint8_t>(token.backend));
	out.push_back(static_cast<std::uint8_t>(token.delaySlotDepth));
	out.push_back(static_cast<std::uint8_t>(token.delaySlotDepth >> 8));
	out.push_back(static_cast<std::uint8_t>(token.opcode));
	out.push_back(static_cast<std::uint8_t>(token.opcode >> 8));
	out.push_back(0); out.push_back(0);
	u64(out, token.generation); u64(out, token.tick); u32(out, token.pc); u32(out, token.pr);
	require(out.size() - start == 32, "internal owner size mismatch");
}
std::vector<std::uint8_t> header(const GdromArtifactSummary& summary, bool complete)
{
	std::vector<std::uint8_t> out;
	out.insert(out.end(), Magic.begin(), Magic.end());
	u32(out, GdromArtifactSchemaVersion); u32(out, GdromArtifactHeaderSize);
	u32(out, 0x01020304); u32(out, complete ? Complete : 0);
	u32(out, static_cast<std::uint32_t>(summary.binding.backend)); u32(out, 0);
	u64(out, summary.eventCount); u64(out, summary.payloadBytes); u64(out, summary.droppedEvents);
	u64(out, summary.startTick); u64(out, summary.endTick);
	out.insert(out.end(), summary.binding.identityDigest.begin(), summary.binding.identityDigest.end());
	out.insert(out.end(), summary.binding.replayDigest.begin(), summary.binding.replayDigest.end());
	out.insert(out.end(), summary.payloadDigest.begin(), summary.payloadDigest.end());
	for (auto count : summary.typeCounts) u64(out, count);
	out.resize(GdromArtifactHeaderSize - 4, 0);
	u32(out, 0);
	put32(out, GdromArtifactHeaderSize - 4, crc32(out.data(), GdromArtifactHeaderSize - 4));
	return out;
}
std::vector<std::uint8_t> eventBytes(const GdromObservation& observation,
		const GdromArtifactBinding& binding, std::uint64_t ordinal)
{
	std::vector<std::uint8_t> payload;
	u64(payload, observation.commandGeneration);
	switch (observation.type) {
	case GdromObservationType::CommandBegin:
		u32(payload, static_cast<std::uint32_t>(observation.path));
		owner(payload, observation.initiator, binding.backend, observation.tick);
		u32(payload, observation.requestId); u32(payload, observation.command);
		for (auto parameter : observation.parameters) u32(payload, parameter);
		break;
	case GdromObservationType::TransferChunk:
		u64(payload, observation.chunkOrdinal); u32(payload, observation.fad);
		u32(payload, observation.sectorCount); u32(payload, observation.destination);
		require(observation.bytes.size() <= std::numeric_limits<std::uint32_t>::max(),
				"chunk is too large");
		u32(payload, static_cast<std::uint32_t>(observation.bytes.size()));
		payload.insert(payload.end(), observation.bytes.begin(), observation.bytes.end());
		break;
	case GdromObservationType::Complete:
		u32(payload, static_cast<std::uint32_t>(observation.completion)); u32(payload, 0);
		u64(payload, observation.transferredBytes); break;
	case GdromObservationType::Abort:
		u32(payload, observation.requestId); u32(payload, 0);
		u64(payload, observation.transferredBytes); break;
	case GdromObservationType::Reset:
		u64(payload, observation.transferredBytes); break;
	default: require(false, "event type is invalid");
	}
	require(payload.size() <= std::numeric_limits<std::uint32_t>::max() - EventHeaderSize,
			"event is too large");
	std::vector<std::uint8_t> out;
	u32(out, static_cast<std::uint32_t>(observation.type));
	u32(out, static_cast<std::uint32_t>(EventHeaderSize + payload.size()));
	u64(out, ordinal); u64(out, observation.emissionOrdinal); u64(out, observation.tick);
	out.insert(out.end(), payload.begin(), payload.end());
	return out;
}

} // namespace

class GdromArtifactWriter::OutputFile
{
public:
	explicit OutputFile(const std::filesystem::path& path)
	{
		std::error_code error;
		if(path.parent_path().empty() || !std::filesystem::is_directory(path.parent_path(),error) || error)
			throw std::runtime_error("GD-ROM output parent is not a directory");
#ifdef _WIN32
		handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,
				nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
		if(handle==INVALID_HANDLE_VALUE) throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"cannot exclusively create GD-ROM output");
#else
		fd=::open(path.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);
		if(fd<0) throw std::system_error(errno,std::generic_category(),"cannot exclusively create GD-ROM output");
#endif
	}
	~OutputFile(){
#ifdef _WIN32
		if(handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
		if(fd>=0) ::close(fd);
#endif
	}
	void write(const std::vector<std::uint8_t>& bytes)
	{
		const std::uint8_t* data=bytes.data(); std::size_t size=bytes.size();
		while(size!=0){
#ifdef _WIN32
			const DWORD chunk=static_cast<DWORD>(std::min<std::size_t>(size,(std::numeric_limits<DWORD>::max)())); DWORD written=0;
			if(!WriteFile(handle,data,chunk,&written,nullptr)||written==0) throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"cannot write GD-ROM output");
#else
			const ssize_t written=::write(fd,data,size); if(written<0&&errno==EINTR) continue;
			if(written<=0) throw std::system_error(errno,std::generic_category(),"cannot write GD-ROM output");
#endif
			data+=written;size-=static_cast<std::size_t>(written);
		}
	}
	void seek(std::uint64_t offset) {
#ifdef _WIN32
		LARGE_INTEGER position;position.QuadPart=static_cast<LONGLONG>(offset);
		if(!SetFilePointerEx(handle,position,nullptr,FILE_BEGIN)) throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"cannot seek GD-ROM output");
#else
		if(::lseek(fd,static_cast<off_t>(offset),SEEK_SET)<0) throw std::system_error(errno,std::generic_category(),"cannot seek GD-ROM output");
#endif
	}
	void flush() {
#ifdef _WIN32
		if(!FlushFileBuffers(handle)) throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"cannot flush GD-ROM output");
#else
		if(::fsync(fd)!=0) throw std::system_error(errno,std::generic_category(),"cannot flush GD-ROM output");
#endif
	}
private:
#ifdef _WIN32
	HANDLE handle=INVALID_HANDLE_VALUE;
#else
	int fd=-1;
#endif
};

GdromArtifactWriter::GdromArtifactWriter(const std::filesystem::path& path,
		const GdromArtifactBinding& binding, std::uint64_t maximumBytes,
		std::uint64_t maximumEvents)
	: path(path), maximumBytes(maximumBytes), maximumEvents(maximumEvents)
{
	if (maximumBytes < GdromArtifactHeaderSize || maximumEvents == 0)
		throw std::invalid_argument("GD-ROM artifact limits are invalid");
	summary.binding = binding;
	output = std::make_unique<OutputFile>(path);
	output->write(header(summary, false)); output->flush();
}
GdromArtifactWriter::~GdromArtifactWriter() { if (!finalized) abandon(); }

void GdromArtifactWriter::write(const GdromObservation& observation)
{
	if (finalized || abandoned) throw std::logic_error("GD-ROM writer is not writable");
	if (observation.schemaVersion != GdromObservationSchemaVersion)
		throw std::runtime_error("GD-ROM observation schema mismatch");
	if (summary.eventCount >= maximumEvents) throw std::runtime_error("GD-ROM event limit exceeded");
	if (hasEvents && observation.tick < summary.endTick) throw std::runtime_error("GD-ROM tick moved backwards");
	if (observation.type == GdromObservationType::CommandBegin) {
		if (commandOpen || observation.commandGeneration == 0) throw std::runtime_error("GD-ROM command overlap");
		commandOpen = true; openGeneration = observation.commandGeneration;
	} else if (observation.type != GdromObservationType::Reset) {
		if (!commandOpen || observation.commandGeneration != openGeneration) throw std::runtime_error("GD-ROM event has no matching command");
		if (observation.type == GdromObservationType::Complete || observation.type == GdromObservationType::Abort) commandOpen = false;
	} else commandOpen = false;
	const auto bytes = eventBytes(observation, summary.binding, summary.eventCount);
	if (summary.payloadBytes > maximumBytes - GdromArtifactHeaderSize
			|| bytes.size() > maximumBytes - GdromArtifactHeaderSize - summary.payloadBytes)
		throw std::runtime_error("GD-ROM byte limit exceeded");
	output->write(bytes); payloadHasher.update(bytes.data(), bytes.size());
	if (!hasEvents) summary.startTick = observation.tick;
	summary.endTick = observation.tick; summary.payloadBytes += bytes.size();
	++summary.typeCounts[static_cast<unsigned>(observation.type) - 1]; ++summary.eventCount;
	hasEvents = true;
}

GdromArtifactSummary GdromArtifactWriter::finalize(std::uint64_t droppedEvents)
{
	if (finalized || abandoned) throw std::logic_error("GD-ROM writer cannot finalize");
	if (!hasEvents || commandOpen) throw std::logic_error("GD-ROM artifact is empty or has an incomplete command");
	summary.droppedEvents = droppedEvents;
	if (droppedEvents != 0) throw std::runtime_error("GD-ROM observations were dropped");
	summary.payloadDigest = payloadHasher.finalize();
	output->seek(0); output->write(header(summary, true)); output->flush(); output.reset();
	finalized = true; return summary;
}

void GdromArtifactWriter::abandon() noexcept
{
	abandoned = true; output.reset();
}

} // namespace research

#include "research/sh4_events_artifact.h"

#include "research/identity_manifest.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

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

constexpr std::array<std::uint8_t, 8> Sh4EventsMagic {
	'F', 'C', 'S', 'H', '4', 'E', 'V', 'T',
};
constexpr std::uint32_t HeaderComplete = 1u << 0;
constexpr std::uint32_t KnownHeaderFlags = HeaderComplete;
constexpr std::uint32_t EventHeaderSize = 16;
constexpr std::uint32_t CallFixedPayloadSize = 144;
constexpr std::uint32_t ReturnFixedPayloadSize = 152;
constexpr std::uint32_t WatchPayloadSize = 32;
constexpr std::uint32_t ExceptionPayloadSize = 124;
constexpr std::uint32_t SnapshotFixedSize = 56;
constexpr std::uint32_t CallEventType = 1;
constexpr std::uint32_t ReturnEventType = 2;
constexpr std::uint32_t WatchEventType = 3;
constexpr std::uint32_t ExceptionEventType = 4;
constexpr std::uint32_t SnapshotAvailable = 1u << 0;
constexpr std::uint32_t SnapshotRequired = 1u << 1;
constexpr std::uint32_t KnownSnapshotFlags = SnapshotAvailable | SnapshotRequired;

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid SH-4 events artifact: " + reason);
}

void require(bool condition, const std::string& reason)
{
	if (!condition)
		invalid(reason);
}

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value));
	bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
	for (unsigned index = 0; index < 4; ++index)
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

void appendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
	for (unsigned index = 0; index < 8; ++index)
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

void appendRegisters(std::vector<std::uint8_t>& bytes,
		const Sh4RegisterSnapshot& registers)
{
	for (const std::uint32_t value : registers.r)
		appendU32(bytes, value);
	appendU32(bytes, registers.pr);
	appendU32(bytes, registers.gbr);
	appendU32(bytes, registers.vbr);
	appendU32(bytes, registers.mach);
	appendU32(bytes, registers.macl);
	appendU32(bytes, registers.sr);
	appendU32(bytes, registers.fpul);
	appendU32(bytes, registers.fpscr);
}

void writeU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
	require(offset <= bytes.size() && bytes.size() - offset >= 4,
			"internal header offset is out of range");
	for (unsigned index = 0; index < 4; ++index)
		bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
}

std::uint64_t checkedAdd(std::uint64_t lhs, std::uint64_t rhs, const char *field)
{
	if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs)
		throw std::overflow_error(std::string(field) + " overflow");
	return lhs + rhs;
}

class ByteReader
{
public:
	ByteReader(const std::uint8_t *data, std::size_t size) : data(data), size(size) {}

	std::uint8_t u8()
	{
		need(1);
		return data[position++];
	}

	std::uint16_t u16()
	{
		need(2);
		const std::uint16_t value = static_cast<std::uint16_t>(data[position])
				| (static_cast<std::uint16_t>(data[position + 1]) << 8);
		position += 2;
		return value;
	}

	std::uint32_t u32()
	{
		need(4);
		std::uint32_t value = 0;
		for (unsigned index = 0; index < 4; ++index)
			value |= static_cast<std::uint32_t>(data[position + index]) << (index * 8);
		position += 4;
		return value;
	}

	std::uint64_t u64()
	{
		need(8);
		std::uint64_t value = 0;
		for (unsigned index = 0; index < 8; ++index)
			value |= static_cast<std::uint64_t>(data[position + index]) << (index * 8);
		position += 8;
		return value;
	}

	std::vector<std::uint8_t> byteVector(std::size_t count)
	{
		need(count);
		std::vector<std::uint8_t> result(data + position, data + position + count);
		position += count;
		return result;
	}

	std::size_t remaining() const { return size - position; }

private:
	void need(std::size_t count) const
	{
		if (position > size || count > size - position)
			invalid("truncated scalar or event payload");
	}

	const std::uint8_t *data;
	std::size_t size;
	std::size_t position = 0;
};

Sh4RegisterSnapshot readRegisters(ByteReader& reader)
{
	Sh4RegisterSnapshot registers;
	for (std::uint32_t& value : registers.r)
		value = reader.u32();
	registers.pr = reader.u32();
	registers.gbr = reader.u32();
	registers.vbr = reader.u32();
	registers.mach = reader.u32();
	registers.macl = reader.u32();
	registers.sr = reader.u32();
	registers.fpul = reader.u32();
	registers.fpscr = reader.u32();
	return registers;
}

std::uint32_t crc32(const std::uint8_t *data, std::size_t size)
{
	std::uint32_t crc = 0xffffffffu;
	for (std::size_t index = 0; index < size; ++index)
	{
		crc ^= data[index];
		for (unsigned bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

std::vector<std::uint8_t> serializeHeader(const Sh4EventsArtifactSummary& summary,
		bool complete)
{
	std::vector<std::uint8_t> header;
	header.reserve(Sh4EventsArtifactHeaderSize);
	header.insert(header.end(), Sh4EventsMagic.begin(), Sh4EventsMagic.end());
	appendU32(header, Sh4EventsArtifactSchemaVersion);
	appendU32(header, Sh4EventsArtifactHeaderSize);
	appendU32(header, Sh4EventsArtifactEndianSentinel);
	appendU32(header, complete ? HeaderComplete : 0);
	appendU64(header, summary.eventCount);
	appendU64(header, summary.callCount);
	appendU64(header, summary.returnCount);
	appendU64(header, summary.watchReadCount);
	appendU64(header, summary.watchWriteCount);
	appendU64(header, summary.snapshotCount);
	appendU64(header, summary.snapshotBytes);
	appendU64(header, summary.exceptionCount);
	appendU64(header, summary.startTick);
	appendU64(header, summary.endTick);
	appendU64(header, summary.payloadBytes);
	appendU64(header, summary.droppedEvents);
	appendU64(header, summary.maximumOpenInvocations);
	header.insert(header.end(), summary.identityDigest.begin(), summary.identityDigest.end());
	header.insert(header.end(), summary.manifestDigest.begin(), summary.manifestDigest.end());
	header.insert(header.end(), summary.payloadDigest.begin(), summary.payloadDigest.end());
	appendU32(header, 0);
	appendU32(header, 0);
	require(header.size() == Sh4EventsArtifactHeaderSize, "internal header size mismatch");
	writeU32(header, Sh4EventsArtifactHeaderSize - 4,
			crc32(header.data(), Sh4EventsArtifactHeaderSize - 4));
	return header;
}

Sh4EventsArtifactSummary parseHeader(const std::uint8_t *bytes,
		const IdentityManifest& identity, const Sh4EventsManifest& manifest)
{
	ByteReader header(bytes, Sh4EventsArtifactHeaderSize);
	require(header.byteVector(Sh4EventsMagic.size())
			== std::vector<std::uint8_t>(Sh4EventsMagic.begin(), Sh4EventsMagic.end()),
			"magic mismatch");
	require(header.u32() == Sh4EventsArtifactSchemaVersion, "unsupported schema version");
	require(header.u32() == Sh4EventsArtifactHeaderSize, "header size mismatch");
	require(header.u32() == Sh4EventsArtifactEndianSentinel, "endian sentinel mismatch");
	const std::uint32_t flags = header.u32();
	require((flags & ~KnownHeaderFlags) == 0, "unknown header flags");
	require((flags & HeaderComplete) != 0, "artifact is incomplete");

	Sh4EventsArtifactSummary summary;
	summary.eventCount = header.u64();
	summary.callCount = header.u64();
	summary.returnCount = header.u64();
	summary.watchReadCount = header.u64();
	summary.watchWriteCount = header.u64();
	summary.snapshotCount = header.u64();
	summary.snapshotBytes = header.u64();
	summary.exceptionCount = header.u64();
	summary.startTick = header.u64();
	summary.endTick = header.u64();
	summary.payloadBytes = header.u64();
	summary.droppedEvents = header.u64();
	summary.maximumOpenInvocations = header.u64();
	const std::vector<std::uint8_t> identityDigest = header.byteVector(32);
	std::copy(identityDigest.begin(), identityDigest.end(), summary.identityDigest.begin());
	const std::vector<std::uint8_t> manifestDigest = header.byteVector(32);
	std::copy(manifestDigest.begin(), manifestDigest.end(), summary.manifestDigest.begin());
	const std::vector<std::uint8_t> payloadDigest = header.byteVector(32);
	std::copy(payloadDigest.begin(), payloadDigest.end(), summary.payloadDigest.begin());
	require(header.u32() == 0, "header reserved field is nonzero");
	const std::uint32_t storedCrc = header.u32();
	require(header.remaining() == 0, "internal header parser mismatch");
	require(storedCrc == crc32(bytes, Sh4EventsArtifactHeaderSize - 4),
			"header CRC mismatch");
	require(sha256Equal(summary.identityDigest, identity.digest),
			"identity manifest digest mismatch");
	require(sha256Equal(summary.manifestDigest, manifest.digest),
			"SH-4 events manifest digest mismatch");
	return summary;
}

std::uint64_t artifactFileSize(const std::filesystem::path& path)
{
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error)
		throw std::runtime_error("cannot stat SH-4 events artifact '" + path.string()
				+ "': " + error.message());
	if (size > std::numeric_limits<std::uint64_t>::max())
		throw std::runtime_error("SH-4 events artifact is too large: " + path.string());
	return static_cast<std::uint64_t>(size);
}

void readExact(std::ifstream& input, void *destination, std::size_t size,
		const char *reason)
{
	if (size == 0)
		return;
	input.read(static_cast<char *>(destination), static_cast<std::streamsize>(size));
	if (input.gcount() != static_cast<std::streamsize>(size))
		invalid(reason);
}

bool deriveSnapshotAddress(const Sh4SnapshotDefinition& definition,
		const Sh4RegisterSnapshot& registers, std::uint32_t& address)
{
	std::int64_t candidate = definition.absoluteAddress;
	if (definition.sourceKind == Sh4SnapshotSourceKind::RegisterRelative)
		candidate = static_cast<std::int64_t>(registers.r[definition.registerIndex])
				+ definition.offset;
	if (candidate < 0 || candidate > std::numeric_limits<std::uint32_t>::max())
		return false;
	const std::uint64_t end = static_cast<std::uint64_t>(candidate) + definition.length;
	if (end > (std::uint64_t {1} << 32))
		return false;
	address = static_cast<std::uint32_t>(candidate);
	return true;
}

bool overlaps(std::uint32_t firstAddress, std::uint32_t firstLength,
		std::uint32_t secondAddress, std::uint32_t secondLength)
{
	const std::uint64_t firstEnd = static_cast<std::uint64_t>(firstAddress) + firstLength;
	const std::uint64_t secondEnd = static_cast<std::uint64_t>(secondAddress) + secondLength;
	return firstAddress < secondEnd && secondAddress < firstEnd;
}

bool decodeCall(std::uint32_t callPc, std::uint16_t opcode,
		const Sh4RegisterSnapshot& registers, Sh4CallKind& kind, std::uint32_t& targetPc)
{
	if ((opcode & 0xf000u) == 0xb000u)
	{
		kind = Sh4CallKind::Bsr;
		const std::int32_t displacement = static_cast<std::int16_t>(
				static_cast<std::uint16_t>((opcode & 0x0fffu) << 4)) >> 4;
		targetPc = callPc + 4u + static_cast<std::uint32_t>(displacement * 2);
		return true;
	}
	if ((opcode & 0xf0ffu) == 0x0003u)
	{
		kind = Sh4CallKind::Bsrf;
		targetPc = callPc + 4u + registers.r[(opcode >> 8) & 0x0fu];
		return true;
	}
	if ((opcode & 0xf0ffu) == 0x400bu)
	{
		kind = Sh4CallKind::Jsr;
		targetPc = registers.r[(opcode >> 8) & 0x0fu];
		return true;
	}
	return false;
}

std::uint64_t widthMask(std::uint8_t width)
{
	return width == 8 ? std::numeric_limits<std::uint64_t>::max()
			: (std::uint64_t {1} << (width * 8)) - 1;
}

bool isSynchronousExceptionCode(std::uint32_t code)
{
	switch (code)
	{
	case 0x40:
	case 0x60:
	case 0x80:
	case 0xa0:
	case 0xc0:
	case 0xe0:
	case 0x100:
	case 0x120:
	case 0x140:
	case 0x160:
	case 0x180:
	case 0x1a0:
	case 0x1e0:
	case 0x800:
	case 0x820:
		return true;
	default:
		return false;
	}
}

} // namespace

class Sh4EventsArtifactWriter::OutputFile
{
public:
	explicit OutputFile(const std::filesystem::path& path)
	{
#ifdef _WIN32
		handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
					"cannot create SH-4 events output exclusively");
#else
		fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot create SH-4 events output exclusively");
#endif
	}

	~OutputFile()
	{
#ifdef _WIN32
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
#else
		if (fd >= 0)
			::close(fd);
#endif
	}

	void write(const void *source, std::size_t size)
	{
		const std::uint8_t *data = static_cast<const std::uint8_t *>(source);
		while (size != 0)
		{
#ifdef _WIN32
			const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size,
					std::numeric_limits<DWORD>::max()));
			DWORD written = 0;
			if (!WriteFile(handle, data, chunk, &written, nullptr) || written == 0)
				throw std::system_error(static_cast<int>(GetLastError()),
						std::system_category(), "cannot write SH-4 events output");
#else
			const std::size_t chunk = std::min<std::size_t>(size,
					static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
			const ssize_t written = ::write(fd, data, chunk);
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0)
				throw std::system_error(errno, std::generic_category(),
						"cannot write SH-4 events output");
#endif
			data += written;
			size -= written;
		}
	}

	void seek(std::uint64_t offset)
	{
#ifdef _WIN32
		LARGE_INTEGER position;
		position.QuadPart = static_cast<LONGLONG>(offset);
		if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN))
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
					"cannot seek SH-4 events output");
#else
		if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot seek SH-4 events output");
#endif
	}

	void flush()
	{
#ifdef _WIN32
		if (!FlushFileBuffers(handle))
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
					"cannot flush SH-4 events output");
#else
		if (::fsync(fd) != 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot flush SH-4 events output");
#endif
	}

private:
#ifdef _WIN32
	HANDLE handle = INVALID_HANDLE_VALUE;
#else
	int fd = -1;
#endif
};

Sh4EventsArtifactWriter::Sh4EventsArtifactWriter(const std::filesystem::path& path,
		const Sha256Digest& identityDigest, const Sh4EventsManifest& manifest,
		std::uint64_t maximumBytes)
	: path(path), identityDigest(identityDigest), manifest(manifest), maximumBytes(maximumBytes)
{
	if (maximumBytes < Sh4EventsArtifactHeaderSize)
		throw std::invalid_argument("SH-4 events size limit is smaller than the header");
	summary.identityDigest = identityDigest;
	summary.manifestDigest = manifest.digest;
	output = std::make_unique<OutputFile>(path);
	const std::vector<std::uint8_t> header = serializeHeader(summary, false);
	output->write(header.data(), header.size());
	output->flush();
}

Sh4EventsArtifactWriter::~Sh4EventsArtifactWriter()
{
	if (!finalized)
		abandon();
}

void Sh4EventsArtifactWriter::ensureWritable() const
{
	if (finalized)
		throw std::logic_error("SH-4 events artifact is already finalized");
	if (abandoned || output == nullptr)
		throw std::logic_error("SH-4 events artifact has been abandoned");
}

void Sh4EventsArtifactWriter::writePayload(const void *data, std::size_t size)
{
	const std::uint64_t newPayloadBytes = checkedAdd(summary.payloadBytes, size,
			"SH-4 events payload byte count");
	if (newPayloadBytes > maximumBytes - Sh4EventsArtifactHeaderSize)
		throw std::runtime_error("SH-4 events artifact exceeds configured byte limit");
	output->write(data, size);
	payloadHasher.update(data, size);
	summary.payloadBytes = newPayloadBytes;
}

void Sh4EventsArtifactWriter::beginEvent(std::uint32_t type, std::uint32_t size,
		std::uint64_t tick)
{
	ensureWritable();
	if (summary.eventCount >= manifest.maximumEvents)
		throw std::runtime_error("SH-4 events manifest event limit exceeded");
	if (size < EventHeaderSize
			|| size > maximumBytes - Sh4EventsArtifactHeaderSize - summary.payloadBytes)
		throw std::runtime_error("SH-4 event exceeds configured byte limit");
	if (hasEvents && tick < summary.endTick)
		throw std::logic_error("SH-4 event ticks are not monotonic");
	if (!hasEvents)
	{
		hasEvents = true;
		summary.startTick = tick;
	}
	summary.endTick = tick;
	std::vector<std::uint8_t> header;
	header.reserve(EventHeaderSize);
	appendU32(header, type);
	appendU32(header, size);
	appendU64(header, nextEventOrdinal++);
	writePayload(header.data(), header.size());
	++summary.eventCount;
}

void Sh4EventsArtifactWriter::writeSnapshots(
		const std::vector<Sh4SnapshotView>& snapshots)
{
	for (const Sh4SnapshotView& snapshot : snapshots)
	{
		if (snapshot.size != 0 && (snapshot.data == nullptr
				|| snapshot.size != snapshot.declaredLength))
			throw std::invalid_argument("SH-4 snapshot view length is inconsistent");
		if (snapshot.required && snapshot.size == 0)
			throw std::invalid_argument("required SH-4 snapshot view is unavailable");
		const Sha256Digest digest = snapshot.size == 0 ? Sha256Digest {}
				: sha256(snapshot.data, snapshot.size);
		std::vector<std::uint8_t> fixed;
		fixed.reserve(SnapshotFixedSize);
		appendU32(fixed, snapshot.definitionIndex);
		appendU32(fixed, snapshot.address);
		appendU32(fixed, snapshot.declaredLength);
		appendU32(fixed, static_cast<std::uint32_t>(snapshot.size));
		std::uint32_t flags = snapshot.size != 0 ? SnapshotAvailable : 0;
		if (snapshot.required)
			flags |= SnapshotRequired;
		appendU32(fixed, flags);
		appendU32(fixed, 0);
		fixed.insert(fixed.end(), digest.begin(), digest.end());
		require(fixed.size() == SnapshotFixedSize,
				"internal snapshot payload size mismatch");
		writePayload(fixed.data(), fixed.size());
		if (snapshot.size != 0)
			writePayload(snapshot.data, snapshot.size);
		++summary.snapshotCount;
		summary.snapshotBytes = checkedAdd(summary.snapshotBytes, snapshot.size,
				"SH-4 snapshot byte count");
	}
}

void Sh4EventsArtifactWriter::writeCall(const Sh4CallEvent& event)
{
	std::uint64_t snapshotBytes = 0;
	for (const Sh4SnapshotView& snapshot : event.snapshots)
		snapshotBytes = checkedAdd(snapshotBytes, snapshot.size + SnapshotFixedSize,
				"SH-4 call snapshot payload");
	if (snapshotBytes > manifest.maximumSnapshotBytesPerEvent
			+ event.snapshots.size() * SnapshotFixedSize)
		throw std::runtime_error("SH-4 call snapshots exceed per-event limit");
	const std::uint64_t rawSnapshotBytes = snapshotBytes
			- event.snapshots.size() * SnapshotFixedSize;
	if (rawSnapshotBytes > manifest.maximumTotalSnapshotBytes - summary.snapshotBytes)
		throw std::runtime_error("SH-4 snapshots exceed total byte limit");
	const std::uint64_t eventSize = EventHeaderSize + CallFixedPayloadSize + snapshotBytes;
	if (eventSize > std::numeric_limits<std::uint32_t>::max())
		throw std::overflow_error("SH-4 call event size overflow");
	beginEvent(CallEventType, static_cast<std::uint32_t>(eventSize), event.tick);
	std::vector<std::uint8_t> fixed;
	fixed.reserve(CallFixedPayloadSize);
	appendU64(fixed, event.tick);
	appendU64(fixed, event.invocationId);
	appendU32(fixed, event.hookIndex);
	appendU16(fixed, static_cast<std::uint16_t>(event.kind));
	appendU16(fixed, event.opcode);
	appendU32(fixed, event.callPc);
	appendU32(fixed, event.targetPc);
	appendU32(fixed, event.returnPc);
	appendU32(fixed, event.delaySlotPc);
	appendU32(fixed, event.delaySlotDepth);
	appendU32(fixed, static_cast<std::uint32_t>(event.snapshots.size()));
	appendRegisters(fixed, event.registers);
	require(fixed.size() == CallFixedPayloadSize,
			"internal call payload size mismatch");
	writePayload(fixed.data(), fixed.size());
	writeSnapshots(event.snapshots);
	++summary.callCount;
}

void Sh4EventsArtifactWriter::writeReturn(const Sh4ReturnEvent& event)
{
	if (event.completionTick < event.instructionTick)
		throw std::invalid_argument("SH-4 return completion tick precedes instruction tick");
	std::uint64_t snapshotBytes = 0;
	for (const Sh4SnapshotView& snapshot : event.snapshots)
		snapshotBytes = checkedAdd(snapshotBytes, snapshot.size + SnapshotFixedSize,
				"SH-4 return snapshot payload");
	const std::uint64_t rawSnapshotBytes = snapshotBytes
			- event.snapshots.size() * SnapshotFixedSize;
	if (rawSnapshotBytes > manifest.maximumSnapshotBytesPerEvent
			|| rawSnapshotBytes > manifest.maximumTotalSnapshotBytes - summary.snapshotBytes)
		throw std::runtime_error("SH-4 return snapshots exceed manifest limits");
	const std::uint64_t eventSize = EventHeaderSize + ReturnFixedPayloadSize + snapshotBytes;
	if (eventSize > std::numeric_limits<std::uint32_t>::max())
		throw std::overflow_error("SH-4 return event size overflow");
	beginEvent(ReturnEventType, static_cast<std::uint32_t>(eventSize), event.completionTick);
	std::vector<std::uint8_t> fixed;
	fixed.reserve(ReturnFixedPayloadSize);
	appendU64(fixed, event.instructionTick);
	appendU64(fixed, event.completionTick);
	appendU64(fixed, event.invocationId);
	appendU32(fixed, event.hookIndex);
	appendU16(fixed, event.opcode);
	appendU16(fixed, 0);
	appendU32(fixed, event.instructionPc);
	appendU32(fixed, event.resumedPc);
	appendU32(fixed, event.expectedReturnPc);
	appendU32(fixed, event.delaySlotDepth);
	appendU32(fixed, static_cast<std::uint32_t>(event.snapshots.size()));
	appendU32(fixed, 0);
	appendRegisters(fixed, event.registers);
	require(fixed.size() == ReturnFixedPayloadSize,
			"internal return payload size mismatch");
	writePayload(fixed.data(), fixed.size());
	writeSnapshots(event.snapshots);
	++summary.returnCount;
}

void Sh4EventsArtifactWriter::writeWatch(const Sh4WatchEvent& event)
{
	if (event.width != 1 && event.width != 2 && event.width != 4 && event.width != 8)
		throw std::invalid_argument("SH-4 watch width is unsupported");
	if (event.kind != Sh4MemoryAccessKind::Read
			&& event.kind != Sh4MemoryAccessKind::Write)
		throw std::invalid_argument("SH-4 watch kind is unsupported");
	if ((event.value & ~widthMask(event.width)) != 0)
		throw std::invalid_argument("SH-4 watch value has bits outside its width");
	beginEvent(WatchEventType, EventHeaderSize + WatchPayloadSize, event.tick);
	std::vector<std::uint8_t> payload;
	payload.reserve(WatchPayloadSize);
	appendU64(payload, event.tick);
	appendU32(payload, event.watchIndex);
	appendU32(payload, event.instructionPc);
	appendU32(payload, event.address);
	payload.push_back(event.width);
	payload.push_back(static_cast<std::uint8_t>(event.kind));
	appendU16(payload, event.delaySlotDepth);
	appendU64(payload, event.value);
	require(payload.size() == WatchPayloadSize,
			"internal watch payload size mismatch");
	writePayload(payload.data(), payload.size());
	if (event.kind == Sh4MemoryAccessKind::Read)
		++summary.watchReadCount;
	else
		++summary.watchWriteCount;
}

void Sh4EventsArtifactWriter::writeException(const Sh4ExceptionEvent& event)
{
	beginEvent(ExceptionEventType, EventHeaderSize + ExceptionPayloadSize, event.tick);
	std::vector<std::uint8_t> payload;
	payload.reserve(ExceptionPayloadSize);
	appendU64(payload, event.tick);
	appendU32(payload, event.instructionPc);
	appendU32(payload, event.exceptionPc);
	appendU32(payload, event.vectorPc);
	appendU32(payload, event.exceptionCode);
	appendU16(payload, event.delaySlotDepth);
	appendU16(payload, 0);
	appendRegisters(payload, event.registers);
	require(payload.size() == ExceptionPayloadSize,
			"internal exception payload size mismatch");
	writePayload(payload.data(), payload.size());
	++summary.exceptionCount;
}

void Sh4EventsArtifactWriter::observeOpenInvocations(std::size_t count)
{
	summary.maximumOpenInvocations = std::max<std::uint64_t>(
			summary.maximumOpenInvocations, count);
}

Sh4EventsArtifactSummary Sh4EventsArtifactWriter::finalize()
{
	ensureWritable();
	if (!hasEvents || summary.callCount < manifest.minimumCallEvents
			|| summary.watchReadCount + summary.watchWriteCount
					< manifest.minimumWatchEvents
			|| summary.callCount != summary.returnCount)
		throw std::logic_error("cannot finalize incomplete SH-4 events evidence");
	summary.payloadDigest = payloadHasher.finalize();
	const std::vector<std::uint8_t> header = serializeHeader(summary, true);
	output->seek(0);
	output->write(header.data(), header.size());
	output->flush();
	finalized = true;
	return summary;
}

void Sh4EventsArtifactWriter::abandon() noexcept
{
	if (finalized || abandoned)
		return;
	abandoned = true;
	output.reset();
}

Sh4EventsArtifactSummary validateProductionSh4EventsArtifactFile(
		const std::filesystem::path& path, const IdentityManifest& identity,
		const Sh4EventsManifest& manifest, std::uint64_t maximumBytes)
{
	requireSh4EventsIdentity(manifest, identity);
	const std::uint64_t sizeBefore = artifactFileSize(path);
	require(sizeBefore <= maximumBytes, "file exceeds size limit");
	require(sizeBefore >= Sh4EventsArtifactHeaderSize, "file is smaller than the header");
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open SH-4 events artifact for reading: "
				+ path.string());
	std::array<std::uint8_t, Sh4EventsArtifactHeaderSize> headerBytes {};
	readExact(input, headerBytes.data(), headerBytes.size(), "truncated artifact header");
	Sh4EventsArtifactSummary summary = parseHeader(headerBytes.data(), identity, manifest);
	require(summary.payloadBytes == sizeBefore - Sh4EventsArtifactHeaderSize,
			"payload byte count mismatch");
	require(summary.droppedEvents == 0, "dropped event count is nonzero");
	require(summary.eventCount <= manifest.maximumEvents, "event count exceeds manifest limit");
	require(summary.snapshotBytes <= manifest.maximumTotalSnapshotBytes,
			"snapshot bytes exceed manifest limit");
	require(summary.maximumOpenInvocations <= manifest.maximumOpenInvocations,
			"open-invocation depth exceeds manifest limit");

	struct Invocation
	{
		std::uint64_t id;
		std::uint32_t hookIndex;
		std::uint32_t returnPc;
		std::uint64_t callTick;
	};
	std::vector<Invocation> invocations;
	std::uint64_t nextInvocationId = 0;
	std::uint64_t observedMaximumOpen = 0;
	std::uint64_t eventOrdinal = 0;
	std::uint64_t callCount = 0;
	std::uint64_t returnCount = 0;
	std::uint64_t watchReadCount = 0;
	std::uint64_t watchWriteCount = 0;
	std::uint64_t snapshotCount = 0;
	std::uint64_t snapshotBytes = 0;
	std::uint64_t exceptionCount = 0;
	std::uint64_t firstTick = 0;
	std::uint64_t lastTick = 0;
	bool hasEvents = false;
	Sha256 payloadHasher;
	std::uint64_t remaining = summary.payloadBytes;

	auto validateSnapshots = [&](ByteReader& payload, const Sh4HookDefinition& hook,
			Sh4SnapshotPhase phase, const Sh4RegisterSnapshot& registers,
			std::uint32_t recordedCount) {
		std::vector<std::size_t> expectedIndexes;
		for (std::size_t index = 0; index < hook.snapshots.size(); ++index)
			if (hook.snapshots[index].phase == phase)
				expectedIndexes.push_back(index);
		require(recordedCount == expectedIndexes.size(),
				"snapshot count does not match manifest phase");
		std::uint64_t eventSnapshotBytes = 0;
		for (std::size_t order = 0; order < expectedIndexes.size(); ++order)
		{
			const Sh4SnapshotDefinition& definition = hook.snapshots[expectedIndexes[order]];
			const std::uint32_t definitionIndex = payload.u32();
			const std::uint32_t address = payload.u32();
			const std::uint32_t declaredLength = payload.u32();
			const std::uint32_t capturedLength = payload.u32();
			const std::uint32_t flags = payload.u32();
			require(payload.u32() == 0, "snapshot reserved field is nonzero");
			Sha256Digest recordedDigest {};
			const std::vector<std::uint8_t> digestBytes = payload.byteVector(32);
			std::copy(digestBytes.begin(), digestBytes.end(), recordedDigest.begin());
			require(definitionIndex == expectedIndexes[order],
					"snapshot definition index is out of order");
			require(declaredLength == definition.length,
					"snapshot declared length differs from manifest");
			require((flags & ~KnownSnapshotFlags) == 0,
					"snapshot has unknown flags");
			const bool available = (flags & SnapshotAvailable) != 0;
			const bool requiredSnapshot = (flags & SnapshotRequired) != 0;
			require(requiredSnapshot == definition.required,
					"snapshot required flag differs from manifest");
			std::uint32_t expectedAddress = 0;
			const bool addressValid = deriveSnapshotAddress(definition, registers,
					expectedAddress);
			require(address == expectedAddress,
					"snapshot address does not match its source rule");
			require(available == (capturedLength != 0),
					"snapshot availability and captured length disagree");
			require(capturedLength == (available ? definition.length : 0),
					"snapshot captured length is invalid");
			require(!definition.required || (addressValid && available),
					"required snapshot is unavailable");
			const std::vector<std::uint8_t> bytes = payload.byteVector(capturedLength);
			const Sha256Digest computedDigest = capturedLength == 0 ? Sha256Digest {}
					: sha256(bytes.data(), bytes.size());
			require(sha256Equal(recordedDigest, computedDigest),
					"snapshot digest does not match its bytes");
			eventSnapshotBytes = checkedAdd(eventSnapshotBytes, capturedLength,
					"validated event snapshot bytes");
			snapshotBytes = checkedAdd(snapshotBytes, capturedLength,
					"validated snapshot bytes");
			++snapshotCount;
		}
		require(eventSnapshotBytes <= manifest.maximumSnapshotBytesPerEvent,
				"event snapshot bytes exceed manifest limit");
	};

	while (remaining != 0)
	{
		require(remaining >= EventHeaderSize, "truncated event header");
		std::array<std::uint8_t, EventHeaderSize> eventHeaderBytes {};
		readExact(input, eventHeaderBytes.data(), eventHeaderBytes.size(),
				"truncated event header");
		payloadHasher.update(eventHeaderBytes.data(), eventHeaderBytes.size());
		ByteReader eventHeader(eventHeaderBytes.data(), eventHeaderBytes.size());
		const std::uint32_t type = eventHeader.u32();
		const std::uint32_t eventSize = eventHeader.u32();
		const std::uint64_t ordinal = eventHeader.u64();
		require(ordinal == eventOrdinal++, "event ordinal is not contiguous");
		require(eventSize >= EventHeaderSize && eventSize <= remaining,
				"event size is out of range");
		const std::size_t payloadSize = eventSize - EventHeaderSize;
		std::vector<std::uint8_t> eventBytes(payloadSize);
		readExact(input, eventBytes.data(), eventBytes.size(), "truncated event payload");
		payloadHasher.update(eventBytes.data(), eventBytes.size());
		ByteReader payload(eventBytes.data(), eventBytes.size());
		std::uint64_t eventTick = 0;

		if (type == CallEventType)
		{
			require(payloadSize >= CallFixedPayloadSize, "call event is truncated");
			eventTick = payload.u64();
			const std::uint64_t invocationId = payload.u64();
			const std::uint32_t hookIndex = payload.u32();
			const Sh4CallKind kind = static_cast<Sh4CallKind>(payload.u16());
			const std::uint16_t opcode = payload.u16();
			const std::uint32_t callPc = payload.u32();
			const std::uint32_t targetPc = payload.u32();
			const std::uint32_t returnPc = payload.u32();
			const std::uint32_t delaySlotPc = payload.u32();
			const std::uint32_t delaySlotDepth = payload.u32();
			const std::uint32_t recordedSnapshotCount = payload.u32();
			const Sh4RegisterSnapshot registers = readRegisters(payload);
			require(hookIndex < manifest.hooks.size(), "call hook index is out of range");
			const Sh4HookDefinition& hook = manifest.hooks[hookIndex];
			Sh4CallKind decodedKind = Sh4CallKind::Bsr;
			std::uint32_t decodedTarget = 0;
			require(decodeCall(callPc, opcode, registers, decodedKind, decodedTarget),
					"call opcode is not BSR, BSRF, or JSR");
			require(kind == decodedKind && targetPc == decodedTarget,
					"call kind/target does not match opcode and registers");
			require(targetPc == hook.entryPc, "call target does not match hook entry");
			require((callPc & 1u) == 0, "call PC is not instruction-aligned");
			require(returnPc == callPc + 4u && delaySlotPc == callPc + 2u,
					"call return/delay-slot PC is invalid");
			require(delaySlotDepth == 0, "call instruction cannot be in a delay slot");
			require(invocationId == nextInvocationId++,
					"call invocation id is not contiguous");
			validateSnapshots(payload, hook, Sh4SnapshotPhase::Call, registers,
					recordedSnapshotCount);
			invocations.push_back(Invocation {invocationId, hookIndex, returnPc,
					eventTick});
			observedMaximumOpen = std::max<std::uint64_t>(observedMaximumOpen,
					invocations.size());
			require(invocations.size() <= manifest.maximumOpenInvocations,
					"open-invocation depth exceeds manifest limit");
			++callCount;
		}
		else if (type == ReturnEventType)
		{
			require(payloadSize >= ReturnFixedPayloadSize, "return event is truncated");
			const std::uint64_t instructionTick = payload.u64();
			eventTick = payload.u64();
			const std::uint64_t invocationId = payload.u64();
			const std::uint32_t hookIndex = payload.u32();
			const std::uint16_t opcode = payload.u16();
			require(payload.u16() == 0, "return reserved field is nonzero");
			const std::uint32_t instructionPc = payload.u32();
			const std::uint32_t resumedPc = payload.u32();
			const std::uint32_t expectedReturnPc = payload.u32();
			const std::uint32_t delaySlotDepth = payload.u32();
			const std::uint32_t recordedSnapshotCount = payload.u32();
			require(payload.u32() == 0, "return reserved field is nonzero");
			const Sh4RegisterSnapshot registers = readRegisters(payload);
			require(!invocations.empty(), "return has no open invocation");
			const Invocation invocation = invocations.back();
			require(invocationId == invocation.id && hookIndex == invocation.hookIndex,
					"return does not close the latest invocation");
			const Sh4HookDefinition& hook = manifest.hooks.at(hookIndex);
			require(opcode == 0x000bu, "return opcode is not RTS");
			require((instructionPc & 1u) == 0,
					"return instruction PC is not instruction-aligned");
			require(instructionPc >= hook.entryPc && instructionPc < hook.endPcExclusive,
					"return instruction is outside its hook interval");
			require(resumedPc == invocation.returnPc
					&& expectedReturnPc == invocation.returnPc,
					"return resumed/expected PC differs from invocation");
			require(delaySlotDepth == 0, "RTS instruction cannot be in a delay slot");
			require(eventTick >= instructionTick,
					"return completion tick precedes its instruction tick");
			require(instructionTick >= invocation.callTick,
					"return instruction tick precedes its call");
			validateSnapshots(payload, hook, Sh4SnapshotPhase::Return, registers,
					recordedSnapshotCount);
			invocations.pop_back();
			++returnCount;
		}
		else if (type == WatchEventType)
		{
			require(payloadSize == WatchPayloadSize, "watch event size mismatch");
			eventTick = payload.u64();
			const std::uint32_t watchIndex = payload.u32();
			const std::uint32_t instructionPc = payload.u32();
			const std::uint32_t address = payload.u32();
			const std::uint8_t width = payload.u8();
			const Sh4MemoryAccessKind kind = static_cast<Sh4MemoryAccessKind>(payload.u8());
			const std::uint16_t delaySlotDepth = payload.u16();
			const std::uint64_t value = payload.u64();
			require(watchIndex < manifest.watchRanges.size(),
					"watch index is out of range");
			require(width == 1 || width == 2 || width == 4 || width == 8,
					"watch width is invalid");
			require((instructionPc & 1u) == 0,
					"watch instruction PC is not instruction-aligned");
			require(static_cast<std::uint64_t>(address) + width
					<= (std::uint64_t {1} << 32),
					"watch access wraps the address space");
			require((value & ~widthMask(width)) == 0,
					"watch value has bits outside its width");
			require(delaySlotDepth <= 1, "watch delay-slot depth is invalid");
			const Sh4WatchRangeDefinition& watch = manifest.watchRanges[watchIndex];
			const std::uint8_t access = kind == Sh4MemoryAccessKind::Read ? Sh4WatchRead
					: kind == Sh4MemoryAccessKind::Write ? Sh4WatchWrite : 0;
			require(access != 0 && (watch.access & access) != 0,
					"watch access kind is not enabled by manifest");
			require(overlaps(address, width, watch.address, watch.length),
					"watch access does not overlap its manifest range");
			if (kind == Sh4MemoryAccessKind::Read)
				++watchReadCount;
			else
				++watchWriteCount;
		}
		else if (type == ExceptionEventType)
		{
			require(payloadSize == ExceptionPayloadSize, "exception event size mismatch");
			eventTick = payload.u64();
			const std::uint32_t instructionPc = payload.u32();
			const std::uint32_t exceptionPc = payload.u32();
			const std::uint32_t vectorPc = payload.u32();
			const std::uint32_t exceptionCode = payload.u32();
			const std::uint16_t delaySlotDepth = payload.u16();
			require(payload.u16() == 0, "exception reserved field is nonzero");
			const Sh4RegisterSnapshot registers = readRegisters(payload);
			bool inHook = false;
			for (const Sh4HookDefinition& hook : manifest.hooks)
				inHook = inHook || (instructionPc >= hook.entryPc
						&& instructionPc < hook.endPcExclusive);
			require(inHook || !invocations.empty(),
					"exception is unrelated to configured hooks");
			require(delaySlotDepth <= 1, "exception delay-slot depth is invalid");
			require((instructionPc & 1u) == 0,
					"exception instruction PC is not instruction-aligned");
			require(isSynchronousExceptionCode(exceptionCode),
					"exception code is not a synchronous SH-4 exception");
			const std::uint32_t expectedExceptionPc = delaySlotDepth == 0
					? instructionPc : instructionPc - 2u;
			require(exceptionPc == expectedExceptionPc,
					"exception PC does not match delay-slot ownership");
			require((delaySlotDepth == 0
					&& exceptionCode != 0x1a0u && exceptionCode != 0x820u)
					|| (delaySlotDepth == 1
							&& exceptionCode != 0x180u && exceptionCode != 0x800u),
					"exception code does not match delay-slot adjustment");
			const std::uint32_t vectorOffset = exceptionCode == 0x40u
					|| exceptionCode == 0x60u ? 0x400u : 0x100u;
			require(vectorPc == registers.vbr + vectorOffset,
					"exception code/vector does not match the recorded VBR");
			++exceptionCount;
		}
		else
		{
			invalid("unknown event type");
		}

		require(payload.remaining() == 0, "event has trailing payload bytes");
		if (hasEvents)
			require(eventTick >= lastTick, "event ticks are not monotonic");
		else
		{
			hasEvents = true;
			firstTick = eventTick;
		}
		lastTick = eventTick;
		remaining -= eventSize;
	}

	char extra = 0;
	input.read(&extra, 1);
	require(input.gcount() == 0, "file grew while reading");
	require(sizeBefore == artifactFileSize(path), "file size changed while reading");
	require(sha256Equal(summary.payloadDigest, payloadHasher.finalize()),
			"payload SHA-256 mismatch");
	require(hasEvents && summary.eventCount == eventOrdinal,
			"event count mismatch");
	require(summary.callCount == callCount && summary.returnCount == returnCount,
			"call/return count mismatch");
	require(summary.watchReadCount == watchReadCount
			&& summary.watchWriteCount == watchWriteCount,
			"watch count mismatch");
	require(summary.snapshotCount == snapshotCount
			&& summary.snapshotBytes == snapshotBytes,
			"snapshot count/byte mismatch");
	require(summary.exceptionCount == exceptionCount, "exception count mismatch");
	require(summary.maximumOpenInvocations == observedMaximumOpen,
			"maximum open-invocation depth mismatch");
	require(invocations.empty() && callCount == returnCount,
			"artifact has unbalanced invocations");
	require(callCount >= manifest.minimumCallEvents,
			"call-event minimum was not reached");
	require(watchReadCount + watchWriteCount >= manifest.minimumWatchEvents,
			"watch-event minimum was not reached");
	require(snapshotBytes <= manifest.maximumTotalSnapshotBytes,
			"snapshot total exceeds manifest limit");
	require(summary.startTick == firstTick && summary.endTick == lastTick,
			"header ticks do not match event stream");
	require(summary.eventCount == callCount + returnCount + watchReadCount
			+ watchWriteCount + exceptionCount,
			"typed event counts do not sum to event_count");
	return summary;
}

} // namespace research

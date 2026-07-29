#include "research/memory_ranges_artifact.h"

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

constexpr std::array<std::uint8_t, 8> MemoryRangesMagic {
	'F', 'C', 'M', 'E', 'M', 'R', 'N', 'G',
};
constexpr std::uint32_t HeaderComplete = 1u << 0;
constexpr std::uint32_t KnownHeaderFlags = HeaderComplete;
constexpr std::uint32_t EventHeaderSize = 16;
constexpr std::uint32_t TriggerPayloadSize = 16;
constexpr std::uint32_t RangeFixedPayloadSize = 56;
constexpr std::uint32_t TriggerEventType = 1;
constexpr std::uint32_t RangeEventType = 2;
constexpr std::size_t IoWindowBytes = 64 * 1024;

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid memory-ranges artifact: " + reason);
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

std::vector<std::uint8_t> serializeHeader(const MemoryRangesArtifactSummary& summary,
		bool complete)
{
	std::vector<std::uint8_t> header;
	header.reserve(MemoryRangesArtifactHeaderSize);
	header.insert(header.end(), MemoryRangesMagic.begin(), MemoryRangesMagic.end());
	appendU32(header, MemoryRangesArtifactSchemaVersion);
	appendU32(header, MemoryRangesArtifactHeaderSize);
	appendU32(header, MemoryRangesArtifactEndianSentinel);
	appendU32(header, complete ? HeaderComplete : 0);
	appendU64(header, summary.eventCount);
	appendU64(header, summary.triggerCount);
	appendU64(header, summary.rangeEventCount);
	appendU64(header, summary.totalMemoryBytes);
	appendU64(header, summary.startTick);
	appendU64(header, summary.endTick);
	appendU64(header, summary.payloadBytes);
	appendU64(header, summary.droppedEvents);
	header.insert(header.end(), summary.identityDigest.begin(), summary.identityDigest.end());
	header.insert(header.end(), summary.manifestDigest.begin(), summary.manifestDigest.end());
	header.insert(header.end(), summary.payloadDigest.begin(), summary.payloadDigest.end());
	appendU32(header, 0);
	appendU32(header, 0);
	require(header.size() == MemoryRangesArtifactHeaderSize, "internal header size mismatch");
	writeU32(header, MemoryRangesArtifactHeaderSize - 4,
			crc32(header.data(), MemoryRangesArtifactHeaderSize - 4));
	return header;
}

MemoryRangesArtifactSummary parseHeader(const std::uint8_t *bytes,
		const IdentityManifest& identity, const MemoryRangesManifest& manifest)
{
	ByteReader header(bytes, MemoryRangesArtifactHeaderSize);
	require(header.byteVector(MemoryRangesMagic.size())
			== std::vector<std::uint8_t>(MemoryRangesMagic.begin(), MemoryRangesMagic.end()),
			"magic mismatch");
	require(header.u32() == MemoryRangesArtifactSchemaVersion, "unsupported schema version");
	require(header.u32() == MemoryRangesArtifactHeaderSize, "header size mismatch");
	require(header.u32() == MemoryRangesArtifactEndianSentinel, "endian sentinel mismatch");
	const std::uint32_t flags = header.u32();
	require((flags & ~KnownHeaderFlags) == 0, "unknown header flags");
	require((flags & HeaderComplete) != 0, "artifact is incomplete");

	MemoryRangesArtifactSummary summary;
	summary.eventCount = header.u64();
	summary.triggerCount = header.u64();
	summary.rangeEventCount = header.u64();
	summary.totalMemoryBytes = header.u64();
	summary.startTick = header.u64();
	summary.endTick = header.u64();
	summary.payloadBytes = header.u64();
	summary.droppedEvents = header.u64();
	const std::vector<std::uint8_t> identityDigest = header.byteVector(32);
	std::copy(identityDigest.begin(), identityDigest.end(), summary.identityDigest.begin());
	const std::vector<std::uint8_t> manifestDigest = header.byteVector(32);
	std::copy(manifestDigest.begin(), manifestDigest.end(), summary.manifestDigest.begin());
	const std::vector<std::uint8_t> payloadDigest = header.byteVector(32);
	std::copy(payloadDigest.begin(), payloadDigest.end(), summary.payloadDigest.begin());
	require(header.u32() == 0, "header reserved field is nonzero");
	const std::uint32_t storedCrc = header.u32();
	require(header.remaining() == 0, "internal header parser mismatch");
	require(storedCrc == crc32(bytes, MemoryRangesArtifactHeaderSize - 4),
			"header CRC mismatch");
	require(sha256Equal(summary.identityDigest, identity.digest),
			"identity manifest digest mismatch");
	require(sha256Equal(summary.manifestDigest, manifest.digest),
			"memory-ranges manifest digest mismatch");
	return summary;
}

std::uint64_t artifactFileSize(const std::filesystem::path& path)
{
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error)
		throw std::runtime_error("cannot stat memory-ranges artifact '" + path.string()
				+ "': " + error.message());
	if (size > std::numeric_limits<std::uint64_t>::max())
		throw std::runtime_error("memory-ranges artifact is too large: " + path.string());
	return static_cast<std::uint64_t>(size);
}

void readExact(std::ifstream& input, void *destination, std::size_t count,
		const std::string& reason)
{
	if (count == 0)
		return;
	input.read(static_cast<char *>(destination), static_cast<std::streamsize>(count));
	require(input.gcount() == static_cast<std::streamsize>(count) && input.good(), reason);
}

std::uint64_t requiredArtifactBytes(const MemoryRangesManifest& manifest)
{
	std::uint64_t total = MemoryRangesArtifactHeaderSize + EventHeaderSize + TriggerPayloadSize;
	for (const MemoryRangeDefinition& range : manifest.ranges)
	{
		total = checkedAdd(total, EventHeaderSize + RangeFixedPayloadSize,
				"memory-ranges artifact size");
		total = checkedAdd(total, range.id.size(), "memory-ranges artifact size");
		total = checkedAdd(total, range.length, "memory-ranges artifact size");
	}
	return total;
}

} // namespace

class MemoryRangesArtifactWriter::OutputFile
{
public:
	explicit OutputFile(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::filesystem::path parent = path.parent_path();
		if (parent.empty() || !std::filesystem::is_directory(parent, error) || error)
			throw std::runtime_error("memory-ranges output parent is not a directory: "
					+ parent.string());
#ifdef _WIN32
		handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
				nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
					"cannot exclusively create memory-ranges output");
#else
		fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot exclusively create memory-ranges output");
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
		const auto *data = static_cast<const std::uint8_t *>(source);
		while (size != 0)
		{
#ifdef _WIN32
			const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size,
					(std::numeric_limits<DWORD>::max)()));
			DWORD written = 0;
			if (!WriteFile(handle, data, chunk, &written, nullptr) || written == 0)
				throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
						"cannot write memory-ranges output");
#else
			const std::size_t chunk = std::min<std::size_t>(size,
					static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
			const ssize_t written = ::write(fd, data, chunk);
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0)
				throw std::system_error(errno, std::generic_category(),
						"cannot write memory-ranges output");
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
					"cannot seek memory-ranges output");
#else
		if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot seek memory-ranges output");
#endif
	}

	void flush()
	{
#ifdef _WIN32
		if (!FlushFileBuffers(handle))
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
					"cannot flush memory-ranges output");
#else
		if (::fsync(fd) != 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot flush memory-ranges output");
#endif
	}

private:
#ifdef _WIN32
	HANDLE handle = INVALID_HANDLE_VALUE;
#else
	int fd = -1;
#endif
};

MemoryRangesArtifactWriter::MemoryRangesArtifactWriter(const std::filesystem::path& path,
		const Sha256Digest& identityDigest, const MemoryRangesManifest& manifest,
		std::uint64_t maximumBytes)
	: path(path), identityDigest(identityDigest), manifest(manifest), maximumBytes(maximumBytes)
{
	if (maximumBytes < MemoryRangesArtifactHeaderSize)
		throw std::invalid_argument("memory-ranges size limit is smaller than the header");
	if (requiredArtifactBytes(manifest) > maximumBytes)
		throw std::runtime_error("memory-ranges artifact exceeds the configured size limit");
	summary.identityDigest = identityDigest;
	summary.manifestDigest = manifest.digest;
	output = std::make_unique<OutputFile>(path);
	const std::vector<std::uint8_t> header = serializeHeader(summary, false);
	output->write(header.data(), header.size());
	output->flush();
}

MemoryRangesArtifactWriter::~MemoryRangesArtifactWriter()
{
	if (!finalized)
		abandon();
}

void MemoryRangesArtifactWriter::ensureWritable() const
{
	if (finalized)
		throw std::logic_error("memory-ranges artifact is already finalized");
	if (abandoned || output == nullptr)
		throw std::logic_error("memory-ranges artifact has been abandoned");
}

void MemoryRangesArtifactWriter::writePayload(const void *data, std::size_t size)
{
	output->write(data, size);
	payloadHasher.update(data, size);
	summary.payloadBytes = checkedAdd(summary.payloadBytes, size,
			"memory-ranges payload byte count");
}

void MemoryRangesArtifactWriter::writeEventHeader(std::uint32_t type, std::uint32_t size)
{
	std::vector<std::uint8_t> header;
	header.reserve(EventHeaderSize);
	appendU32(header, type);
	appendU32(header, size);
	appendU64(header, nextEventOrdinal++);
	require(header.size() == EventHeaderSize, "internal event header size mismatch");
	writePayload(header.data(), header.size());
	++summary.eventCount;
}

void MemoryRangesArtifactWriter::capture(std::uint32_t triggerPc, std::uint64_t tick,
		const std::vector<MemoryRangeView>& ranges)
{
	ensureWritable();
	if (captured)
		throw std::logic_error("memory-ranges snapshot was already captured");
	if (triggerPc < manifest.triggerStart || triggerPc >= manifest.triggerEndExclusive)
		throw std::invalid_argument("memory-ranges trigger PC is outside the manifest interval");
	if (ranges.size() != manifest.ranges.size())
		throw std::invalid_argument("memory-ranges view count does not match the manifest");
	for (std::size_t index = 0; index < ranges.size(); ++index)
		if (ranges[index].data == nullptr || ranges[index].size != manifest.ranges[index].length)
			throw std::invalid_argument("memory-ranges view does not match its declared length");

	writeEventHeader(TriggerEventType, EventHeaderSize + TriggerPayloadSize);
	std::vector<std::uint8_t> trigger;
	trigger.reserve(TriggerPayloadSize);
	appendU64(trigger, tick);
	appendU32(trigger, triggerPc);
	appendU32(trigger, 0);
	writePayload(trigger.data(), trigger.size());
	++summary.triggerCount;
	summary.triggerPc = triggerPc;
	summary.startTick = tick;
	summary.endTick = tick;

	for (std::size_t index = 0; index < ranges.size(); ++index)
	{
		const MemoryRangeDefinition& definition = manifest.ranges[index];
		const MemoryRangeView& view = ranges[index];
		const std::uint64_t eventSize64 = EventHeaderSize + RangeFixedPayloadSize
				+ definition.id.size() + view.size;
		if (eventSize64 > std::numeric_limits<std::uint32_t>::max())
			throw std::overflow_error("memory-ranges event size overflow");
		writeEventHeader(RangeEventType, static_cast<std::uint32_t>(eventSize64));
		const Sha256Digest observedDigest = sha256(view.data, view.size);
		std::vector<std::uint8_t> fixed;
		fixed.reserve(RangeFixedPayloadSize);
		appendU64(fixed, tick);
		appendU32(fixed, static_cast<std::uint32_t>(index));
		appendU32(fixed, definition.address);
		appendU32(fixed, definition.length);
		appendU16(fixed, static_cast<std::uint16_t>(definition.id.size()));
		appendU16(fixed, 0);
		fixed.insert(fixed.end(), observedDigest.begin(), observedDigest.end());
		require(fixed.size() == RangeFixedPayloadSize,
				"internal range payload size mismatch");
		writePayload(fixed.data(), fixed.size());
		writePayload(definition.id.data(), definition.id.size());
		writePayload(view.data, view.size);
		++summary.rangeEventCount;
		summary.totalMemoryBytes = checkedAdd(summary.totalMemoryBytes, view.size,
				"memory-ranges captured byte count");
	}
	output->flush();
	captured = true;
}

MemoryRangesArtifactSummary MemoryRangesArtifactWriter::finalize()
{
	ensureWritable();
	if (!captured || summary.triggerCount != 1
			|| summary.rangeEventCount != manifest.ranges.size())
		throw std::logic_error("cannot finalize an incomplete memory-ranges snapshot");
	summary.payloadDigest = payloadHasher.finalize();
	const std::vector<std::uint8_t> header = serializeHeader(summary, true);
	output->seek(0);
	output->write(header.data(), header.size());
	output->flush();
	finalized = true;
	return summary;
}

void MemoryRangesArtifactWriter::abandon() noexcept
{
	if (finalized || abandoned)
		return;
	abandoned = true;
	output.reset();
}

MemoryRangesArtifactSummary validateProductionMemoryRangesArtifactFile(
		const std::filesystem::path& path, const IdentityManifest& identity,
		const MemoryRangesManifest& manifest, std::uint64_t maximumBytes)
{
	requireMemoryRangesIdentity(manifest, identity);
	const std::uint64_t sizeBefore = artifactFileSize(path);
	require(sizeBefore <= maximumBytes, "file exceeds size limit");
	require(sizeBefore >= MemoryRangesArtifactHeaderSize, "file is smaller than the header");
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open memory-ranges artifact for reading: "
				+ path.string());

	std::array<std::uint8_t, MemoryRangesArtifactHeaderSize> headerBytes {};
	readExact(input, headerBytes.data(), headerBytes.size(), "truncated artifact header");
	MemoryRangesArtifactSummary summary = parseHeader(headerBytes.data(), identity, manifest);
	require(summary.payloadBytes == sizeBefore - MemoryRangesArtifactHeaderSize,
			"payload byte count mismatch");
	require(summary.droppedEvents == 0, "dropped event count is nonzero");

	Sha256 payloadHasher;
	std::array<std::uint8_t, EventHeaderSize> eventHeaderBytes {};
	std::array<std::uint8_t, RangeFixedPayloadSize> fixedBytes {};
	std::array<std::uint8_t, IoWindowBytes> window {};
	std::uint64_t remaining = summary.payloadBytes;
	std::uint64_t eventOrdinal = 0;
	std::uint64_t triggerCount = 0;
	std::uint64_t rangeCount = 0;
	std::uint64_t totalMemoryBytes = 0;
	std::uint64_t triggerTick = 0;
	std::uint32_t triggerPc = 0;
	while (remaining != 0)
	{
		require(remaining >= EventHeaderSize, "truncated event header");
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

		if (type == TriggerEventType)
		{
			require(triggerCount == 0 && rangeCount == 0,
					"trigger event is missing, duplicated, or out of order");
			require(eventSize == EventHeaderSize + TriggerPayloadSize,
					"trigger event size mismatch");
			std::array<std::uint8_t, TriggerPayloadSize> triggerBytes {};
			readExact(input, triggerBytes.data(), triggerBytes.size(),
					"truncated trigger payload");
			payloadHasher.update(triggerBytes.data(), triggerBytes.size());
			ByteReader trigger(triggerBytes.data(), triggerBytes.size());
			triggerTick = trigger.u64();
			triggerPc = trigger.u32();
			require(trigger.u32() == 0, "trigger reserved field is nonzero");
			require(trigger.remaining() == 0, "internal trigger parser mismatch");
			require(triggerPc >= manifest.triggerStart
					&& triggerPc < manifest.triggerEndExclusive,
					"trigger PC is outside the manifest interval");
			++triggerCount;
		}
		else if (type == RangeEventType)
		{
			require(triggerCount == 1, "range event precedes the trigger");
			require(rangeCount < manifest.ranges.size(), "artifact has too many range events");
			require(eventSize >= EventHeaderSize + RangeFixedPayloadSize,
					"range event is smaller than its fixed payload");
			readExact(input, fixedBytes.data(), fixedBytes.size(),
					"truncated range fixed payload");
			payloadHasher.update(fixedBytes.data(), fixedBytes.size());
			ByteReader fixed(fixedBytes.data(), fixedBytes.size());
			const std::uint64_t tick = fixed.u64();
			const std::uint32_t rangeIndex = fixed.u32();
			const std::uint32_t address = fixed.u32();
			const std::uint32_t length = fixed.u32();
			const std::uint16_t idLength = fixed.u16();
			require(fixed.u16() == 0, "range reserved field is nonzero");
			Sha256Digest recordedDigest {};
			const std::vector<std::uint8_t> digestBytes = fixed.byteVector(32);
			std::copy(digestBytes.begin(), digestBytes.end(), recordedDigest.begin());
			require(fixed.remaining() == 0, "internal range parser mismatch");

			const MemoryRangeDefinition& definition = manifest.ranges[rangeCount];
			require(tick == triggerTick, "range tick differs from the trigger boundary");
			require(rangeIndex == rangeCount, "range index is not contiguous");
			require(address == definition.address, "range address does not match manifest");
			require(length == definition.length, "range length does not match manifest");
			require(idLength == definition.id.size(), "range id length does not match manifest");
			const std::uint64_t expectedEventSize = EventHeaderSize + RangeFixedPayloadSize
					+ idLength + static_cast<std::uint64_t>(length);
			require(eventSize == expectedEventSize, "range event size mismatch");
			std::vector<std::uint8_t> idBytes(idLength);
			readExact(input, idBytes.data(), idBytes.size(), "truncated range id");
			payloadHasher.update(idBytes.data(), idBytes.size());
			require(std::string(idBytes.begin(), idBytes.end()) == definition.id,
					"range id does not match manifest");

			Sha256 rangeHasher;
			std::uint64_t dataRemaining = length;
			while (dataRemaining != 0)
			{
				const std::size_t count = static_cast<std::size_t>(
						std::min<std::uint64_t>(dataRemaining, window.size()));
				readExact(input, window.data(), count, "truncated range bytes");
				payloadHasher.update(window.data(), count);
				rangeHasher.update(window.data(), count);
				dataRemaining -= count;
			}
			const Sha256Digest computedDigest = rangeHasher.finalize();
			require(sha256Equal(computedDigest, recordedDigest),
					"recorded range SHA-256 does not match its bytes");
			require(sha256Equal(computedDigest, definition.expectedDigest),
					"range SHA-256 does not match manifest expectation");
			totalMemoryBytes = checkedAdd(totalMemoryBytes, length,
					"validated memory byte count");
			++rangeCount;
		}
		else
		{
			invalid("unknown event type");
		}
		remaining -= eventSize;
	}

	char extra = 0;
	input.read(&extra, 1);
	require(input.gcount() == 0, "file grew while reading");
	require(sizeBefore == artifactFileSize(path), "file size changed while reading");
	require(sha256Equal(summary.payloadDigest, payloadHasher.finalize()),
			"payload SHA-256 mismatch");
	require(triggerCount == 1 && summary.triggerCount == triggerCount,
			"trigger count mismatch");
	require(rangeCount == manifest.ranges.size() && summary.rangeEventCount == rangeCount,
			"range event count mismatch");
	require(summary.eventCount == eventOrdinal
			&& summary.eventCount == triggerCount + rangeCount,
			"event count mismatch");
	require(summary.totalMemoryBytes == totalMemoryBytes,
			"captured memory byte count mismatch");
	require(summary.startTick == triggerTick && summary.endTick == triggerTick,
			"header tick does not match the single snapshot boundary");
	summary.triggerPc = triggerPc;
	return summary;
}

} // namespace research

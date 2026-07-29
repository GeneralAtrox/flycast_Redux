#include "research/maple_trace.h"

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

constexpr std::array<std::uint8_t, 8> MapleTraceMagic {
	'F', 'C', 'R', 'M', 'A', 'P', 'L', 'E',
};
constexpr std::uint32_t HeaderComplete = 1 << 0;
constexpr std::uint32_t KnownHeaderFlags = HeaderComplete;
constexpr std::uint32_t EventHeaderSize = 16;
constexpr std::uint32_t MaximumEventSize = 4096;

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid Maple research trace: " + reason);
}

void require(bool condition, const std::string& reason)
{
	if (!condition)
		invalid(reason);
}

void appendU8(std::vector<std::uint8_t>& bytes, std::uint8_t value)
{
	bytes.push_back(value);
}

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value));
	bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i)
		bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

void appendS32(std::vector<std::uint8_t>& bytes, std::int32_t value)
{
	appendU32(bytes, static_cast<std::uint32_t>(value));
}

void appendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
	for (unsigned i = 0; i < 8; ++i)
		bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

void writeU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value)
{
	require(offset <= bytes.size() && bytes.size() - offset >= 4,
			"internal header offset is out of range");
	for (unsigned i = 0; i < 4; ++i)
		bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
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
		for (unsigned i = 0; i < 4; ++i)
			value |= static_cast<std::uint32_t>(data[position + i]) << (i * 8);
		position += 4;
		return value;
	}

	std::int32_t s32()
	{
		return static_cast<std::int32_t>(u32());
	}

	std::uint64_t u64()
	{
		need(8);
		std::uint64_t value = 0;
		for (unsigned i = 0; i < 8; ++i)
			value |= static_cast<std::uint64_t>(data[position + i]) << (i * 8);
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

	void skip(std::size_t count)
	{
		need(count);
		position += count;
	}

	std::size_t remaining() const { return size - position; }
	std::size_t offset() const { return position; }

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
	for (std::size_t i = 0; i < size; ++i)
	{
		crc ^= data[i];
		for (unsigned bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

std::vector<std::uint8_t> serializeHeader(const MapleTraceSummary& summary, bool complete)
{
	std::vector<std::uint8_t> header;
	header.reserve(MapleTraceHeaderSize);
	header.insert(header.end(), MapleTraceMagic.begin(), MapleTraceMagic.end());
	appendU32(header, MapleTraceSchemaVersion);
	appendU32(header, MapleTraceHeaderSize);
	appendU32(header, MapleTraceEndianSentinel);
	appendU32(header, complete ? HeaderComplete : 0);
	appendU64(header, summary.eventCount);
	appendU64(header, summary.transactionCount);
	appendU64(header, summary.dmaCount);
	appendU64(header, summary.startTick);
	appendU64(header, summary.endTick);
	appendU64(header, summary.payloadBytes);
	appendU64(header, summary.droppedEvents);
	header.insert(header.end(), summary.identityDigest.begin(), summary.identityDigest.end());
	header.insert(header.end(), summary.payloadDigest.begin(), summary.payloadDigest.end());
	header.insert(header.end(), 12, 0);
	appendU32(header, 0);
	require(header.size() == MapleTraceHeaderSize, "internal header size mismatch");
	writeU32(header, 156, crc32(header.data(), 156));
	return header;
}

std::uint64_t eventTick(const MapleTraceEvent& event)
{
	return std::visit([](const auto& value) { return value.tick; }, event.data);
}

std::uint32_t responseFrameSize(const std::vector<std::uint8_t>& response)
{
	if (response.empty())
		return 0;
	return (static_cast<std::uint32_t>(response[3]) + 1) * 4;
}

void validateTransactionShape(const MapleTransactionEvent& event)
{
	require(event.bus <= 3, "transaction bus is out of range");
	require(event.port <= 5, "transaction port is out of range");
	require((event.flags & ~MapleTransactionDevicePresent) == 0,
			"transaction has unknown flags");
	require(!event.request.empty() && event.request.size() <= 1024
			&& event.request.size() % 4 == 0,
			"transaction request size is invalid");
	require(event.response.size() <= 1024 && event.response.size() % 4 == 0,
			"transaction response size is invalid");
	require(event.request[0] == event.command,
			"transaction command does not match request frame");
	require(((event.descriptorHeader1 & 0xffu) + 1u) * 4u == event.request.size(),
			"transaction request length does not match descriptor");
	require((static_cast<std::uint32_t>(event.request[3]) + 1u) * 4u
				== event.request.size(),
			"transaction request frame length is inconsistent");
	require(((event.descriptorHeader1 >> 8) & 7u) == 0,
			"transaction descriptor is not a START operation");
	require(((event.descriptorHeader1 >> 16) & 3u) == event.bus,
			"transaction bus does not match descriptor");
	require((event.descriptorHeader2 & 0x1fffffe0u) == event.destinationAddress,
			"transaction destination does not match descriptor");
	require(event.destinationAddress != 0 && (event.destinationAddress & 31u) == 0,
			"transaction destination is invalid");
	require((event.descriptorAddress & 3u) == 0,
			"transaction descriptor address is unaligned");
	const bool present = (event.flags & MapleTransactionDevicePresent) != 0;
	require(present == (event.deviceType != UINT32_MAX),
			"transaction device presence and type disagree");
	if (present && !event.response.empty())
		require(responseFrameSize(event.response) == event.response.size(),
				"transaction response frame length is inconsistent");
}

class ProductionTraceValidator
{
	public:
	void consume(const MapleTraceEvent& event)
	{
		require(event.ordinal == parsedEventCount, "event ordinal is not contiguous");
		const std::uint64_t tick = eventTick(event);
		if (!firstTick)
			require(tick >= previousTick, "event tick regressed");
		else
		{
			startTick = tick;
			firstTick = false;
		}
		previousTick = tick;
		++parsedEventCount;

		switch (event.type)
		{
		case MapleTraceEventType::DmaBegin:
		{
			const auto& value = std::get<MapleDmaBeginEvent>(event.data);
			require(!openDma && pending.empty(), "DMA began before prior DMA committed");
			require(value.dmaOrdinal == expectedDma++, "DMA ordinal is not contiguous");
			require(value.trigger == MapleDmaTrigger::Software
					|| value.trigger == MapleDmaTrigger::VBlank,
					"DMA trigger is invalid");
			require(value.mden == 1 && value.mdst == 1, "DMA begin register state is invalid");
			require(value.mmsel <= 1 && value.swapMsb == (value.mmsel == 0),
					"DMA byte-order state is inconsistent");
			require((value.descriptorAddress & 31u) == 0,
					"DMA descriptor table is not 32-byte aligned");
			openDma = true;
			currentDma = value.dmaOrdinal;
			currentResponses = 0;
			currentInputWireBytes = 0;
			currentOutputWireBytes = 0;
			++parsedDmaCount;
			break;
		}
		case MapleTraceEventType::Transaction:
		{
			const auto& value = std::get<MapleTransactionEvent>(event.data);
			require(openDma && value.dmaOrdinal == currentDma,
					"transaction is outside its DMA");
			require(value.transactionOrdinal == expectedTransaction++,
					"transaction ordinal is not contiguous");
			validateTransactionShape(value);
			if ((value.flags & MapleTransactionDevicePresent) != 0)
			{
				require(value.request.size() <= UINT32_MAX - 3u
						&& currentInputWireBytes <= UINT32_MAX - value.request.size() - 3u,
						"DMA input wire-byte count overflow");
				require(value.response.size() <= UINT32_MAX - 3u
						&& currentOutputWireBytes <= UINT32_MAX - value.response.size() - 3u,
						"DMA output wire-byte count overflow");
				currentInputWireBytes += static_cast<std::uint32_t>(value.request.size()) + 3u;
				currentOutputWireBytes += static_cast<std::uint32_t>(value.response.size()) + 3u;
			}
			++currentResponses;
			++parsedTransactionCount;
			break;
		}
		case MapleTraceEventType::DmaSchedule:
		{
			const auto& value = std::get<MapleDmaScheduleEvent>(event.data);
			require(openDma && value.dmaOrdinal == currentDma,
					"DMA schedule does not close the active DMA");
			require(value.responseCount == currentResponses,
					"DMA schedule response count mismatch");
			require(value.inputWireBytes == currentInputWireBytes
					&& value.outputWireBytes == currentOutputWireBytes,
					"DMA schedule wire-byte counts mismatch");
			require((value.flags & ~MapleScheduleDeferredUntilVBlank) == 0,
					"DMA schedule has unknown flags");
			if ((value.flags & MapleScheduleDeferredUntilVBlank) != 0)
				require(value.scheduledCycles == 0,
						"deferred DMA must not have scheduled cycles");
			pending.emplace_back(currentDma, currentResponses);
			openDma = false;
			currentDma = UINT64_MAX;
			break;
		}
		case MapleTraceEventType::DmaCommit:
		{
			const auto& value = std::get<MapleDmaCommitEvent>(event.data);
			require(!openDma && !pending.empty(), "DMA commit has no pending DMA");
			require(value.dmaOrdinal == pending.front().first,
					"DMA commit ordinal mismatch");
			require(value.responseCount == pending.front().second,
					"DMA commit response count mismatch");
			require(value.flags == MapleCommitInterruptRaised,
					"production DMA commit must raise exactly the DMA interrupt");
			pending.pop_front();
			break;
		}
		case MapleTraceEventType::DmaAbort:
			invalid("production trace contains a DMA abort");
		default:
			invalid("unknown event type");
		}
	}

	void finish(const MapleTraceSummary& summary) const
	{
		require(summary.droppedEvents == 0, "dropped event count is nonzero");
		require(summary.eventCount == parsedEventCount, "header event count mismatch");
		require(parsedEventCount != 0, "trace has no events");
		require(!openDma, "trace ends with an open DMA");
		require(pending.empty(), "trace ends with an uncommitted DMA");
		require(parsedDmaCount != 0, "trace contains no DMA");
		require(parsedTransactionCount != 0, "trace contains no transactions");
		require(summary.dmaCount == parsedDmaCount, "header DMA count mismatch");
		require(summary.transactionCount == parsedTransactionCount,
				"header transaction count mismatch");
		require(summary.startTick == startTick, "header start tick mismatch");
		require(summary.endTick == previousTick, "header end tick mismatch");
	}

private:
	std::uint64_t parsedEventCount = 0;
	std::uint64_t expectedDma = 0;
	std::uint64_t expectedTransaction = 0;
	std::uint64_t parsedDmaCount = 0;
	std::uint64_t parsedTransactionCount = 0;
	std::uint64_t startTick = 0;
	std::uint64_t previousTick = 0;
	bool firstTick = true;
	bool openDma = false;
	std::uint64_t currentDma = UINT64_MAX;
	std::uint32_t currentResponses = 0;
	std::uint32_t currentInputWireBytes = 0;
	std::uint32_t currentOutputWireBytes = 0;
	std::deque<std::pair<std::uint64_t, std::uint32_t>> pending;
};

void validateProductionTrace(const MapleTrace& trace)
{
	ProductionTraceValidator validator;
	for (const MapleTraceEvent& event : trace.events)
		validator.consume(event);
	validator.finish(trace.summary);
}

MapleTraceEvent parseEvent(MapleTraceEventType type, std::uint64_t ordinal,
		const std::uint8_t *payload, std::size_t payloadSize)
{
	ByteReader reader(payload, payloadSize);
	MapleTraceEvent event;
	event.type = type;
	event.ordinal = ordinal;

	switch (type)
	{
	case MapleTraceEventType::DmaBegin:
	{
		require(payloadSize == 36, "DMA begin payload size is invalid");
		MapleDmaBeginEvent value;
		value.dmaOrdinal = reader.u64();
		value.tick = reader.u64();
		value.descriptorAddress = reader.u32();
		value.mden = reader.u32();
		value.mdst = reader.u32();
		value.mmsel = reader.u32();
		value.trigger = static_cast<MapleDmaTrigger>(reader.u8());
		value.swapMsb = reader.u8() != 0;
		require(reader.u16() == 0, "DMA begin reserved field is nonzero");
		event.data = value;
		break;
	}
	case MapleTraceEventType::Transaction:
	{
		require(payloadSize >= 56, "transaction payload is too small");
		MapleTransactionEvent value;
		value.dmaOrdinal = reader.u64();
		value.transactionOrdinal = reader.u64();
		value.tick = reader.u64();
		value.descriptorAddress = reader.u32();
		value.destinationAddress = reader.u32();
		value.descriptorHeader1 = reader.u32();
		value.descriptorHeader2 = reader.u32();
		value.deviceType = reader.u32();
		const std::uint32_t requestSize = reader.u32();
		const std::uint32_t responseSize = reader.u32();
		value.bus = reader.u8();
		value.port = reader.u8();
		value.command = reader.u8();
		value.flags = reader.u8();
		require(static_cast<std::uint64_t>(requestSize) + responseSize == reader.remaining(),
				"transaction byte lengths do not match event size");
		value.request = reader.byteVector(requestSize);
		value.response = reader.byteVector(responseSize);
		event.data = std::move(value);
		break;
	}
	case MapleTraceEventType::DmaSchedule:
	{
		require(payloadSize == 40, "DMA schedule payload size is invalid");
		MapleDmaScheduleEvent value;
		value.dmaOrdinal = reader.u64();
		value.tick = reader.u64();
		value.inputWireBytes = reader.u32();
		value.outputWireBytes = reader.u32();
		value.scheduledCycles = reader.u32();
		value.responseCount = reader.u32();
		value.flags = reader.u32();
		require(reader.u32() == 0, "DMA schedule reserved field is nonzero");
		event.data = value;
		break;
	}
	case MapleTraceEventType::DmaCommit:
	{
		require(payloadSize == 32, "DMA commit payload size is invalid");
		MapleDmaCommitEvent value;
		value.dmaOrdinal = reader.u64();
		value.tick = reader.u64();
		value.callbackCycles = reader.s32();
		value.jitter = reader.s32();
		value.responseCount = reader.u32();
		value.flags = reader.u32();
		event.data = value;
		break;
	}
	case MapleTraceEventType::DmaAbort:
	{
		require(payloadSize == 24, "DMA abort payload size is invalid");
		MapleDmaAbortEvent value;
		value.dmaOrdinal = reader.u64();
		value.tick = reader.u64();
		value.reason = static_cast<MapleDmaAbortReason>(reader.u32());
		value.stage = reader.u32();
		require(static_cast<std::uint32_t>(value.reason)
				>= static_cast<std::uint32_t>(MapleDmaAbortReason::InvalidDescriptor)
				&& static_cast<std::uint32_t>(value.reason)
						<= static_cast<std::uint32_t>(
								MapleDmaAbortReason::UnsupportedDescriptor),
				"DMA abort reason is invalid");
		event.data = value;
		break;
	}
	default:
		invalid("unknown event type");
	}
	require(reader.remaining() == 0, "event contains trailing bytes");
	return event;
}

MapleTraceSummary parseTraceHeader(const std::uint8_t *bytes,
		const Sha256Digest& expectedIdentity)
{
	ByteReader header(bytes, MapleTraceHeaderSize);
	const std::vector<std::uint8_t> magic = header.byteVector(MapleTraceMagic.size());
	require(std::equal(magic.begin(), magic.end(), MapleTraceMagic.begin()), "magic mismatch");
	require(header.u32() == MapleTraceSchemaVersion, "unsupported schema version");
	require(header.u32() == MapleTraceHeaderSize, "header size mismatch");
	require(header.u32() == MapleTraceEndianSentinel, "endian sentinel mismatch");
	const std::uint32_t flags = header.u32();
	require((flags & ~KnownHeaderFlags) == 0, "unknown header flags");
	require((flags & HeaderComplete) != 0, "trace is incomplete");

	MapleTraceSummary summary;
	summary.eventCount = header.u64();
	summary.transactionCount = header.u64();
	summary.dmaCount = header.u64();
	summary.startTick = header.u64();
	summary.endTick = header.u64();
	summary.payloadBytes = header.u64();
	summary.droppedEvents = header.u64();
	const std::vector<std::uint8_t> identity = header.byteVector(32);
	std::copy(identity.begin(), identity.end(), summary.identityDigest.begin());
	const std::vector<std::uint8_t> payloadDigest = header.byteVector(32);
	std::copy(payloadDigest.begin(), payloadDigest.end(), summary.payloadDigest.begin());
	for (unsigned i = 0; i < 12; ++i)
		require(header.u8() == 0, "header reserved field is nonzero");
	const std::uint32_t storedHeaderCrc = header.u32();
	require(header.remaining() == 0, "internal header parser mismatch");
	require(storedHeaderCrc == crc32(bytes, 156), "header CRC mismatch");
	require(sha256Equal(summary.identityDigest, expectedIdentity),
			"identity manifest digest mismatch");
	return summary;
}

std::uint64_t traceFileSize(const std::filesystem::path& path)
{
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error)
		throw std::runtime_error("cannot stat Maple trace '" + path.string()
				+ "': " + error.message());
	if (size > std::numeric_limits<std::uint64_t>::max())
		throw std::runtime_error("Maple trace is too large: " + path.string());
	return static_cast<std::uint64_t>(size);
}

void readTraceExact(std::ifstream& input, std::uint8_t *destination, std::size_t count,
		const std::string& truncatedReason)
{
	if (count == 0)
		return;
	input.read(reinterpret_cast<char *>(destination), static_cast<std::streamsize>(count));
	require(input.gcount() == static_cast<std::streamsize>(count) && input.good(),
			truncatedReason);
}

} // namespace

class MapleTraceWriter::OutputFile
{
public:
	explicit OutputFile(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::filesystem::path parent = path.parent_path();
		if (parent.empty() || !std::filesystem::is_directory(parent, error) || error)
			throw std::runtime_error("Maple trace output parent is not a directory: " + parent.string());
#ifdef _WIN32
		handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
				nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
					"cannot exclusively create Maple trace output");
#else
		fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot exclusively create Maple trace output");
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

	void write(const std::uint8_t *data, std::size_t size)
	{
		while (size != 0)
		{
#ifdef _WIN32
			const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size,
					(std::numeric_limits<DWORD>::max)()));
			DWORD written = 0;
			if (!WriteFile(handle, data, chunk, &written, nullptr) || written == 0)
				throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
						"cannot write Maple trace output");
#else
			const std::size_t chunk = std::min<std::size_t>(size,
					static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
			const ssize_t written = ::write(fd, data, chunk);
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0)
				throw std::system_error(errno, std::generic_category(),
						"cannot write Maple trace output");
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
					"cannot seek Maple trace output");
#else
		if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot seek Maple trace output");
#endif
	}

	void flush()
	{
#ifdef _WIN32
		if (!FlushFileBuffers(handle))
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
					"cannot flush Maple trace output");
#else
		if (::fsync(fd) != 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot flush Maple trace output");
#endif
	}

private:
#ifdef _WIN32
	HANDLE handle = INVALID_HANDLE_VALUE;
#else
	int fd = -1;
#endif
};

MapleTrace loadProductionMapleTrace(const std::filesystem::path& path,
		const Sha256Digest& expectedIdentity, std::uint64_t maximumBytes)
{
	const std::vector<std::uint8_t> bytes = readFileExact(path, maximumBytes);
	require(bytes.size() >= MapleTraceHeaderSize, "file is smaller than the header");
	MapleTrace trace;
	trace.summary = parseTraceHeader(bytes.data(), expectedIdentity);
	require(trace.summary.payloadBytes == bytes.size() - MapleTraceHeaderSize,
			"payload byte count mismatch");
	const Sha256Digest computedPayload = sha256(bytes.data() + MapleTraceHeaderSize,
			bytes.size() - MapleTraceHeaderSize);
	require(sha256Equal(trace.summary.payloadDigest, computedPayload), "payload SHA-256 mismatch");

	std::size_t offset = MapleTraceHeaderSize;
	trace.events.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
			trace.summary.eventCount, trace.summary.payloadBytes / EventHeaderSize)));
	while (offset < bytes.size())
	{
		require(bytes.size() - offset >= EventHeaderSize, "truncated event header");
		ByteReader eventHeader(bytes.data() + offset, EventHeaderSize);
		const auto type = static_cast<MapleTraceEventType>(eventHeader.u32());
		const std::uint32_t eventSize = eventHeader.u32();
		const std::uint64_t ordinal = eventHeader.u64();
		require(eventSize >= EventHeaderSize && eventSize <= MaximumEventSize,
				"event size is out of range");
		require(eventSize <= bytes.size() - offset, "event extends beyond payload");
		trace.events.push_back(parseEvent(type, ordinal, bytes.data() + offset + EventHeaderSize,
				eventSize - EventHeaderSize));
		offset += eventSize;
	}
	require(offset == bytes.size(), "payload contains trailing bytes");
	validateProductionTrace(trace);
	return trace;
}

MapleTraceSummary validateProductionMapleTraceFile(const std::filesystem::path& path,
		const Sha256Digest& expectedIdentity, std::uint64_t maximumBytes)
{
	const std::uint64_t sizeBefore = traceFileSize(path);
	require(sizeBefore <= maximumBytes, "file exceeds size limit");
	require(sizeBefore >= MapleTraceHeaderSize, "file is smaller than the header");

	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open Maple trace for reading: " + path.string());
	std::array<std::uint8_t, MapleTraceHeaderSize> headerBytes {};
	readTraceExact(input, headerBytes.data(), headerBytes.size(), "truncated trace header");
	MapleTraceSummary summary = parseTraceHeader(headerBytes.data(), expectedIdentity);
	require(summary.payloadBytes == sizeBefore - MapleTraceHeaderSize,
			"payload byte count mismatch");

	Sha256 payloadHasher;
	ProductionTraceValidator validator;
	std::array<std::uint8_t, EventHeaderSize> eventHeaderBytes {};
	std::array<std::uint8_t, MaximumEventSize - EventHeaderSize> eventPayload {};
	std::uint64_t remaining = summary.payloadBytes;
	while (remaining != 0)
	{
		require(remaining >= EventHeaderSize, "truncated event header");
		readTraceExact(input, eventHeaderBytes.data(), eventHeaderBytes.size(),
				"truncated event header");
		payloadHasher.update(eventHeaderBytes.data(), eventHeaderBytes.size());
		ByteReader eventHeader(eventHeaderBytes.data(), eventHeaderBytes.size());
		const auto type = static_cast<MapleTraceEventType>(eventHeader.u32());
		const std::uint32_t eventSize = eventHeader.u32();
		const std::uint64_t ordinal = eventHeader.u64();
		require(eventSize >= EventHeaderSize && eventSize <= MaximumEventSize,
				"event size is out of range");
		require(eventSize <= remaining, "event extends beyond payload");
		const std::size_t payloadSize = eventSize - EventHeaderSize;
		readTraceExact(input, eventPayload.data(), payloadSize, "truncated event payload");
		payloadHasher.update(eventPayload.data(), payloadSize);
		validator.consume(parseEvent(type, ordinal, eventPayload.data(), payloadSize));
		remaining -= eventSize;
	}

	char extra = 0;
	input.read(&extra, 1);
	require(input.gcount() == 0, "file grew while reading");
	const std::uint64_t sizeAfter = traceFileSize(path);
	require(sizeBefore == sizeAfter, "file size changed while reading");
	require(sha256Equal(summary.payloadDigest, payloadHasher.finalize()),
			"payload SHA-256 mismatch");
	validator.finish(summary);
	return summary;
}

MapleTraceWriter::MapleTraceWriter(const std::filesystem::path& path,
		const Sha256Digest& identityDigest, std::uint64_t maximumBytes)
	: path(path), identityDigest(identityDigest), maximumBytes(maximumBytes)
{
	if (maximumBytes < MapleTraceHeaderSize)
		throw std::invalid_argument("Maple trace size limit is smaller than the header");
	output = std::make_unique<OutputFile>(path);
	summary.identityDigest = identityDigest;
	const std::vector<std::uint8_t> header = serializeHeader(summary, false);
	output->write(header.data(), header.size());
	output->flush();
}

MapleTraceWriter::~MapleTraceWriter()
{
	if (!finalized)
		abandon();
}

void MapleTraceWriter::ensureWritable() const
{
	if (finalized)
		throw std::logic_error("Maple trace is already finalized");
	if (abandoned || output == nullptr)
		throw std::logic_error("Maple trace has been abandoned");
}

void MapleTraceWriter::appendEvent(MapleTraceEventType type, std::uint64_t tick,
		const std::vector<std::uint8_t>& payload)
{
	ensureWritable();
	if (payload.size() > MaximumEventSize - EventHeaderSize)
		throw std::runtime_error("Maple trace event exceeds the schema size limit");
	if (summary.payloadBytes > UINT64_MAX - EventHeaderSize - payload.size())
		throw std::overflow_error("Maple trace payload byte count overflow");
	const std::uint64_t eventBytes = EventHeaderSize + payload.size();
	if (summary.payloadBytes > maximumBytes - MapleTraceHeaderSize
			|| eventBytes > maximumBytes - MapleTraceHeaderSize - summary.payloadBytes)
		throw std::runtime_error("Maple trace exceeds the configured size limit");

	std::vector<std::uint8_t> bytes;
	bytes.reserve(EventHeaderSize + payload.size());
	appendU32(bytes, static_cast<std::uint32_t>(type));
	appendU32(bytes, static_cast<std::uint32_t>(EventHeaderSize + payload.size()));
	appendU64(bytes, nextEventOrdinal++);
	bytes.insert(bytes.end(), payload.begin(), payload.end());
	output->write(bytes.data(), bytes.size());
	output->flush();
	payloadHasher.update(bytes.data(), bytes.size());
	if (summary.eventCount == 0)
		summary.startTick = tick;
	summary.endTick = tick;
	++summary.eventCount;
	summary.payloadBytes += bytes.size();
}

std::uint64_t MapleTraceWriter::beginDma(MapleDmaBeginEvent event)
{
	ensureWritable();
	if (openDma || !pendingDmas.empty())
		throw std::logic_error("Maple DMA began before the prior DMA committed");
	if (event.trigger != MapleDmaTrigger::Software && event.trigger != MapleDmaTrigger::VBlank)
		throw std::invalid_argument("invalid Maple DMA trigger");
	if (event.mden != 1 || event.mdst != 1 || event.mmsel > 1
			|| event.swapMsb != (event.mmsel == 0) || (event.descriptorAddress & 31u) != 0)
		throw std::invalid_argument("invalid Maple DMA begin state");

	event.dmaOrdinal = nextDmaOrdinal++;
	std::vector<std::uint8_t> payload;
	payload.reserve(36);
	appendU64(payload, event.dmaOrdinal);
	appendU64(payload, event.tick);
	appendU32(payload, event.descriptorAddress);
	appendU32(payload, event.mden);
	appendU32(payload, event.mdst);
	appendU32(payload, event.mmsel);
	appendU8(payload, static_cast<std::uint8_t>(event.trigger));
	appendU8(payload, event.swapMsb ? 1 : 0);
	appendU16(payload, 0);
	appendEvent(MapleTraceEventType::DmaBegin, event.tick, payload);
	openDma = true;
	currentDmaOrdinal = event.dmaOrdinal;
	currentDmaResponses = 0;
	++summary.dmaCount;
	return event.dmaOrdinal;
}

std::uint64_t MapleTraceWriter::writeTransaction(MapleTransactionEvent event)
{
	ensureWritable();
	if (!openDma || event.dmaOrdinal != currentDmaOrdinal)
		throw std::logic_error("Maple transaction is outside the active DMA");
	event.transactionOrdinal = nextTransactionOrdinal++;
	validateTransactionShape(event);

	std::vector<std::uint8_t> payload;
	payload.reserve(56 + event.request.size() + event.response.size());
	appendU64(payload, event.dmaOrdinal);
	appendU64(payload, event.transactionOrdinal);
	appendU64(payload, event.tick);
	appendU32(payload, event.descriptorAddress);
	appendU32(payload, event.destinationAddress);
	appendU32(payload, event.descriptorHeader1);
	appendU32(payload, event.descriptorHeader2);
	appendU32(payload, event.deviceType);
	appendU32(payload, static_cast<std::uint32_t>(event.request.size()));
	appendU32(payload, static_cast<std::uint32_t>(event.response.size()));
	appendU8(payload, event.bus);
	appendU8(payload, event.port);
	appendU8(payload, event.command);
	appendU8(payload, event.flags);
	payload.insert(payload.end(), event.request.begin(), event.request.end());
	payload.insert(payload.end(), event.response.begin(), event.response.end());
	appendEvent(MapleTraceEventType::Transaction, event.tick, payload);
	++summary.transactionCount;
	++currentDmaResponses;
	return event.transactionOrdinal;
}

void MapleTraceWriter::scheduleDma(MapleDmaScheduleEvent event)
{
	ensureWritable();
	if (!openDma || event.dmaOrdinal != currentDmaOrdinal)
		throw std::logic_error("Maple DMA schedule does not match the active DMA");
	if (event.responseCount != currentDmaResponses)
		throw std::invalid_argument("Maple DMA schedule response count mismatch");
	if ((event.flags & ~MapleScheduleDeferredUntilVBlank) != 0
			|| ((event.flags & MapleScheduleDeferredUntilVBlank) != 0 && event.scheduledCycles != 0))
		throw std::invalid_argument("invalid Maple DMA schedule flags");

	std::vector<std::uint8_t> payload;
	payload.reserve(40);
	appendU64(payload, event.dmaOrdinal);
	appendU64(payload, event.tick);
	appendU32(payload, event.inputWireBytes);
	appendU32(payload, event.outputWireBytes);
	appendU32(payload, event.scheduledCycles);
	appendU32(payload, event.responseCount);
	appendU32(payload, event.flags);
	appendU32(payload, 0);
	appendEvent(MapleTraceEventType::DmaSchedule, event.tick, payload);
	pendingDmas.emplace_back(event.dmaOrdinal, event.responseCount);
	openDma = false;
	currentDmaOrdinal = UINT64_MAX;
	currentDmaResponses = 0;
}

void MapleTraceWriter::commitDma(MapleDmaCommitEvent event)
{
	ensureWritable();
	if (openDma || pendingDmas.empty() || event.dmaOrdinal != pendingDmas.front().first)
		throw std::logic_error("Maple DMA commit does not match the pending DMA");
	if (event.responseCount != pendingDmas.front().second
			|| event.flags != MapleCommitInterruptRaised)
		throw std::invalid_argument("invalid Maple DMA commit state");

	std::vector<std::uint8_t> payload;
	payload.reserve(32);
	appendU64(payload, event.dmaOrdinal);
	appendU64(payload, event.tick);
	appendS32(payload, event.callbackCycles);
	appendS32(payload, event.jitter);
	appendU32(payload, event.responseCount);
	appendU32(payload, event.flags);
	appendEvent(MapleTraceEventType::DmaCommit, event.tick, payload);
	pendingDmas.pop_front();
}

void MapleTraceWriter::abortDma(MapleDmaAbortEvent event)
{
	ensureWritable();
	if (openDma)
	{
		if (event.dmaOrdinal != currentDmaOrdinal)
			throw std::logic_error("Maple DMA abort does not match the active DMA");
		openDma = false;
		currentDmaOrdinal = UINT64_MAX;
		currentDmaResponses = 0;
	}
	else
	{
		if (pendingDmas.empty() || event.dmaOrdinal != pendingDmas.front().first)
			throw std::logic_error("Maple DMA abort does not match the pending DMA");
		pendingDmas.pop_front();
	}

	std::vector<std::uint8_t> payload;
	payload.reserve(24);
	appendU64(payload, event.dmaOrdinal);
	appendU64(payload, event.tick);
	appendU32(payload, static_cast<std::uint32_t>(event.reason));
	appendU32(payload, event.stage);
	appendEvent(MapleTraceEventType::DmaAbort, event.tick, payload);
}

MapleTraceSummary MapleTraceWriter::finalize()
{
	ensureWritable();
	if (openDma || !pendingDmas.empty())
		throw std::logic_error("cannot finalize Maple trace with active DMA work");
	if (summary.dmaCount == 0 || summary.transactionCount == 0 || summary.eventCount == 0)
		throw std::logic_error("cannot finalize an empty Maple trace");
	summary.payloadDigest = payloadHasher.finalize();
	const std::vector<std::uint8_t> header = serializeHeader(summary, true);
	output->seek(0);
	output->write(header.data(), header.size());
	output->flush();
	finalized = true;
	return summary;
}

void MapleTraceWriter::abandon() noexcept
{
	if (finalized || abandoned)
		return;
	abandoned = true;
	output.reset();
}

} // namespace research

// Private helpers shared by the Maple trace format, validator, reader and
// writer translation units. Not part of the public research API; include
// research/maple_trace.h instead.
#pragma once

#include "research/maple_trace.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace research
{
namespace maple_trace_detail
{

inline constexpr std::array<std::uint8_t, 8> MapleTraceMagic {
	'F', 'C', 'R', 'M', 'A', 'P', 'L', 'E',
};
inline constexpr std::uint32_t HeaderComplete = 1 << 0;
inline constexpr std::uint32_t KnownHeaderFlags = HeaderComplete;
inline constexpr std::uint32_t EventHeaderSize = 16;
inline constexpr std::uint32_t MaximumEventSize = 4096;

// Every structural failure is reported as a std::runtime_error carrying the
// "invalid Maple research trace: " prefix.
[[noreturn]] void invalid(const std::string& reason);
void require(bool condition, const std::string& reason);

// Little-endian scalar encoding.
void appendU8(std::vector<std::uint8_t>& bytes, std::uint8_t value);
void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value);
void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value);
void appendS32(std::vector<std::uint8_t>& bytes, std::int32_t value);
void appendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value);
void writeU32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value);

// Bounds-checked little-endian scalar decoding over a fixed byte range.
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

std::uint32_t crc32(const std::uint8_t *data, std::size_t size);

// Fixed-size (MapleTraceHeaderSize) file header encoding and decoding.
std::vector<std::uint8_t> serializeHeader(const MapleTraceSummary& summary, bool complete);
MapleTraceSummary parseTraceHeader(const std::uint8_t *bytes,
		const Sha256Digest *expectedIdentity);

// Event payload encoding (the bytes following the 16-byte event header) and
// decoding. Ordinals must already be assigned before serialization.
std::vector<std::uint8_t> serializePayload(const MapleDmaBeginEvent& event);
std::vector<std::uint8_t> serializePayload(const MapleTransactionEvent& event);
std::vector<std::uint8_t> serializePayload(const MapleControlDescriptorEvent& event);
std::vector<std::uint8_t> serializePayload(const MapleDmaScheduleEvent& event);
std::vector<std::uint8_t> serializePayload(const MapleDmaCommitEvent& event);
std::vector<std::uint8_t> serializePayload(const MapleDmaAbortEvent& event);
MapleTraceEvent parseEvent(std::uint32_t schemaVersion, MapleTraceEventType type,
		std::uint64_t ordinal, const std::uint8_t *payload, std::size_t payloadSize);

// Structural checks shared by the reader and the writer.
void validateTransactionShape(const MapleTransactionEvent& event);

// Streaming validator for a production trace: events are consumed in file
// order and finish() reconciles the running totals with the header.
class ProductionTraceValidator
{
public:
	explicit ProductionTraceValidator(std::uint32_t schemaVersion);

	void consume(const MapleTraceEvent& event);
	void finish(const MapleTraceSummary& summary) const;

private:
	std::uint32_t schemaVersion = MapleTraceSchemaVersionV1;
	std::uint64_t parsedEventCount = 0;
	std::uint64_t expectedDma = 0;
	std::uint64_t expectedTransaction = 0;
	std::uint64_t parsedDmaCount = 0;
	std::uint64_t parsedTransactionCount = 0;
	std::uint64_t parsedControlCount = 0;
	std::uint64_t expectedControl = 0;
	std::uint64_t startTick = 0;
	std::uint64_t previousTick = 0;
	bool firstTick = true;
	bool openDma = false;
	std::uint64_t currentDma = UINT64_MAX;
	std::uint32_t currentResponses = 0;
	std::uint32_t currentInputWireBytes = 0;
	std::uint32_t currentOutputWireBytes = 0;
	std::uint32_t nextDescriptorAddress = 0;
	bool descriptorTerminalSeen = false;
	std::deque<std::pair<std::uint64_t, std::uint32_t>> pending;
};

void validateProductionTrace(const MapleTrace& trace);

} // namespace maple_trace_detail
} // namespace research

// Maple trace byte format: scalar encoding, header layout, and event payload
// serialization/deserialization. Structural validation lives in
// maple_trace_validator.cpp.
#include "research/maple_trace_internal.h"

#include <algorithm>
#include <stdexcept>

namespace research
{
namespace maple_trace_detail
{

void invalid(const std::string& reason)
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
	appendU32(header, summary.schemaVersion);
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
	if (summary.schemaVersion == MapleTraceSchemaVersionV2)
	{
		appendU64(header, summary.controlDescriptorCount);
		appendU32(header, 0);
	}
	else
		header.insert(header.end(), 12, 0);
	appendU32(header, 0);
	require(header.size() == MapleTraceHeaderSize, "internal header size mismatch");
	writeU32(header, 156, crc32(header.data(), 156));
	return header;
}

MapleTraceSummary parseTraceHeader(const std::uint8_t *bytes,
		const Sha256Digest *expectedIdentity)
{
	ByteReader header(bytes, MapleTraceHeaderSize);
	const std::vector<std::uint8_t> magic = header.byteVector(MapleTraceMagic.size());
	require(std::equal(magic.begin(), magic.end(), MapleTraceMagic.begin()), "magic mismatch");
	const std::uint32_t schemaVersion = header.u32();
	require(schemaVersion == MapleTraceSchemaVersionV1
			|| schemaVersion == MapleTraceSchemaVersionV2,
			"unsupported schema version");
	require(header.u32() == MapleTraceHeaderSize, "header size mismatch");
	require(header.u32() == MapleTraceEndianSentinel, "endian sentinel mismatch");
	const std::uint32_t flags = header.u32();
	require((flags & ~KnownHeaderFlags) == 0, "unknown header flags");
	require((flags & HeaderComplete) != 0, "trace is incomplete");

	MapleTraceSummary summary;
	summary.schemaVersion = schemaVersion;
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
	if (schemaVersion == MapleTraceSchemaVersionV2)
	{
		summary.controlDescriptorCount = header.u64();
		require(header.u32() == 0, "header reserved field is nonzero");
	}
	else
	{
		for (unsigned i = 0; i < 12; ++i)
			require(header.u8() == 0, "header reserved field is nonzero");
	}
	const std::uint32_t storedHeaderCrc = header.u32();
	require(header.remaining() == 0, "internal header parser mismatch");
	require(storedHeaderCrc == crc32(bytes, 156), "header CRC mismatch");
	require(expectedIdentity == nullptr
			|| sha256Equal(summary.identityDigest, *expectedIdentity),
			"identity manifest digest mismatch");
	return summary;
}

std::vector<std::uint8_t> serializePayload(const MapleDmaBeginEvent& event)
{
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
	return payload;
}

std::vector<std::uint8_t> serializePayload(const MapleTransactionEvent& event)
{
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
	return payload;
}

std::vector<std::uint8_t> serializePayload(const MapleControlDescriptorEvent& event)
{
	std::vector<std::uint8_t> payload;
	payload.reserve(40);
	appendU64(payload, event.dmaOrdinal);
	appendU64(payload, event.controlOrdinal);
	appendU64(payload, event.tick);
	appendU32(payload, event.descriptorAddress);
	appendU32(payload, event.descriptorHeader);
	appendU8(payload, static_cast<std::uint8_t>(event.operation));
	appendU8(payload, event.last ? 1 : 0);
	appendU16(payload, 0);
	appendU32(payload, 0);
	return payload;
}

std::vector<std::uint8_t> serializePayload(const MapleDmaScheduleEvent& event)
{
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
	return payload;
}

std::vector<std::uint8_t> serializePayload(const MapleDmaCommitEvent& event)
{
	std::vector<std::uint8_t> payload;
	payload.reserve(32);
	appendU64(payload, event.dmaOrdinal);
	appendU64(payload, event.tick);
	appendS32(payload, event.callbackCycles);
	appendS32(payload, event.jitter);
	appendU32(payload, event.responseCount);
	appendU32(payload, event.flags);
	return payload;
}

std::vector<std::uint8_t> serializePayload(const MapleDmaAbortEvent& event)
{
	std::vector<std::uint8_t> payload;
	payload.reserve(24);
	appendU64(payload, event.dmaOrdinal);
	appendU64(payload, event.tick);
	appendU32(payload, static_cast<std::uint32_t>(event.reason));
	appendU32(payload, event.stage);
	return payload;
}

MapleTraceEvent parseEvent(std::uint32_t schemaVersion, MapleTraceEventType type,
		std::uint64_t ordinal, const std::uint8_t *payload, std::size_t payloadSize)
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
	case MapleTraceEventType::ControlDescriptor:
	{
		require(schemaVersion == MapleTraceSchemaVersionV2,
				"control descriptor is not permitted by this schema");
		require(payloadSize == 40, "control-descriptor payload size is invalid");
		MapleControlDescriptorEvent value;
		value.dmaOrdinal = reader.u64();
		value.controlOrdinal = reader.u64();
		value.tick = reader.u64();
		value.descriptorAddress = reader.u32();
		value.descriptorHeader = reader.u32();
		value.operation = static_cast<MapleControlOperation>(reader.u8());
		value.last = reader.u8() != 0;
		require(reader.u16() == 0 && reader.u32() == 0,
				"control-descriptor reserved field is nonzero");
		event.data = value;
		break;
	}
	default:
		invalid("unknown event type");
	}
	require(reader.remaining() == 0, "event contains trailing bytes");
	return event;
}

} // namespace maple_trace_detail
} // namespace research

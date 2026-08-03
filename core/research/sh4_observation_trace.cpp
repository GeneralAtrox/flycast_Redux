#include "research/sh4_observation_trace.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
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

constexpr std::array<std::uint8_t, 8> TraceMagic {
	'F', 'C', 'S', 'H', '4', 'O', 'B', '1',
};
constexpr std::uint32_t HeaderComplete = 1u << 0;
constexpr std::uint32_t KnownHeaderFlags = HeaderComplete;
constexpr std::uint32_t InstructionFields = Sh4Observation::HasNextPc
		| Sh4Observation::HasRegisters;

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid SH-4 observation trace: " + reason);
}

void require(bool condition, const std::string& reason)
{
	if (!condition)
		invalid(reason);
}

bool validBackend(Sh4ObservationBackend backend)
{
	return backend == Sh4ObservationBackend::Interpreter
			|| backend == Sh4ObservationBackend::Dynarec;
}

bool validType(Sh4ObservationType type)
{
	return type >= Sh4ObservationType::InstructionBegin
			&& type <= Sh4ObservationType::Return;
}

bool hasInstructionSnapshot(Sh4ObservationType type)
{
	return type == Sh4ObservationType::InstructionBegin
			|| type == Sh4ObservationType::InstructionEnd
			|| type == Sh4ObservationType::Exception
			|| type == Sh4ObservationType::Call
			|| type == Sh4ObservationType::Return;
}

bool isMemory(Sh4ObservationType type)
{
	return type == Sh4ObservationType::MemoryRead
			|| type == Sh4ObservationType::MemoryWrite;
}

bool validMemoryWidth(std::uint8_t width)
{
	return width == 1 || width == 2 || width == 4 || width == 8;
}

std::uint64_t widthMask(std::uint8_t width)
{
	return width == 8 ? std::numeric_limits<std::uint64_t>::max()
			: (std::uint64_t {1} << (width * 8)) - 1;
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

void appendZeros(std::vector<std::uint8_t>& bytes, std::size_t count)
{
	bytes.insert(bytes.end(), count, 0);
}

void writeU32(std::vector<std::uint8_t>& bytes, std::size_t offset,
		std::uint32_t value)
{
	require(offset <= bytes.size() && bytes.size() - offset >= 4,
			"internal header offset is out of range");
	for (unsigned index = 0; index < 4; ++index)
		bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
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
		const std::uint16_t result = static_cast<std::uint16_t>(data[position])
				| static_cast<std::uint16_t>(data[position + 1] << 8);
		position += 2;
		return result;
	}

	std::uint32_t u32()
	{
		need(4);
		std::uint32_t result = 0;
		for (unsigned index = 0; index < 4; ++index)
			result |= static_cast<std::uint32_t>(data[position + index]) << (index * 8);
		position += 4;
		return result;
	}

	std::uint64_t u64()
	{
		need(8);
		std::uint64_t result = 0;
		for (unsigned index = 0; index < 8; ++index)
			result |= static_cast<std::uint64_t>(data[position + index]) << (index * 8);
		position += 8;
		return result;
	}

	void bytes(void *destination, std::size_t count)
	{
		need(count);
		std::memcpy(destination, data + position, count);
		position += count;
	}

	std::size_t remaining() const { return size - position; }

private:
	void need(std::size_t count) const
	{
		if (position > size || count > size - position)
			invalid("truncated scalar");
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

bool allZero(const Sh4RegisterSnapshot& registers)
{
	return std::all_of(registers.r.begin(), registers.r.end(),
			[](std::uint32_t value) { return value == 0; })
			&& registers.pr == 0 && registers.gbr == 0 && registers.vbr == 0
			&& registers.mach == 0 && registers.macl == 0 && registers.sr == 0
			&& registers.fpul == 0 && registers.fpscr == 0;
}

bool equalRegisters(const Sh4RegisterSnapshot& lhs,
		const Sh4RegisterSnapshot& rhs)
{
	return lhs.r == rhs.r && lhs.pr == rhs.pr && lhs.gbr == rhs.gbr
			&& lhs.vbr == rhs.vbr && lhs.mach == rhs.mach
			&& lhs.macl == rhs.macl && lhs.sr == rhs.sr
			&& lhs.fpul == rhs.fpul && lhs.fpscr == rhs.fpscr;
}

std::vector<std::uint8_t> serializeHeader(const Sh4ObservationTraceSummary& summary,
		bool complete)
{
	std::vector<std::uint8_t> header;
	header.reserve(Sh4ObservationTraceHeaderSize);
	header.insert(header.end(), TraceMagic.begin(), TraceMagic.end());
	appendU32(header, Sh4ObservationTraceSchemaVersion);
	appendU32(header, Sh4ObservationTraceHeaderSize);
	appendU32(header, Sh4ObservationTraceEndianSentinel);
	appendU32(header, complete ? HeaderComplete : 0);
	appendU32(header, static_cast<std::uint32_t>(summary.binding.backend));
	appendU32(header, Sh4ObservationTraceEventSize);
	appendU64(header, summary.eventCount);
	appendU64(header, summary.payloadBytes);
	appendU64(header, summary.droppedEvents);
	appendU64(header, summary.startTick);
	appendU64(header, summary.endTick);
	header.insert(header.end(), summary.binding.identityDigest.begin(),
			summary.binding.identityDigest.end());
	header.insert(header.end(), summary.binding.replayDigest.begin(),
			summary.binding.replayDigest.end());
	header.insert(header.end(), summary.binding.manifestSetDigest.begin(),
			summary.binding.manifestSetDigest.end());
	header.insert(header.end(), summary.payloadDigest.begin(), summary.payloadDigest.end());
	appendU32(header, 0);
	appendU32(header, 0);
	require(header.size() == Sh4ObservationTraceHeaderSize,
			"internal header size mismatch");
	writeU32(header, Sh4ObservationTraceHeaderSize - 4,
			crc32(header.data(), Sh4ObservationTraceHeaderSize - 4));
	return header;
}

void validateBinding(const Sh4ObservationTraceBinding& binding)
{
	if (!validBackend(binding.backend))
		throw std::invalid_argument("SH-4 observation trace backend is invalid");
}

void validateObservation(const Sh4Observation& observation,
		Sh4ObservationBackend backend)
{
	if (observation.schemaVersion != Sh4ObservationSchemaVersion)
		throw std::invalid_argument("unsupported SH-4 observation schema version");
	if (observation.backend != backend)
		throw std::invalid_argument("SH-4 observation backend differs from trace binding");
	if (!validType(observation.type))
		throw std::invalid_argument("SH-4 observation type is invalid");
	const std::uint32_t expectedFields = hasInstructionSnapshot(observation.type)
			? InstructionFields : 0;
	if (observation.availableFields != expectedFields)
		throw std::invalid_argument("SH-4 observation availability mask is noncanonical");
	if (isMemory(observation.type))
	{
		if (!validMemoryWidth(observation.memoryWidth))
			throw std::invalid_argument("SH-4 observation memory width is invalid");
		if (static_cast<std::uint64_t>(observation.memoryAddress)
				+ observation.memoryWidth > (std::uint64_t {1} << 32))
			throw std::invalid_argument("SH-4 observation memory range wraps");
		if ((observation.memoryValue & ~widthMask(observation.memoryWidth)) != 0)
			throw std::invalid_argument("SH-4 observation memory value exceeds its width");
	}
	if (observation.type == Sh4ObservationType::Call
			&& observation.callKind != Sh4CallKind::Bsr
			&& observation.callKind != Sh4CallKind::Bsrf
			&& observation.callKind != Sh4CallKind::Jsr)
		throw std::invalid_argument("SH-4 observation call kind is invalid");
}

std::vector<std::uint8_t> serializeEvent(const Sh4Observation& observation,
		std::uint64_t ordinal)
{
	std::vector<std::uint8_t> bytes;
	bytes.reserve(Sh4ObservationTraceEventSize);
	appendU32(bytes, static_cast<std::uint32_t>(observation.type));
	appendU32(bytes, Sh4ObservationTraceEventSize);
	appendU64(bytes, ordinal);
	appendU64(bytes, observation.tick);
	appendU32(bytes, observation.instructionPc);
	appendU32(bytes, (observation.availableFields & Sh4Observation::HasNextPc) != 0
			? observation.nextPc : 0);
	appendU16(bytes, observation.opcode);
	appendU16(bytes, observation.delaySlotDepth);
	appendU32(bytes, observation.availableFields);
	if ((observation.availableFields & Sh4Observation::HasRegisters) != 0)
		appendRegisters(bytes, observation.registers);
	else
		appendZeros(bytes, 96);
	if (isMemory(observation.type))
	{
		appendU32(bytes, observation.memoryAddress);
		bytes.push_back(observation.memoryWidth);
	}
	else
	{
		appendU32(bytes, 0);
		bytes.push_back(0);
	}
	appendZeros(bytes, 3);
	appendU64(bytes, isMemory(observation.type) ? observation.memoryValue : 0);
	if (observation.type == Sh4ObservationType::Exception)
	{
		appendU32(bytes, observation.exceptionPc);
		appendU32(bytes, observation.vectorPc);
		appendU32(bytes, observation.exceptionCode);
	}
	else
		appendZeros(bytes, 12);
	appendU16(bytes, observation.type == Sh4ObservationType::Call
			? static_cast<std::uint16_t>(observation.callKind) : 0);
	appendU16(bytes, 0);
	const bool controlFlow = observation.type == Sh4ObservationType::Call
			|| observation.type == Sh4ObservationType::Return;
	appendU32(bytes, controlFlow ? observation.targetPc : 0);
	appendU32(bytes, controlFlow ? observation.returnPc : 0);
	appendU32(bytes, controlFlow ? observation.delaySlotPc : 0);
	appendU32(bytes, 0);
	require(bytes.size() == Sh4ObservationTraceEventSize,
			"internal event size mismatch");
	return bytes;
}

struct ParsedEvent
{
	Sh4ObservationType type = Sh4ObservationType::InstructionBegin;
	std::uint64_t ordinal = 0;
	std::uint64_t tick = 0;
	std::uint32_t instructionPc = 0;
	std::uint32_t nextPc = 0;
	std::uint16_t opcode = 0;
	std::uint16_t delaySlotDepth = 0;
	std::uint32_t availableFields = 0;
	Sh4RegisterSnapshot registers;
	std::uint32_t memoryAddress = 0;
	std::uint8_t memoryWidth = 0;
	std::uint64_t memoryValue = 0;
	std::uint32_t exceptionPc = 0;
	std::uint32_t vectorPc = 0;
	std::uint32_t exceptionCode = 0;
	std::uint16_t callKind = 0;
	std::uint32_t targetPc = 0;
	std::uint32_t returnPc = 0;
	std::uint32_t delaySlotPc = 0;
};

ParsedEvent parseEvent(const std::uint8_t *bytes)
{
	ByteReader reader(bytes, Sh4ObservationTraceEventSize);
	ParsedEvent event;
	event.type = static_cast<Sh4ObservationType>(reader.u32());
	require(validType(event.type), "event type is invalid");
	require(reader.u32() == Sh4ObservationTraceEventSize, "event size mismatch");
	event.ordinal = reader.u64();
	event.tick = reader.u64();
	event.instructionPc = reader.u32();
	event.nextPc = reader.u32();
	event.opcode = reader.u16();
	event.delaySlotDepth = reader.u16();
	event.availableFields = reader.u32();
	event.registers = readRegisters(reader);
	event.memoryAddress = reader.u32();
	event.memoryWidth = reader.u8();
	require(reader.u8() == 0 && reader.u8() == 0 && reader.u8() == 0,
			"event memory reserved field is nonzero");
	event.memoryValue = reader.u64();
	event.exceptionPc = reader.u32();
	event.vectorPc = reader.u32();
	event.exceptionCode = reader.u32();
	event.callKind = reader.u16();
	require(reader.u16() == 0, "event control-flow reserved field is nonzero");
	event.targetPc = reader.u32();
	event.returnPc = reader.u32();
	event.delaySlotPc = reader.u32();
	require(reader.u32() == 0, "event trailing reserved field is nonzero");
	require(reader.remaining() == 0, "internal event parser mismatch");
	return event;
}

void validateCanonicalEvent(const ParsedEvent& event)
{
	const std::uint32_t expectedFields = hasInstructionSnapshot(event.type)
			? InstructionFields : 0;
	require(event.availableFields == expectedFields,
			"event availability mask is noncanonical");
	if ((expectedFields & Sh4Observation::HasNextPc) == 0)
		require(event.nextPc == 0, "unavailable next PC is nonzero");
	if ((expectedFields & Sh4Observation::HasRegisters) == 0)
		require(allZero(event.registers), "unavailable register snapshot is nonzero");

	if (isMemory(event.type))
	{
		require(validMemoryWidth(event.memoryWidth), "memory width is invalid");
		require(static_cast<std::uint64_t>(event.memoryAddress) + event.memoryWidth
				<= (std::uint64_t {1} << 32), "memory range wraps");
		require((event.memoryValue & ~widthMask(event.memoryWidth)) == 0,
				"memory value exceeds its width");
	}
	else
		require(event.memoryAddress == 0 && event.memoryWidth == 0
				&& event.memoryValue == 0, "non-memory event has memory fields");

	if (event.type != Sh4ObservationType::Exception)
		require(event.exceptionPc == 0 && event.vectorPc == 0
				&& event.exceptionCode == 0, "non-exception event has exception fields");

	if (event.type == Sh4ObservationType::Call)
		require(event.callKind >= static_cast<std::uint16_t>(Sh4CallKind::Bsr)
				&& event.callKind <= static_cast<std::uint16_t>(Sh4CallKind::Jsr),
				"call kind is invalid");
	else
		require(event.callKind == 0, "non-call event has a call kind");

	if (event.type != Sh4ObservationType::Call
			&& event.type != Sh4ObservationType::Return)
		require(event.targetPc == 0 && event.returnPc == 0
				&& event.delaySlotPc == 0,
				"non-control-flow event has control-flow fields");
}

Sh4ObservationTraceSummary parseHeader(const std::uint8_t *bytes,
		const Sh4ObservationTraceBinding& expectedBinding)
{
	ByteReader reader(bytes, Sh4ObservationTraceHeaderSize);
	std::array<std::uint8_t, TraceMagic.size()> magic {};
	reader.bytes(magic.data(), magic.size());
	require(magic == TraceMagic, "magic mismatch");
	require(reader.u32() == Sh4ObservationTraceSchemaVersion,
			"unsupported schema version");
	require(reader.u32() == Sh4ObservationTraceHeaderSize, "header size mismatch");
	require(reader.u32() == Sh4ObservationTraceEndianSentinel,
			"endian sentinel mismatch");
	const std::uint32_t flags = reader.u32();
	require((flags & ~KnownHeaderFlags) == 0, "unknown header flags");
	require((flags & HeaderComplete) != 0, "trace is incomplete");

	Sh4ObservationTraceSummary summary;
	summary.binding.backend = static_cast<Sh4ObservationBackend>(reader.u32());
	require(validBackend(summary.binding.backend), "backend is invalid");
	require(reader.u32() == Sh4ObservationTraceEventSize, "event size mismatch");
	summary.eventCount = reader.u64();
	summary.payloadBytes = reader.u64();
	summary.droppedEvents = reader.u64();
	summary.startTick = reader.u64();
	summary.endTick = reader.u64();
	reader.bytes(summary.binding.identityDigest.data(),
			summary.binding.identityDigest.size());
	reader.bytes(summary.binding.replayDigest.data(), summary.binding.replayDigest.size());
	reader.bytes(summary.binding.manifestSetDigest.data(),
			summary.binding.manifestSetDigest.size());
	reader.bytes(summary.payloadDigest.data(), summary.payloadDigest.size());
	require(reader.u32() == 0, "header reserved field is nonzero");
	const std::uint32_t storedCrc = reader.u32();
	require(reader.remaining() == 0, "internal header parser mismatch");
	require(storedCrc == crc32(bytes, Sh4ObservationTraceHeaderSize - 4),
			"header CRC mismatch");
	require(summary.binding.backend == expectedBinding.backend,
			"backend binding mismatch");
	require(sha256Equal(summary.binding.identityDigest,
			expectedBinding.identityDigest), "identity binding mismatch");
	require(sha256Equal(summary.binding.replayDigest, expectedBinding.replayDigest),
			"replay binding mismatch");
	require(sha256Equal(summary.binding.manifestSetDigest,
			expectedBinding.manifestSetDigest), "manifest-set binding mismatch");
	return summary;
}

std::uint64_t fileSize(const std::filesystem::path& path)
{
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error)
		throw std::runtime_error("cannot stat SH-4 observation trace '"
				+ path.string() + "': " + error.message());
	if (size > std::numeric_limits<std::uint64_t>::max())
		throw std::runtime_error("SH-4 observation trace is too large: " + path.string());
	return static_cast<std::uint64_t>(size);
}

void readExact(std::ifstream& input, void *destination, std::size_t size,
		const char *reason)
{
	input.read(static_cast<char *>(destination), static_cast<std::streamsize>(size));
	if (input.gcount() != static_cast<std::streamsize>(size))
		invalid(reason);
}

struct InstructionFrame
{
	std::uint32_t pc = 0;
	std::uint16_t opcode = 0;
	std::uint16_t depth = 0;
	std::uint32_t pr = 0;
	std::uint64_t beginTick = 0;
	std::uint32_t beginNextPc = 0;
	Sh4RegisterSnapshot beginRegisters;
	bool callSeen = false;
	bool returnSeen = false;
	bool delaySlotSeen = false;
	bool exceptionSeen = false;
	std::uint64_t returnTick = 0;
	std::uint32_t returnNextPc = 0;
	Sh4RegisterSnapshot returnRegisters;
};

bool executesDelaySlot(const InstructionFrame& frame)
{
	const std::uint16_t opcode = frame.opcode;
	if ((opcode & 0xff00u) == 0x8d00u)
		return (frame.beginRegisters.sr & 1u) != 0;
	if ((opcode & 0xff00u) == 0x8f00u)
		return (frame.beginRegisters.sr & 1u) == 0;
	return (opcode & 0xf000u) == 0xa000u
			|| (opcode & 0xf000u) == 0xb000u
			|| (opcode & 0xf0ffu) == 0x0003u
			|| (opcode & 0xf0ffu) == 0x0023u
			|| (opcode & 0xf0ffu) == 0x400bu
			|| (opcode & 0xf0ffu) == 0x402bu
			|| opcode == 0x000bu || opcode == 0x002bu;
}

bool isCallInstruction(const InstructionFrame& frame)
{
	Sh4InstructionState state;
	state.pc = frame.pc;
	state.opcode = frame.opcode;
	state.registers = frame.beginRegisters;
	Sh4CallKind kind = Sh4CallKind::Bsr;
	std::uint32_t target = 0;
	return decodeSh4Call(state, kind, target);
}

void validateSequenceEvent(const ParsedEvent& event,
		std::vector<InstructionFrame>& frames)
{
	if (event.type == Sh4ObservationType::InstructionBegin)
	{
		require(event.delaySlotDepth == frames.size(),
				"instruction-begin depth is not contiguous");
		if (!frames.empty())
		{
			InstructionFrame& owner = frames.back();
			require(executesDelaySlot(owner) && !owner.delaySlotSeen
					&& event.instructionPc == owner.pc + 2u,
					"nested instruction is not the owner's delay slot");
			owner.delaySlotSeen = true;
		}
		frames.push_back(InstructionFrame {event.instructionPc, event.opcode,
				event.delaySlotDepth, event.registers.pr, event.tick, event.nextPc,
				event.registers, false, false, false, false, 0, 0, {}});
		return;
	}

	// Interrupt-like exception observations may occur at a scheduler boundary
	// without an active guest instruction. All other event kinds are owned by
	// the innermost instruction frame.
	if (frames.empty())
	{
		require(event.type == Sh4ObservationType::Exception
				&& event.delaySlotDepth == 0,
				"event has no owning instruction");
		return;
	}

	InstructionFrame& frame = frames.back();
	require(event.instructionPc == frame.pc && event.opcode == frame.opcode
			&& event.delaySlotDepth == frame.depth,
			"event does not match its owning instruction");
	if (event.type == Sh4ObservationType::Call)
	{
		require(!frame.callSeen, "instruction has duplicate call events");
		require(isCallInstruction(frame), "call event opcode is not a call");
		Sh4InstructionState state;
		state.pc = event.instructionPc;
		state.opcode = event.opcode;
		state.registers = event.registers;
		Sh4CallKind decodedKind = Sh4CallKind::Bsr;
		std::uint32_t decodedTarget = 0;
		require(decodeSh4Call(state, decodedKind, decodedTarget),
				"call event cannot be decoded");
		require(event.callKind == static_cast<std::uint16_t>(decodedKind)
				&& event.targetPc == decodedTarget
				&& event.returnPc == event.instructionPc + 4u
				&& event.delaySlotPc == event.instructionPc + 2u
				&& event.tick == frame.beginTick
				&& event.nextPc == frame.beginNextPc
				&& equalRegisters(event.registers, frame.beginRegisters),
				"call event disagrees with SH-4 semantics");
		frame.callSeen = true;
	}
	else if (event.type == Sh4ObservationType::Return)
	{
		require(!frame.returnSeen && frame.opcode == 0x000bu,
				"return event opcode is not RTS or is duplicated");
		require(event.targetPc == frame.pr && event.nextPc == frame.pr
				&& event.returnPc == frame.pr
				&& event.delaySlotPc == event.instructionPc + 2u,
				"return event disagrees with SH-4 semantics");
		frame.returnSeen = true;
		frame.returnTick = event.tick;
		frame.returnNextPc = event.nextPc;
		frame.returnRegisters = event.registers;
	}
	else if (event.type == Sh4ObservationType::Exception)
	{
		// An exception in a delay slot aborts both the nested instruction and
		// its delayed-control-flow owner.
		for (InstructionFrame& openFrame : frames)
			openFrame.exceptionSeen = true;
	}
	else if (event.type == Sh4ObservationType::InstructionEnd
			|| event.type == Sh4ObservationType::InstructionAbort)
	{
		if (event.type == Sh4ObservationType::InstructionEnd)
		{
			require(frame.callSeen == isCallInstruction(frame),
					"call opcode is missing its call event");
			require(frame.returnSeen == (frame.opcode == 0x000bu),
					"RTS opcode is missing its return event");
			require(frame.delaySlotSeen == executesDelaySlot(frame),
					"delayed instruction is missing its delay-slot observation");
			if (frame.returnSeen)
				require(event.tick == frame.returnTick
						&& event.nextPc == frame.returnNextPc
						&& equalRegisters(event.registers, frame.returnRegisters),
						"return event differs from its instruction end");
		}
		else
		{
			require(frame.exceptionSeen,
					"instruction abort is not justified by an exception");
			require(frame.callSeen == isCallInstruction(frame),
					"aborted call opcode is missing its pre-execution call event");
		}
		frames.pop_back();
	}
}

} // namespace

class Sh4ObservationTraceWriter::OutputFile
{
public:
	explicit OutputFile(const std::filesystem::path& path)
	{
#ifdef _WIN32
		handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			throw std::system_error(static_cast<int>(GetLastError()),
					std::system_category(),
					"cannot create SH-4 observation trace exclusively");
#else
		fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot create SH-4 observation trace exclusively");
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
					std::numeric_limits<DWORD>::max()));
			DWORD written = 0;
			if (!WriteFile(handle, data, chunk, &written, nullptr) || written == 0)
				throw std::system_error(static_cast<int>(GetLastError()),
						std::system_category(),
						"cannot write SH-4 observation trace");
#else
			const std::size_t chunk = std::min<std::size_t>(size,
					static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
			const ssize_t written = ::write(fd, data, chunk);
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0)
				throw std::system_error(errno, std::generic_category(),
						"cannot write SH-4 observation trace");
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
			throw std::system_error(static_cast<int>(GetLastError()),
					std::system_category(),
					"cannot seek SH-4 observation trace");
#else
		if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot seek SH-4 observation trace");
#endif
	}

	void flush()
	{
#ifdef _WIN32
		if (!FlushFileBuffers(handle))
			throw std::system_error(static_cast<int>(GetLastError()),
					std::system_category(),
					"cannot flush SH-4 observation trace");
#else
		if (::fsync(fd) != 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot flush SH-4 observation trace");
#endif
	}

private:
#ifdef _WIN32
	HANDLE handle = INVALID_HANDLE_VALUE;
#else
	int fd = -1;
#endif
};

Sh4ObservationTraceWriter::Sh4ObservationTraceWriter(
		const std::filesystem::path& path,
		const Sh4ObservationTraceBinding& binding, std::uint64_t maximumBytes,
		std::uint64_t maximumEvents)
	: path(path), maximumBytes(maximumBytes), maximumEvents(maximumEvents)
{
	validateBinding(binding);
	if (maximumBytes < Sh4ObservationTraceHeaderSize)
		throw std::invalid_argument("SH-4 observation trace byte limit is too small");
	if (maximumEvents == 0)
		throw std::invalid_argument("SH-4 observation trace event limit is zero");
	summary.binding = binding;
	output = std::make_unique<OutputFile>(path);
	const std::vector<std::uint8_t> header = serializeHeader(summary, false);
	output->write(header.data(), header.size());
	output->flush();
}

Sh4ObservationTraceWriter::~Sh4ObservationTraceWriter()
{
	if (!finalized)
		abandon();
}

void Sh4ObservationTraceWriter::ensureWritable() const
{
	if (finalized)
		throw std::logic_error("SH-4 observation trace is already finalized");
	if (abandoned || output == nullptr)
		throw std::logic_error("SH-4 observation trace has been abandoned");
}

void Sh4ObservationTraceWriter::write(const Sh4Observation& observation)
{
	ensureWritable();
	validateObservation(observation, summary.binding.backend);
	if (summary.eventCount >= maximumEvents)
		throw std::runtime_error("SH-4 observation trace event limit exceeded");
	if (summary.payloadBytes > maximumBytes - Sh4ObservationTraceHeaderSize
			|| Sh4ObservationTraceEventSize > maximumBytes
					- Sh4ObservationTraceHeaderSize - summary.payloadBytes)
		throw std::runtime_error("SH-4 observation trace byte limit exceeded");
	if (hasEvents && observation.tick < summary.endTick)
		throw std::logic_error("SH-4 observation trace ticks are not monotonic: "
				+ std::to_string(observation.tick) + " follows "
				+ std::to_string(summary.endTick));
	if (!hasEvents)
	{
		hasEvents = true;
		summary.startTick = observation.tick;
	}
	summary.endTick = observation.tick;
	const std::vector<std::uint8_t> event = serializeEvent(observation,
			summary.eventCount);
	output->write(event.data(), event.size());
	payloadHasher.update(event.data(), event.size());
	++summary.eventCount;
	summary.payloadBytes += event.size();
}

Sh4ObservationTraceSummary Sh4ObservationTraceWriter::finalize()
{
	ensureWritable();
	if (!hasEvents)
		throw std::logic_error("cannot finalize an empty SH-4 observation trace");
	summary.payloadDigest = payloadHasher.finalize();
	// Make the payload durable before the complete bit can become durable.
	// A second flush below publishes the complete header.
	output->flush();
	const std::vector<std::uint8_t> header = serializeHeader(summary, true);
	output->seek(0);
	output->write(header.data(), header.size());
	output->flush();
	finalized = true;
	return summary;
}

void Sh4ObservationTraceWriter::abandon() noexcept
{
	if (finalized || abandoned)
		return;
	abandoned = true;
	output.reset();
}

Sh4ObservationTraceSummary validateSh4ObservationTraceFile(
		const std::filesystem::path& path,
		const Sh4ObservationTraceBinding& expectedBinding,
		std::uint64_t maximumBytes, std::uint64_t maximumEvents)
{
	validateBinding(expectedBinding);
	const std::uint64_t sizeBefore = fileSize(path);
	require(sizeBefore <= maximumBytes, "file exceeds byte limit");
	require(sizeBefore >= Sh4ObservationTraceHeaderSize,
			"file is smaller than the header");
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open SH-4 observation trace: " + path.string());
	std::array<std::uint8_t, Sh4ObservationTraceHeaderSize> headerBytes {};
	readExact(input, headerBytes.data(), headerBytes.size(), "truncated header");
	Sh4ObservationTraceSummary summary = parseHeader(headerBytes.data(), expectedBinding);
	require(summary.eventCount != 0, "trace has no events");
	require(summary.eventCount <= maximumEvents, "event count exceeds limit");
	require(summary.droppedEvents == 0, "dropped event count is nonzero");
	require(summary.eventCount <= std::numeric_limits<std::uint64_t>::max()
			/ Sh4ObservationTraceEventSize, "payload size overflows");
	require(summary.payloadBytes == summary.eventCount * Sh4ObservationTraceEventSize,
			"payload size does not match event count");
	require(summary.payloadBytes == sizeBefore - Sh4ObservationTraceHeaderSize,
			"payload size does not match file size");

	Sha256 payloadHasher;
	std::uint64_t firstTick = 0;
	std::uint64_t lastTick = 0;
	std::vector<InstructionFrame> frames;
	for (std::uint64_t ordinal = 0; ordinal < summary.eventCount; ++ordinal)
	{
		std::array<std::uint8_t, Sh4ObservationTraceEventSize> bytes {};
		readExact(input, bytes.data(), bytes.size(), "truncated event");
		payloadHasher.update(bytes.data(), bytes.size());
		const ParsedEvent event = parseEvent(bytes.data());
		require(event.ordinal == ordinal, "event ordinal is not contiguous");
		if (ordinal == 0)
			firstTick = event.tick;
		else
			require(event.tick >= lastTick, "event ticks are not monotonic");
		lastTick = event.tick;
		validateCanonicalEvent(event);
		validateSequenceEvent(event, frames);
	}
	require(frames.empty(), "trace ends with open instruction frames");
	require(firstTick == summary.startTick && lastTick == summary.endTick,
			"header tick range does not match events");
	require(sha256Equal(payloadHasher.finalize(), summary.payloadDigest),
			"payload digest mismatch");
	input.peek();
	require(input.eof(), "file has trailing data");
	require(fileSize(path) == sizeBefore, "file size changed during validation");
	return summary;
}

} // namespace research

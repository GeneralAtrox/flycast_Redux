#include "research/sh4_observation_compare.h"

#include <algorithm>
#include <array>
#include <cstring>
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

// This reader intentionally duplicates the on-disk decoder and semantic
// checks instead of calling validateSh4ObservationTraceFile(). That separation
// prevents a writer/validator bug from automatically blessing both inputs.
constexpr std::array<std::uint8_t, 8> TraceMagic {
	'F', 'C', 'S', 'H', '4', 'O', 'B', '1',
};
constexpr std::uint32_t HeaderComplete = 1u << 0;
constexpr std::uint32_t InstructionFields = Sh4Observation::HasNextPc
		| Sh4Observation::HasRegisters;

[[noreturn]] void reject(const std::filesystem::path& path,
		const std::string& reason)
{
	throw std::runtime_error("rejected SH-4 observation trace '" + path.string()
			+ "': " + reason);
}

class Decoder
{
public:
	Decoder(const std::uint8_t *bytes, std::size_t size,
			const std::filesystem::path& path)
		: bytes(bytes), size(size), path(path) {}

	std::uint8_t u8()
	{
		need(1);
		return bytes[position++];
	}

	std::uint16_t u16()
	{
		need(2);
		const std::uint16_t value = static_cast<std::uint16_t>(bytes[position])
				| static_cast<std::uint16_t>(bytes[position + 1] << 8);
		position += 2;
		return value;
	}

	std::uint32_t u32()
	{
		need(4);
		std::uint32_t value = 0;
		for (unsigned index = 0; index < 4; ++index)
			value |= static_cast<std::uint32_t>(bytes[position + index]) << (index * 8);
		position += 4;
		return value;
	}

	std::uint64_t u64()
	{
		need(8);
		std::uint64_t value = 0;
		for (unsigned index = 0; index < 8; ++index)
			value |= static_cast<std::uint64_t>(bytes[position + index]) << (index * 8);
		position += 8;
		return value;
	}

	void copy(void *destination, std::size_t count)
	{
		need(count);
		std::memcpy(destination, bytes + position, count);
		position += count;
	}

	std::size_t remaining() const { return size - position; }

private:
	void need(std::size_t count) const
	{
		if (position > size || count > size - position)
			reject(path, "truncated scalar");
	}

	const std::uint8_t *bytes;
	std::size_t size;
	const std::filesystem::path& path;
	std::size_t position = 0;
};

std::uint32_t independentCrc32(const std::uint8_t *bytes, std::size_t size)
{
	std::uint32_t crc = 0xffffffffu;
	for (std::size_t index = 0; index < size; ++index)
	{
		crc ^= bytes[index];
		for (unsigned bit = 0; bit < 8; ++bit)
		{
			const bool lowBit = (crc & 1u) != 0;
			crc >>= 1;
			if (lowBit)
				crc ^= 0xedb88320u;
		}
	}
	return crc ^ 0xffffffffu;
}

std::uint64_t stableFileSize(const std::filesystem::path& path)
{
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error)
		reject(path, "cannot stat file: " + error.message());
	if (size > std::numeric_limits<std::uint64_t>::max())
		reject(path, "file size is not representable");
	return static_cast<std::uint64_t>(size);
}

void readExact(std::ifstream& input, void *destination, std::size_t size,
		const std::filesystem::path& path, const char *what)
{
	input.read(static_cast<char *>(destination), static_cast<std::streamsize>(size));
	if (input.gcount() != static_cast<std::streamsize>(size))
		reject(path, std::string("truncated ") + what);
}

Sh4RegisterSnapshot decodeRegisters(Decoder& decoder)
{
	Sh4RegisterSnapshot registers;
	for (std::uint32_t& value : registers.r)
		value = decoder.u32();
	registers.pr = decoder.u32();
	registers.gbr = decoder.u32();
	registers.vbr = decoder.u32();
	registers.mach = decoder.u32();
	registers.macl = decoder.u32();
	registers.sr = decoder.u32();
	registers.fpul = decoder.u32();
	registers.fpscr = decoder.u32();
	return registers;
}

bool registersAreZero(const Sh4RegisterSnapshot& registers)
{
	if (std::any_of(registers.r.begin(), registers.r.end(),
			[](std::uint32_t value) { return value != 0; }))
		return false;
	return registers.pr == 0 && registers.gbr == 0 && registers.vbr == 0
			&& registers.mach == 0 && registers.macl == 0 && registers.sr == 0
			&& registers.fpul == 0 && registers.fpscr == 0;
}

bool registersEqual(const Sh4RegisterSnapshot& lhs,
		const Sh4RegisterSnapshot& rhs)
{
	return lhs.r == rhs.r && lhs.pr == rhs.pr && lhs.gbr == rhs.gbr
			&& lhs.vbr == rhs.vbr && lhs.mach == rhs.mach
			&& lhs.macl == rhs.macl && lhs.sr == rhs.sr
			&& lhs.fpul == rhs.fpul && lhs.fpscr == rhs.fpscr;
}

bool validType(std::uint32_t type)
{
	return type >= static_cast<std::uint32_t>(Sh4ObservationType::InstructionBegin)
			&& type <= static_cast<std::uint32_t>(Sh4ObservationType::Return);
}

bool snapshotType(Sh4ObservationType type)
{
	return type == Sh4ObservationType::InstructionBegin
			|| type == Sh4ObservationType::InstructionEnd
			|| type == Sh4ObservationType::Exception
			|| type == Sh4ObservationType::Call
			|| type == Sh4ObservationType::Return;
}

bool memoryType(Sh4ObservationType type)
{
	return type == Sh4ObservationType::MemoryRead
			|| type == Sh4ObservationType::MemoryWrite;
}

std::uint64_t memoryMask(std::uint8_t width)
{
	if (width == 8)
		return std::numeric_limits<std::uint64_t>::max();
	return (std::uint64_t {1} << (width * 8)) - 1;
}

struct IndependentEvent
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

struct Frame
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

bool decodeCallIndependently(const IndependentEvent& event,
		std::uint16_t& kind, std::uint32_t& target)
{
	if ((event.opcode & 0xf000u) == 0xb000u)
	{
		kind = static_cast<std::uint16_t>(Sh4CallKind::Bsr);
		const std::int32_t displacement = static_cast<std::int16_t>(
				static_cast<std::uint16_t>((event.opcode & 0x0fffu) << 4)) >> 4;
		target = event.instructionPc + 4u
				+ static_cast<std::uint32_t>(displacement * 2);
		return true;
	}
	if ((event.opcode & 0xf0ffu) == 0x0003u)
	{
		kind = static_cast<std::uint16_t>(Sh4CallKind::Bsrf);
		target = event.instructionPc + 4u
				+ event.registers.r[(event.opcode >> 8) & 0x0fu];
		return true;
	}
	if ((event.opcode & 0xf0ffu) == 0x400bu)
	{
		kind = static_cast<std::uint16_t>(Sh4CallKind::Jsr);
		target = event.registers.r[(event.opcode >> 8) & 0x0fu];
		return true;
	}
	return false;
}

bool executesDelaySlotIndependently(const Frame& frame)
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

class IndependentTraceReader
{
public:
	IndependentTraceReader(const std::filesystem::path& path,
			Sh4ObservationBackend expectedBackend,
			const Sha256Digest& expectedIdentity,
			const Sh4ObservationEquivalenceContract& contract)
		: path(path), sizeBefore(stableFileSize(path)), input(path, std::ios::binary)
	{
		if (contract.maximumTraceBytes < Sh4ObservationTraceHeaderSize)
			throw std::invalid_argument("equivalence trace byte limit is too small");
		if (contract.maximumEvents == 0)
			throw std::invalid_argument("equivalence event limit is zero");
		if (sizeBefore > contract.maximumTraceBytes)
			reject(path, "file exceeds byte limit");
		if (sizeBefore < Sh4ObservationTraceHeaderSize)
			reject(path, "file is smaller than the header");
		if (!input)
			reject(path, "cannot open file");

		std::array<std::uint8_t, Sh4ObservationTraceHeaderSize> header {};
		readExact(input, header.data(), header.size(), path, "header");
		Decoder decoder(header.data(), header.size(), path);
		std::array<std::uint8_t, TraceMagic.size()> magic {};
		decoder.copy(magic.data(), magic.size());
		if (magic != TraceMagic)
			reject(path, "magic mismatch");
		if (decoder.u32() != Sh4ObservationTraceSchemaVersion)
			reject(path, "schema version mismatch");
		if (decoder.u32() != Sh4ObservationTraceHeaderSize)
			reject(path, "header size mismatch");
		if (decoder.u32() != Sh4ObservationTraceEndianSentinel)
			reject(path, "endian sentinel mismatch");
		const std::uint32_t flags = decoder.u32();
		if (flags != HeaderComplete)
			reject(path, (flags & HeaderComplete) == 0
					? "trace is incomplete" : "header has unknown flags");
		const std::uint32_t backend = decoder.u32();
		if (backend != static_cast<std::uint32_t>(expectedBackend))
			reject(path, "backend binding mismatch");
		summary.binding.backend = expectedBackend;
		if (decoder.u32() != Sh4ObservationTraceEventSize)
			reject(path, "event size mismatch");
		summary.eventCount = decoder.u64();
		summary.payloadBytes = decoder.u64();
		summary.droppedEvents = decoder.u64();
		summary.startTick = decoder.u64();
		summary.endTick = decoder.u64();
		decoder.copy(summary.binding.identityDigest.data(),
				summary.binding.identityDigest.size());
		decoder.copy(summary.binding.replayDigest.data(),
				summary.binding.replayDigest.size());
		decoder.copy(summary.binding.manifestSetDigest.data(),
				summary.binding.manifestSetDigest.size());
		decoder.copy(summary.payloadDigest.data(), summary.payloadDigest.size());
		if (decoder.u32() != 0)
			reject(path, "header reserved field is nonzero");
		const std::uint32_t storedCrc = decoder.u32();
		if (decoder.remaining() != 0)
			reject(path, "internal header decoder mismatch");
		if (storedCrc != independentCrc32(header.data(), header.size() - 4))
			reject(path, "header CRC mismatch");
		if (!sha256Equal(summary.binding.identityDigest, expectedIdentity))
			reject(path, "identity binding mismatch");
		if (!sha256Equal(summary.binding.replayDigest, contract.replayDigest))
			reject(path, "replay binding mismatch");
		if (!sha256Equal(summary.binding.manifestSetDigest,
				contract.manifestSetDigest))
			reject(path, "manifest-set binding mismatch");
		if (summary.eventCount == 0)
			reject(path, "trace has no events");
		if (summary.eventCount > contract.maximumEvents)
			reject(path, "event count exceeds limit");
		if (summary.droppedEvents != 0)
			reject(path, "dropped event count is nonzero");
		if (summary.eventCount > std::numeric_limits<std::uint64_t>::max()
				/ Sh4ObservationTraceEventSize)
			reject(path, "payload size overflows");
		if (summary.payloadBytes != summary.eventCount * Sh4ObservationTraceEventSize)
			reject(path, "payload size does not match event count");
		if (summary.payloadBytes != sizeBefore - Sh4ObservationTraceHeaderSize)
			reject(path, "payload size does not match file size");
	}

	IndependentEvent next()
	{
		if (nextOrdinal >= summary.eventCount)
			reject(path, "reader advanced past the declared event count");
		std::array<std::uint8_t, Sh4ObservationTraceEventSize> bytes {};
		readExact(input, bytes.data(), bytes.size(), path, "event");
		payloadHasher.update(bytes.data(), bytes.size());
		Decoder decoder(bytes.data(), bytes.size(), path);
		const std::uint32_t rawType = decoder.u32();
		if (!validType(rawType))
			reject(path, "event type is invalid");
		IndependentEvent event;
		event.type = static_cast<Sh4ObservationType>(rawType);
		if (decoder.u32() != Sh4ObservationTraceEventSize)
			reject(path, "event record size mismatch");
		event.ordinal = decoder.u64();
		if (event.ordinal != nextOrdinal)
			reject(path, "event ordinal is not contiguous");
		event.tick = decoder.u64();
		event.instructionPc = decoder.u32();
		event.nextPc = decoder.u32();
		event.opcode = decoder.u16();
		event.delaySlotDepth = decoder.u16();
		event.availableFields = decoder.u32();
		event.registers = decodeRegisters(decoder);
		event.memoryAddress = decoder.u32();
		event.memoryWidth = decoder.u8();
		if (decoder.u8() != 0 || decoder.u8() != 0 || decoder.u8() != 0)
			reject(path, "memory reserved field is nonzero");
		event.memoryValue = decoder.u64();
		event.exceptionPc = decoder.u32();
		event.vectorPc = decoder.u32();
		event.exceptionCode = decoder.u32();
		event.callKind = decoder.u16();
		if (decoder.u16() != 0)
			reject(path, "control-flow reserved field is nonzero");
		event.targetPc = decoder.u32();
		event.returnPc = decoder.u32();
		event.delaySlotPc = decoder.u32();
		if (decoder.u32() != 0 || decoder.remaining() != 0)
			reject(path, "trailing reserved field is nonzero");

		validateCanonical(event);
		validateSequence(event);
		if (nextOrdinal == 0)
			observedStartTick = event.tick;
		else if (event.tick < observedEndTick)
			reject(path, "event ticks are not monotonic");
		observedEndTick = event.tick;
		++nextOrdinal;
		return event;
	}

	void finish()
	{
		if (nextOrdinal != summary.eventCount)
			reject(path, "not all declared events were read");
		if (!frames.empty())
			reject(path, "trace ends with open instruction frames");
		if (summary.startTick != observedStartTick || summary.endTick != observedEndTick)
			reject(path, "header tick range does not match events");
		if (!sha256Equal(payloadHasher.finalize(), summary.payloadDigest))
			reject(path, "payload digest mismatch");
		input.peek();
		if (!input.eof())
			reject(path, "file has trailing data");
		if (stableFileSize(path) != sizeBefore)
			reject(path, "file size changed during comparison");
	}

	const Sh4ObservationTraceSummary& getSummary() const { return summary; }

private:
	void validateCanonical(const IndependentEvent& event)
	{
		const std::uint32_t expectedFields = snapshotType(event.type)
				? InstructionFields : 0;
		if (event.availableFields != expectedFields)
			reject(path, "event availability mask is noncanonical");
		if (expectedFields == 0 && (event.nextPc != 0
				|| !registersAreZero(event.registers)))
			reject(path, "unavailable instruction fields are nonzero");
		if (memoryType(event.type))
		{
			if (event.memoryWidth != 1 && event.memoryWidth != 2
					&& event.memoryWidth != 4 && event.memoryWidth != 8)
				reject(path, "memory width is invalid");
			if (static_cast<std::uint64_t>(event.memoryAddress) + event.memoryWidth
					> (std::uint64_t {1} << 32))
				reject(path, "memory range wraps");
			if ((event.memoryValue & ~memoryMask(event.memoryWidth)) != 0)
				reject(path, "memory value exceeds its width");
		}
		else if (event.memoryAddress != 0 || event.memoryWidth != 0
				|| event.memoryValue != 0)
			reject(path, "non-memory event has memory fields");
		if (event.type != Sh4ObservationType::Exception
				&& (event.exceptionPc != 0 || event.vectorPc != 0
						|| event.exceptionCode != 0))
			reject(path, "non-exception event has exception fields");
		if (event.type == Sh4ObservationType::Call)
		{
			if (event.callKind < static_cast<std::uint16_t>(Sh4CallKind::Bsr)
					|| event.callKind > static_cast<std::uint16_t>(Sh4CallKind::Jsr))
				reject(path, "call kind is invalid");
		}
		else if (event.callKind != 0)
			reject(path, "non-call event has a call kind");
		if (event.type != Sh4ObservationType::Call
				&& event.type != Sh4ObservationType::Return
				&& (event.targetPc != 0 || event.returnPc != 0
						|| event.delaySlotPc != 0))
			reject(path, "non-control-flow event has control-flow fields");
	}

	void validateSequence(const IndependentEvent& event)
	{
		if (event.type == Sh4ObservationType::InstructionBegin)
		{
			if (event.delaySlotDepth != frames.size())
				reject(path, "instruction-begin depth is not contiguous");
			if (!frames.empty())
			{
				Frame& owner = frames.back();
				if (!executesDelaySlotIndependently(owner)
						|| owner.delaySlotSeen
						|| event.instructionPc != owner.pc + 2u)
					reject(path,
							"nested instruction is not the owner's delay slot");
				owner.delaySlotSeen = true;
			}
			frames.push_back(Frame {event.instructionPc, event.opcode,
					event.delaySlotDepth, event.registers.pr, event.tick, event.nextPc,
					event.registers, false, false, false, false, 0, 0, {}});
			return;
		}
		if (frames.empty())
		{
			if (event.type != Sh4ObservationType::Exception
					|| event.delaySlotDepth != 0)
				reject(path, "event has no owning instruction");
			return;
		}
		Frame& frame = frames.back();
		if (event.instructionPc != frame.pc || event.opcode != frame.opcode
				|| event.delaySlotDepth != frame.depth)
			reject(path, "event does not match its owning instruction");
		if (event.type == Sh4ObservationType::Call)
		{
			if (frame.callSeen)
				reject(path, "instruction has duplicate call events");
			std::uint16_t decodedKind = 0;
			std::uint32_t decodedTarget = 0;
			if (!decodeCallIndependently(event, decodedKind, decodedTarget)
					|| event.callKind != decodedKind
					|| event.targetPc != decodedTarget
					|| event.returnPc != event.instructionPc + 4u
					|| event.delaySlotPc != event.instructionPc + 2u
					|| event.tick != frame.beginTick
					|| event.nextPc != frame.beginNextPc
					|| !registersEqual(event.registers, frame.beginRegisters))
				reject(path, "call event disagrees with SH-4 semantics");
			frame.callSeen = true;
		}
		else if (event.type == Sh4ObservationType::Return)
		{
			if (frame.returnSeen || frame.opcode != 0x000bu
					|| event.targetPc != frame.pr || event.nextPc != frame.pr
					|| event.returnPc != frame.pr
					|| event.delaySlotPc != event.instructionPc + 2u)
				reject(path, "return event disagrees with SH-4 semantics");
			frame.returnSeen = true;
			frame.returnTick = event.tick;
			frame.returnNextPc = event.nextPc;
			frame.returnRegisters = event.registers;
		}
		else if (event.type == Sh4ObservationType::Exception)
		{
			for (Frame& openFrame : frames)
				openFrame.exceptionSeen = true;
		}
		else if (event.type == Sh4ObservationType::InstructionEnd
				|| event.type == Sh4ObservationType::InstructionAbort)
		{
			if (event.type == Sh4ObservationType::InstructionEnd)
			{
				IndependentEvent probe;
				probe.instructionPc = frame.pc;
				probe.opcode = frame.opcode;
				std::uint16_t unusedKind = 0;
				std::uint32_t unusedTarget = 0;
				const bool callOpcode = decodeCallIndependently(probe,
						unusedKind, unusedTarget);
				if (frame.callSeen != callOpcode)
					reject(path, "call opcode is missing its call event");
				if (frame.returnSeen != (frame.opcode == 0x000bu))
					reject(path, "RTS opcode is missing its return event");
				if (frame.delaySlotSeen
						!= executesDelaySlotIndependently(frame))
					reject(path,
							"delayed instruction is missing its delay-slot observation");
				if (frame.returnSeen && (event.tick != frame.returnTick
						|| event.nextPc != frame.returnNextPc
						|| !registersEqual(event.registers,
								frame.returnRegisters)))
					reject(path, "return event differs from its instruction end");
			}
			else
			{
				if (!frame.exceptionSeen)
					reject(path,
							"instruction abort is not justified by an exception");
				IndependentEvent probe;
				probe.instructionPc = frame.pc;
				probe.opcode = frame.opcode;
				probe.registers = frame.beginRegisters;
				std::uint16_t unusedKind = 0;
				std::uint32_t unusedTarget = 0;
				const bool callOpcode = decodeCallIndependently(probe,
						unusedKind, unusedTarget);
				if (frame.callSeen != callOpcode)
					reject(path,
							"aborted call opcode is missing its pre-execution call event");
			}
			frames.pop_back();
		}
	}

	std::filesystem::path path;
	std::uint64_t sizeBefore = 0;
	std::ifstream input;
	Sh4ObservationTraceSummary summary;
	Sha256 payloadHasher;
	std::uint64_t nextOrdinal = 0;
	std::uint64_t observedStartTick = 0;
	std::uint64_t observedEndTick = 0;
	std::vector<Frame> frames;
};

std::string decimal(std::uint64_t value)
{
	return std::to_string(value);
}

std::string hex(std::uint64_t value, unsigned width)
{
	std::ostringstream stream;
	stream << "0x" << std::hex << std::setfill('0') << std::setw(width) << value;
	return stream.str();
}

void noteDifference(std::optional<Sh4ObservationDivergence>& divergence,
		std::uint64_t ordinal, const std::string& field,
		const std::string& interpreterValue, const std::string& dynarecValue)
{
	if (!divergence.has_value())
		divergence = Sh4ObservationDivergence {ordinal, field,
				interpreterValue, dynarecValue};
}

bool compareEvent(const IndependentEvent& interpreter,
		const IndependentEvent& dynarec,
		std::optional<Sh4ObservationDivergence>& divergence)
{
	const std::uint64_t ordinal = interpreter.ordinal;
#define COMPARE_DEC(fieldName) \
	do { if (interpreter.fieldName != dynarec.fieldName) { \
		noteDifference(divergence, ordinal, #fieldName, \
				decimal(static_cast<std::uint64_t>(interpreter.fieldName)), \
				decimal(static_cast<std::uint64_t>(dynarec.fieldName))); return false; } } while (false)
#define COMPARE_HEX(fieldName, width) \
	do { if (interpreter.fieldName != dynarec.fieldName) { \
		noteDifference(divergence, ordinal, #fieldName, \
				hex(static_cast<std::uint64_t>(interpreter.fieldName), width), \
				hex(static_cast<std::uint64_t>(dynarec.fieldName), width)); return false; } } while (false)
	COMPARE_DEC(type);
	COMPARE_DEC(tick);
	COMPARE_HEX(instructionPc, 8);
	COMPARE_HEX(nextPc, 8);
	COMPARE_HEX(opcode, 4);
	COMPARE_DEC(delaySlotDepth);
	COMPARE_HEX(availableFields, 8);
	for (std::size_t index = 0; index < interpreter.registers.r.size(); ++index)
	{
		if (interpreter.registers.r[index] != dynarec.registers.r[index])
		{
			noteDifference(divergence, ordinal, "registers.r[" + std::to_string(index)
					+ "]", hex(interpreter.registers.r[index], 8),
					hex(dynarec.registers.r[index], 8));
			return false;
		}
	}
	COMPARE_HEX(registers.pr, 8);
	COMPARE_HEX(registers.gbr, 8);
	COMPARE_HEX(registers.vbr, 8);
	COMPARE_HEX(registers.mach, 8);
	COMPARE_HEX(registers.macl, 8);
	COMPARE_HEX(registers.sr, 8);
	COMPARE_HEX(registers.fpul, 8);
	COMPARE_HEX(registers.fpscr, 8);
	COMPARE_HEX(memoryAddress, 8);
	COMPARE_DEC(memoryWidth);
	COMPARE_HEX(memoryValue, 16);
	COMPARE_HEX(exceptionPc, 8);
	COMPARE_HEX(vectorPc, 8);
	COMPARE_HEX(exceptionCode, 8);
	COMPARE_DEC(callKind);
	COMPARE_HEX(targetPc, 8);
	COMPARE_HEX(returnPc, 8);
	COMPARE_HEX(delaySlotPc, 8);
#undef COMPARE_HEX
#undef COMPARE_DEC
	return true;
}

} // namespace

Sh4ObservationComparison compareSh4ObservationTraces(
		const std::filesystem::path& interpreterPath,
		const std::filesystem::path& dynarecPath,
		const Sh4ObservationEquivalenceContract& contract)
{
	IndependentTraceReader interpreter(interpreterPath,
			Sh4ObservationBackend::Interpreter,
			contract.interpreterIdentityDigest, contract);
	IndependentTraceReader dynarec(dynarecPath, Sh4ObservationBackend::Dynarec,
			contract.dynarecIdentityDigest, contract);

	Sh4ObservationComparison result;
	result.interpreter = interpreter.getSummary();
	result.dynarec = dynarec.getSummary();
	const std::uint64_t commonCount = std::min(result.interpreter.eventCount,
			result.dynarec.eventCount);
	bool prefixMatches = true;
	for (std::uint64_t ordinal = 0; ordinal < commonCount; ++ordinal)
	{
		const IndependentEvent interpreterEvent = interpreter.next();
		const IndependentEvent dynarecEvent = dynarec.next();
		if (prefixMatches && compareEvent(interpreterEvent, dynarecEvent,
				result.firstDivergence))
			++result.matchedEventCount;
		else
			prefixMatches = false;
	}
	for (std::uint64_t ordinal = commonCount;
			ordinal < result.interpreter.eventCount; ++ordinal)
		interpreter.next();
	for (std::uint64_t ordinal = commonCount;
			ordinal < result.dynarec.eventCount; ++ordinal)
		dynarec.next();
	interpreter.finish();
	dynarec.finish();

	if (!result.firstDivergence.has_value()
			&& result.interpreter.eventCount != result.dynarec.eventCount)
		noteDifference(result.firstDivergence, commonCount, "event-presence",
				commonCount < result.interpreter.eventCount ? "present" : "absent",
				commonCount < result.dynarec.eventCount ? "present" : "absent");
	result.equivalent = !result.firstDivergence.has_value();
	return result;
}

} // namespace research

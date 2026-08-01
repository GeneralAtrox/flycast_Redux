#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace research
{

constexpr std::uint32_t Sh4ObservationSchemaVersion = 1;

enum class Sh4ObservationBackend : std::uint8_t
{
	Interpreter = 1,
	Dynarec = 2,
};

constexpr std::uint32_t sh4ObservationBackendBit(Sh4ObservationBackend backend)
{
	return std::uint32_t {1} << (static_cast<unsigned>(backend) - 1u);
}

constexpr std::uint32_t AllSh4ObservationBackends =
		sh4ObservationBackendBit(Sh4ObservationBackend::Interpreter)
		| sh4ObservationBackendBit(Sh4ObservationBackend::Dynarec);

enum class Sh4ObservationType : std::uint8_t
{
	InstructionBegin = 1,
	InstructionEnd = 2,
	InstructionAbort = 3,
	MemoryRead = 4,
	MemoryWrite = 5,
	Exception = 6,
	Call = 7,
	Return = 8,
};

enum class Sh4CallKind : std::uint16_t
{
	Bsr = 1,
	Bsrf = 2,
	Jsr = 3,
};

enum class Sh4MemoryAccessKind : std::uint8_t
{
	Read = 1,
	Write = 2,
};

struct Sh4RegisterSnapshot
{
	std::array<std::uint32_t, 16> r {};
	std::uint32_t pr = 0;
	std::uint32_t gbr = 0;
	std::uint32_t vbr = 0;
	std::uint32_t mach = 0;
	std::uint32_t macl = 0;
	std::uint32_t sr = 0;
	std::uint32_t fpul = 0;
	std::uint32_t fpscr = 0;
};

struct Sh4InstructionState
{
	std::uint32_t pc = 0;
	std::uint32_t nextPc = 0;
	std::uint16_t opcode = 0;
	std::uint64_t tick = 0;
	Sh4RegisterSnapshot registers;
};

struct Sh4Observation
{
	static constexpr std::uint32_t HasNextPc = 1u << 0;
	static constexpr std::uint32_t HasRegisters = 1u << 1;

	std::uint32_t schemaVersion = Sh4ObservationSchemaVersion;
	std::uint32_t availableFields = 0;
	std::uint64_t emissionOrdinal = 0;
	Sh4ObservationBackend backend = Sh4ObservationBackend::Interpreter;
	Sh4ObservationType type = Sh4ObservationType::InstructionBegin;
	std::uint64_t tick = 0;
	std::uint32_t instructionPc = 0;
	std::uint32_t nextPc = 0;
	std::uint16_t opcode = 0;
	std::uint16_t delaySlotDepth = 0;
	Sh4RegisterSnapshot registers;
	std::uint32_t memoryAddress = 0;
	std::uint8_t memoryWidth = 0;
	std::uint64_t memoryValue = 0;
	std::uint32_t exceptionPc = 0;
	std::uint32_t vectorPc = 0;
	std::uint32_t exceptionCode = 0;
	Sh4CallKind callKind = Sh4CallKind::Bsr;
	std::uint32_t targetPc = 0;
	std::uint32_t returnPc = 0;
	std::uint32_t delaySlotPc = 0;
};

constexpr std::uint64_t sh4ObservationTypeBit(Sh4ObservationType type)
{
	return std::uint64_t {1} << (static_cast<unsigned>(type) - 1u);
}

constexpr std::uint64_t AllSh4ObservationTypes =
		sh4ObservationTypeBit(Sh4ObservationType::InstructionBegin)
		| sh4ObservationTypeBit(Sh4ObservationType::InstructionEnd)
		| sh4ObservationTypeBit(Sh4ObservationType::InstructionAbort)
		| sh4ObservationTypeBit(Sh4ObservationType::MemoryRead)
		| sh4ObservationTypeBit(Sh4ObservationType::MemoryWrite)
		| sh4ObservationTypeBit(Sh4ObservationType::Exception)
		| sh4ObservationTypeBit(Sh4ObservationType::Call)
		| sh4ObservationTypeBit(Sh4ObservationType::Return);

struct Sh4ObservationFilter
{
	std::uint32_t backendMask = AllSh4ObservationBackends;
	std::uint64_t typeMask = AllSh4ObservationTypes;
	bool hasInstructionPcRange = false;
	std::uint32_t instructionPcStart = 0;
	std::uint64_t instructionPcEndExclusive = std::uint64_t {1} << 32;
	bool hasMemoryRange = false;
	std::uint32_t memoryStart = 0;
	std::uint64_t memoryEndExclusive = std::uint64_t {1} << 32;
};

using Sh4ObservationSubscription = std::uint64_t;
using Sh4ObservationCallback = std::function<void(const Sh4Observation&)>;

Sh4ObservationSubscription subscribeSh4Observations(
		const Sh4ObservationFilter& filter, Sh4ObservationCallback callback);
bool unsubscribeSh4Observations(Sh4ObservationSubscription subscription) noexcept;
bool sh4ObservationBusActive() noexcept;
bool sh4ObservationBusActive(Sh4ObservationBackend backend) noexcept;
std::size_t sh4ObservationSubscriberCount() noexcept;
std::size_t sh4ObservationSubscriberCount(Sh4ObservationBackend backend) noexcept;
// Changes whenever the matching backend's subscription set changes. Runtime
// emitters use it to avoid publishing a partial instruction to a replacement
// subscriber that did not observe the instruction begin.
std::uint64_t sh4ObservationSubscriptionGeneration(
		Sh4ObservationBackend backend) noexcept;

// Assigns the process-wide emission ordinal and synchronously invokes matching
// native subscribers. Subscriber exceptions propagate to the observation owner.
// Publication is serialized; observation producers must not publish recursively.
bool publishSh4Observation(Sh4Observation observation);

bool decodeSh4Call(const Sh4InstructionState& state, Sh4CallKind& kind,
		std::uint32_t& targetPc);

} // namespace research

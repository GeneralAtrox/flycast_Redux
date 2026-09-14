#pragma once

#include "research/sh4_observation.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct Sh4Context;

namespace research
{

struct Sh4InstructionOwnerToken
{
	bool valid = false;
	Sh4ObservationBackend backend = Sh4ObservationBackend::Interpreter;
	std::uint64_t generation = 0;
	std::uint64_t tick = 0;
	std::uint32_t pc = 0;
	std::uint32_t pr = 0;
	std::uint16_t opcode = 0;
	std::uint16_t delaySlotDepth = 0;
};

constexpr std::size_t Sh4DynarecTimingDiagnosticHistoryCapacity = 256;

enum class Sh4DynarecTimingDiagnosticState : std::uint8_t
{
	Open = 0,
	Completed = 1,
	Exception = 2,
	Interrupt = 3,
};

struct Sh4DynarecTimingDiagnosticRecord
{
	std::uint64_t sequence = 0;
	Sh4DynarecTimingDiagnosticState state =
			Sh4DynarecTimingDiagnosticState::Open;
	std::uint32_t pc = 0;
	std::uint32_t nextPc = 0;
	std::uint32_t boundaryCode = 0;
	std::uint16_t opcode = 0;
	std::uint16_t depth = 0;
	bool precise = false;
	std::int32_t instructionCycles = 0;
	std::int64_t cycleCounterBegin = 0;
	std::int64_t cycleCounterEnd = 0;
	std::uint64_t schedulerTickBegin = 0;
	std::uint64_t schedulerTickEnd = 0;
	std::uint64_t executionTickBegin = 0;
	std::uint64_t executionTickEnd = 0;
};

struct Sh4DynarecTimingDiagnosticSnapshot
{
	bool active = false;
	std::uint64_t zeroBasedDmaOrdinal = 0;
	std::uint64_t observedTick = 0;
	std::uint64_t expectedTick = 0;
	std::uint64_t nextSequence = 0;
	std::vector<Sh4DynarecTimingDiagnosticRecord> records;
};

#ifdef LIBRETRO

// Research SH-4 instruction emission is intentionally unavailable in libretro
// builds. The generic observation bus remains a usable in-process API, so its
// subscription state must not be interpreted as enabling these no-op hooks.
inline Sh4RegisterSnapshot snapshotSh4Registers(const Sh4Context&) { return {}; }
inline void sh4ObservationInstructionBegin(Sh4ObservationBackend, std::uint32_t,
		std::uint16_t, std::uint64_t, const Sh4Context&) {}
inline void sh4ObservationInstructionEnd(Sh4ObservationBackend, std::uint32_t,
		std::uint16_t, std::uint64_t, const Sh4Context&) {}
inline void sh4ObservationInstructionAbort(Sh4ObservationBackend) noexcept {}
inline void sh4ObservationInstructionAbortAll(Sh4ObservationBackend) noexcept {}
inline void sh4ObservationMemoryAccess(Sh4ObservationBackend, std::uint32_t,
		std::uint8_t, Sh4MemoryAccessKind, std::uint64_t) {}
inline void sh4ObservationException(Sh4ObservationBackend, std::uint32_t,
		std::uint32_t, std::uint32_t, std::uint64_t, const Sh4Context&) {}
inline void sh4ObservationExceptionRaised(std::uint32_t, std::uint32_t,
		const Sh4Context&) noexcept {}
inline void sh4ObservationInterruptRaised(Sh4ObservationBackend, std::uint32_t,
		std::uint64_t, const Sh4Context&) noexcept {}
inline Sh4ObservationBackend sh4ObservationCurrentInstructionBackend(
		Sh4ObservationBackend fallback) noexcept { return fallback; }
inline void retainSh4InstructionOwnership(Sh4ObservationBackend) noexcept {}
inline void releaseSh4InstructionOwnership(Sh4ObservationBackend) noexcept {}
inline bool sh4InstructionOwnershipActive(Sh4ObservationBackend) noexcept
{
	return false;
}
inline Sh4InstructionOwnerToken sh4ObservationCurrentInstructionOwner() noexcept
{
	return {};
}
inline std::uint64_t sh4ObservationSynchronousHardwareTick(
		std::uint64_t fallbackTick) noexcept
{
	return fallbackTick;
}
using Sh4DynarecObservationMarker = void (*)(Sh4Context *, std::uint32_t,
		std::uint32_t, std::uint32_t) noexcept;
inline void sh4DynarecObservationMarkerUnavailable(Sh4Context *, std::uint32_t,
		std::uint32_t, std::uint32_t) noexcept {}
inline Sh4DynarecObservationMarker sh4DynarecObservationMarkerFor(
		std::uint32_t) noexcept { return sh4DynarecObservationMarkerUnavailable; }
inline void sh4DynarecObservationMemoryBegin(std::uint32_t, std::uint32_t,
		std::uint64_t) noexcept {}
inline void sh4DynarecObservationMemoryEnd(std::uint32_t, std::uint32_t,
		std::uint64_t) noexcept {}
inline void sh4DynarecExecutionTimingReset() noexcept {}
inline void sh4DynarecTimingDiagnosticSetActive(bool) noexcept {}
inline Sh4DynarecTimingDiagnosticSnapshot sh4DynarecTimingDiagnosticSnapshot(
		std::uint64_t, std::uint64_t, std::uint64_t) { return {}; }
inline bool sh4ObservationPreciseTimingActive(Sh4ObservationBackend) noexcept
{
	return false;
}
inline void sh4ObservationSetPreciseTiming(Sh4ObservationBackend, bool) noexcept {}
inline void sh4ObservationResetPreciseTiming() noexcept {}

#else

Sh4RegisterSnapshot snapshotSh4Registers(const Sh4Context& context);

// Backend-neutral semantic ownership used by the interpreter and dynarec.
// The backend-specific activity gate runs before frame allocation or snapshot
// construction. Calls and returns are derived here so backends cannot define
// competing observation semantics.
void sh4ObservationInstructionBegin(Sh4ObservationBackend backend,
		std::uint32_t pc, std::uint16_t opcode, std::uint64_t tick,
		const Sh4Context& context);
void sh4ObservationInstructionEnd(Sh4ObservationBackend backend,
		std::uint32_t pc, std::uint16_t opcode, std::uint64_t tick,
		const Sh4Context& context);
void sh4ObservationInstructionAbort(Sh4ObservationBackend backend) noexcept;
void sh4ObservationInstructionAbortAll(Sh4ObservationBackend backend) noexcept;
void sh4ObservationMemoryAccess(Sh4ObservationBackend backend,
		std::uint32_t address, std::uint8_t width, Sh4MemoryAccessKind kind,
		std::uint64_t value);
void sh4ObservationException(Sh4ObservationBackend backend,
		std::uint32_t exceptionPc, std::uint32_t vectorPc,
		std::uint32_t exceptionCode, std::uint64_t tick,
		const Sh4Context& context);
// Called immediately before the common SH-4 exception state transition. If an
// observed instruction owns the fault, publish it and close that frame before
// a dynarec exception trampoline can bypass the remaining generated markers.
void sh4ObservationExceptionRaised(std::uint32_t exceptionPc,
		std::uint32_t exceptionCode, const Sh4Context& context) noexcept;
void sh4ObservationInterruptRaised(Sh4ObservationBackend backend,
		std::uint32_t interruptCode, std::uint64_t tick,
		const Sh4Context& context) noexcept;
Sh4ObservationBackend sh4ObservationCurrentInstructionBackend(
		Sh4ObservationBackend fallback) noexcept;
// Lightweight instruction ownership can be retained by other research buses
// without enabling full SH-4 event construction and publication.
void retainSh4InstructionOwnership(Sh4ObservationBackend backend) noexcept;
void releaseSh4InstructionOwnership(Sh4ObservationBackend backend) noexcept;
bool sh4InstructionOwnershipActive(Sh4ObservationBackend backend) noexcept;
// Returns only the currently open instruction frame. Callers must not infer an
// owner from Sh4Context::pc after the observed hardware boundary has passed.
Sh4InstructionOwnerToken sh4ObservationCurrentInstructionOwner() noexcept;
std::uint64_t sh4ObservationSynchronousHardwareTick(
		std::uint64_t fallbackTick) noexcept;
// Compile-time selection for the generated marker call. Generated code embeds
// only POD immediates, never a pointer into RuntimeBlockInfo::oplist.
using Sh4DynarecObservationMarker = void (*)(Sh4Context *, std::uint32_t pc,
		std::uint32_t opcodeAndRemainingCycles,
		std::uint32_t primaryNextPc) noexcept;
Sh4DynarecObservationMarker sh4DynarecObservationMarkerFor(
		std::uint32_t shilOpcode) noexcept;
// Generated-code ABI boundaries. Begin preserves the effective address before
// a load can replace its address register; end publishes only on success.
void sh4DynarecObservationMemoryBegin(std::uint32_t address,
		std::uint32_t widthAndKind, std::uint64_t writeValue) noexcept;
void sh4DynarecObservationMemoryEnd(std::uint32_t unusedAddress,
		std::uint32_t unusedWidthAndKind, std::uint64_t readValue) noexcept;
// Research dynarec uses the interpreter's instruction-boundary cycle model so
// deferred captures can authenticate the same replay checkpoint. Normal
// dynarec execution never calls this state machine.
void sh4DynarecExecutionTimingReset() noexcept;
// Diagnostic replay keeps only the most recent instruction/scheduler boundary
// records in fixed memory. A snapshot is diagnostic-only and is never an SH-4
// equivalence artifact.
void sh4DynarecTimingDiagnosticSetActive(bool active) noexcept;
Sh4DynarecTimingDiagnosticSnapshot sh4DynarecTimingDiagnosticSnapshot(
		std::uint64_t zeroBasedDmaOrdinal, std::uint64_t observedTick,
		std::uint64_t expectedTick);
// Native equivalence capture owns the timing transition. A generic observation
// subscriber (including Lua discovery) must never change guest scheduling.
bool sh4ObservationPreciseTimingActive(Sh4ObservationBackend backend) noexcept;
void sh4ObservationSetPreciseTiming(Sh4ObservationBackend backend,
		bool active) noexcept;
void sh4ObservationResetPreciseTiming() noexcept;

#endif

} // namespace research

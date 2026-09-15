#pragma once

// Private state and helpers shared by the SH-4 observation runtime translation
// units:
//   sh4_observation_runtime.cpp  instruction frame lifecycle and ownership
//   sh4_observation_events.cpp   memory, exception and interrupt observations
//   sh4_observation_dynarec.cpp  generated-code marker and memory ABI
//   sh4_observation_timing.cpp   execution timing, semantic clocks, diagnostics
// Nothing here is part of the public research API. Each declared object is
// defined in exactly one of the units named next to it.

#include "research/sh4_observation_runtime.h"

#include "hw/sh4/sh4_cycles.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct Sh4Context;

namespace research
{

struct EmissionInstructionFrame
{
	Sh4ObservationBackend backend = Sh4ObservationBackend::Interpreter;
	std::uint32_t pc = 0;
	std::uint16_t opcode = 0;
	std::uint64_t tick = 0;
	std::uint32_t pr = 0;
	std::uint64_t ownerGeneration = 0;
	std::uint64_t subscriptionGeneration = 0;
	bool emitting = false;
	bool memoryPending = false;
	std::uint32_t nestedHardwareMemoryDepth = 0;
	std::uint32_t memoryAddress = 0;
	std::uint8_t memoryWidth = 0;
	Sh4MemoryAccessKind memoryKind = Sh4MemoryAccessKind::Read;
	std::uint64_t memoryValue = 0;
	bool interruptPending = false;
	Sh4Observation pendingInterrupt;
};

// Open instruction frames of the executing thread, innermost last.
// Defined in sh4_observation_runtime.cpp.
extern thread_local std::vector<EmissionInstructionFrame> instructionFrames;

#ifdef STRICT_MODE
constexpr int InterpreterWarmupCycleRatio = 1;
#else
constexpr int InterpreterWarmupCycleRatio = 8;
#endif

struct DynarecExecutionTimingFrame
{
	std::uint32_t pc = 0;
	std::uint16_t opcode = 0;
	bool precise = false;
	std::uint64_t diagnosticSequence = 0;
};

struct DynarecExecutionTiming
{
	Sh4Cycles warmupCycles {InterpreterWarmupCycleRatio};
	Sh4Cycles preciseCycles {1};
	std::vector<DynarecExecutionTimingFrame> frames;
};

// Research dynarec instruction-boundary cycle model state.
// Defined in sh4_observation_timing.cpp.
extern thread_local DynarecExecutionTiming dynarecExecutionTiming;

inline std::size_t timingBackendIndex(Sh4ObservationBackend backend) noexcept
{
	return backend == Sh4ObservationBackend::Dynarec ? 1u : 0u;
}

// Defined in sh4_observation_runtime.cpp.
Sh4Observation instructionObservation(Sh4ObservationBackend backend,
		Sh4ObservationType type, std::uint32_t pc, std::uint16_t opcode,
		std::uint64_t tick, const Sh4Context& context,
		std::uint16_t delaySlotDepth);
void requireFrameBackend(Sh4ObservationBackend backend,
		const EmissionInstructionFrame& frame);
bool frameCanEmit(const EmissionInstructionFrame& frame) noexcept;

// Defined in sh4_observation_timing.cpp.
std::uint64_t dynarecMarkerTick(const Sh4Context& context,
		std::uint32_t remainingCycles) noexcept;
std::uint64_t dynarecCurrentTick(const Sh4Context& context) noexcept;
void completeDiagnosticInstruction(const Sh4Context& context,
		std::uint64_t sequence, std::uint32_t nextPc, int instructionCycles,
		Sh4DynarecTimingDiagnosticState state, std::uint32_t boundaryCode = 0) noexcept;
void appendDiagnosticInterrupt(const Sh4Context& context,
		std::uint32_t interruptCode, std::uint64_t schedulerTick) noexcept;
bool dynarecExecutionTimingBegin(const Sh4Context& context,
		std::uint32_t pc, std::uint16_t opcode);
bool dynarecExecutionTimingEnd(Sh4Context& context, std::uint32_t pc,
		std::uint16_t opcode, std::uint32_t nextPc);
void dynarecExecutionTimingException(Sh4Context& context,
		std::uint32_t exceptionCode) noexcept;
bool preciseDynarecSemanticClockEnabled() noexcept;
std::uint64_t dynarecSemanticBeginTick(std::uint64_t initialTick);
std::uint64_t dynarecSemanticEndTick(std::uint16_t opcode,
		std::uint64_t initialTick);
std::uint64_t dynarecSemanticInterruptTick(std::uint64_t fallbackTick) noexcept;
void recordInterpreterSemanticTick(std::uint64_t tick) noexcept;
std::uint64_t interpreterSemanticInterruptTick(
		std::uint64_t fallbackTick) noexcept;

} // namespace research

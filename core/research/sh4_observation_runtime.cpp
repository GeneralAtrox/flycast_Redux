#include "research/sh4_observation_runtime.h"
#include "research/pvr_presentation_observation.h"

#include "cfg/option.h"
#include "hw/sh4/sh4_cycles.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/dyna/shil.h"
#include "log/Log.h"
#include "types.h"

#include <array>
#include <atomic>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace research
{
namespace
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
	std::uint32_t memoryAddress = 0;
	std::uint8_t memoryWidth = 0;
	Sh4MemoryAccessKind memoryKind = Sh4MemoryAccessKind::Read;
	std::uint64_t memoryValue = 0;
	bool interruptPending = false;
	Sh4Observation pendingInterrupt;
};

thread_local std::vector<EmissionInstructionFrame> instructionFrames;

std::array<std::atomic<std::size_t>, 2> instructionOwnershipCounts {};
std::atomic<std::uint64_t> nextInstructionOwnerGeneration {1};

struct DynarecSemanticClock
{
	std::uint64_t subscriptionGeneration = 0;
	std::uint64_t tick = 0;
	Sh4Cycles cycles;
	bool active = false;
};

thread_local DynarecSemanticClock dynarecSemanticClock;

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
};

struct DynarecExecutionTiming
{
	Sh4Cycles warmupCycles {InterpreterWarmupCycleRatio};
	Sh4Cycles preciseCycles {1};
	std::vector<DynarecExecutionTimingFrame> frames;
};

thread_local DynarecExecutionTiming dynarecExecutionTiming;

std::array<std::atomic<bool>, 2> preciseTimingActive {};

std::size_t timingBackendIndex(Sh4ObservationBackend backend) noexcept
{
	return backend == Sh4ObservationBackend::Dynarec ? 1u : 0u;
}

bool isCallWithDelayedPrWrite(std::uint16_t opcode) noexcept
{
	return (opcode & 0xf000u) == 0xb000u
			|| (opcode & 0xf0ffu) == 0x0003u
			|| (opcode & 0xf0ffu) == 0x400bu;
}

Sh4Observation instructionObservation(Sh4ObservationBackend backend,
		Sh4ObservationType type, std::uint32_t pc, std::uint16_t opcode,
		std::uint64_t tick, const Sh4Context& context,
		std::uint16_t delaySlotDepth)
{
	Sh4Observation observation;
	observation.availableFields = Sh4Observation::HasNextPc
			| Sh4Observation::HasRegisters;
	observation.backend = backend;
	observation.type = type;
	observation.tick = tick;
	observation.instructionPc = pc;
	observation.nextPc = context.pc;
	observation.opcode = opcode;
	observation.delaySlotDepth = delaySlotDepth;
	observation.registers = snapshotSh4Registers(context);
	if (backend == Sh4ObservationBackend::Dynarec && delaySlotDepth != 0
			&& instructionFrames.size() >= 2)
	{
		const EmissionInstructionFrame& owner =
				instructionFrames[instructionFrames.size() - 2];
		if (isCallWithDelayedPrWrite(owner.opcode))
			observation.registers.pr = owner.pr;
	}
	return observation;
}

void requireFrameBackend(Sh4ObservationBackend backend,
		const EmissionInstructionFrame& frame)
{
	if (frame.backend != backend)
		throw std::logic_error("SH-4 observation backend changed inside an instruction");
}

bool frameCanEmit(const EmissionInstructionFrame& frame) noexcept
{
	return frame.emitting && sh4ObservationBusActive(frame.backend)
			&& frame.subscriptionGeneration
					== sh4ObservationSubscriptionGeneration(frame.backend);
}

std::uint64_t dynarecMarkerTick(const Sh4Context& context,
		std::uint32_t remainingCycles) noexcept
{
	const std::int64_t blockEndTick = static_cast<std::int64_t>(sh4_sched_now64())
			+ SH4_TIMESLICE - context.cycle_counter;
	const std::int64_t markerTick = blockEndTick - remainingCycles;
	return markerTick < 0 ? 0 : static_cast<std::uint64_t>(markerTick);
}

std::uint64_t dynarecCurrentTick(const Sh4Context& context) noexcept
{
	const std::int64_t tick = static_cast<std::int64_t>(sh4_sched_now64())
			+ SH4_TIMESLICE - context.cycle_counter;
	return tick < 0 ? 0 : static_cast<std::uint64_t>(tick);
}

bool dynarecExecutionTimingBegin(std::uint32_t pc, std::uint16_t opcode)
{
	const bool precise = sh4ObservationPreciseTimingActive(
			Sh4ObservationBackend::Dynarec);
	dynarecExecutionTiming.frames.push_back({pc, opcode, precise});
	return precise;
}

bool dynarecExecutionTimingEnd(Sh4Context& context, std::uint32_t pc,
		std::uint16_t opcode)
{
	if (dynarecExecutionTiming.frames.empty())
		throw std::logic_error("dynarec timing end without begin");
	const DynarecExecutionTimingFrame frame =
			dynarecExecutionTiming.frames.back();
	if (frame.pc != pc || frame.opcode != opcode)
		throw std::logic_error("dynarec timing end does not match begin");
	dynarecExecutionTiming.frames.pop_back();
	Sh4Cycles& cycles = frame.precise ? dynarecExecutionTiming.preciseCycles
			: dynarecExecutionTiming.warmupCycles;
	const int instructionCycles = cycles.countCycles(opcode);
	context.cycle_counter -= instructionCycles;
	if (frame.precise && OpDesc[opcode]->SetPC())
		cycles.reset();
	return frame.precise;
}

void dynarecExecutionTimingException(Sh4Context& context) noexcept
{
	if (!config::ResearchDynarecObservation.get()
			|| dynarecExecutionTiming.frames.empty())
		return;
	const bool precise = dynarecExecutionTiming.frames.back().precise;
	const int exceptionCycles = 5 * (precise ? 1 : InterpreterWarmupCycleRatio);
	context.cycle_counter -= exceptionCycles;
	dynarecExecutionTiming.frames.clear();
}

bool preciseDynarecSemanticClockEnabled() noexcept
{
	return config::ResearchDynarecObservation.get()
			&& sh4ObservationPreciseTimingActive(
					Sh4ObservationBackend::Dynarec);
}

void ensureDynarecSemanticClock(std::uint64_t initialTick)
{
	const std::uint64_t generation = sh4ObservationSubscriptionGeneration(
			Sh4ObservationBackend::Dynarec);
	if (dynarecSemanticClock.active
			&& dynarecSemanticClock.subscriptionGeneration == generation)
		return;
	dynarecSemanticClock.subscriptionGeneration = generation;
	dynarecSemanticClock.tick = initialTick;
	dynarecSemanticClock.cycles.reset();
	dynarecSemanticClock.active = true;
}

std::uint64_t dynarecSemanticBeginTick(std::uint64_t initialTick)
{
	ensureDynarecSemanticClock(initialTick);
	return dynarecSemanticClock.tick;
}

std::uint64_t dynarecSemanticEndTick(std::uint16_t opcode,
		std::uint64_t initialTick)
{
	ensureDynarecSemanticClock(initialTick);
	dynarecSemanticClock.tick +=
			dynarecSemanticClock.cycles.countCycles(opcode);
	const std::uint64_t tick = dynarecSemanticClock.tick;
	if (OpDesc[opcode]->SetPC())
		dynarecSemanticClock.cycles.reset();
	return tick;
}

std::uint64_t dynarecSemanticInterruptTick(std::uint64_t fallbackTick) noexcept
{
	const std::uint64_t generation = sh4ObservationSubscriptionGeneration(
			Sh4ObservationBackend::Dynarec);
	return dynarecSemanticClock.active
			&& dynarecSemanticClock.subscriptionGeneration == generation
			? dynarecSemanticClock.tick : fallbackTick;
}

bool conditionalBranchTaken(std::uint16_t opcode, std::uint32_t condition) noexcept
{
	if ((opcode & 0xff00u) == 0x8b00u || (opcode & 0xff00u) == 0x8f00u)
		return condition == 0;
	if ((opcode & 0xff00u) == 0x8900u || (opcode & 0xff00u) == 0x8d00u)
		return condition != 0;
	return false;
}

std::uint32_t markerNextPc(std::uint32_t primaryNextPc,
		const Sh4Context& context) noexcept
{
	return primaryNextPc == 0xffffffffu ? context.jdyn : primaryNextPc;
}

enum class DynarecMarkerKind : std::uint8_t
{
	Begin,
	End,
	ConditionalEnd,
	ConditionalBeforeDelay,
	ConditionalAfterDelay,
};

void runDynarecObservationMarker(DynarecMarkerKind kind, Sh4Context *context,
		std::uint32_t pc, std::uint32_t opcodeAndRemainingCycles,
		std::uint32_t primaryNextPc) noexcept
{
	if (context == nullptr)
		return;
	const std::uint16_t opcode = static_cast<std::uint16_t>(
			opcodeAndRemainingCycles);
	const std::uint32_t remainingCycles = opcodeAndRemainingCycles >> 16;
	const bool executionTiming = config::ResearchDynarecObservation.get();
	std::uint64_t markerTick = executionTiming
			? dynarecCurrentTick(*context)
			: dynarecMarkerTick(*context, remainingCycles);
	std::uint64_t tick = markerTick;
	const bool preciseClock = preciseDynarecSemanticClockEnabled();
	try
	{
		switch (kind)
		{
		case DynarecMarkerKind::Begin:
		{
			const bool preciseInstruction = executionTiming
					? dynarecExecutionTimingBegin(pc, opcode) : preciseClock;
			if (executionTiming)
			{
				markerTick = dynarecCurrentTick(*context);
				tick = markerTick;
			}
			if (preciseInstruction)
				tick = dynarecSemanticBeginTick(markerTick);
			context->pc = pc + 2u;
			sh4ObservationInstructionBegin(Sh4ObservationBackend::Dynarec, pc,
					opcode, tick, *context);
			break;
		}
		case DynarecMarkerKind::End:
		{
			const bool preciseInstruction = executionTiming
					? dynarecExecutionTimingEnd(*context, pc, opcode) : preciseClock;
			if (executionTiming)
				markerTick = dynarecCurrentTick(*context);
			if (preciseInstruction)
				tick = dynarecSemanticEndTick(opcode, markerTick);
			else
				tick = markerTick;
			context->pc = markerNextPc(primaryNextPc, *context);
			sh4ObservationInstructionEnd(Sh4ObservationBackend::Dynarec, pc,
					opcode, tick, *context);
			break;
		}
		case DynarecMarkerKind::ConditionalEnd:
		{
			const bool preciseInstruction = executionTiming
					? dynarecExecutionTimingEnd(*context, pc, opcode) : preciseClock;
			if (executionTiming)
				markerTick = dynarecCurrentTick(*context);
			if (preciseInstruction)
				tick = dynarecSemanticEndTick(opcode, markerTick);
			else
				tick = markerTick;
			context->pc = conditionalBranchTaken(opcode, context->sr.T)
					? primaryNextPc : pc + 2u;
			sh4ObservationInstructionEnd(Sh4ObservationBackend::Dynarec, pc,
					opcode, tick, *context);
			break;
		}
		case DynarecMarkerKind::ConditionalBeforeDelay:
			if (!conditionalBranchTaken(opcode, context->jdyn))
			{
				const bool preciseInstruction = executionTiming
						? dynarecExecutionTimingEnd(*context, pc, opcode) : preciseClock;
				if (executionTiming)
					markerTick = dynarecCurrentTick(*context);
				if (preciseInstruction)
					tick = dynarecSemanticEndTick(opcode, markerTick);
				else
					tick = markerTick;
				context->pc = pc + 2u;
				sh4ObservationInstructionEnd(Sh4ObservationBackend::Dynarec, pc,
						opcode, tick, *context);
			}
			break;
		case DynarecMarkerKind::ConditionalAfterDelay:
			if (conditionalBranchTaken(opcode, context->jdyn))
			{
				const bool preciseInstruction = executionTiming
						? dynarecExecutionTimingEnd(*context, pc, opcode) : preciseClock;
				if (executionTiming)
					markerTick = dynarecCurrentTick(*context);
				if (preciseInstruction)
					tick = dynarecSemanticEndTick(opcode, markerTick);
				else
					tick = markerTick;
				context->pc = primaryNextPc;
				sh4ObservationInstructionEnd(Sh4ObservationBackend::Dynarec, pc,
						opcode, tick, *context);
			}
			break;
		}
	}
	catch (const std::exception& exception)
	{
		WARN_LOG(SH4, "Dynarec observation marker failed: %s", exception.what());
		sh4ObservationInstructionAbort(Sh4ObservationBackend::Dynarec);
	}
	catch (...)
	{
		WARN_LOG(SH4, "Dynarec observation marker failed");
		sh4ObservationInstructionAbort(Sh4ObservationBackend::Dynarec);
	}
}

#define DEFINE_DYNAREC_MARKER(name, kind) \
	void name(Sh4Context *context, std::uint32_t pc, \
			std::uint32_t opcodeAndRemainingCycles, \
			std::uint32_t primaryNextPc) noexcept \
	{ \
		runDynarecObservationMarker(kind, context, pc, \
				opcodeAndRemainingCycles, primaryNextPc); \
	}

DEFINE_DYNAREC_MARKER(dynarecBeginMarker, DynarecMarkerKind::Begin)
DEFINE_DYNAREC_MARKER(dynarecEndMarker, DynarecMarkerKind::End)
DEFINE_DYNAREC_MARKER(dynarecConditionalEndMarker,
		DynarecMarkerKind::ConditionalEnd)
DEFINE_DYNAREC_MARKER(dynarecConditionalBeforeDelayMarker,
		DynarecMarkerKind::ConditionalBeforeDelay)
DEFINE_DYNAREC_MARKER(dynarecConditionalAfterDelayMarker,
		DynarecMarkerKind::ConditionalAfterDelay)

#undef DEFINE_DYNAREC_MARKER

} // namespace

Sh4RegisterSnapshot snapshotSh4Registers(const Sh4Context& context)
{
	Sh4RegisterSnapshot snapshot;
	for (std::size_t index = 0; index < snapshot.r.size(); ++index)
		snapshot.r[index] = context.r[index];
	snapshot.pr = context.pr;
	snapshot.gbr = context.gbr;
	snapshot.vbr = context.vbr;
	snapshot.mach = context.mac.h;
	snapshot.macl = context.mac.l;
	snapshot.sr = context.sr.getFull();
	snapshot.fpul = context.fpul;
	snapshot.fpscr = context.fpscr.full;
	return snapshot;
}

void sh4ObservationInstructionBegin(Sh4ObservationBackend backend,
		std::uint32_t pc, std::uint16_t opcode, std::uint64_t tick,
		const Sh4Context& context)
{
	const bool active = sh4ObservationBusActive(backend);
	const bool ownershipActive = sh4InstructionOwnershipActive(backend);
	if (!active && !ownershipActive && instructionFrames.empty())
		return;
	if (instructionFrames.size() > std::numeric_limits<std::uint16_t>::max())
		throw std::overflow_error("SH-4 observation delay-slot depth overflow");
	if (!instructionFrames.empty())
		requireFrameBackend(backend, instructionFrames.back());
	const std::uint16_t depth = static_cast<std::uint16_t>(instructionFrames.size());
	const std::uint64_t generation = active
			? sh4ObservationSubscriptionGeneration(backend) : 0;
	const bool emitting = active && (instructionFrames.empty()
			|| (instructionFrames.back().emitting
					&& instructionFrames.back().subscriptionGeneration == generation));
	const std::uint64_t ownerGeneration = nextInstructionOwnerGeneration.fetch_add(1,
			std::memory_order_relaxed);
	instructionFrames.push_back(EmissionInstructionFrame {backend, pc, opcode, tick,
			context.pr, ownerGeneration, generation, emitting});
	if (!emitting)
		return;
	try
	{
		Sh4Observation begin = instructionObservation(backend,
				Sh4ObservationType::InstructionBegin, pc, opcode, tick, context, depth);
		publishSh4Observation(begin);
		if (!frameCanEmit(instructionFrames.back()))
		{
			instructionFrames.back().emitting = false;
			return;
		}
		Sh4InstructionState state;
		state.pc = pc;
		state.nextPc = context.pc;
		state.opcode = opcode;
		state.tick = tick;
		state.registers = begin.registers;
		Sh4CallKind kind = Sh4CallKind::Bsr;
		std::uint32_t targetPc = 0;
		if (decodeSh4Call(state, kind, targetPc))
		{
			Sh4Observation call = begin;
			call.type = Sh4ObservationType::Call;
			call.callKind = kind;
			call.targetPc = targetPc;
			call.returnPc = pc + 4u;
			call.delaySlotPc = pc + 2u;
			publishSh4Observation(std::move(call));
		}
	}
	catch (...)
	{
		const std::exception_ptr failure = std::current_exception();
		sh4ObservationInstructionAbort(backend);
		std::rethrow_exception(failure);
	}
}

void sh4ObservationInstructionEnd(Sh4ObservationBackend backend,
		std::uint32_t pc, std::uint16_t opcode, std::uint64_t tick,
		const Sh4Context& context)
{
	if (instructionFrames.empty())
		return;
	const EmissionInstructionFrame& frame = instructionFrames.back();
	requireFrameBackend(backend, frame);
	if (frame.pc != pc || frame.opcode != opcode)
		throw std::logic_error("SH-4 observation end does not match its begin");
	const std::uint16_t depth = static_cast<std::uint16_t>(
			instructionFrames.size() - 1);
	try
	{
		if (frameCanEmit(frame))
		{
			Sh4Observation end = instructionObservation(backend,
					Sh4ObservationType::InstructionEnd, pc, opcode, tick, context, depth);
			if (frame.interruptPending)
			{
				// UpdateSR may have entered the interrupt before the interpreter
				// publishes this end marker. Reconstruct the architectural
				// pre-interrupt boundary captured by the pending observation.
				end.nextPc = frame.pendingInterrupt.exceptionPc;
				end.registers = frame.pendingInterrupt.registers;
			}
			if (opcode == 0x000bu)
			{
				Sh4Observation returned = end;
				returned.type = Sh4ObservationType::Return;
				returned.targetPc = context.pc;
				returned.returnPc = frame.pr;
				returned.delaySlotPc = pc + 2u;
				publishSh4Observation(std::move(returned));
			}
			publishSh4Observation(std::move(end));
			if (frame.interruptPending)
			{
				Sh4Observation interrupt = frame.pendingInterrupt;
				// Architecturally, the interrupt is accepted at the boundary
				// after the SR-changing instruction completes.
				interrupt.tick = tick;
				publishSh4Observation(std::move(interrupt));
			}
		}
	}
	catch (...)
	{
		const std::exception_ptr failure = std::current_exception();
		sh4ObservationInstructionAbort(backend);
		std::rethrow_exception(failure);
	}
	instructionFrames.pop_back();
}

void sh4ObservationInstructionAbort(Sh4ObservationBackend backend) noexcept
{
	if (instructionFrames.empty())
		return;
	const EmissionInstructionFrame frame = instructionFrames.back();
	if (frame.backend != backend)
	{
		WARN_LOG(SH4, "SH-4 observation abort backend differs from its frame");
		instructionFrames.clear();
		return;
	}
	const std::uint16_t depth = static_cast<std::uint16_t>(
			instructionFrames.size() - 1);
	if (frameCanEmit(frame))
	{
		try
		{
			Sh4Observation observation;
			observation.backend = backend;
			observation.type = Sh4ObservationType::InstructionAbort;
			observation.tick = frame.tick;
			observation.instructionPc = frame.pc;
			observation.opcode = frame.opcode;
			observation.delaySlotDepth = depth;
			publishSh4Observation(std::move(observation));
		}
		catch (const std::exception& exception)
		{
			WARN_LOG(SH4, "SH-4 observation abort subscriber failed: %s",
					exception.what());
		}
		catch (...)
		{
			WARN_LOG(SH4, "SH-4 observation abort subscriber failed");
		}
	}
	instructionFrames.pop_back();
}

void sh4ObservationInstructionAbortAll(Sh4ObservationBackend backend) noexcept
{
	while (!instructionFrames.empty())
	{
		if (instructionFrames.back().backend != backend)
		{
			WARN_LOG(SH4, "SH-4 observation abort-all backend differs from its frame");
			instructionFrames.clear();
			return;
		}
		sh4ObservationInstructionAbort(backend);
	}
}

void sh4ObservationMemoryAccess(Sh4ObservationBackend backend,
		std::uint32_t address, std::uint8_t width, Sh4MemoryAccessKind kind,
		std::uint64_t value)
{
	if (instructionFrames.empty())
		return;
	const EmissionInstructionFrame& frame = instructionFrames.back();
	requireFrameBackend(backend, frame);
	if (kind == Sh4MemoryAccessKind::Write
			&& pvrPresentationObservationBusActive())
	{
		const std::uint32_t physical = address & 0x1fffffffu;
		const std::uint32_t area = physical >> 24;
		if (area == 0x04u || area == 0x06u || area == 0x07u)
		{
			std::uint8_t bytes[8] {};
			for (std::uint8_t index = 0; index < width; ++index)
				bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
			observePvrVramWrite(PvrVramWriteSource::Sh4Area1Direct,
					address, physical & 0x007fffffu, bytes, width, 0, frame.tick);
		}
	}
	if (!frameCanEmit(frame))
		return;
	Sh4Observation observation;
	observation.backend = backend;
	observation.type = kind == Sh4MemoryAccessKind::Read
			? Sh4ObservationType::MemoryRead : Sh4ObservationType::MemoryWrite;
	observation.tick = frame.tick;
	observation.instructionPc = frame.pc;
	observation.opcode = frame.opcode;
	observation.delaySlotDepth = static_cast<std::uint16_t>(
			instructionFrames.size() - 1);
	observation.memoryAddress = address;
	observation.memoryWidth = width;
	observation.memoryValue = value;
	publishSh4Observation(std::move(observation));
}

void sh4ObservationException(Sh4ObservationBackend backend,
		std::uint32_t exceptionPc, std::uint32_t vectorPc,
		std::uint32_t exceptionCode, std::uint64_t tick,
		const Sh4Context& context)
{
	std::uint32_t ownerPc = context.pc;
	std::uint16_t ownerOpcode = 0;
	std::uint16_t depth = 0;
	if (!instructionFrames.empty())
	{
		const EmissionInstructionFrame& frame = instructionFrames.back();
		requireFrameBackend(backend, frame);
		if (!frameCanEmit(frame) && !pvrPresentationObservationBusActive())
			return;
		ownerPc = frame.pc;
		ownerOpcode = frame.opcode;
		depth = static_cast<std::uint16_t>(instructionFrames.size() - 1);
	}
	else if (!sh4ObservationBusActive(backend))
		return;
	Sh4Observation observation = instructionObservation(backend,
			Sh4ObservationType::Exception, ownerPc, ownerOpcode, tick, context, depth);
	if (!instructionFrames.empty())
		observation.nextPc = ownerPc + 2u;
	observation.exceptionPc = exceptionPc;
	observation.vectorPc = vectorPc;
	observation.exceptionCode = exceptionCode;
	publishSh4Observation(std::move(observation));
}

void sh4ObservationExceptionRaised(std::uint32_t exceptionPc,
		std::uint32_t exceptionCode, const Sh4Context& context) noexcept
{
	dynarecExecutionTimingException(const_cast<Sh4Context&>(context));
	if (instructionFrames.empty())
		return;
	const Sh4ObservationBackend backend = instructionFrames.back().backend;
	const std::uint64_t tick = instructionFrames.back().tick;
	const std::uint32_t vectorPc = context.vbr
			+ (exceptionCode == Sh4Ex_TlbMissRead
					|| exceptionCode == Sh4Ex_TlbMissWrite ? 0x400u : 0x100u);
	try
	{
		sh4ObservationException(backend, exceptionPc, vectorPc, exceptionCode,
				tick, context);
	}
	catch (const std::exception& exception)
	{
		WARN_LOG(SH4, "SH-4 exception observation failed: %s", exception.what());
	}
	catch (...)
	{
		WARN_LOG(SH4, "SH-4 exception observation failed");
	}
	sh4ObservationInstructionAbortAll(backend);
}

void sh4ObservationInterruptRaised(Sh4ObservationBackend backend,
		std::uint32_t interruptCode, std::uint64_t tick,
		const Sh4Context& context) noexcept
{
	if (backend == Sh4ObservationBackend::Dynarec
			&& !dynarecExecutionTiming.frames.empty())
	{
		WARN_LOG(SH4, "SH-4 interrupt reached with an open dynarec timing frame");
		dynarecExecutionTiming.frames.clear();
	}
	// The interpreter can accept an interrupt synchronously inside RTE or an
	// LDC-to-SR instruction after the architectural SR write.  That interrupt is
	// owned by the still-open instruction and the instruction subsequently
	// reaches its normal end.  Dynarec interrupt checks remain scheduler
	// boundaries, so an open dynarec frame is still treated as leaked state and
	// fails closed.
	if (!instructionFrames.empty())
	{
		if (backend == Sh4ObservationBackend::Interpreter)
		{
			try
			{
				EmissionInstructionFrame& frame = instructionFrames.back();
				if (frame.interruptPending)
					throw std::logic_error(
							"multiple interrupts reached one interpreter instruction");
				if (frameCanEmit(frame))
				{
					frame.pendingInterrupt = instructionObservation(backend,
							Sh4ObservationType::Exception, context.pc, 0,
							frame.tick, context, static_cast<std::uint16_t>(
									0));
					frame.pendingInterrupt.exceptionPc = context.pc;
					frame.pendingInterrupt.vectorPc = context.vbr + 0x600u;
					frame.pendingInterrupt.exceptionCode = interruptCode;
					frame.interruptPending = true;
				}
			}
			catch (const std::exception& exception)
			{
				WARN_LOG(SH4, "SH-4 interrupt observation failed: %s",
						exception.what());
			}
			catch (...)
			{
				WARN_LOG(SH4, "SH-4 interrupt observation failed");
			}
			return;
		}
		WARN_LOG(SH4, "SH-4 interrupt reached with an open observation frame");
		instructionFrames.clear();
		return;
	}
	try
	{
		if (!sh4ObservationBusActive(backend))
			return;
		if (backend == Sh4ObservationBackend::Dynarec)
			tick = dynarecSemanticInterruptTick(tick);
		Sh4Observation observation = instructionObservation(backend,
				Sh4ObservationType::Exception, context.pc, 0, tick, context, 0);
		observation.exceptionPc = context.pc;
		observation.vectorPc = context.vbr + 0x600u;
		observation.exceptionCode = interruptCode;
		publishSh4Observation(std::move(observation));
	}
	catch (const std::exception& exception)
	{
		WARN_LOG(SH4, "SH-4 interrupt observation failed: %s", exception.what());
	}
	catch (...)
	{
		WARN_LOG(SH4, "SH-4 interrupt observation failed");
	}
}

Sh4ObservationBackend sh4ObservationCurrentInstructionBackend(
		Sh4ObservationBackend fallback) noexcept
{
	return instructionFrames.empty() ? fallback : instructionFrames.back().backend;
}

void retainSh4InstructionOwnership(Sh4ObservationBackend backend) noexcept
{
	instructionOwnershipCounts[timingBackendIndex(backend)].fetch_add(1,
			std::memory_order_release);
}

void releaseSh4InstructionOwnership(Sh4ObservationBackend backend) noexcept
{
	std::atomic<std::size_t>& count =
			instructionOwnershipCounts[timingBackendIndex(backend)];
	std::size_t current = count.load(std::memory_order_acquire);
	while (current != 0 && !count.compare_exchange_weak(current, current - 1,
			std::memory_order_acq_rel, std::memory_order_acquire))
	{
	}
}

bool sh4InstructionOwnershipActive(Sh4ObservationBackend backend) noexcept
{
	return instructionOwnershipCounts[timingBackendIndex(backend)].load(
			std::memory_order_acquire) != 0;
}

Sh4InstructionOwnerToken sh4ObservationCurrentInstructionOwner() noexcept
{
	if (instructionFrames.empty())
		return {};
	const EmissionInstructionFrame& frame = instructionFrames.back();
	Sh4InstructionOwnerToken token;
	token.valid = true;
	token.backend = frame.backend;
	token.generation = frame.ownerGeneration;
	token.tick = frame.tick;
	token.pc = frame.pc;
	token.opcode = frame.opcode;
	token.pr = frame.pr;
	token.delaySlotDepth = static_cast<std::uint16_t>(
			instructionFrames.size() - 1);
	if (frame.backend == Sh4ObservationBackend::Dynarec
			&& token.delaySlotDepth != 0 && instructionFrames.size() >= 2)
	{
		const EmissionInstructionFrame& parent =
				instructionFrames[instructionFrames.size() - 2];
		if (isCallWithDelayedPrWrite(parent.opcode))
			token.pr = parent.pr;
	}
	return token;
}

Sh4DynarecObservationMarker sh4DynarecObservationMarkerFor(
		std::uint32_t shilOpcode) noexcept
{
	switch (static_cast<shilop>(shilOpcode))
	{
	case shop_research_begin:
		return dynarecBeginMarker;
	case shop_research_end:
		return dynarecEndMarker;
	case shop_research_conditional_end:
		return dynarecConditionalEndMarker;
	case shop_research_conditional_before_delay:
		return dynarecConditionalBeforeDelayMarker;
	case shop_research_conditional_after_delay:
		return dynarecConditionalAfterDelayMarker;
	default:
		return nullptr;
	}
}

void sh4DynarecObservationMemoryBegin(std::uint32_t address,
		std::uint32_t widthAndKind, std::uint64_t writeValue) noexcept
{
	try
	{
		if (instructionFrames.empty())
			return;
		EmissionInstructionFrame& frame = instructionFrames.back();
		requireFrameBackend(Sh4ObservationBackend::Dynarec, frame);
		if (!frameCanEmit(frame) && !pvrPresentationObservationBusActive())
			return;
		if (frame.memoryPending)
			throw std::logic_error("nested dynarec memory observation");
		const std::uint8_t width = static_cast<std::uint8_t>(widthAndKind);
		if (width != 1 && width != 2 && width != 4 && width != 8)
			throw std::invalid_argument("invalid dynarec memory width");
		frame.memoryPending = true;
		frame.memoryAddress = address;
		frame.memoryWidth = width;
		frame.memoryKind = (widthAndKind & 0x100u) != 0
				? Sh4MemoryAccessKind::Write : Sh4MemoryAccessKind::Read;
		frame.memoryValue = writeValue;
	}
	catch (const std::exception& exception)
	{
		WARN_LOG(SH4, "Dynarec memory begin marker failed: %s", exception.what());
		sh4ObservationInstructionAbort(Sh4ObservationBackend::Dynarec);
	}
	catch (...)
	{
		WARN_LOG(SH4, "Dynarec memory begin marker failed");
		sh4ObservationInstructionAbort(Sh4ObservationBackend::Dynarec);
	}
}

void sh4DynarecObservationMemoryEnd(std::uint32_t, std::uint32_t,
		std::uint64_t readValue) noexcept
{
	try
	{
		if (instructionFrames.empty())
			return;
		EmissionInstructionFrame& frame = instructionFrames.back();
		requireFrameBackend(Sh4ObservationBackend::Dynarec, frame);
		if (!frameCanEmit(frame))
			return;
		if (!frame.memoryPending)
			throw std::logic_error("dynarec memory end without begin");
		const std::uint32_t address = frame.memoryAddress;
		const std::uint8_t width = frame.memoryWidth;
		const Sh4MemoryAccessKind kind = frame.memoryKind;
		std::uint64_t value = kind == Sh4MemoryAccessKind::Read
				? readValue : frame.memoryValue;
		frame.memoryPending = false;
		const std::uint64_t mask = width == 8 ? ~std::uint64_t {0}
				: (std::uint64_t {1} << (width * 8)) - 1;
		sh4ObservationMemoryAccess(Sh4ObservationBackend::Dynarec, address,
				width, kind, value & mask);
	}
	catch (const std::exception& exception)
	{
		WARN_LOG(SH4, "Dynarec memory end marker failed: %s", exception.what());
		sh4ObservationInstructionAbort(Sh4ObservationBackend::Dynarec);
	}
	catch (...)
	{
		WARN_LOG(SH4, "Dynarec memory end marker failed");
		sh4ObservationInstructionAbort(Sh4ObservationBackend::Dynarec);
	}
}

void sh4DynarecExecutionTimingReset() noexcept
{
	dynarecExecutionTiming.frames.clear();
	dynarecExecutionTiming.warmupCycles.reset();
	dynarecExecutionTiming.preciseCycles.reset();
	dynarecSemanticClock.active = false;
	dynarecSemanticClock.cycles.reset();
}

bool sh4ObservationPreciseTimingActive(
		Sh4ObservationBackend backend) noexcept
{
	if (backend != Sh4ObservationBackend::Interpreter
			&& backend != Sh4ObservationBackend::Dynarec)
		return false;
	return preciseTimingActive[timingBackendIndex(backend)].load(
			std::memory_order_acquire);
}

void sh4ObservationSetPreciseTiming(Sh4ObservationBackend backend,
		bool active) noexcept
{
	if (backend != Sh4ObservationBackend::Interpreter
			&& backend != Sh4ObservationBackend::Dynarec)
		return;
	preciseTimingActive[timingBackendIndex(backend)].store(active,
			std::memory_order_release);
}

void sh4ObservationResetPreciseTiming() noexcept
{
	for (std::atomic<bool>& active : preciseTimingActive)
		active.store(false, std::memory_order_release);
}

} // namespace research

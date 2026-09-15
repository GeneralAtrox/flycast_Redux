#include "research/sh4_observation_runtime_internal.h"

#include "cfg/option.h"
#include "hw/sh4/sh4_cycles.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/sh4_sched.h"
#include "types.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <stdexcept>

namespace research
{

thread_local DynarecExecutionTiming dynarecExecutionTiming;

namespace
{

struct DynarecSemanticClock
{
	std::uint64_t subscriptionGeneration = 0;
	std::uint64_t tick = 0;
	Sh4Cycles cycles;
	bool active = false;
};

thread_local DynarecSemanticClock dynarecSemanticClock;

struct InterpreterSemanticClock
{
	std::uint64_t subscriptionGeneration = 0;
	std::uint64_t tick = 0;
	bool active = false;
};

thread_local InterpreterSemanticClock interpreterSemanticClock;

struct DynarecTimingDiagnosticHistory
{
	bool active = false;
	std::uint64_t activationGeneration = 0;
	std::array<Sh4DynarecTimingDiagnosticRecord,
			Sh4DynarecTimingDiagnosticHistoryCapacity> records {};
	std::size_t nextIndex = 0;
	std::size_t count = 0;
	std::uint64_t nextSequence = 1;
};

thread_local DynarecTimingDiagnosticHistory dynarecTimingDiagnosticHistory;
std::atomic<bool> dynarecTimingDiagnosticEnabled {false};
std::atomic<std::uint64_t> dynarecTimingDiagnosticActivationGeneration {1};

std::array<std::atomic<bool>, 2> preciseTimingActive {};

Sh4DynarecTimingDiagnosticRecord *diagnosticRecordFor(
		std::uint64_t sequence) noexcept
{
	if (sequence == 0)
		return nullptr;
	Sh4DynarecTimingDiagnosticRecord& record =
			dynarecTimingDiagnosticHistory.records[(sequence - 1)
					% Sh4DynarecTimingDiagnosticHistoryCapacity];
	return record.sequence == sequence ? &record : nullptr;
}

void synchronizeDynarecTimingDiagnosticHistory() noexcept
{
	const std::uint64_t generation =
			dynarecTimingDiagnosticActivationGeneration.load(
					std::memory_order_acquire);
	if (dynarecTimingDiagnosticHistory.activationGeneration == generation)
		return;
	dynarecTimingDiagnosticHistory = {};
	dynarecTimingDiagnosticHistory.active =
			dynarecTimingDiagnosticEnabled.load(std::memory_order_acquire);
	dynarecTimingDiagnosticHistory.activationGeneration = generation;
}

std::uint64_t appendDiagnosticInstruction(const Sh4Context& context,
		std::uint32_t pc, std::uint16_t opcode, std::uint16_t depth,
		bool precise) noexcept
{
	synchronizeDynarecTimingDiagnosticHistory();
	if (!dynarecTimingDiagnosticHistory.active)
		return 0;
	Sh4DynarecTimingDiagnosticRecord& record =
			dynarecTimingDiagnosticHistory.records[
					dynarecTimingDiagnosticHistory.nextIndex];
	record = {};
	record.sequence = dynarecTimingDiagnosticHistory.nextSequence++;
	record.pc = pc;
	record.opcode = opcode;
	record.depth = depth;
	record.precise = precise;
	record.cycleCounterBegin = context.cycle_counter;
	record.cycleCounterEnd = context.cycle_counter;
	record.schedulerTickBegin = sh4_sched_now64();
	record.schedulerTickEnd = record.schedulerTickBegin;
	record.executionTickBegin = dynarecCurrentTick(context);
	record.executionTickEnd = record.executionTickBegin;
	dynarecTimingDiagnosticHistory.nextIndex =
			(dynarecTimingDiagnosticHistory.nextIndex + 1)
			% Sh4DynarecTimingDiagnosticHistoryCapacity;
	dynarecTimingDiagnosticHistory.count = std::min(
			dynarecTimingDiagnosticHistory.count + 1,
			Sh4DynarecTimingDiagnosticHistoryCapacity);
	return record.sequence;
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

} // namespace

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

void completeDiagnosticInstruction(const Sh4Context& context,
		std::uint64_t sequence, std::uint32_t nextPc, int instructionCycles,
		Sh4DynarecTimingDiagnosticState state, std::uint32_t boundaryCode) noexcept
{
	Sh4DynarecTimingDiagnosticRecord *record = diagnosticRecordFor(sequence);
	if (record == nullptr)
		return;
	record->state = state;
	record->nextPc = nextPc;
	record->boundaryCode = boundaryCode;
	record->instructionCycles = instructionCycles;
	record->cycleCounterEnd = context.cycle_counter;
	record->schedulerTickEnd = sh4_sched_now64();
	record->executionTickEnd = dynarecCurrentTick(context);
}

void appendDiagnosticInterrupt(const Sh4Context& context,
		std::uint32_t interruptCode, std::uint64_t schedulerTick) noexcept
{
	synchronizeDynarecTimingDiagnosticHistory();
	if (!dynarecTimingDiagnosticHistory.active)
		return;
	Sh4DynarecTimingDiagnosticRecord& record =
			dynarecTimingDiagnosticHistory.records[
					dynarecTimingDiagnosticHistory.nextIndex];
	record = {};
	record.sequence = dynarecTimingDiagnosticHistory.nextSequence++;
	record.state = Sh4DynarecTimingDiagnosticState::Interrupt;
	record.pc = context.pc;
	record.nextPc = context.pc;
	record.boundaryCode = interruptCode;
	record.cycleCounterBegin = context.cycle_counter;
	record.cycleCounterEnd = context.cycle_counter;
	record.schedulerTickBegin = schedulerTick;
	record.schedulerTickEnd = schedulerTick;
	record.executionTickBegin = dynarecCurrentTick(context);
	record.executionTickEnd = record.executionTickBegin;
	dynarecTimingDiagnosticHistory.nextIndex =
			(dynarecTimingDiagnosticHistory.nextIndex + 1)
			% Sh4DynarecTimingDiagnosticHistoryCapacity;
	dynarecTimingDiagnosticHistory.count = std::min(
			dynarecTimingDiagnosticHistory.count + 1,
			Sh4DynarecTimingDiagnosticHistoryCapacity);
}

bool dynarecExecutionTimingBegin(const Sh4Context& context,
		std::uint32_t pc, std::uint16_t opcode)
{
	const bool precise = sh4ObservationPreciseTimingActive(
			Sh4ObservationBackend::Dynarec);
	const std::uint64_t diagnosticSequence = appendDiagnosticInstruction(context,
			pc, opcode, static_cast<std::uint16_t>(
				dynarecExecutionTiming.frames.size()), precise);
	dynarecExecutionTiming.frames.push_back(
			{pc, opcode, precise, diagnosticSequence});
	return precise;
}

bool dynarecExecutionTimingEnd(Sh4Context& context, std::uint32_t pc,
		std::uint16_t opcode, std::uint32_t nextPc)
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
	completeDiagnosticInstruction(context, frame.diagnosticSequence, nextPc,
			instructionCycles, Sh4DynarecTimingDiagnosticState::Completed);
	if (frame.precise && OpDesc[opcode]->SetPC())
		cycles.reset();
	return frame.precise;
}

void dynarecExecutionTimingException(Sh4Context& context,
		std::uint32_t exceptionCode) noexcept
{
	if (!config::ResearchDynarecObservation.get()
			|| dynarecExecutionTiming.frames.empty())
		return;
	const bool precise = dynarecExecutionTiming.frames.back().precise;
	const int exceptionCycles = 5 * (precise ? 1 : InterpreterWarmupCycleRatio);
	context.cycle_counter -= exceptionCycles;
	for (const DynarecExecutionTimingFrame& frame : dynarecExecutionTiming.frames)
		completeDiagnosticInstruction(context, frame.diagnosticSequence, context.pc,
				exceptionCycles, Sh4DynarecTimingDiagnosticState::Exception,
				exceptionCode);
	dynarecExecutionTiming.frames.clear();
}

bool preciseDynarecSemanticClockEnabled() noexcept
{
	return config::ResearchDynarecObservation.get()
			&& sh4ObservationPreciseTimingActive(
					Sh4ObservationBackend::Dynarec);
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

void recordInterpreterSemanticTick(std::uint64_t tick) noexcept
{
	const std::uint64_t generation = sh4ObservationSubscriptionGeneration(
			Sh4ObservationBackend::Interpreter);
	if (!interpreterSemanticClock.active
			|| interpreterSemanticClock.subscriptionGeneration != generation)
	{
		interpreterSemanticClock.active = true;
		interpreterSemanticClock.subscriptionGeneration = generation;
		interpreterSemanticClock.tick = tick;
		return;
	}
	interpreterSemanticClock.tick = std::max(interpreterSemanticClock.tick, tick);
}

std::uint64_t interpreterSemanticInterruptTick(
		std::uint64_t fallbackTick) noexcept
{
	const std::uint64_t generation = sh4ObservationSubscriptionGeneration(
			Sh4ObservationBackend::Interpreter);
	return interpreterSemanticClock.active
			&& interpreterSemanticClock.subscriptionGeneration == generation
		? interpreterSemanticClock.tick : fallbackTick;
}

std::uint64_t sh4ObservationSynchronousHardwareTick(
		std::uint64_t fallbackTick) noexcept
{
	if (!config::DynarecEnabled.get()
			|| !config::ResearchDynarecObservation.get())
		return fallbackTick;
	return std::max(fallbackTick, dynarecCurrentTick(Sh4cntx));
}

void sh4DynarecExecutionTimingReset() noexcept
{
	dynarecExecutionTiming.frames.clear();
	dynarecExecutionTiming.warmupCycles.reset();
	dynarecExecutionTiming.preciseCycles.reset();
	dynarecSemanticClock.active = false;
	dynarecSemanticClock.cycles.reset();
}

void sh4DynarecTimingDiagnosticSetActive(bool active) noexcept
{
	dynarecTimingDiagnosticEnabled.store(active, std::memory_order_release);
	dynarecTimingDiagnosticActivationGeneration.fetch_add(1,
			std::memory_order_acq_rel);
	synchronizeDynarecTimingDiagnosticHistory();
}

Sh4DynarecTimingDiagnosticSnapshot sh4DynarecTimingDiagnosticSnapshot(
		std::uint64_t zeroBasedDmaOrdinal, std::uint64_t observedTick,
		std::uint64_t expectedTick)
{
	synchronizeDynarecTimingDiagnosticHistory();
	Sh4DynarecTimingDiagnosticSnapshot snapshot;
	snapshot.active = dynarecTimingDiagnosticHistory.active;
	snapshot.zeroBasedDmaOrdinal = zeroBasedDmaOrdinal;
	snapshot.observedTick = observedTick;
	snapshot.expectedTick = expectedTick;
	snapshot.nextSequence = dynarecTimingDiagnosticHistory.nextSequence;
	if (!snapshot.active)
		return snapshot;
	snapshot.records.reserve(dynarecTimingDiagnosticHistory.count);
	const std::size_t first = (dynarecTimingDiagnosticHistory.nextIndex
			+ Sh4DynarecTimingDiagnosticHistoryCapacity
			- dynarecTimingDiagnosticHistory.count)
			% Sh4DynarecTimingDiagnosticHistoryCapacity;
	for (std::size_t offset = 0;
			offset < dynarecTimingDiagnosticHistory.count; ++offset)
	{
		const Sh4DynarecTimingDiagnosticRecord& record =
				dynarecTimingDiagnosticHistory.records[(first + offset)
						% Sh4DynarecTimingDiagnosticHistoryCapacity];
		if (record.sequence != 0)
			snapshot.records.push_back(record);
	}
	return snapshot;
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
	interpreterSemanticClock.active = false;
}

} // namespace research

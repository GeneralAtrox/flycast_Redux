#include "research/sh4_observation_runtime_internal.h"
#include "research/sh4_pc_checkpoint_runtime.h"

#include "hw/sh4/sh4_if.h"
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

thread_local std::vector<EmissionInstructionFrame> instructionFrames;

namespace
{

std::array<std::atomic<std::size_t>, 2> instructionOwnershipCounts {};
std::atomic<std::uint64_t> nextInstructionOwnerGeneration {1};

bool isCallWithDelayedPrWrite(std::uint16_t opcode) noexcept
{
	return (opcode & 0xf000u) == 0xb000u
			|| (opcode & 0xf0ffu) == 0x0003u
			|| (opcode & 0xf0ffu) == 0x400bu;
}

} // namespace

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
	{
		sh4PcCheckpointInstructionEnd(backend, pc);
		return;
	}
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
			if (backend == Sh4ObservationBackend::Interpreter)
				recordInterpreterSemanticTick(tick);
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
	if (instructionFrames.empty())
		sh4PcCheckpointInstructionEnd(backend, pc);
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

} // namespace research

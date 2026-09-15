#include "research/sh4_observation_runtime_internal.h"
#include "research/pvr_presentation_observation.h"

#include "cfg/option.h"
#include "hw/sh4/sh4_if.h"
#include "log/Log.h"
#include "types.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace research
{

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
	dynarecExecutionTimingException(const_cast<Sh4Context&>(context),
			exceptionCode);
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
		for (const DynarecExecutionTimingFrame& frame :
				dynarecExecutionTiming.frames)
			completeDiagnosticInstruction(context, frame.diagnosticSequence,
					context.pc, 0,
					Sh4DynarecTimingDiagnosticState::Interrupt, interruptCode);
		dynarecExecutionTiming.frames.clear();
	}
	if (backend == Sh4ObservationBackend::Dynarec
			&& dynarecExecutionTiming.frames.empty())
		appendDiagnosticInterrupt(context, interruptCode, tick);
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
		{
			// UpdateINTC reports the scheduler boundary.  An observed dynarec
			// block can have completed an instruction a few cycles beyond that
			// boundary before returning to the scheduler, so the raw interrupt
			// tick must not precede the execution position already published by
			// the instruction stream.
			if (config::ResearchDynarecObservation.get())
				tick = std::max(tick, dynarecCurrentTick(context));
			tick = dynarecSemanticInterruptTick(tick);
		}
		else
			tick = interpreterSemanticInterruptTick(tick);
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

} // namespace research

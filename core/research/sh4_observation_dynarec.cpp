#include "research/sh4_observation_runtime_internal.h"
#include "research/pvr_presentation_observation.h"

#include "cfg/option.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/dyna/shil.h"
#include "log/Log.h"
#include "types.h"

#include <exception>
#include <limits>
#include <stdexcept>

namespace research
{
namespace
{

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
					? dynarecExecutionTimingBegin(*context, pc, opcode) : preciseClock;
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
			const std::uint32_t nextPc = markerNextPc(primaryNextPc, *context);
			const bool preciseInstruction = executionTiming
					? dynarecExecutionTimingEnd(*context, pc, opcode, nextPc)
					: preciseClock;
			if (executionTiming)
				markerTick = dynarecCurrentTick(*context);
			if (preciseInstruction)
				tick = dynarecSemanticEndTick(opcode, markerTick);
			else
				tick = markerTick;
			context->pc = nextPc;
			sh4ObservationInstructionEnd(Sh4ObservationBackend::Dynarec, pc,
					opcode, tick, *context);
			break;
		}
		case DynarecMarkerKind::ConditionalEnd:
		{
			const std::uint32_t nextPc = conditionalBranchTaken(opcode,
					context->sr.T) ? primaryNextPc : pc + 2u;
			const bool preciseInstruction = executionTiming
					? dynarecExecutionTimingEnd(*context, pc, opcode, nextPc)
					: preciseClock;
			if (executionTiming)
				markerTick = dynarecCurrentTick(*context);
			if (preciseInstruction)
				tick = dynarecSemanticEndTick(opcode, markerTick);
			else
				tick = markerTick;
			context->pc = nextPc;
			sh4ObservationInstructionEnd(Sh4ObservationBackend::Dynarec, pc,
					opcode, tick, *context);
			break;
		}
		case DynarecMarkerKind::ConditionalBeforeDelay:
			if (!conditionalBranchTaken(opcode, context->jdyn))
			{
				const std::uint32_t nextPc = pc + 2u;
				const bool preciseInstruction = executionTiming
						? dynarecExecutionTimingEnd(*context, pc, opcode, nextPc)
						: preciseClock;
				if (executionTiming)
					markerTick = dynarecCurrentTick(*context);
				if (preciseInstruction)
					tick = dynarecSemanticEndTick(opcode, markerTick);
				else
					tick = markerTick;
				context->pc = nextPc;
				sh4ObservationInstructionEnd(Sh4ObservationBackend::Dynarec, pc,
						opcode, tick, *context);
			}
			break;
		case DynarecMarkerKind::ConditionalAfterDelay:
			if (conditionalBranchTaken(opcode, context->jdyn))
			{
				const bool preciseInstruction = executionTiming
						? dynarecExecutionTimingEnd(*context, pc, opcode,
								primaryNextPc) : preciseClock;
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
		const std::uint8_t width = static_cast<std::uint8_t>(widthAndKind);
		if (width != 1 && width != 2 && width != 4 && width != 8)
			throw std::invalid_argument("invalid dynarec memory width");
		if (frame.memoryPending)
		{
			if (frame.nestedHardwareMemoryDepth
					== std::numeric_limits<std::uint32_t>::max())
				throw std::overflow_error("nested dynarec hardware memory depth overflow");
			++frame.nestedHardwareMemoryDepth;
			return;
		}
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
		if (frame.nestedHardwareMemoryDepth != 0)
		{
			--frame.nestedHardwareMemoryDepth;
			return;
		}
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

} // namespace research

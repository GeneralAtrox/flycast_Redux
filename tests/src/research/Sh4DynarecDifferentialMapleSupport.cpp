#include "Sh4DynarecDifferentialSupport.h"

#include "cfg/option.h"
#include "emulator.h"
#include "hw/mem/addrspace.h"
#include "hw/holly/sb.h"
#include "hw/sh4/modules/ccn.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_sched.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace sh4_dynarec_test
{

std::pair<std::vector<research::Sh4Observation>, MapleDmaTickSample>
runMapleMmioBackend(research::Sh4ObservationBackend backend)
{
	const bool dynarec = backend == research::Sh4ObservationBackend::Dynarec;
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	emu.dc_reset(true);
	NativeCaptureTimingGuard captureTiming(backend, false);
	configureMmu(false);
	for (std::uint32_t pc = StartPc; pc < MapleStorePc; pc += 2)
		addrspace::write16(pc, 0x0009); // accumulate within one scheduler slice
	addrspace::write16(MapleStorePc, 0x2102); // mov.l r0,@r1 -> SB_MDST
	addrspace::write16(MapleStorePc + 2u, 0xaffe); // bra MapleStorePc + 2
	addrspace::write16(MapleStorePc + 4u, 0x0009); // delay-slot nop
	addrspace::write32(DataAddress, 0x80000700u); // terminal Maple NOP
	addrspace::write32(DataAddress + 4u, 0);
	SB_MDSTAR = DataAddress;
	SB_MDEN = 1;
	SB_MDST = 0;
	SB_MMSEL = 1;
	Sh4cntx.pc = StartPc;
	Sh4cntx.r[0] = 1;
	Sh4cntx.r[1] = 0xa05f6c18u;
	Sh4cntx.cycle_counter = SH4_TIMESLICE;

	MapleDmaTickSample sample;
	MapleDmaTickObserverGuard observer(sample);
	std::vector<research::Sh4Observation> events;
	Sh4Executor *executor = emu.getSh4Executor();
	bool storeCompleted = false;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(backend);
	const research::Sh4ObservationSubscription subscription =
			research::subscribeSh4Observations(filter,
					[&events, &storeCompleted, executor](
							const research::Sh4Observation& event) {
						events.push_back(event);
						if (event.type == research::Sh4ObservationType::InstructionEnd
								&& event.instructionPc == MapleStorePc
								&& event.delaySlotDepth == 0)
						{
							storeCompleted = true;
							executor->Stop();
						}
					});
	executor->Start();
	executor->Run();
	EXPECT_TRUE(storeCompleted);
	EXPECT_TRUE(sample.captured);
	EXPECT_TRUE(research::unsubscribeSh4Observations(subscription));
	return {std::move(events), sample};
}

std::pair<std::vector<research::Sh4Observation>, MapleDmaTickSample>
runInterruptMapleMmioBackend(research::Sh4ObservationBackend backend)
{
	const bool dynarec = backend == research::Sh4ObservationBackend::Dynarec;
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	emu.dc_reset(true);
	NativeCaptureTimingGuard captureTiming(backend, false);
	configureMmu(false);
	writeProgram({0xaffe, 0x0009}); // bra StartPc; nop until interrupt entry
	for (std::uint32_t pc = InterruptHandlerPc;
			pc < InterruptMapleStorePc; pc += 2)
		addrspace::write16(pc, 0x0009); // handler work across more than 33 slices
	addrspace::write16(InterruptMapleStorePc,
			0x2102); // mov.l r0,@r1 -> SB_MDST
	addrspace::write16(InterruptMapleStorePc + 2u,
			0xaffe); // bra InterruptMapleStorePc + 2
	addrspace::write16(InterruptMapleStorePc + 4u, 0x0009); // delay-slot nop
	addrspace::write32(DataAddress, 0x80000700u); // terminal Maple NOP
	addrspace::write32(DataAddress + 4u, 0);
	SB_MDSTAR = DataAddress;
	SB_MDEN = 1;
	SB_MDST = 0;
	SB_MMSEL = 1;
	Sh4cntx.pc = StartPc;
	Sh4cntx.vbr = InterruptVectorBase;
	Sh4cntx.r[0] = 1;
	Sh4cntx.r[1] = 0xa05f6c18u;
	Sh4cntx.sr.BL = 0;
	Sh4cntx.sr.IMASK = 0;
	Sh4cntx.old_sr.status = Sh4cntx.sr.status;
	UpdateSR();
	SetInterruptMask(sh4_IRL_9);
	SetInterruptPend(sh4_IRL_9);
	Sh4cntx.cycle_counter = SH4_TIMESLICE;

	MapleDmaTickSample sample;
	MapleDmaTickObserverGuard observer(sample);
	std::vector<research::Sh4Observation> events;
	Sh4Executor *executor = emu.getSh4Executor();
	bool interruptSeen = false;
	bool storeCompleted = false;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(backend);
	const research::Sh4ObservationSubscription subscription =
			research::subscribeSh4Observations(filter,
					[&events, &interruptSeen, &storeCompleted, executor](
							const research::Sh4Observation& event) {
						events.push_back(event);
						if (event.type == research::Sh4ObservationType::Exception
								&& event.exceptionCode == Sh4Ex_ExtInterrupt9
								&& event.vectorPc == InterruptHandlerPc)
							interruptSeen = true;
						if (event.type
								== research::Sh4ObservationType::InstructionEnd
								&& event.instructionPc == InterruptMapleStorePc
								&& event.delaySlotDepth == 0)
						{
							storeCompleted = true;
							executor->Stop();
						}
					});
	executor->Start();
	executor->Run();
	EXPECT_TRUE(interruptSeen);
	EXPECT_TRUE(storeCompleted);
	EXPECT_TRUE(sample.captured);
	EXPECT_TRUE(research::unsubscribeSh4Observations(subscription));
	return {std::move(events), sample};
}

std::vector<research::Sh4Observation> runSelfModifyingBackend(
		research::Sh4ObservationBackend backend)
{
	const bool dynarec = backend == research::Sh4ObservationBackend::Dynarec;
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	emu.dc_reset(true);
	NativeCaptureTimingGuard captureTiming(backend);
	configureMmu(false);
	writeProgram({
			0x7501, // add #1,r5 (rewritten to add #2,r5)
			0xaffd, // bra StartPc
			0x0009, // delay-slot nop
	});
	Sh4cntx.pc = StartPc;
	Sh4cntx.cycle_counter = SH4_TIMESLICE;

	std::vector<research::Sh4Observation> events;
	Sh4Executor *executor = emu.getSh4Executor();
	std::size_t completedLoops = 0;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(backend);
	const research::Sh4ObservationSubscription subscription =
			research::subscribeSh4Observations(filter,
					[&events, &completedLoops, executor](
							const research::Sh4Observation& event) {
						events.push_back(event);
						if (event.type != research::Sh4ObservationType::InstructionEnd
								|| event.instructionPc != StartPc + 2u
								|| event.delaySlotDepth != 0)
							return;
						++completedLoops;
						if (completedLoops == 1)
							addrspace::write16(StartPc, 0x7502);
						else if (completedLoops == 3)
							executor->Stop();
					});
	executor->Start();
	executor->Run();
	EXPECT_EQ(completedLoops, 3u);
	EXPECT_TRUE(research::unsubscribeSh4Observations(subscription));
	std::vector<std::uint16_t> executedOpcodes;
	for (const research::Sh4Observation& event : events)
	{
		if (event.type == research::Sh4ObservationType::InstructionBegin
				&& event.instructionPc == StartPc)
			executedOpcodes.push_back(event.opcode);
	}
	EXPECT_EQ(executedOpcodes,
			(std::vector<std::uint16_t> {0x7501, 0x7502, 0x7502}));
	return events;
}

std::vector<research::Sh4Observation> runConditionalDelayBackend(
		research::Sh4ObservationBackend backend, bool taken)
{
	const bool dynarec = backend == research::Sh4ObservationBackend::Dynarec;
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	emu.dc_reset(true);
	NativeCaptureTimingGuard captureTiming(backend);
	configureMmu(false);
	writeProgram({
			taken ? std::uint16_t {0x0008} : std::uint16_t {0x0018}, // clrt/sett
			0x8f01, // bf/s StartPc + 8
			0x0009, // taken delay slot; ordinary nop when not taken
			0x7508, // fallthrough-only add #8,r5
			0x7501, // branch target add #1,r5
			0xaff9, // bra StartPc
			0x0009, // delay-slot nop
	});
	Sh4cntx.pc = StartPc;
	Sh4cntx.cycle_counter = SH4_TIMESLICE;

	std::vector<research::Sh4Observation> events;
	Sh4Executor *executor = emu.getSh4Executor();
	std::size_t completedLoops = 0;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(backend);
	const research::Sh4ObservationSubscription subscription =
			research::subscribeSh4Observations(filter,
					[&events, &completedLoops, executor](
							const research::Sh4Observation& event) {
						events.push_back(event);
						if (event.type == research::Sh4ObservationType::InstructionEnd
								&& event.instructionPc == StartPc + 0x0au
								&& event.delaySlotDepth == 0
								&& ++completedLoops == LoopCount)
							executor->Stop();
					});
	executor->Start();
	executor->Run();
	EXPECT_EQ(completedLoops, LoopCount);
	EXPECT_TRUE(research::unsubscribeSh4Observations(subscription));
	return events;
}

std::vector<research::Sh4Observation> runFallbackMemoryBackend(
		research::Sh4ObservationBackend backend)
{
	const bool dynarec = backend == research::Sh4ObservationBackend::Dynarec;
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	emu.dc_reset(true);
	NativeCaptureTimingGuard captureTiming(backend);
	configureMmu(false);
	writeProgram({
			0x09af, // mac.l @r10+,@r9+ (interpreter-fallback SHIL)
			0xaffd, // bra StartPc
			0x0009, // delay-slot nop
	});
	Sh4cntx.pc = StartPc;
	Sh4cntx.r[9] = DataAddress + 0x20;
	Sh4cntx.r[10] = DataAddress + 0x40;
	for (std::uint32_t index = 0; index < 3; ++index)
	{
		addrspace::write32(DataAddress + 0x20 + index * 4, index + 2);
		addrspace::write32(DataAddress + 0x40 + index * 4, index + 5);
	}
	Sh4cntx.cycle_counter = SH4_TIMESLICE;

	std::vector<research::Sh4Observation> events;
	Sh4Executor *executor = emu.getSh4Executor();
	std::size_t completedLoops = 0;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(backend);
	const research::Sh4ObservationSubscription subscription =
			research::subscribeSh4Observations(filter,
					[&events, &completedLoops, executor](
							const research::Sh4Observation& event) {
						events.push_back(event);
						if (event.type == research::Sh4ObservationType::InstructionEnd
								&& event.instructionPc == StartPc + 2u
								&& event.delaySlotDepth == 0
								&& ++completedLoops == 3)
							executor->Stop();
					});
	executor->Start();
	executor->Run();
	EXPECT_EQ(completedLoops, 3u);
	EXPECT_TRUE(research::unsubscribeSh4Observations(subscription));
	std::size_t reads = 0;
	for (const research::Sh4Observation& event : events)
	{
		if (event.type == research::Sh4ObservationType::MemoryRead)
		{
			++reads;
			EXPECT_EQ(event.memoryWidth, 4u);
		}
	}
	EXPECT_EQ(reads, 6u);
	return events;
}

void expectSameSemanticEvent(const research::Sh4Observation& interpreter,
		const research::Sh4Observation& dynarec, std::size_t index)
{
	SCOPED_TRACE(index);
	EXPECT_EQ(interpreter.schemaVersion, dynarec.schemaVersion);
	EXPECT_EQ(interpreter.availableFields, dynarec.availableFields);
	EXPECT_EQ(interpreter.type, dynarec.type);
	EXPECT_EQ(interpreter.tick, dynarec.tick);
	EXPECT_EQ(interpreter.instructionPc, dynarec.instructionPc);
	EXPECT_EQ(interpreter.nextPc, dynarec.nextPc);
	EXPECT_EQ(interpreter.opcode, dynarec.opcode);
	EXPECT_EQ(interpreter.delaySlotDepth, dynarec.delaySlotDepth);
	EXPECT_EQ(interpreter.registers.r, dynarec.registers.r);
	EXPECT_EQ(interpreter.registers.pr, dynarec.registers.pr);
	EXPECT_EQ(interpreter.registers.gbr, dynarec.registers.gbr);
	EXPECT_EQ(interpreter.registers.vbr, dynarec.registers.vbr);
	EXPECT_EQ(interpreter.registers.mach, dynarec.registers.mach);
	EXPECT_EQ(interpreter.registers.macl, dynarec.registers.macl);
	EXPECT_EQ(interpreter.registers.sr, dynarec.registers.sr);
	EXPECT_EQ(interpreter.registers.fpul, dynarec.registers.fpul);
	EXPECT_EQ(interpreter.registers.fpscr, dynarec.registers.fpscr);
	EXPECT_EQ(interpreter.memoryAddress, dynarec.memoryAddress);
	EXPECT_EQ(interpreter.memoryWidth, dynarec.memoryWidth);
	EXPECT_EQ(interpreter.memoryValue, dynarec.memoryValue);
	EXPECT_EQ(interpreter.exceptionPc, dynarec.exceptionPc);
	EXPECT_EQ(interpreter.vectorPc, dynarec.vectorPc);
	EXPECT_EQ(interpreter.exceptionCode, dynarec.exceptionCode);
	EXPECT_EQ(interpreter.callKind, dynarec.callKind);
	EXPECT_EQ(interpreter.targetPc, dynarec.targetPc);
	EXPECT_EQ(interpreter.returnPc, dynarec.returnPc);
	EXPECT_EQ(interpreter.delaySlotPc, dynarec.delaySlotPc);
}

void expectSameSemanticStream(
		const std::vector<research::Sh4Observation>& interpreter,
		const std::vector<research::Sh4Observation>& dynarec)
{
	ASSERT_FALSE(interpreter.empty());
	for (std::size_t index = 0;
			index < std::min(interpreter.size(), dynarec.size()); ++index)
	{
		if (interpreter[index].tick != dynarec[index].tick)
		{
			ADD_FAILURE() << "first tick divergence at event " << index
					<< ": interpreter(type="
					<< static_cast<unsigned>(interpreter[index].type)
					<< ", pc=0x" << std::hex << interpreter[index].instructionPc
					<< ", tick=" << std::dec << interpreter[index].tick
					<< "), dynarec(type="
					<< static_cast<unsigned>(dynarec[index].type)
					<< ", pc=0x" << std::hex << dynarec[index].instructionPc
					<< ", tick=" << std::dec << dynarec[index].tick << ')';
			return;
		}
		expectSameSemanticEvent(interpreter[index], dynarec[index], index);
	}
	ASSERT_EQ(interpreter.size(), dynarec.size());
}

} // namespace sh4_dynarec_test

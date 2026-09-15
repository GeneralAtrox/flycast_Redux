#include "Sh4DynarecDifferentialSupport.h"

#include "cfg/option.h"
#include "emulator.h"
#include "hw/mem/addrspace.h"
#include "hw/holly/sb.h"
#include "hw/maple/maple_if.h"
#include "hw/sh4/modules/ccn.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "oslib/oslib.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>

namespace sh4_dynarec_test
{

namespace
{

MapleDmaTickSample *activeMapleDmaTickSample = nullptr;

void captureMapleDmaTick(std::uint64_t rawTick, std::uint64_t executionTick,
		std::uint32_t ownerPc, std::uint16_t ownerOpcode, bool ownerValid)
{
	if (activeMapleDmaTickSample == nullptr)
		return;
	*activeMapleDmaTickSample = {rawTick, executionTick, ownerPc, ownerOpcode,
			ownerValid, true};
}

} // namespace

MapleDmaTickObserverGuard::MapleDmaTickObserverGuard(
		MapleDmaTickSample& sample)
{
	activeMapleDmaTickSample = &sample;
	research_test::setMapleDmaTickObserver(captureMapleDmaTick);
}

MapleDmaTickObserverGuard::~MapleDmaTickObserverGuard()
{
	research_test::setMapleDmaTickObserver(nullptr);
	activeMapleDmaTickSample = nullptr;
}

void writeProgram(std::initializer_list<std::uint16_t> opcodes)
{
	std::uint32_t pc = StartPc;
	for (const std::uint16_t opcode : opcodes)
	{
		addrspace::write16(pc, opcode);
		pc += 2;
	}
}

void configureMmu(bool fullMmu)
{
	CCN_MMUCR.AT = 0;
	MMU_reset();
	if (fullMmu)
	{
		constexpr std::uint8_t WindowsCeMagic[] = {
				'S', 0, 'H', 0, '-', 0, '4', 0, ' ', 0, 'K', 0, 'e', 0,
				'r', 0, 'n', 0, 'e', 0, 'l', 0,
		};
		for (std::size_t index = 0; index < std::size(WindowsCeMagic); ++index)
			addrspace::write8(0x8c0110a8u + static_cast<std::uint32_t>(index),
					WindowsCeMagic[index]);
		CCN_MMUCR.AT = 1;
		MMU_reset();
	}
	EXPECT_EQ(mmu_enabled(), fullMmu);
}

NativeCaptureTimingGuard::NativeCaptureTimingGuard(
		research::Sh4ObservationBackend backend, bool precise)
	: backend(backend), precise(precise)
{
	research::sh4DynarecExecutionTimingReset();
	research::sh4ObservationSetPreciseTiming(backend, precise);
}

NativeCaptureTimingGuard::~NativeCaptureTimingGuard()
{
	if (precise)
		research::sh4ObservationSetPreciseTiming(backend, false);
	research::sh4DynarecExecutionTimingReset();
}

std::vector<research::Sh4Observation> runBackend(
		research::Sh4ObservationBackend backend, bool fullMmu,
		bool precise)
{
	const bool dynarec = backend == research::Sh4ObservationBackend::Dynarec;
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	emu.dc_reset(true);
	NativeCaptureTimingGuard captureTiming(backend, precise);
	configureMmu(fullMmu);

	// Three exact-width stores and reads, a static call/return pair with delay
	// slots, then a block-ending backward branch. The loop runs for precisely
	// one scheduler slice in both backends.
	writeProgram({
			0x2100, // mov.b r0,@r1
			0x2101, // mov.w r0,@r1
			0x2102, // mov.l r0,@r1
			0x6210, // mov.b @r1,r2
			0x6311, // mov.w @r1,r3
			0x6412, // mov.l @r1,r4
			0xf80a, // fmov dr0,@r8 (FPSCR.SZ=1)
			0xf288, // fmov @r8,dr1 (FPSCR.SZ=1)
			0xb006, // bsr StartPc + 0x20
			0x0009, // delay-slot nop
			0x0603, // bsrf r6 -> StartPc + 0x26
			0x0009, // delay-slot nop
			0x470b, // jsr @r7 -> StartPc + 0x2c
			0x0009, // delay-slot nop
			0xaff2, // bra StartPc
			0x0009, // delay-slot nop
			0x7501, // add #1,r5
			0x000b, // rts
			0x0009, // delay-slot nop
			0x7502, // add #2,r5
			0x000b, // rts
			0x0009, // delay-slot nop
			0x7504, // add #4,r5
			0x000b, // rts
			0x0009, // delay-slot nop
	});
	Sh4cntx.pc = StartPc;
	Sh4cntx.r[0] = 0x12345678;
	Sh4cntx.r[1] = DataAddress;
	Sh4cntx.r[6] = 0x0e;
	Sh4cntx.r[7] = StartPc + 0x2c;
	Sh4cntx.r[8] = DataAddress + 8;
	Sh4cntx.dr_hex(0) = 0x8877665544332211ull;
	Sh4cntx.fpscr.SZ = 1;
	Sh4cntx.old_fpscr = Sh4cntx.fpscr;
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
								&& event.instructionPc == LoopEndPc
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

std::vector<research::Sh4Observation> runFaultBackend(
		research::Sh4ObservationBackend backend, bool delaySlot)
{
	const bool dynarec = backend == research::Sh4ObservationBackend::Dynarec;
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	emu.dc_reset(true);
	NativeCaptureTimingGuard captureTiming(backend);
	configureMmu(true);
	UTLB[0].Address.VPN = 0x02001000u >> 10;
	UTLB[0].Data.SZ0 = 1;
	UTLB[0].Data.V = 1;
	UTLB[0].Data.PR = 3;
	UTLB[0].Data.D = 1;
	UTLB[0].Data.PPN = 0x0c001000u >> 10;
	UTLB_Sync(0);
	if (delaySlot)
		writeProgram({0xaffe, 0x6011}); // bra StartPc; mov.w @r1,r0
	else
		writeProgram({0x6011}); // mov.w @r1,r0
	Sh4cntx.pc = StartPc;
	Sh4cntx.r[1] = 0x02101000;
	Sh4cntx.vbr = 0x8c002000;
	Sh4cntx.sr.BL = 0;
	Sh4cntx.old_sr.status = Sh4cntx.sr.status;
	UpdateSR();
	Sh4cntx.cycle_counter = SH4_TIMESLICE;

	std::vector<research::Sh4Observation> events;
	Sh4Executor *executor = emu.getSh4Executor();
	bool exceptionSeen = false;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(backend);
	const research::Sh4ObservationSubscription subscription =
			research::subscribeSh4Observations(filter,
					[&events, &exceptionSeen, executor](
							const research::Sh4Observation& event) {
						events.push_back(event);
						if (event.type == research::Sh4ObservationType::Exception)
						{
							exceptionSeen = true;
							executor->Stop();
						}
					});
	executor->Start();
	executor->Run();
	EXPECT_TRUE(exceptionSeen);
	EXPECT_TRUE(research::unsubscribeSh4Observations(subscription));
	return events;
}

std::vector<research::Sh4Observation> runInterruptBackend(
		research::Sh4ObservationBackend backend, bool precise)
{
	const bool dynarec = backend == research::Sh4ObservationBackend::Dynarec;
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	emu.dc_reset(true);
	NativeCaptureTimingGuard captureTiming(backend, precise);
	configureMmu(false);
	writeProgram({0xaffe, 0x0009}); // bra StartPc; nop
	Sh4cntx.pc = StartPc;
	Sh4cntx.vbr = 0x8c002000;
	Sh4cntx.sr.BL = 0;
	Sh4cntx.sr.IMASK = 0;
	Sh4cntx.old_sr.status = Sh4cntx.sr.status;
	UpdateSR();
	SetInterruptMask(sh4_IRL_9);
	SetInterruptPend(sh4_IRL_9);
	Sh4cntx.cycle_counter = SH4_TIMESLICE;

	std::vector<research::Sh4Observation> events;
	Sh4Executor *executor = emu.getSh4Executor();
	bool interruptSeen = false;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(backend);
	const research::Sh4ObservationSubscription subscription =
			research::subscribeSh4Observations(filter,
					[&events, &interruptSeen, executor](
							const research::Sh4Observation& event) {
						events.push_back(event);
						if (event.type == research::Sh4ObservationType::Exception
								&& event.delaySlotDepth == 0
								&& event.exceptionCode == Sh4Ex_ExtInterrupt9
								&& event.vectorPc == Sh4cntx.vbr + 0x600u)
						{
							interruptSeen = true;
							executor->Stop();
						}
					});
	executor->Start();
	executor->Run();
	EXPECT_TRUE(interruptSeen);
	EXPECT_TRUE(research::unsubscribeSh4Observations(subscription));
	return events;
}

std::vector<research::Sh4Observation> runRteInterruptBackend(
		research::Sh4ObservationBackend backend, bool precise)
{
	const bool dynarec = backend == research::Sh4ObservationBackend::Dynarec;
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	emu.dc_reset(true);
	NativeCaptureTimingGuard captureTiming(backend, precise);
	configureMmu(false);
	writeProgram({0x002b, 0x0009}); // rte; nop
	addrspace::write16(StartPc + 0x100, 0xaffe); // bra resume PC
	addrspace::write16(StartPc + 0x102, 0x0009); // delay-slot nop
	Sh4cntx.pc = StartPc;
	Sh4cntx.vbr = 0x8c002000;
	Sh4cntx.spc = StartPc + 0x100;
	Sh4cntx.ssr = Sh4cntx.sr.getFull();
	Sh4cntx.ssr &= ~(1u << 28); // restored SR.BL = 0
	Sh4cntx.ssr &= ~(0xfu << 4); // restored SR.IMASK = 0
	Sh4cntx.sr.BL = 1;
	Sh4cntx.old_sr.status = Sh4cntx.sr.status;
	UpdateSR();
	SetInterruptMask(sh4_IRL_9);
	SetInterruptPend(sh4_IRL_9);
	Sh4cntx.cycle_counter = SH4_TIMESLICE;

	std::vector<research::Sh4Observation> events;
	Sh4Executor *executor = emu.getSh4Executor();
	bool interruptSeen = false;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(backend);
	const research::Sh4ObservationSubscription subscription =
			research::subscribeSh4Observations(filter,
					[&events, &interruptSeen, executor](
							const research::Sh4Observation& event) {
						events.push_back(event);
						if (event.type == research::Sh4ObservationType::Exception
								&& event.delaySlotDepth == 0
								&& event.exceptionCode == Sh4Ex_ExtInterrupt9
								&& event.vectorPc == Sh4cntx.vbr + 0x600u
								&& event.exceptionPc == StartPc + 0x100u)
						{
							interruptSeen = true;
							executor->Stop();
						}
					});
	executor->Start();
	executor->Run();
	EXPECT_TRUE(interruptSeen);
	EXPECT_TRUE(research::unsubscribeSh4Observations(subscription));
	return events;
}

} // namespace sh4_dynarec_test

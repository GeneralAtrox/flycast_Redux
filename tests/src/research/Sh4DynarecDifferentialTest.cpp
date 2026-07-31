#include "cfg/option.h"
#include "emulator.h"
#include "hw/mem/addrspace.h"
#include "hw/sh4/modules/ccn.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "oslib/oslib.h"
#include "research/sh4_observation.h"
#include "research/sh4_observation_compare.h"
#include "research/sh4_observation_runtime.h"
#include "research/sh4_observation_trace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace
{

constexpr std::uint32_t StartPc = 0xac000000;
constexpr std::uint32_t DataAddress = 0x8c001000;
constexpr std::uint32_t LoopEndPc = StartPc + 0x1c;
constexpr std::size_t LoopCount = 8;

class TemporaryTraceDirectory
{
public:
	TemporaryTraceDirectory()
	{
		static std::atomic<unsigned> sequence {0};
		const auto processId =
#ifdef _WIN32
				_getpid();
#else
				getpid();
#endif
		path = std::filesystem::temp_directory_path()
				/ ("flycast-sh4-real-differential-"
						+ std::to_string(processId) + "-"
						+ std::to_string(sequence.fetch_add(1)));
		std::error_code error;
		std::filesystem::remove_all(path, error);
		std::filesystem::create_directories(path);
	}

	~TemporaryTraceDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	std::filesystem::path file(const char *name) const { return path / name; }

private:
	std::filesystem::path path;
};

research::Sh4ObservationTraceBinding traceBinding(
		research::Sh4ObservationBackend backend)
{
	research::Sh4ObservationTraceBinding binding;
	binding.backend = backend;
	const std::string identity = backend
			== research::Sh4ObservationBackend::Interpreter
			? "real-interpreter-fixture-identity" : "real-dynarec-fixture-identity";
	binding.identityDigest = research::sha256(identity.data(), identity.size());
	constexpr char Replay[] = "real-differential-replay";
	constexpr char Manifests[] = "real-differential-manifest-set";
	binding.replayDigest = research::sha256(Replay, sizeof(Replay) - 1);
	binding.manifestSetDigest = research::sha256(Manifests,
			sizeof(Manifests) - 1);
	return binding;
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

class NativeCaptureTimingGuard
{
public:
	explicit NativeCaptureTimingGuard(
			research::Sh4ObservationBackend backend) : backend(backend)
	{
		research::sh4DynarecExecutionTimingReset();
		research::sh4ObservationSetPreciseTiming(backend, true);
	}

	~NativeCaptureTimingGuard()
	{
		research::sh4ObservationSetPreciseTiming(backend, false);
		research::sh4DynarecExecutionTimingReset();
	}

private:
	research::Sh4ObservationBackend backend;
};

std::vector<research::Sh4Observation> runBackend(
		research::Sh4ObservationBackend backend, bool fullMmu)
{
	const bool dynarec = backend == research::Sh4ObservationBackend::Dynarec;
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	emu.dc_reset(true);
	NativeCaptureTimingGuard captureTiming(backend);
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
		research::Sh4ObservationBackend backend)
{
	const bool dynarec = backend == research::Sh4ObservationBackend::Dynarec;
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	emu.dc_reset(true);
	NativeCaptureTimingGuard captureTiming(backend);
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
								&& event.opcode == 0
								&& event.delaySlotDepth == 0)
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

void expectIndependentEquivalent(
		const std::vector<research::Sh4Observation>& interpreter,
		const std::vector<research::Sh4Observation>& dynarec)
{
	TemporaryTraceDirectory temporary;
	const std::filesystem::path interpreterPath = temporary.file("interpreter.fcso");
	const std::filesystem::path dynarecPath = temporary.file("dynarec.fcso");
	const auto interpreterBinding = traceBinding(
			research::Sh4ObservationBackend::Interpreter);
	const auto dynarecBinding = traceBinding(
			research::Sh4ObservationBackend::Dynarec);
	{
		research::Sh4ObservationTraceWriter writer(interpreterPath,
				interpreterBinding);
		for (const research::Sh4Observation& event : interpreter)
			writer.write(event);
		writer.finalize();
	}
	{
		research::Sh4ObservationTraceWriter writer(dynarecPath, dynarecBinding);
		for (const research::Sh4Observation& event : dynarec)
			writer.write(event);
		writer.finalize();
	}
	research::Sh4ObservationEquivalenceContract contract;
	contract.interpreterIdentityDigest = interpreterBinding.identityDigest;
	contract.dynarecIdentityDigest = dynarecBinding.identityDigest;
	contract.replayDigest = interpreterBinding.replayDigest;
	contract.manifestSetDigest = interpreterBinding.manifestSetDigest;
	const research::Sh4ObservationComparison comparison =
			research::compareSh4ObservationTraces(interpreterPath, dynarecPath,
					contract);
	EXPECT_TRUE(comparison.equivalent);
	EXPECT_FALSE(comparison.firstDivergence.has_value());
	EXPECT_EQ(comparison.matchedEventCount, interpreter.size());
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
	expectIndependentEquivalent(interpreter, dynarec);
}

} // namespace

TEST(ResearchSh4DynarecDifferential,
		BlockBoundaryCallReturnMemoryAndSchedulerStreamMatchesInterpreter)
{
	if (!addrspace::reserve())
		GTEST_SKIP() << "address-space reservation is unavailable";
	config::DynarecEnabled.override(true);
	os_InstallFaultHandler();
	emu.init();
	mem_map_default();
	for (const bool fullMmu : {false, true})
	{
		SCOPED_TRACE(fullMmu ? "full MMU" : "fastmem");
		const std::vector<research::Sh4Observation> interpreter = runBackend(
				research::Sh4ObservationBackend::Interpreter, fullMmu);
		const std::vector<research::Sh4Observation> dynarec = runBackend(
				research::Sh4ObservationBackend::Dynarec, fullMmu);
		expectSameSemanticStream(interpreter, dynarec);
	}
	for (const bool delaySlot : {false, true})
	{
		SCOPED_TRACE(delaySlot ? "delay-slot fault" : "synchronous fault");
		const std::vector<research::Sh4Observation> interpreter =
				runFaultBackend(research::Sh4ObservationBackend::Interpreter,
						delaySlot);
		const std::vector<research::Sh4Observation> dynarec =
				runFaultBackend(research::Sh4ObservationBackend::Dynarec,
						delaySlot);
		expectSameSemanticStream(interpreter, dynarec);
	}
	{
		SCOPED_TRACE("scheduler-boundary interrupt");
		const std::vector<research::Sh4Observation> interpreter =
				runInterruptBackend(research::Sh4ObservationBackend::Interpreter);
		const std::vector<research::Sh4Observation> dynarec =
				runInterruptBackend(research::Sh4ObservationBackend::Dynarec);
		expectSameSemanticStream(interpreter, dynarec);
	}
	{
		SCOPED_TRACE("self-modifying code invalidation");
		const std::vector<research::Sh4Observation> interpreter =
				runSelfModifyingBackend(
						research::Sh4ObservationBackend::Interpreter);
		const std::vector<research::Sh4Observation> dynarec =
				runSelfModifyingBackend(research::Sh4ObservationBackend::Dynarec);
		expectSameSemanticStream(interpreter, dynarec);
	}
	for (const bool taken : {false, true})
	{
		SCOPED_TRACE(taken ? "BF/S taken" : "BF/S not taken");
		const std::vector<research::Sh4Observation> interpreter =
				runConditionalDelayBackend(
						research::Sh4ObservationBackend::Interpreter, taken);
		const std::vector<research::Sh4Observation> dynarec =
				runConditionalDelayBackend(
						research::Sh4ObservationBackend::Dynarec, taken);
		expectSameSemanticStream(interpreter, dynarec);
	}
	{
		SCOPED_TRACE("interpreter-fallback ordered memory reads");
		const std::vector<research::Sh4Observation> interpreter =
				runFallbackMemoryBackend(
						research::Sh4ObservationBackend::Interpreter);
		const std::vector<research::Sh4Observation> dynarec =
				runFallbackMemoryBackend(
						research::Sh4ObservationBackend::Dynarec);
		expectSameSemanticStream(interpreter, dynarec);
	}
	config::ResearchDynarecObservation.override(false);
	config::DynarecEnabled.override(false);
	os_UninstallFaultHandler();
}

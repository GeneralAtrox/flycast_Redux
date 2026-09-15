#include "Sh4DynarecDifferentialSupport.h"

#include "cfg/option.h"
#include "emulator.h"
#include "hw/mem/addrspace.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "oslib/oslib.h"
#include "research/sh4_observation_runtime.h"

#include <gtest/gtest.h>

#include <vector>

using namespace sh4_dynarec_test;

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

TEST(ResearchSh4DynarecDifferential,
		WarmupBlockBoundaryAndSchedulerStreamMatchesInterpreter)
{
	if (!addrspace::reserve())
		GTEST_SKIP() << "address-space reservation is unavailable";
	config::DynarecEnabled.override(true);
	os_InstallFaultHandler();
	emu.init();
	mem_map_default();
	const std::vector<research::Sh4Observation> interpreter = runBackend(
			research::Sh4ObservationBackend::Interpreter, false, false);
	const std::vector<research::Sh4Observation> dynarec = runBackend(
			research::Sh4ObservationBackend::Dynarec, false, false);
	expectSameSemanticStream(interpreter, dynarec);
	const std::vector<research::Sh4Observation> interruptInterpreter =
			runInterruptBackend(research::Sh4ObservationBackend::Interpreter,
					false);
	const std::vector<research::Sh4Observation> interruptDynarec =
			runInterruptBackend(research::Sh4ObservationBackend::Dynarec, false);
	expectSameSemanticStream(interruptInterpreter, interruptDynarec);
	const std::vector<research::Sh4Observation> rteInterpreter =
			runRteInterruptBackend(research::Sh4ObservationBackend::Interpreter,
					false);
	const std::vector<research::Sh4Observation> rteDynarec =
			runRteInterruptBackend(research::Sh4ObservationBackend::Dynarec, false);
	expectSameSemanticStream(rteInterpreter, rteDynarec);
	const auto [mapleInterpreterEvents, mapleInterpreterSample] =
			runMapleMmioBackend(research::Sh4ObservationBackend::Interpreter);
	const auto [mapleDynarecEvents, mapleDynarecSample] =
			runMapleMmioBackend(research::Sh4ObservationBackend::Dynarec);
	expectSameSemanticStream(mapleInterpreterEvents, mapleDynarecEvents);
	EXPECT_TRUE(mapleInterpreterSample.ownerValid);
	EXPECT_TRUE(mapleDynarecSample.ownerValid);
	EXPECT_EQ(mapleInterpreterSample.ownerPc, MapleStorePc);
	EXPECT_EQ(mapleDynarecSample.ownerPc, MapleStorePc);
	EXPECT_EQ(mapleInterpreterSample.ownerOpcode, 0x2102u);
	EXPECT_EQ(mapleDynarecSample.ownerOpcode, 0x2102u);
	EXPECT_EQ(mapleInterpreterSample.rawTick,
			mapleInterpreterSample.executionTick);
	EXPECT_EQ(mapleInterpreterSample.rawTick, mapleDynarecSample.rawTick);
	EXPECT_GT(mapleDynarecSample.executionTick, mapleDynarecSample.rawTick);
	EXPECT_LT(mapleDynarecSample.executionTick,
			mapleDynarecSample.rawTick + SH4_TIMESLICE);
	const auto [interruptMapleInterpreterEvents,
			interruptMapleInterpreterSample] = runInterruptMapleMmioBackend(
			research::Sh4ObservationBackend::Interpreter);
	const auto [interruptMapleDynarecEvents, interruptMapleDynarecSample] =
			runInterruptMapleMmioBackend(
					research::Sh4ObservationBackend::Dynarec);
	expectSameSemanticStream(interruptMapleInterpreterEvents,
			interruptMapleDynarecEvents);
	EXPECT_TRUE(interruptMapleInterpreterSample.ownerValid);
	EXPECT_TRUE(interruptMapleDynarecSample.ownerValid);
	EXPECT_EQ(interruptMapleInterpreterSample.ownerPc,
			InterruptMapleStorePc);
	EXPECT_EQ(interruptMapleDynarecSample.ownerPc, InterruptMapleStorePc);
	EXPECT_EQ(interruptMapleInterpreterSample.ownerOpcode, 0x2102u);
	EXPECT_EQ(interruptMapleDynarecSample.ownerOpcode, 0x2102u);
	EXPECT_EQ(interruptMapleInterpreterSample.rawTick,
			interruptMapleInterpreterSample.executionTick);
	EXPECT_EQ(interruptMapleInterpreterSample.rawTick,
			interruptMapleDynarecSample.rawTick);
	EXPECT_GT(interruptMapleDynarecSample.executionTick,
			interruptMapleDynarecSample.rawTick);
	EXPECT_LT(interruptMapleDynarecSample.executionTick,
			interruptMapleDynarecSample.rawTick + SH4_TIMESLICE);
	config::ResearchDynarecObservation.override(false);
	config::DynarecEnabled.override(false);
	os_UninstallFaultHandler();
}

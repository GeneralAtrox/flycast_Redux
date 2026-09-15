#include "Sh4ObservationTestSupport.h"

TEST(ResearchSh4Observation,
		ResearchDynarecTimingChangesOnlyAtNativeCaptureBoundary)
{
	using Backend = research::Sh4ObservationBackend;
	config::ResearchDynarecObservation.override(true);
	research::sh4DynarecExecutionTimingReset();
	struct Cleanup
	{
		~Cleanup()
		{
			research::sh4ObservationResetPreciseTiming();
			research::sh4DynarecExecutionTimingReset();
			config::ResearchDynarecObservation.override(false);
		}
	} cleanup;
	Sh4Context context {};
	context.cycle_counter = 1000;

#ifdef STRICT_MODE
	Sh4Cycles warmupCycles {1};
#else
	Sh4Cycles warmupCycles {8};
#endif
	const int warmupDebit = warmupCycles.countCycles(0x0009);
	shil_opcode begin {};
	begin.op = shop_research_begin;
	begin.rs1 = shil_param(0x8c010000);
	begin.rs2 = shil_param(0x0009);
	shil_opcode end {};
	end.op = shop_research_end;
	end.rs1 = begin.rs1;
	end.rs2 = begin.rs2;
	end.rs3 = shil_param(0x8c010002);
	invokeDynarecMarker(context, begin);
	invokeDynarecMarker(context, end);
	EXPECT_EQ(1000 - warmupDebit, context.cycle_counter);

	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Cycles preciseCycles {1};
	const int preciseDebit = preciseCycles.countCycles(0x0009);
	begin.rs1 = shil_param(0x8c010002);
	end.rs1 = begin.rs1;
	end.rs3 = shil_param(0x8c010004);
	invokeDynarecMarker(context, begin);
	invokeDynarecMarker(context, end);
	EXPECT_EQ(1000 - 2 * warmupDebit, context.cycle_counter);
	ASSERT_EQ(2u, observed.size());
	EXPECT_EQ(research::Sh4ObservationType::InstructionBegin, observed[0].type);
	EXPECT_EQ(research::Sh4ObservationType::InstructionEnd, observed[1].type);

	research::sh4ObservationSetPreciseTiming(Backend::Dynarec, true);
	begin.rs1 = shil_param(0x8c010004);
	end.rs1 = begin.rs1;
	end.rs3 = shil_param(0x8c010006);
	invokeDynarecMarker(context, begin);
	invokeDynarecMarker(context, end);
	EXPECT_EQ(1000 - 2 * warmupDebit - preciseDebit, context.cycle_counter);

}

TEST(ResearchSh4Observation,
		DynarecTimingDiagnosticRetainsBoundedCompletedAndOpenHistory)
{
	config::ResearchDynarecObservation.override(true);
	research::sh4DynarecExecutionTimingReset();
	research::sh4DynarecTimingDiagnosticSetActive(true);
	struct Cleanup
	{
		~Cleanup()
		{
			research::sh4DynarecTimingDiagnosticSetActive(false);
			research::sh4DynarecExecutionTimingReset();
			config::ResearchDynarecObservation.override(false);
		}
	} cleanup;

	Sh4Context context {};
	context.cycle_counter = 1000;
	shil_opcode begin {};
	begin.op = shop_research_begin;
	begin.rs2 = shil_param(0x0009);
	shil_opcode end {};
	end.op = shop_research_end;
	end.rs2 = begin.rs2;
	for (std::size_t index = 0;
			index < research::Sh4DynarecTimingDiagnosticHistoryCapacity + 4;
			++index)
	{
		begin.rs1 = shil_param(0x8c010000u + static_cast<std::uint32_t>(index * 2));
		end.rs1 = begin.rs1;
		end.rs3 = shil_param(begin.rs1._imm + 2u);
		invokeDynarecMarker(context, begin);
		invokeDynarecMarker(context, end);
	}
	begin.rs1 = shil_param(0x8c020000);
	invokeDynarecMarker(context, begin);

	const auto snapshot = research::sh4DynarecTimingDiagnosticSnapshot(
			1209, 100, 548);
	ASSERT_TRUE(snapshot.active);
	EXPECT_EQ(1209u, snapshot.zeroBasedDmaOrdinal);
	EXPECT_EQ(100u, snapshot.observedTick);
	EXPECT_EQ(548u, snapshot.expectedTick);
	ASSERT_EQ(research::Sh4DynarecTimingDiagnosticHistoryCapacity,
			snapshot.records.size());
	EXPECT_EQ(6u, snapshot.records.front().sequence);
	EXPECT_EQ(research::Sh4DynarecTimingDiagnosticState::Completed,
			snapshot.records.front().state);
	EXPECT_EQ(261u, snapshot.records.back().sequence);
	EXPECT_EQ(research::Sh4DynarecTimingDiagnosticState::Open,
			snapshot.records.back().state);
	EXPECT_EQ(0x8c020000u, snapshot.records.back().pc);
	EXPECT_EQ(0, snapshot.records.back().instructionCycles);
}

TEST(ResearchSh4Observation,
		DynarecTimingDiagnosticRecordsInterruptBoundaryWithoutSubscriber)
{
	using Backend = research::Sh4ObservationBackend;
	config::ResearchDynarecObservation.override(true);
	research::sh4DynarecExecutionTimingReset();
	research::sh4DynarecTimingDiagnosticSetActive(true);
	struct Cleanup
	{
		~Cleanup()
		{
			research::sh4DynarecTimingDiagnosticSetActive(false);
			research::sh4DynarecExecutionTimingReset();
			config::ResearchDynarecObservation.override(false);
		}
	} cleanup;
	Sh4Context context {};
	context.pc = 0x8c020000;
	context.cycle_counter = SH4_TIMESLICE - 8;
	research::sh4ObservationInterruptRaised(Backend::Dynarec, 0x320, 123,
			context);

	const auto snapshot = research::sh4DynarecTimingDiagnosticSnapshot(
			1216, 1000, 552);
	ASSERT_EQ(1u, snapshot.records.size());
	EXPECT_EQ(research::Sh4DynarecTimingDiagnosticState::Interrupt,
			snapshot.records[0].state);
	EXPECT_EQ(0x320u, snapshot.records[0].boundaryCode);
	EXPECT_EQ(0x8c020000u, snapshot.records[0].pc);
}

TEST(ResearchSh4Observation,
		DynarecTimingDiagnosticActivationCrossesThreadBoundaryAndResetsGeneration)
{
	config::ResearchDynarecObservation.override(true);
	research::sh4DynarecTimingDiagnosticSetActive(true);
	struct Cleanup
	{
		~Cleanup()
		{
			research::sh4DynarecTimingDiagnosticSetActive(false);
			config::ResearchDynarecObservation.override(false);
		}
	} cleanup;

	auto first = std::async(std::launch::async, [] {
		research::sh4DynarecExecutionTimingReset();
		Sh4Context context {};
		context.cycle_counter = 1000;
		shil_opcode begin {};
		begin.op = shop_research_begin;
		begin.rs1 = shil_param(0x8c010000);
		begin.rs2 = shil_param(0x0009);
		invokeDynarecMarker(context, begin);
		return research::sh4DynarecTimingDiagnosticSnapshot(1209, 1, 2);
	}).get();
	ASSERT_TRUE(first.active);
	ASSERT_EQ(1u, first.records.size());
	EXPECT_EQ(0x8c010000u, first.records[0].pc);

	research::sh4DynarecTimingDiagnosticSetActive(false);
	auto inactive = std::async(std::launch::async, [] {
		return research::sh4DynarecTimingDiagnosticSnapshot(1216, 3, 4);
	}).get();
	EXPECT_FALSE(inactive.active);
	EXPECT_TRUE(inactive.records.empty());
}

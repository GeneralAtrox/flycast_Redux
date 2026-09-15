#include "Sh4ObservationTestSupport.h"

TEST(ResearchSh4Observation, DynarecExceptionOwnsAndClosesItsFrame)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&observed](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.pc = 0x8c010002;
	context.vbr = 0x8c000000;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x0009, 20, context);
	research::sh4ObservationException(Backend::Dynarec, 0x8c010000,
			0x8c000100, 0x160, 21, context);
	research::sh4ObservationInstructionAbort(Backend::Dynarec);
	ASSERT_EQ(3u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(Type::Exception, observed[1].type);
	EXPECT_EQ(Type::InstructionAbort, observed[2].type);
	EXPECT_EQ(0x8c010000u, observed[1].instructionPc);
}

TEST(ResearchSh4Observation,
		DynarecBoundaryInterruptUsesTheActiveSemanticClock)
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
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	research::sh4ObservationSetPreciseTiming(Backend::Dynarec, true);
	Sh4Context context {};
	context.cycle_counter = 1000;
	context.vbr = 0x8c000000;
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
	context.pc = 0x8c020000;
	research::sh4ObservationInterruptRaised(Backend::Dynarec, 0x320,
			observed.back().tick + 500, context);

	ASSERT_EQ(3u, observed.size());
	EXPECT_EQ(research::Sh4ObservationType::Exception, observed[2].type);
	EXPECT_EQ(observed[1].tick, observed[2].tick);
}

TEST(ResearchSh4Observation,
		DynarecWarmupBoundaryInterruptCannotPrecedeExecutionPosition)
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
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.cycle_counter = SH4_TIMESLICE - 8;
	context.pc = 0x8c020000;
	context.vbr = 0x8c000000;
	const std::uint64_t schedulerBoundary = sh4_sched_now64();
	research::sh4ObservationInterruptRaised(Backend::Dynarec, 0x320,
			schedulerBoundary, context);

	ASSERT_EQ(1u, observed.size());
	EXPECT_EQ(research::Sh4ObservationType::Exception, observed[0].type);
	EXPECT_EQ(schedulerBoundary + 8u, observed[0].tick);
}

TEST(ResearchSh4Observation,
		InterpreterBoundaryInterruptCannotPrecedeCompletedInstruction)
{
	using Backend = research::Sh4ObservationBackend;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Interpreter);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	struct Cleanup
	{
		~Cleanup() { research::sh4ObservationResetPreciseTiming(); }
	} cleanup;
	research::sh4ObservationSetPreciseTiming(Backend::Interpreter, true);
	Sh4Context context {};
	context.pc = 0x8c010002;
	context.vbr = 0x8c000000;
	research::sh4ObservationInstructionBegin(Backend::Interpreter,
			0x8c010000, 0x0009, 100, context);
	research::sh4ObservationInstructionEnd(Backend::Interpreter,
			0x8c010000, 0x0009, 103, context);
	context.pc = 0x8c020000;
	research::sh4ObservationInterruptRaised(Backend::Interpreter, 0x320,
			102, context);

	ASSERT_EQ(3u, observed.size());
	EXPECT_EQ(research::Sh4ObservationType::Exception, observed[2].type);
	EXPECT_EQ(103u, observed[2].tick);
}

TEST(ResearchSh4Observation,
		InterpreterBoundaryInterruptDoesNotAdvancePastActiveSemanticClock)
{
	using Backend = research::Sh4ObservationBackend;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Interpreter);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	struct Cleanup
	{
		~Cleanup() { research::sh4ObservationResetPreciseTiming(); }
	} cleanup;
	research::sh4ObservationSetPreciseTiming(Backend::Interpreter, true);
	Sh4Context context {};
	context.pc = 0x8c010002;
	context.vbr = 0x8c000000;
	research::sh4ObservationInstructionBegin(Backend::Interpreter,
			0x8c010000, 0x0009, 100, context);
	research::sh4ObservationInstructionEnd(Backend::Interpreter,
			0x8c010000, 0x0009, 103, context);
	context.pc = 0x8c020000;
	research::sh4ObservationInterruptRaised(Backend::Interpreter, 0x320,
			104, context);

	ASSERT_EQ(3u, observed.size());
	EXPECT_EQ(research::Sh4ObservationType::Exception, observed[2].type);
	EXPECT_EQ(103u, observed[2].tick);
}

TEST(ResearchSh4Observation, ExceptionAbortsEveryNestedOwner)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));

	Sh4Context context {};
	context.pc = 0x8c010002;
	context.vbr = 0x8c000000;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0xa001, 100, context);
	context.pc = 0x8c010004;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010002,
			0x6010, 101, context);
	research::sh4DynarecObservationMemoryBegin(0xdeadbeef, 4, 0);
	research::sh4ObservationExceptionRaised(0x8c010000, 0x1a0, context);

	context.pc = 0x8c010006;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010004,
			0x0009, 102, context);
	research::sh4ObservationInstructionAbort(Backend::Dynarec);

	ASSERT_EQ(7u, observed.size());
	EXPECT_EQ(Type::Exception, observed[2].type);
	EXPECT_EQ(1u, observed[2].delaySlotDepth);
	EXPECT_EQ(Type::InstructionAbort, observed[3].type);
	EXPECT_EQ(1u, observed[3].delaySlotDepth);
	EXPECT_EQ(Type::InstructionAbort, observed[4].type);
	EXPECT_EQ(0u, observed[4].delaySlotDepth);
	EXPECT_EQ(Type::InstructionBegin, observed[5].type);
	EXPECT_EQ(0u, observed[5].delaySlotDepth);
}

TEST(ResearchSh4Observation, InterruptIsAnUnownedBackendSpecificException)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.pc = 0x8c010000;
	context.vbr = 0x8c000000;
	context.r[15] = 0x8cffff00;
	research::sh4ObservationInterruptRaised(Backend::Dynarec, 0x320, 123,
			context);

	ASSERT_EQ(1u, observed.size());
	EXPECT_EQ(Type::Exception, observed[0].type);
	EXPECT_EQ(Backend::Dynarec, observed[0].backend);
	EXPECT_EQ(0u, observed[0].opcode);
	EXPECT_EQ(0u, observed[0].delaySlotDepth);
	EXPECT_EQ(0x8c010000u, observed[0].exceptionPc);
	EXPECT_EQ(0x8c000600u, observed[0].vectorPc);
	EXPECT_EQ(0x320u, observed[0].exceptionCode);
	EXPECT_EQ(123u, observed[0].tick);
}

TEST(ResearchSh4Observation,
		InterruptFailsClosedInsteadOfStealingOpenInstructionOwnership)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.pc = 0x8c010002;
	context.vbr = 0x8c000000;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x0009, 120, context);
	research::sh4ObservationInterruptRaised(Backend::Dynarec, 0x320, 121,
			context);

	ASSERT_EQ(1u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	// The stale internal frame was cleared. A later clean scheduler boundary can
	// publish only the documented unowned interrupt shape.
	context.pc = 0x8c020000;
	research::sh4ObservationInterruptRaised(Backend::Dynarec, 0x360, 122,
			context);
	ASSERT_EQ(2u, observed.size());
	EXPECT_EQ(Type::Exception, observed[1].type);
	EXPECT_EQ(0u, observed[1].opcode);
	EXPECT_EQ(0u, observed[1].delaySlotDepth);
}

TEST(ResearchSh4Observation,
		InterpreterSynchronousInterruptRetainsInstructionOwnership)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Interpreter);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.pc = 0x8c010002;
	context.vbr = 0x8c000000;
	research::sh4ObservationInstructionBegin(Backend::Interpreter, 0x8c010000,
			0x402eu, 120, context);
	context.pc = 0x8c020000;
	research::sh4ObservationInterruptRaised(Backend::Interpreter, 0x320, 121,
			context);
	research::sh4ObservationInstructionEnd(Backend::Interpreter, 0x8c010000,
			0x402eu, 122, context);

	ASSERT_EQ(3u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(Type::InstructionEnd, observed[1].type);
	EXPECT_EQ(Type::Exception, observed[2].type);
	EXPECT_EQ(0x8c020000u, observed[2].instructionPc);
	EXPECT_EQ(0u, observed[2].opcode);
	EXPECT_EQ(0u, observed[2].delaySlotDepth);
	EXPECT_EQ(0x8c020000u, observed[2].exceptionPc);
	EXPECT_EQ(0x8c000600u, observed[2].vectorPc);
	EXPECT_EQ(122u, observed[2].tick);
	EXPECT_EQ(0x8c020000u, observed[1].nextPc);
}

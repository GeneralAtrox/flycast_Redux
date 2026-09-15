#include "Sh4ObservationTestSupport.h"

TEST(ResearchSh4Observation, InactiveBackendDoesNotCreateInstructionFrame)
{
	using Backend = research::Sh4ObservationBackend;
	ASSERT_FALSE(research::sh4ObservationBusActive(Backend::Dynarec));
	Sh4Context context {};
	context.pc = 0x8c010002;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x0009, 10, context);

	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&observed](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010000,
			0x0009, 11, context);
	EXPECT_TRUE(observed.empty());
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x0009, 12, context);
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010000,
			0x0009, 13, context);
	ASSERT_EQ(2u, observed.size());
	EXPECT_EQ(0u, observed[0].delaySlotDepth);
}

TEST(ResearchSh4Observation, SubscriptionChangeInsideNestedInstructionStaysBalanced)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	std::vector<research::Sh4Observation> firstObserved;
	research::Sh4ObservationSubscription firstSubscription = 0;
	firstSubscription = research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				firstObserved.push_back(observation);
				if (observation.type == Type::InstructionBegin
						&& observation.delaySlotDepth == 0)
					EXPECT_TRUE(research::unsubscribeSh4Observations(firstSubscription));
			});

	Sh4Context context {};
	context.pc = 0x8c010002;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x0009, 10, context);
	ASSERT_EQ(1u, firstObserved.size());

	std::vector<research::Sh4Observation> replacementObserved;
	ObservationSubscription replacement(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				replacementObserved.push_back(observation);
			}));
	context.pc = 0x8c010004;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010002,
			0x0009, 11, context);
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010002,
			0x0009, 12, context);
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010000,
			0x0009, 13, context);
	EXPECT_TRUE(replacementObserved.empty());

	context.pc = 0x8c020002;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c020000,
			0x0009, 20, context);
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c020000,
			0x0009, 21, context);
	ASSERT_EQ(2u, replacementObserved.size());
	EXPECT_EQ(0u, replacementObserved[0].delaySlotDepth);
	EXPECT_EQ(Type::InstructionEnd, replacementObserved[1].type);
}

TEST(ResearchSh4Observation, MismatchedAbortClearsEmissionFrames)
{
	using Backend = research::Sh4ObservationBackend;
	std::vector<research::Sh4Observation> observed;
	ObservationSubscription subscription(research::subscribeSh4Observations(
			research::Sh4ObservationFilter {},
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.pc = 0x8c010002;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x0009, 10, context);
	research::sh4ObservationInstructionAbort(Backend::Interpreter);
	context.pc = 0x8c020002;
	EXPECT_NO_THROW(research::sh4ObservationInstructionBegin(Backend::Interpreter,
			0x8c020000, 0x0009, 20, context));
	EXPECT_NO_THROW(research::sh4ObservationInstructionEnd(Backend::Interpreter,
			0x8c020000, 0x0009, 21, context));
	ASSERT_EQ(3u, observed.size());
	EXPECT_EQ(Backend::Dynarec, observed[0].backend);
	EXPECT_EQ(Backend::Interpreter, observed[1].backend);
	EXPECT_EQ(0u, observed[1].delaySlotDepth);
}

TEST(ResearchSh4Observation, DynarecMemoryMarkersPublishOnlyCompletedAccesses)
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
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x6010, 100, context);
	research::sh4DynarecObservationMemoryBegin(0x8c020000, 1, 0);
	research::sh4DynarecObservationMemoryEnd(0, 0, 0xffffffffffffff80ull);
	research::sh4DynarecObservationMemoryBegin(0x8c020004, 0x100u | 2u,
			0x12345678u);
	research::sh4DynarecObservationMemoryEnd(0, 0, 0);
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010000,
			0x6010, 101, context);

	ASSERT_EQ(4u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(Type::MemoryRead, observed[1].type);
	EXPECT_EQ(0x8c020000u, observed[1].memoryAddress);
	EXPECT_EQ(1u, observed[1].memoryWidth);
	EXPECT_EQ(0x80u, observed[1].memoryValue);
	EXPECT_EQ(Type::MemoryWrite, observed[2].type);
	EXPECT_EQ(0x8c020004u, observed[2].memoryAddress);
	EXPECT_EQ(2u, observed[2].memoryWidth);
	EXPECT_EQ(0x5678u, observed[2].memoryValue);
	EXPECT_EQ(Type::InstructionEnd, observed[3].type);

	// A fault between the markers aborts the owner and never publishes access.
	observed.clear();
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010002,
			0x6120, 102, context);
	research::sh4DynarecObservationMemoryBegin(0xdeadbeef, 4, 0);
	research::sh4ObservationInstructionAbort(Backend::Dynarec);
	ASSERT_EQ(2u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(Type::InstructionAbort, observed[1].type);
}

TEST(ResearchSh4Observation,
		NestedDynarecHardwareMemoryPreservesGuestAccessAndCycleDebit)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	config::ResearchDynarecObservation.override(true);
	research::sh4DynarecExecutionTimingReset();
	struct Cleanup
	{
		~Cleanup()
		{
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
	context.cycle_counter = 1000;
	context.pc = 0x8c010002;
	shil_opcode begin {};
	begin.op = shop_research_begin;
	begin.rs1 = shil_param(0x8c010000);
	begin.rs2 = shil_param(0x0009);
	shil_opcode end {};
	end.op = shop_research_end;
	end.rs1 = begin.rs1;
	end.rs2 = begin.rs2;
	end.rs3 = shil_param(0x8c010002);

#ifdef STRICT_MODE
	Sh4Cycles warmupCycles {1};
#else
	Sh4Cycles warmupCycles {8};
#endif
	const int expectedDebit = warmupCycles.countCycles(0x0009);
	invokeDynarecMarker(context, begin);
	research::sh4DynarecObservationMemoryBegin(0x005f6c18, 0x100u | 4u,
			0x00000001u);
	// A synchronous device callback may read guest descriptor memory while the
	// generated guest store still owns its memory marker. Those hardware reads
	// are not separate guest-instruction accesses.
	research::sh4DynarecObservationMemoryBegin(0x0c001000, 4u, 0);
	research::sh4DynarecObservationMemoryEnd(0, 0, 0x80000001u);
	research::sh4DynarecObservationMemoryEnd(0, 0, 0);
	invokeDynarecMarker(context, end);

	EXPECT_EQ(1000 - expectedDebit, context.cycle_counter);
	ASSERT_EQ(3u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(Type::MemoryWrite, observed[1].type);
	EXPECT_EQ(0x005f6c18u, observed[1].memoryAddress);
	EXPECT_EQ(1u, observed[1].memoryValue);
	EXPECT_EQ(Type::InstructionEnd, observed[2].type);
}

TEST(ResearchSh4Observation, DynarecFaultDropsPendingMemoryAndRestoresDepth)
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
			0x6010, 100, context);
	research::sh4DynarecObservationMemoryBegin(0xdeadbeef, 4, 0);
	research::sh4ObservationExceptionRaised(0x8c010000, 0x0e0, context);

	context.pc = 0x8c010004;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010002,
			0x0009, 101, context);
	research::sh4DynarecObservationMemoryBegin(0x8c020000, 4, 0);
	research::sh4DynarecObservationMemoryEnd(0, 0, 0x12345678);
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010002,
			0x0009, 102, context);

	ASSERT_EQ(6u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(Type::Exception, observed[1].type);
	EXPECT_EQ(0x8c000100u, observed[1].vectorPc);
	EXPECT_EQ(Type::InstructionAbort, observed[2].type);
	EXPECT_EQ(Type::InstructionBegin, observed[3].type);
	EXPECT_EQ(0u, observed[3].delaySlotDepth);
	EXPECT_EQ(Type::MemoryRead, observed[4].type);
	EXPECT_EQ(0x12345678u, observed[4].memoryValue);
	EXPECT_EQ(Type::InstructionEnd, observed[5].type);
}

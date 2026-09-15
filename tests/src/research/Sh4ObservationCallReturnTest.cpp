#include "Sh4ObservationTestSupport.h"

TEST(ResearchSh4Observation, InterpreterPublishesCanonicalCallAndReturnOrdering)
{
	std::vector<research::Sh4Observation> observed;
	ObservationSubscription subscription(research::subscribeSh4Observations(
			research::Sh4ObservationFilter {},
			[&observed](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.pc = 0x8c020002;
	context.r[1] = 0x8c010100;
	context.pr = 0x8c030000;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter, 0x8c020000, 0x410b, 100, context);
	context.pc = 0x8c010100;
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter, 0x8c020000, 0x410b, 104, context);

	context.pc = 0x8c010112;
	context.pr = 0x8c020004;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter, 0x8c010110, 0x000b, 120, context);
	context.pc = 0x8c020004;
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter, 0x8c010110, 0x000b, 124, context);
	context.pc = 0x8c030002;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter, 0x8c030000, 0x0009, 130, context);
	research::sh4ObservationInstructionAbort(
			research::Sh4ObservationBackend::Interpreter);

	ASSERT_EQ(8u, observed.size());
	EXPECT_EQ(research::Sh4ObservationType::InstructionBegin, observed[0].type);
	EXPECT_EQ(research::Sh4ObservationType::Call, observed[1].type);
	EXPECT_EQ(research::Sh4CallKind::Jsr, observed[1].callKind);
	EXPECT_EQ(0x8c010100u, observed[1].targetPc);
	EXPECT_EQ(0x8c020004u, observed[1].returnPc);
	EXPECT_EQ(0x8c020002u, observed[1].delaySlotPc);
	EXPECT_EQ(research::Sh4ObservationType::InstructionEnd, observed[2].type);
	EXPECT_EQ(research::Sh4ObservationType::InstructionBegin, observed[3].type);
	EXPECT_EQ(research::Sh4ObservationType::Return, observed[4].type);
	EXPECT_EQ(0x8c020004u, observed[4].targetPc);
	EXPECT_EQ(0x8c020004u, observed[4].returnPc);
	EXPECT_EQ(0x8c010112u, observed[4].delaySlotPc);
	EXPECT_EQ(research::Sh4ObservationType::InstructionEnd, observed[5].type);
	EXPECT_EQ(research::Sh4Observation::HasNextPc
			| research::Sh4Observation::HasRegisters, observed[6].availableFields);
	EXPECT_EQ(research::Sh4ObservationType::InstructionAbort, observed[7].type);
	EXPECT_EQ(0u, observed[7].availableFields);
	for (std::size_t index = 1; index < observed.size(); ++index)
		EXPECT_EQ(observed[index - 1].emissionOrdinal + 1,
				observed[index].emissionOrdinal);
}

TEST(ResearchSh4Observation, SharedRuntimePublishesDynarecInstructionOwnership)
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
	context.r[1] = 0x8c020000;
	context.pr = 0x8c030000;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x410b, 100, context);
	context.pc = 0x8c010004;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010002,
			0x0009, 101, context);
	// Interpreter opcode handlers are also used as dynarec fallbacks. Their
	// legacy memory wrapper must inherit the owning dynarec frame.
	research::sh4ObservationMemoryAccess(
			research::sh4ObservationCurrentInstructionBackend(
					research::Sh4ObservationBackend::Interpreter), 0x8c100000, 4,
			research::Sh4MemoryAccessKind::Read, 0x44332211);
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010002,
			0x0009, 102, context);
	context.pc = 0x8c020000;
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010000,
			0x410b, 103, context);

	ASSERT_EQ(6u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(Type::Call, observed[1].type);
	EXPECT_EQ(Type::InstructionBegin, observed[2].type);
	EXPECT_EQ(1u, observed[2].delaySlotDepth);
	EXPECT_EQ(Type::MemoryRead, observed[3].type);
	EXPECT_EQ(1u, observed[3].delaySlotDepth);
	EXPECT_EQ(Type::InstructionEnd, observed[4].type);
	EXPECT_EQ(Type::InstructionEnd, observed[5].type);
	EXPECT_EQ(Backend::Dynarec, observed[5].backend);
	EXPECT_EQ(0x8c020000u, observed[5].nextPc);
}

TEST(ResearchSh4Observation, DynarecMarkersReconstructTickAndDynamicNextPc)
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
	context.cycle_counter = 100;
	context.jdyn = 0x8c020000;
	context.pr = 0x8c030000;
	shil_opcode begin;
	begin.op = shop_research_begin;
	begin.rs1 = shil_param(0x8c010000);
	begin.rs2 = shil_param(0x000b);
	begin.size = 10;
	invokeDynarecMarker(context, begin);
	shil_opcode end;
	end.op = shop_research_end;
	end.rs1 = shil_param(0x8c010000);
	end.rs2 = shil_param(0x000b);
	end.rs3 = shil_param(0xffffffffu);
	end.size = 0;
	invokeDynarecMarker(context, end);

	ASSERT_EQ(3u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(1338u, observed[0].tick);
	EXPECT_EQ(0x8c010002u, observed[0].nextPc);
	EXPECT_EQ(Type::Return, observed[1].type);
	EXPECT_EQ(0x8c020000u, observed[1].targetPc);
	EXPECT_EQ(0x8c020000u, observed[2].nextPc);
}

TEST(ResearchSh4Observation, ConditionalDelayMarkersMatchInterpreterNesting)
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
	context.cycle_counter = 100;

	auto marker = [](shilop type, std::uint32_t pc, std::uint16_t opcode,
			std::uint32_t nextPc = 0xffffffffu) {
		shil_opcode value;
		value.op = type;
		value.size = 0;
		value.rs1 = shil_param(pc);
		value.rs2 = shil_param(opcode);
		value.rs3 = shil_param(nextPc);
		return value;
	};

	// BF/S with T=1 is not taken in the authoritative interpreter: the branch
	// closes first and the following instruction is observed at depth zero.
	context.jdyn = 1;
	context.sr.T = 1;
	shil_opcode branchBegin = marker(shop_research_begin, 0x8c010000, 0x8f01);
	shil_opcode beforeDelay = marker(shop_research_conditional_before_delay,
			0x8c010000, 0x8f01, 0x8c010006);
	shil_opcode delayBegin = marker(shop_research_begin, 0x8c010002, 0x0009);
	shil_opcode delayEnd = marker(shop_research_end, 0x8c010002, 0x0009,
			0x8c010004);
	shil_opcode afterDelay = marker(shop_research_conditional_after_delay,
			0x8c010000, 0x8f01, 0x8c010006);
	invokeDynarecMarker(context, branchBegin);
	invokeDynarecMarker(context, beforeDelay);
	context.sr.T = 0; // The slot may mutate T; ownership must use latched jdyn.
	invokeDynarecMarker(context, delayBegin);
	invokeDynarecMarker(context, delayEnd);
	invokeDynarecMarker(context, afterDelay);
	ASSERT_EQ(4u, observed.size());
	EXPECT_EQ(Type::InstructionEnd, observed[1].type);
	EXPECT_EQ(0u, observed[2].delaySlotDepth);

	// With T=0, BF/S is taken and owns the delay slot as a nested instruction.
	observed.clear();
	context.jdyn = 0;
	context.sr.T = 0;
	invokeDynarecMarker(context, branchBegin);
	invokeDynarecMarker(context, beforeDelay);
	context.sr.T = 1; // Prove the slot cannot change the parent's decision.
	invokeDynarecMarker(context, delayBegin);
	invokeDynarecMarker(context, delayEnd);
	invokeDynarecMarker(context, afterDelay);
	ASSERT_EQ(4u, observed.size());
	EXPECT_EQ(1u, observed[1].delaySlotDepth);
	EXPECT_EQ(1u, observed[2].delaySlotDepth);
	EXPECT_EQ(0x8c010006u, observed[3].nextPc);
}

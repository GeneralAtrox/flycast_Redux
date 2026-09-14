#include "research/sh4_profile.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

namespace
{
research::Sh4Observation event(research::Sh4ObservationType type,std::uint32_t pc,std::uint16_t opcode,std::uint64_t tick,std::uint32_t next=0)
{research::Sh4Observation e;e.backend=research::Sh4ObservationBackend::Dynarec;e.type=type;e.instructionPc=pc;e.opcode=opcode;e.tick=tick;if(type==research::Sh4ObservationType::InstructionEnd){e.availableFields=research::Sh4Observation::HasNextPc;e.nextPc=next;}return e;}

}

TEST(ResearchSh4Profile, AggregatesExactGuestBytesCyclesAndDynamicBranches)
{
	research::Sh4ProfileAccumulator profile(research::Sh4ObservationBackend::Dynarec,8,8,32);
	profile.observe(event(research::Sh4ObservationType::InstructionBegin,0x8c010000,0x8b02,100));
	profile.observe(event(research::Sh4ObservationType::InstructionEnd,0x8c010000,0x8b02,102,0x8c010008));
	profile.observe(event(research::Sh4ObservationType::InstructionBegin,0x8c010000,0x8b02,200));
	profile.observe(event(research::Sh4ObservationType::InstructionEnd,0x8c010000,0x8b02,202,0x8c010002));
	const auto result=profile.snapshot();ASSERT_EQ(1u,result.blocks.size());EXPECT_EQ((std::vector<std::uint8_t>{0x02,0x8b}),result.blocks[0].guestBytes);EXPECT_EQ(2u,result.blocks[0].executionCount);EXPECT_EQ(4u,result.blocks[0].totalCycles);
	ASSERT_EQ(2u,result.branches.size());EXPECT_FALSE(result.branches[0].taken);EXPECT_TRUE(result.branches[1].taken);EXPECT_EQ(0u,result.incompleteExecutions);
}

TEST(ResearchSh4Profile, FailsClosedAtConfiguredBounds)
{
	research::Sh4ProfileAccumulator profile(research::Sh4ObservationBackend::Dynarec,1,1,3);
	profile.observe(event(research::Sh4ObservationType::InstructionBegin,0x1000,0x0009,1));profile.observe(event(research::Sh4ObservationType::InstructionEnd,0x1000,0x0009,2,0x1002));
	profile.observe(event(research::Sh4ObservationType::InstructionBegin,0x1002,0x0009,3));EXPECT_THROW(profile.observe(event(research::Sh4ObservationType::InstructionEnd,0x1002,0x0009,4,0x1004)),std::overflow_error);
}

TEST(ResearchSh4Profile, AggregatesProductionBlockGenerationsAndActualEdges)
{
	research::Sh4DynarecProfileCollector profile(4, 4, 16);
	research::Sh4DynarecBlockDefinition conditional;
	conditional.virtualAddress = 0x8c010000;
	conditional.physicalAddress = 0x0c010000;
	conditional.guestCodeSize = 4;
	conditional.guestCycles = 3;
	conditional.guestOpcodes = 2;
	conditional.guestBytes = {0x01, 0x89, 0x09, 0x00};
	conditional.byteStatus = research::Sh4DynarecProfileByteStatus::Complete;
	conditional.branchKind = research::Sh4DynarecBranchKind::Conditional;
	conditional.branchSource = 0x8c010000;
	conditional.branchOpcode = 0x8901;
	conditional.branchTarget = 0x8c010006;
	conditional.fallthroughTarget = 0x8c010002;
	const auto first = profile.registerBlock(conditional);
	ASSERT_NE(0u, first);
	profile.enter(first, 100);
	profile.exit(first, conditional.branchTarget, 103);
	profile.enter(first, 110);
	profile.exit(first, conditional.fallthroughTarget, 113);

	auto modified = conditional;
	modified.guestBytes[0] = 0xff;
	modified.byteStatus = research::Sh4DynarecProfileByteStatus::Incomplete;
	const auto second = profile.registerBlock(modified);
	ASSERT_NE(0u, second);
	ASSERT_NE(first, second);
	profile.enter(second, 120);
	// A later entry proves that the previous generation escaped without a
	// normal block completion, as happens on a dynarec exception path.
	profile.enter(first, 125);
	profile.exit(first, conditional.branchTarget, 128);

	const auto snapshot = profile.snapshot();
	ASSERT_TRUE(snapshot.complete) << snapshot.failure;
	ASSERT_EQ(2u, snapshot.blocks.size());
	EXPECT_EQ(4u, snapshot.enteredExecutions);
	EXPECT_EQ(3u, snapshot.completedExecutions);
	EXPECT_EQ(1u, snapshot.abortedExecutions);
	EXPECT_EQ(2u, snapshot.branches.size());
	EXPECT_EQ(9u, snapshot.blocks[0].totalCycles);
	EXPECT_EQ(1u, snapshot.blocks[1].abortedCount);
}

TEST(ResearchSh4Profile, ProductionProfileLatchesBoundsWithoutThrowingIntoDynarec)
{
	research::Sh4DynarecProfileCollector profile(1, 1, 1);
	research::Sh4DynarecBlockDefinition block;
	block.guestCodeSize = 2;
	block.guestCycles = 1;
	block.guestOpcodes = 1;
	block.guestBytes = {0x09, 0x00};
	block.byteStatus = research::Sh4DynarecProfileByteStatus::Complete;
	const auto generation = profile.registerBlock(block);
	ASSERT_NE(0u, generation);
	profile.enter(generation, 1);
	profile.exit(generation, 2, 2);
	profile.enter(generation, 3);
	EXPECT_EQ(0u, profile.registerBlock(block));
	const auto snapshot = profile.snapshot();
	EXPECT_FALSE(snapshot.complete);
	EXPECT_FALSE(snapshot.failure.empty());
}

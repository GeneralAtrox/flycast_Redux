#include "cfg/option.h"
#include "hw/sh4/sh4_if.h"
#include "research/sh4_observation_runtime.h"
#include "research/sh4_pc_checkpoint_runtime.h"
#include "ResearchRuntimeStubs.h"
#include "types.h"

#include <gtest/gtest.h>

namespace
{

class ResearchSh4PcCheckpointRuntime : public testing::Test
{
protected:
	void SetUp() override
	{
		research::stopSh4PcCheckpointRuntime();
		config::ResearchSh4PcCheckpoint.set(0);
		config::ResearchSh4PcCheckpointU32Address.set(0);
		config::ResearchSh4PcCheckpointU32Value.set(0);
		config::DynarecEnabled.set(false);
		config::ResearchDynarecObservation.set(false);
		config::ThreadedRendering.set(false);
	}

	void TearDown() override
	{
		research::stopSh4PcCheckpointRuntime();
		config::ResearchSh4PcCheckpoint.set(0);
		config::ResearchSh4PcCheckpointU32Address.set(0);
		config::ResearchSh4PcCheckpointU32Value.set(0);
		config::DynarecEnabled.set(true);
		config::ResearchDynarecObservation.set(false);
		config::ThreadedRendering.set(true);
	}
};

TEST_F(ResearchSh4PcCheckpointRuntime, RequiresAlignedGuestPcAndNonThreadedRendering)
{
	config::ResearchSh4PcCheckpoint.set(0x8c010001);
	EXPECT_THROW(research::configureSh4PcCheckpointRuntime(), FlycastException);

	config::ResearchSh4PcCheckpoint.set(0x8c010000);
	EXPECT_NO_THROW(research::configureSh4PcCheckpointRuntime());

	config::ThreadedRendering.set(true);
	EXPECT_THROW(research::configureSh4PcCheckpointRuntime(), FlycastException);
}

TEST_F(ResearchSh4PcCheckpointRuntime, DynarecRequiresObservationMarkers)
{
	config::ResearchSh4PcCheckpoint.set(0x8c010000);
	config::DynarecEnabled.set(true);
	EXPECT_THROW(research::configureSh4PcCheckpointRuntime(), FlycastException);

	config::ResearchDynarecObservation.set(true);
	EXPECT_NO_THROW(research::configureSh4PcCheckpointRuntime());
}

TEST_F(ResearchSh4PcCheckpointRuntime, GateValueRequiresGateAddress)
{
	config::ResearchSh4PcCheckpoint.set(0x8c010000);
	config::ResearchSh4PcCheckpointU32Value.set(1);
	EXPECT_THROW(research::configureSh4PcCheckpointRuntime(), FlycastException);

	config::ResearchSh4PcCheckpointU32Address.set(0x8c001002);
	EXPECT_THROW(research::configureSh4PcCheckpointRuntime(), FlycastException);

	config::ResearchSh4PcCheckpointU32Address.set(0x8c001000);
	EXPECT_NO_THROW(research::configureSh4PcCheckpointRuntime());
}

TEST_F(ResearchSh4PcCheckpointRuntime, FiresOnceAfterMatchingTopLevelInstruction)
{
	config::ResearchSh4PcCheckpoint.set(0x8c010100);
	research::configureSh4PcCheckpointRuntime();
	unsigned calls = 0;
	research::startSh4PcCheckpointRuntime([&calls] { ++calls; });

	Sh4Context context {};
	context.pc = 0x8c010002;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter, 0x8c010000, 0x0009,
			1, context);
	context.pc = 0x8c010102;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter, 0x8c010100, 0x0009,
			2, context);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter, 0x8c010100, 0x0009,
			3, context);
	// The matching PC ran as a nested (delay-slot) frame: not top level yet.
	EXPECT_EQ(0u, calls);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter, 0x8c010000, 0x0009,
			4, context);
	EXPECT_EQ(0u, calls);

	context.pc = 0x8c010102;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter, 0x8c010100, 0x0009,
			5, context);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter, 0x8c010100, 0x0009,
			6, context);
	EXPECT_EQ(1u, calls);
	EXPECT_TRUE(research::sh4PcCheckpointRuntimeTriggered());
	EXPECT_FALSE(research::sh4PcCheckpointRuntimeActive());

	research::sh4PcCheckpointInstructionEnd(
			research::Sh4ObservationBackend::Interpreter, 0x8c010100);
	EXPECT_EQ(1u, calls);
}

TEST_F(ResearchSh4PcCheckpointRuntime, GuestU32GateDefersTheMatchingPc)
{
	constexpr std::uint32_t pc = 0x8c010100;
	constexpr std::uint32_t gateAddress = 0x8c001000;
	constexpr std::uint32_t gateValue = 1;
	config::ResearchSh4PcCheckpoint.set(pc);
	config::ResearchSh4PcCheckpointU32Address.set(gateAddress);
	config::ResearchSh4PcCheckpointU32Value.set(gateValue);
	research_test::clearGuestRam();
	research::configureSh4PcCheckpointRuntime();
	unsigned calls = 0;
	research::startSh4PcCheckpointRuntime([&calls] { ++calls; });

	research::sh4PcCheckpointInstructionEnd(
			research::Sh4ObservationBackend::Interpreter, pc);
	EXPECT_EQ(0u, calls);
	EXPECT_TRUE(research::sh4PcCheckpointRuntimeActive());

	ASSERT_TRUE(research_test::writeGuestRam(gateAddress, {1, 0, 0, 0}));
	research::sh4PcCheckpointInstructionEnd(
			research::Sh4ObservationBackend::Interpreter, pc);
	EXPECT_EQ(1u, calls);
	EXPECT_TRUE(research::sh4PcCheckpointRuntimeTriggered());
	EXPECT_FALSE(research::sh4PcCheckpointRuntimeActive());
}

} // namespace

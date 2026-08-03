#include "research/cdda_observation.h"
#include "hw/sh4/sh4_if.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace
{
class Subscription
{
public:
	explicit Subscription(research::CddaObservationSubscription id) : id(id) {}
	~Subscription() { research::unsubscribeCddaObservations(id); }
private:
	research::CddaObservationSubscription id;
};
}

TEST(ResearchCddaObservation, JoinsAcceptedOwnerAppliedStateAndSector)
{
	research::resetCddaObservation(1);
	std::vector<research::CddaObservation> events;
	Subscription subscription(research::subscribeCddaEvidenceObservations(
			[&](const auto& event) { events.push_back(event); }));

	Sh4Context context {};
	context.pc = 0x8c010102;
	context.pr = 0x8c020000;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter,
			0x8c010100, 0x2102, 100, context);
	const std::uint32_t parameters[4] {600, 601, 0, 0};
	research::observeReiosCddaControlAccepted(7, 0x15, parameters, 99);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter,
			0x8c010100, 0x2102, 102, context);

	research::CddaDriveState before;
	before.status = 0;
	research::CddaDriveState after;
	after.status = 1;
	after.currentFad = after.startFad = 600;
	after.endFad = 601;
	research::observeReiosCddaControlApplied(7, 0x15, before, after, true, 110);

	std::array<std::uint8_t, 2352> sector {};
	sector[0] = 0x34;
	sector[1] = 0x12;
	research::CddaDriveState consumed = after;
	consumed.status = 3;
	consumed.currentFad = 601;
	research::observeCddaSector(9, 600, after, consumed, true,
			sector.data(), sector.size(), 120);

	ASSERT_EQ(3u, events.size());
	EXPECT_EQ(research::CddaObservationType::ControlAccepted, events[0].type);
	EXPECT_TRUE(events[0].initiator.valid);
	EXPECT_EQ(0x8c010100u, events[0].initiator.pc);
	EXPECT_EQ(100u, events[0].tick);
	EXPECT_EQ(events[0].controlGeneration, events[1].controlGeneration);
	EXPECT_EQ(events[0].controlGeneration, events[2].controlGeneration);
	EXPECT_EQ(9u, events[2].aicaGeneration);
	EXPECT_EQ(600u, events[2].fad);
	EXPECT_TRUE(events[2].readSuccessful);
	EXPECT_EQ(sector.size(), events[2].bytes.size());
	EXPECT_EQ(0x34u, events[2].bytes[0]);
	EXPECT_EQ(events[0].controlGeneration,
			research::currentCddaControlGeneration());
}

TEST(ResearchCddaObservation, RestoredGenerationCannotCollide)
{
	research::resetCddaObservation(1);
	research::restoreCddaControlGeneration(500);
	EXPECT_EQ(500u, research::currentCddaControlGeneration());

	const std::uint32_t parameters[4] {600, 601, 0, 0};
	research::observeReiosCddaControlAccepted(8, 0x15, parameters, 10);
	research::CddaDriveState before;
	research::CddaDriveState after;
	after.status = 1;
	research::observeReiosCddaControlApplied(8, 0x15, before, after, true, 11);
	EXPECT_GT(research::currentCddaControlGeneration(), 500u);
	research::resetCddaObservation(12);
}

TEST(ResearchCddaObservation, RejectsNonControlCommands)
{
	EXPECT_FALSE(research::isReiosCddaControlCommand(0x11));
	EXPECT_TRUE(research::isReiosCddaControlCommand(0x14));
	EXPECT_TRUE(research::isReiosCddaControlCommand(0x15));
	EXPECT_TRUE(research::isReiosCddaControlCommand(0x16));
	EXPECT_TRUE(research::isReiosCddaControlCommand(0x17));
	EXPECT_TRUE(research::isReiosCddaControlCommand(0x1b));
	EXPECT_TRUE(research::isReiosCddaControlCommand(0x21));
	EXPECT_TRUE(research::isGdromPacketCddaControlCommand(0x20));
	EXPECT_TRUE(research::isGdromPacketCddaControlCommand(0x21));
	EXPECT_FALSE(research::isGdromPacketCddaControlCommand(0x14));
}

TEST(ResearchCddaObservation, RetainsExactGdromPacketAndFinalDataWriteOwner)
{
	research::resetCddaObservation(1);
	std::vector<research::CddaObservation> events;
	Subscription subscription(research::subscribeCddaEvidenceObservations(
			[&](const auto& event) { events.push_back(event); }));
	Sh4Context context {};
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter,
			0x8c020000, 0x2101, 50, context);
	const std::uint8_t packet[12] {0x20, 0x01, 0x00, 0x02, 0x58, 0x00,
			0x00, 0x00, 0x00, 0x02, 0x59, 0x00};
	research::observeGdromPacketCddaControlAccepted(packet, 49);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter,
			0x8c020000, 0x2101, 52, context);
	research::CddaDriveState before, after;
	after.status = 1;
	after.currentFad = after.startFad = 600;
	after.endFad = 601;
	research::observeGdromPacketCddaControlApplied(0x20,
			before, after, true, 53);
	ASSERT_EQ(2u, events.size());
	EXPECT_EQ(research::CddaControlPath::GdromPacket, events[0].path);
	EXPECT_EQ(events[0].controlGeneration, events[1].controlGeneration);
	EXPECT_TRUE(events[0].initiator.valid);
	EXPECT_EQ(0x8c020000u, events[0].initiator.pc);
	EXPECT_EQ(0x02000120u, events[0].parameters[0]);
	EXPECT_EQ(0x00000058u, events[0].parameters[1]);
	EXPECT_EQ(0x00590200u, events[0].parameters[2]);
	EXPECT_EQ(0u, events[0].parameters[3]);
}

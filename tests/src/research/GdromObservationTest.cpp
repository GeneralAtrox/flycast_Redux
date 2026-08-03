#include "research/gdrom_observation.h"
#include "hw/sh4/sh4_if.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace
{
class Subscription
{
public:
	explicit Subscription(research::GdromObservationSubscription id):id(id){}
	~Subscription(){ research::unsubscribeGdromObservations(id); }
private: research::GdromObservationSubscription id;
};
}

TEST(ResearchGdromObservation, RetainsRequestOwnerAcrossAsynchronousChunks)
{
	std::vector<research::GdromObservation> events;
	Subscription subscription(research::subscribeGdromEvidenceObservations(
			[&](const auto& event){ events.push_back(event); }));
	Sh4Context context{}; context.pc=0x8c010102; context.pr=0x8c020000;
	research::sh4ObservationInstructionBegin(research::Sh4ObservationBackend::Interpreter,
			0x8c010100,0x2102,100,context);
	const std::uint32_t parameters[4]{45150,2,0x0c100000,0};
	research::observeReiosGdromCommand(7,0x11,parameters,99);
	research::sh4ObservationInstructionEnd(research::Sh4ObservationBackend::Interpreter,
			0x8c010100,0x2102,102,context);
	std::array<std::uint8_t,4096> bytes{};
	research::observeReiosGdromTransfer(45150,2,0x0c100000,bytes.data(),bytes.size(),200);
	research::observeReiosGdromComplete(300);
	ASSERT_EQ(3u,events.size());
	EXPECT_TRUE(events[0].initiator.valid);
	EXPECT_EQ(100u,events[0].tick);
	EXPECT_EQ(0x8c010100u,events[0].initiator.pc);
	EXPECT_FALSE(events[1].initiator.valid);
	EXPECT_EQ(events[0].commandGeneration,events[1].commandGeneration);
	EXPECT_EQ(events[0].commandGeneration,events[2].commandGeneration);
	EXPECT_EQ(research::GdromCompletionMechanism::Status,events[2].completion);
	EXPECT_EQ(bytes.size(),events[2].transferredBytes);
}

TEST(ResearchGdromObservation, AbortClosesOnlyTheMatchingRequest)
{
	std::vector<research::GdromObservation> events;
	Subscription subscription(research::subscribeGdromEvidenceObservations(
			[&](const auto& event){ events.push_back(event); }));
	Sh4Context context{}; context.pc=0x8c010102;
	research::sh4ObservationInstructionBegin(research::Sh4ObservationBackend::Interpreter,
			0x8c010100,0x2102,10,context);
	const std::uint32_t parameters[4]{45150,1,0x0c100000,0};
	research::observeReiosGdromCommand(9,0x11,parameters,11);
	research::observeReiosGdromAbort(8,12);
	EXPECT_TRUE(research::reiosGdromObservationCommandActive());
	research::observeReiosGdromAbort(9,13);
	EXPECT_FALSE(research::reiosGdromObservationCommandActive());
	research::sh4ObservationInstructionAbort(research::Sh4ObservationBackend::Interpreter);
	ASSERT_EQ(2u,events.size());
	EXPECT_EQ(research::GdromObservationType::Abort,events.back().type);
}

TEST(ResearchGdromObservation, DiscoverySubscribersRemainSeparateFromEvidence)
{
	std::size_t first = 0, second = 0;
	{
		Subscription one(research::subscribeGdromObservations(
				[&](const auto&) { ++first; }));
		Subscription two(research::subscribeGdromObservations(
				[&](const auto&) { ++second; }));
		EXPECT_THROW(research::subscribeGdromEvidenceObservations(
				[](const auto&) {}), std::logic_error);
		const std::uint32_t parameters[4] {45150, 1, 0x0c100000, 0};
		research::observeReiosGdromCommand(31, 0x11, parameters, 20);
		research::observeReiosGdromAbort(31, 21);
	}
	EXPECT_EQ(2u, first);
	EXPECT_EQ(2u, second);
	Subscription evidence(research::subscribeGdromEvidenceObservations(
			[](const auto&) {}));
	EXPECT_THROW(research::subscribeGdromObservations(
			[](const auto&) {}), std::logic_error);
}

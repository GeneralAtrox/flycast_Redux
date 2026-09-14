#include "research/aica_observation.h"
#include "hw/sh4/sh4_if.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace
{

const std::array<std::int32_t, 16> zeroDspInputs {};
const std::array<std::int16_t, 16> zeroDspOutputs {};
const std::array<std::uint8_t, 8192> keySourceRam {};

class AicaSubscription
{
public:
	explicit AicaSubscription(research::AicaObservationSubscription id)
		: id(id) {}
	~AicaSubscription() { research::unsubscribeAicaObservations(id); }

	AicaSubscription(const AicaSubscription&) = delete;
	AicaSubscription& operator=(const AicaSubscription&) = delete;

private:
	research::AicaObservationSubscription id;
};

} // namespace

TEST(ResearchAicaObservation, RetainsSh4OwnerAcrossG2DmaLifecycle)
{
	using Backend = research::Sh4ObservationBackend;
	std::vector<research::AicaObservation> events;
	AicaSubscription subscription(research::subscribeAicaEvidenceObservations(
			[&](const auto& event) { events.push_back(event); }));
	EXPECT_TRUE(research::sh4InstructionOwnershipActive(Backend::Interpreter));
	EXPECT_TRUE(research::sh4InstructionOwnershipActive(Backend::Dynarec));

	Sh4Context context {};
	context.pc = 0x8c010102;
	context.pr = 0x8c020000;
	research::sh4ObservationInstructionBegin(Backend::Interpreter, 0x8c010100,
			0x2102, 100, context);
	const std::uint64_t generation = research::observeAicaG2DmaBegin(
			0x0c100000, 0x00800000, 4, true, 101);
	research::sh4ObservationInstructionEnd(Backend::Interpreter, 0x8c010100,
			0x2102, 102, context);
	const std::array<std::uint8_t, 4> bytes {0x11, 0x22, 0x33, 0x44};
	research::observeAicaG2DmaTransfer(generation, bytes.data(), bytes.size(), 103);
	research::observeAicaG2DmaComplete(200);

	ASSERT_EQ(3u, events.size());
	EXPECT_EQ(research::AicaObservationType::G2DmaBegin, events[0].type);
	EXPECT_EQ(research::AicaObservationType::G2DmaTransfer, events[1].type);
	EXPECT_EQ(research::AicaObservationType::G2DmaComplete, events[2].type);
	EXPECT_NE(0u, generation);
	for (const auto& event : events)
	{
		EXPECT_EQ(generation, event.dmaGeneration);
		EXPECT_EQ(research::AicaWriter::Sh4G2Dma, event.owner.writer);
		EXPECT_TRUE(event.owner.sh4.valid);
		EXPECT_EQ(0x8c010100u, event.owner.sh4.pc);
	}
	EXPECT_EQ(std::vector<std::uint8_t>(bytes.begin(), bytes.end()), events[1].bytes);
}

TEST(ResearchAicaObservation, PreservesRawChannelStateAndMixerComponents)
{
	std::vector<research::AicaObservation> events;
	AicaSubscription subscription(research::subscribeAicaEvidenceObservations(
			[&](const auto& event) { events.push_back(event); }));
	std::array<std::uint8_t, 0x80> registers {};
	for (std::size_t index = 0; index < registers.size(); ++index)
		registers[index] = static_cast<std::uint8_t>(index);
	{
		research::AicaWriterScope writer(research::AicaWriter::Arm7);
		research::observeAicaKeyTransition(true, 17, registers.data(),
				keySourceRam.data(), keySourceRam.size(), 300);
	}
	research::observeAicaSampleFrame(1ull << 17, 100, -101, 20, -21,
			3, -4, true, 5, -6, zeroDspInputs.data(), zeroDspOutputs.data(),
			123, -124, 9, 3, 301);

	ASSERT_EQ(2u, events.size());
	EXPECT_EQ(research::AicaObservationType::KeyOn, events[0].type);
	EXPECT_EQ(17u, events[0].channel);
	EXPECT_EQ(registers, events[0].channelRegisters);
	EXPECT_EQ(research::AicaWriter::Arm7, events[0].owner.writer);
	EXPECT_FALSE(events[0].owner.arm7PcAvailable);
	EXPECT_EQ(research::AicaObservationType::SampleFrame, events[1].type);
	EXPECT_EQ(1ull << 17, events[1].activeChannelMask);
	EXPECT_EQ(100, events[1].dryLeft);
	EXPECT_EQ(-101, events[1].dryRight);
	EXPECT_EQ(20, events[1].cddaInputLeft);
	EXPECT_EQ(-21, events[1].cddaInputRight);
	EXPECT_EQ(3, events[1].cddaContributionLeft);
	EXPECT_EQ(-4, events[1].cddaContributionRight);
	EXPECT_EQ(9u, events[1].cddaGeneration);
	EXPECT_EQ(3u, events[1].cddaFrameIndex);
	EXPECT_TRUE(events[1].dspEnabled);
	EXPECT_EQ(5, events[1].dspContributionLeft);
	EXPECT_EQ(-6, events[1].dspContributionRight);
	EXPECT_EQ(123, events[1].finalLeft);
	EXPECT_EQ(-124, events[1].finalRight);
}

TEST(ResearchAicaObservation, RejectsCompetingEvidenceSubscriber)
{
	AicaSubscription subscription(research::subscribeAicaEvidenceObservations(
			[](const auto&) {}));
	EXPECT_THROW(research::subscribeAicaEvidenceObservations(
			[](const auto&) {}), std::logic_error);
}

TEST(ResearchAicaObservation, BindsPreKeyBatchAndTransitionsToExactSampleCut)
{
	std::vector<research::AicaObservation> events;
	AicaSubscription subscription(research::subscribeAicaEvidenceObservations(
			[&](const auto& event) { events.push_back(event); }));
	const auto cut=research::aicaObservationNextSampleOrdinal();
	std::array<std::uint8_t,0x80> registers{};
	registers[12]=4;
	{
		research::AicaWriterScope writer(research::AicaWriter::Arm7);
		research::observeAicaKeyBatchBegin(100);
		research::observeAicaKeyTransition(true,3,registers.data(),
				keySourceRam.data(),keySourceRam.size(),101);
		research::observeAicaKeyBatchComplete(1ull<<3,0,102);
	}
	research::observeAicaSampleFrame(1ull<<3,0,0,0,0,0,0,true,0,0,
			zeroDspInputs.data(),zeroDspOutputs.data(),0,0,1,0,103);
	ASSERT_EQ(4u,events.size());
	EXPECT_EQ(research::AicaObservationType::KeyBatchBegin,events[0].type);
	EXPECT_EQ(cut,events[0].sampleCutOrdinal);
	EXPECT_EQ(cut,events[1].sampleCutOrdinal);
	EXPECT_EQ(cut,events[2].sampleCutOrdinal);
	EXPECT_EQ(cut,events[3].sampleOrdinal);
	EXPECT_EQ(cut+1,research::aicaObservationNextSampleOrdinal());
}

TEST(ResearchAicaObservation, PreservesStableBatchCddaJoinAndSuppression)
{
	std::vector<research::AicaObservation> events;
	AicaSubscription subscription(research::subscribeAicaEvidenceObservations(
			[&](const auto& event) { events.push_back(event); }));
	research::observeAicaKeyBatchComplete(0x5, 0x2, 10);
	std::array<std::uint8_t, 2352> sector {};
	sector[0] = 0x34; sector[1] = 0x12; sector[2] = 0xfe; sector[3] = 0xff;
	const auto generation = research::observeAicaCddaSector(45150, 1, 3, true,
			sector.data(), sector.size(), 11);
	research::observeAicaSampleFrame(1, 1, 2, 0x1234, -2, 3, 4, true,
			5, 6, zeroDspInputs.data(), zeroDspOutputs.data(),
			7, 8, generation, 0, 12);
	research::observeAicaSampleSuppressed(
			research::AicaSampleSuppression::FastForward, 13);
	ASSERT_EQ(4u, events.size());
	EXPECT_EQ(research::AicaObservationType::KeyBatchComplete, events[0].type);
	EXPECT_EQ(0x5u, events[0].keyOnMask);
	EXPECT_EQ(0x2u, events[0].keyOffMask);
	EXPECT_NE(0u, generation);
	EXPECT_EQ(sector.size(), events[1].bytes.size());
	EXPECT_EQ(generation, events[2].cddaGeneration);
	EXPECT_EQ(0u, events[2].cddaFrameIndex);
	EXPECT_EQ(research::AicaSampleSuppression::FastForward,
			events[3].suppression);
}

TEST(ResearchAicaObservation, DiscoverySubscribersRemainSeparateFromEvidence)
{
	std::size_t first = 0, second = 0;
	{
		AicaSubscription one(research::subscribeAicaObservations(
				[&](const auto&) { ++first; }));
		AicaSubscription two(research::subscribeAicaObservations(
				[&](const auto&) { ++second; }));
		EXPECT_THROW(research::subscribeAicaEvidenceObservations(
				[](const auto&) {}), std::logic_error);
		research::observeAicaRegisterWrite(research::AicaWriter::Sh4Direct,
				0x123, 2, 0x4567, 40);
	}
	EXPECT_EQ(1u, first);
	EXPECT_EQ(1u, second);
	AicaSubscription evidence(research::subscribeAicaEvidenceObservations(
			[](const auto&) {}));
	EXPECT_THROW(research::subscribeAicaObservations(
			[](const auto&) {}), std::logic_error);
}

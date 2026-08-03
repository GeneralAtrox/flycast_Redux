#include "research/pvr_ta_observation.h"

#ifndef FLYCAST_RESEARCH_STANDALONE_TESTS
#include "emulator.h"
#include "hw/mem/addrspace.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/ta_selection.h"
#endif
#include "hw/sh4/sh4_if.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace
{

class PvrSubscription
{
public:
	explicit PvrSubscription(research::PvrTaObservationSubscription id)
		: id(id) {}
	~PvrSubscription() { research::unsubscribePvrTaObservations(id); }

	PvrSubscription(const PvrSubscription&) = delete;
	PvrSubscription& operator=(const PvrSubscription&) = delete;

private:
	research::PvrTaObservationSubscription id;
};

research::PvrTaRenderSelectionTranscript selectionTranscript()
{
	research::PvrTaRenderSelectionTranscript transcript;
	transcript.initialized = true;
	transcript.regionBase = 0x00200000;
	transcript.fpuParamCfg = 0;
	transcript.record(0x00200010, 0);
	transcript.record(0x00200000, 0x80000000);
	transcript.record(0x00200004, 0x00300000);
	transcript.record(0x00300000, 0x00100000);
	return transcript;
}

} // namespace

TEST(ResearchPvrTaObservation, CapturesAcceptedBlockAndRenderCausality)
{
	using Backend = research::Sh4ObservationBackend;
	std::vector<research::PvrTaObservation> observed;
	PvrSubscription subscription(research::subscribePvrTaObservations(
			research::PvrTaObservationFilter {},
			[&observed](const research::PvrTaObservation& observation) {
				observed.push_back(observation);
			}));
	EXPECT_TRUE(research::sh4InstructionOwnershipActive(Backend::Interpreter));
	EXPECT_TRUE(research::sh4InstructionOwnershipActive(Backend::Dynarec));

	Sh4Context context {};
	context.pc = 0x8c010102;
	context.pr = 0x8c020000;
	research::sh4ObservationInstructionBegin(Backend::Interpreter, 0x8c010100,
			0x0083, 100, context);
	research::observePvrTaListBoundary(false, 0x00100000, 0, 101);
	std::array<std::uint8_t, 32> block {};
	for (std::size_t index = 0; index < block.size(); ++index)
		block[index] = static_cast<std::uint8_t>(index);
	const research::PvrTaBlockProvenance provenance =
			research::observePvrTaAcceptedBlock(
			research::PvrTaInputSource::StoreQueue,
			0xe0000020, 0x10000020, block.data(), 0x00100000, 0,
			7, 0, 0, 1, 102);
	const std::uint32_t selected[] {0x00100000};
	const bool available[] {true};
	const auto transcript = selectionTranscript();
	research::observePvrTaStartRender(selected, available, 1, &transcript, 103);
	research::sh4ObservationInstructionEnd(Backend::Interpreter, 0x8c010100,
			0x0083, 104, context);
	research::observePvrTaRenderDone(200);

	ASSERT_EQ(4u, observed.size());
	EXPECT_EQ(research::PvrTaObservationType::ListInit, observed[0].type);
	EXPECT_EQ(research::PvrTaObservationType::AcceptedBlock, observed[1].type);
	EXPECT_EQ(research::PvrTaObservationType::StartRender, observed[2].type);
	EXPECT_EQ(research::PvrTaObservationType::RenderDone, observed[3].type);
	ASSERT_NE(0u, observed[0].contextGeneration);
	EXPECT_EQ(observed[0].contextGeneration, observed[1].contextGeneration);
	ASSERT_EQ(1u, observed[2].selectedContexts.size());
	EXPECT_TRUE(observed[2].selectedContexts[0].available);
	EXPECT_EQ(observed[0].contextGeneration,
			observed[2].selectedContexts[0].generation);
	EXPECT_EQ(observed[2].renderGeneration, observed[3].renderGeneration);
	EXPECT_NE(0u, observed[2].renderGeneration);
	EXPECT_EQ(block, observed[1].block);
	EXPECT_EQ(0u, observed[1].contextBlockOrdinal);
	EXPECT_EQ(7u, observed[1].listTypeBefore);
	EXPECT_EQ(0u, observed[1].listTypeAfter);
	EXPECT_EQ(0u, observed[1].parserStateBefore);
	EXPECT_EQ(1u, observed[1].parserStateAfter);
	EXPECT_TRUE(provenance.available);
	EXPECT_EQ(observed[1].contextGeneration, provenance.contextGeneration);
	EXPECT_EQ(observed[1].contextBlockOrdinal, provenance.contextBlockOrdinal);
	EXPECT_EQ(observed[1].initiator.generation,
			provenance.initiator.generation);
	EXPECT_EQ(transcript.regionBase, observed[2].regionBase);
	EXPECT_EQ(transcript.fpuParamCfg, observed[2].fpuParamCfg);
	ASSERT_EQ(transcript.readCount, observed[2].renderSelectionReadCount);
	for (std::size_t index = 0; index < transcript.readCount; ++index)
	{
		EXPECT_EQ(transcript.reads[index].address,
				observed[2].renderSelectionReads[index].address);
		EXPECT_EQ(transcript.reads[index].value,
				observed[2].renderSelectionReads[index].value);
	}
	for (std::size_t index = 0; index < 3; ++index)
	{
		EXPECT_TRUE(observed[index].initiator.valid);
		EXPECT_EQ(Backend::Interpreter, observed[index].initiator.backend);
		EXPECT_EQ(0x8c010100u, observed[index].initiator.pc);
		EXPECT_EQ(0x0083u, observed[index].initiator.opcode);
		EXPECT_EQ(observed[0].initiator.generation,
				observed[index].initiator.generation);
	}
	EXPECT_FALSE(observed[3].initiator.valid);
	for (std::size_t index = 1; index < observed.size(); ++index)
		EXPECT_EQ(observed[index - 1].emissionOrdinal + 1,
				observed[index].emissionOrdinal);
}

TEST(ResearchPvrTaObservation, RenderGenerationIsSealedAtStartRender)
{
	std::vector<research::PvrTaObservation> observed;
	PvrSubscription subscription(research::subscribePvrTaObservations(
			research::PvrTaObservationFilter {},
			[&observed](const research::PvrTaObservation& observation) {
				observed.push_back(observation);
			}));
	research::observePvrTaListBoundary(false, 0x00100000, 0, 1);
	const std::uint32_t selected[] {0x00100000};
	const bool available[] {true};
	const auto transcript = selectionTranscript();
	research::observePvrTaStartRender(selected, available, 1, &transcript, 2);
	const std::uint64_t sealedRender = observed.back().renderGeneration;
	const std::uint64_t renderedContext =
			observed.back().selectedContexts[0].generation;

	// TA input can begin building the next context while the previous render is
	// pending. It must not change the render-done ownership already sealed.
	research::observePvrTaListBoundary(false, 0x00100000, 0, 3);
	ASSERT_NE(renderedContext, observed.back().contextGeneration);
	research::observePvrTaRenderDone(4);

	ASSERT_EQ(4u, observed.size());
	EXPECT_EQ(sealedRender, observed.back().renderGeneration);
}

TEST(ResearchPvrTaObservation,
		StartRenderRecordsEveryPopResultAndConsumesOnlyAvailableContexts)
{
	std::vector<research::PvrTaObservation> observed;
	PvrSubscription subscription(research::subscribePvrTaObservations(
			research::PvrTaObservationFilter {},
			[&observed](const research::PvrTaObservation& observation) {
				observed.push_back(observation);
			}));
	research::observePvrTaListBoundary(false, 0x00100000, 0, 1);
	const std::uint64_t firstGeneration = observed.back().contextGeneration;
	research::observePvrTaListBoundary(false, 0x00200000, 0, 2);
	const std::uint64_t secondGeneration = observed.back().contextGeneration;

	const std::uint32_t selected[] {
		0x00100000, 0x00200000, 0x00100000,
	};
	const bool available[] {true, false, false};
	const auto transcript = selectionTranscript();
	research::observePvrTaStartRender(selected, available, 3, &transcript, 3);

	const research::PvrTaObservation& render = observed.back();
	ASSERT_EQ(3u, render.selectedContexts.size());
	EXPECT_TRUE(render.selectedContexts[0].available);
	EXPECT_EQ(firstGeneration, render.selectedContexts[0].generation);
	EXPECT_FALSE(render.selectedContexts[1].available);
	EXPECT_EQ(0u, render.selectedContexts[1].generation);
	EXPECT_FALSE(render.selectedContexts[2].available);
	EXPECT_EQ(0u, render.selectedContexts[2].generation);

	const std::uint32_t secondSelected[] {0x00100000, 0x00200000};
	const bool secondAvailable[] {false, true};
	research::observePvrTaStartRender(secondSelected, secondAvailable, 2,
			&transcript, 4);
	const research::PvrTaObservation& secondRender = observed.back();
	ASSERT_EQ(2u, secondRender.selectedContexts.size());
	EXPECT_EQ(0u, secondRender.selectedContexts[0].generation);
	EXPECT_EQ(secondGeneration, secondRender.selectedContexts[1].generation);
}

TEST(ResearchPvrTaObservation,
		RestoredContextWithoutObservedTaInputIsNotClaimedAsCausalEvidence)
{
	std::vector<research::PvrTaObservation> observed;
	PvrSubscription subscription(research::subscribePvrTaObservations(
			research::PvrTaObservationFilter {},
			[&observed](const research::PvrTaObservation& observation) {
				observed.push_back(observation);
			}));
	const std::uint32_t selected[] {0x00100000};
	const bool rendererAvailable[] {true};
	const auto transcript = selectionTranscript();

	research::observePvrTaStartRender(selected, rendererAvailable, 1,
			&transcript, 1);

	ASSERT_EQ(1u, observed.size());
	ASSERT_EQ(1u, observed[0].selectedContexts.size());
	EXPECT_EQ(0x00100000u, observed[0].selectedContexts[0].address);
	EXPECT_FALSE(observed[0].renderContextAvailable);
	EXPECT_FALSE(observed[0].selectedContexts[0].available);
	EXPECT_EQ(0u, observed[0].selectedContexts[0].generation);
}

TEST(ResearchPvrTaObservation, FiltersSourcesAndIsolatesCallbackFailures)
{
	const std::uint64_t dropsBefore = research::pvrTaObservationDroppedCount();
	std::vector<research::PvrTaObservation> filtered;
	research::PvrTaObservationFilter storeQueueOnly;
	storeQueueOnly.typeMask = research::pvrTaObservationTypeBit(
			research::PvrTaObservationType::AcceptedBlock);
	storeQueueOnly.sourceMask = research::pvrTaInputSourceBit(
			research::PvrTaInputSource::StoreQueue);
	PvrSubscription throwing(research::subscribePvrTaObservations(storeQueueOnly,
			[](const research::PvrTaObservation&) {
				throw std::runtime_error("discovery failure");
			}));
	PvrSubscription collecting(research::subscribePvrTaObservations(storeQueueOnly,
			[&filtered](const research::PvrTaObservation& observation) {
				filtered.push_back(observation);
			}));
	research::observePvrTaListBoundary(false, 0x00100000, 0, 1);
	std::array<std::uint8_t, 32> block {};
	research::observePvrTaAcceptedBlock(research::PvrTaInputSource::Channel2Dma,
			0x0c001000, 0x10000000, block.data(), 0x00100000, 0,
			7, 7, 0, 0, 2);
	EXPECT_NO_THROW(research::observePvrTaAcceptedBlock(
			research::PvrTaInputSource::StoreQueue, 0xe0000000, 0x10000000,
			block.data(), 0x00100000, 0, 7, 7, 0, 0, 3));

	ASSERT_EQ(1u, filtered.size());
	EXPECT_EQ(research::PvrTaInputSource::StoreQueue, filtered[0].source);
	EXPECT_EQ(dropsBefore + 1, research::pvrTaObservationDroppedCount());
}

TEST(ResearchPvrTaObservation, ResetInvalidatesRawAddressGeneration)
{
	std::vector<research::PvrTaObservation> observed;
	PvrSubscription subscription(research::subscribePvrTaObservations(
			research::PvrTaObservationFilter {},
			[&observed](const research::PvrTaObservation& observation) {
				observed.push_back(observation);
			}));
	research::observePvrTaListBoundary(false, 0x00100000, 0, 1);
	const std::uint64_t beforeReset = observed.back().contextGeneration;
	research::resetPvrTaObservation(2);
	research::observePvrTaListBoundary(true, 0x00100000, 1, 3);
	research::observePvrTaListBoundary(false, 0x00100000, 0, 4);

	ASSERT_EQ(4u, observed.size());
	EXPECT_EQ(research::PvrTaObservationType::Reset, observed[1].type);
	EXPECT_EQ(0u, observed[2].contextGeneration);
	EXPECT_NE(0u, observed[3].contextGeneration);
	EXPECT_NE(beforeReset, observed[3].contextGeneration);
}

TEST(ResearchPvrTaObservation, EvidenceSubscriptionOwnsBusExclusively)
{
	PvrSubscription evidence(research::subscribePvrTaEvidenceObservations(
			[](const research::PvrTaObservation&) {}));
	EXPECT_TRUE(research::pvrTaEvidenceSubscriptionActive());
	EXPECT_THROW(research::subscribePvrTaObservations(
			research::PvrTaObservationFilter {},
			[](const research::PvrTaObservation&) {}), std::logic_error);
}

TEST(ResearchPvrTaObservation, EvidenceSubscriptionRejectsExistingDiscovery)
{
	PvrSubscription discovery(research::subscribePvrTaObservations(
			research::PvrTaObservationFilter {},
			[](const research::PvrTaObservation&) {}));
	EXPECT_THROW(research::subscribePvrTaEvidenceObservations(
			[](const research::PvrTaObservation&) {}), std::logic_error);
}

TEST(ResearchPvrTaObservation, IncompleteRenderSelectionIsCountedAsDropped)
{
	std::vector<research::PvrTaObservation> observed;
	PvrSubscription evidence(research::subscribePvrTaEvidenceObservations(
			[&observed](const research::PvrTaObservation& observation) {
				observed.push_back(observation);
			}));
	const std::uint64_t dropsBefore = research::pvrTaObservationDroppedCount();
	const std::uint32_t selected[] {0x00100000};
	const bool available[] {true};
	research::observePvrTaStartRender(selected, available, 1, nullptr, 1);

	EXPECT_TRUE(observed.empty());
	EXPECT_EQ(dropsBefore + 1, research::pvrTaObservationDroppedCount());
}

TEST(ResearchPvrTaObservation, CanonicalizesSystemRamOffsets)
{
	EXPECT_EQ(0x0c000000u,
			research::canonicalPvrTaSystemRamAddress(0));
	EXPECT_EQ(0x0c123456u,
			research::canonicalPvrTaSystemRamAddress(0x00123456));
}

TEST(ResearchPvrTaObservation, SchedulerBaseTickCannotPrecedeInstructionOwner)
{
	std::vector<research::PvrTaObservation> observed;
	PvrSubscription subscription(research::subscribePvrTaObservations(
			research::PvrTaObservationFilter {},
			[&observed](const research::PvrTaObservation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.pc = 0x8c010102;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter,
			0x8c010100, 0x2102, 100, context);
	research::observePvrTaListBoundary(false, 0x00100000, 0, 90);
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter,
			0x8c010100, 0x2102, 101, context);
	ASSERT_EQ(1u, observed.size());
	EXPECT_EQ(100u, observed[0].initiator.tick);
	EXPECT_EQ(100u, observed[0].tick);
}

#ifndef FLYCAST_RESEARCH_STANDALONE_TESTS
TEST(ResearchPvrTaObservation,
		RenderSelectionTranscriptUsesTheExactValuesThatSelectTheContext)
{
	if (!addrspace::reserve())
		GTEST_SKIP() << "address-space reservation is unavailable";
	emu.init();
	vram.zero();
	REGION_BASE = 0x00001000;
	FPU_PARAM_CFG = 0; // Type-1, 20-byte region-array tiles.
	constexpr std::uint32_t lastRegion = 0x80000000;
	constexpr std::uint32_t opbAddress = 0x00002000;
	constexpr std::uint32_t contextAddress = 0x00123400;
	pvr_write32p<std::uint32_t>(REGION_BASE, lastRegion);
	pvr_write32p<std::uint32_t>(REGION_BASE + 4, opbAddress);
	pvr_write32p<std::uint32_t>(opbAddress, contextAddress);

	std::array<std::uint32_t, 10> untracedAddresses {};
	std::array<std::uint32_t, 10> tracedAddresses {};
	ASSERT_EQ(1, getTAContextAddresses(untracedAddresses.data()));
	research::PvrTaRenderSelectionTranscript transcript;
	ASSERT_EQ(1, getTAContextAddresses(tracedAddresses.data(), &transcript));
	EXPECT_EQ(untracedAddresses[0], tracedAddresses[0]);
	EXPECT_EQ(contextAddress, tracedAddresses[0]);
	EXPECT_TRUE(transcript.initialized);
	EXPECT_FALSE(transcript.overflow);
	EXPECT_EQ(REGION_BASE, transcript.regionBase);
	EXPECT_EQ(FPU_PARAM_CFG, transcript.fpuParamCfg);

	const std::array<research::PvrTaVramRead, 6> expected {{
			{REGION_BASE + 16, 0},
			{REGION_BASE, lastRegion},
			{REGION_BASE, lastRegion},
			{REGION_BASE, lastRegion},
			{REGION_BASE + 4, opbAddress},
			{opbAddress, contextAddress},
	}};
	ASSERT_EQ(expected.size(), transcript.readCount);
	for (std::size_t index = 0; index < expected.size(); ++index)
	{
		EXPECT_EQ(expected[index].address, transcript.reads[index].address)
				<< "read " << index;
		EXPECT_EQ(expected[index].value, transcript.reads[index].value)
				<< "read " << index;
	}
}

TEST(ResearchPvrTaObservation,
		RenderSelectionTranscriptPreservesEmptyType2PunchThroughFallback)
{
	if (!addrspace::reserve())
		GTEST_SKIP() << "address-space reservation is unavailable";
	emu.init();
	vram.zero();
	REGION_BASE = 0x00003000;
	FPU_PARAM_CFG = 1u << 21; // Type-2, 24-byte region-array tiles.
	constexpr std::uint32_t nullPointer = 0x80000000;
	for (std::uint32_t offset : {4u, 8u, 12u, 16u, 20u})
		pvr_write32p<std::uint32_t>(REGION_BASE + offset, nullPointer);
	constexpr std::uint32_t secondRegion = 0x00003018;
	constexpr std::uint32_t lastPreSortedRegion = 0xa0000000;
	constexpr std::uint32_t opbAddress = 0x00004000;
	constexpr std::uint32_t contextAddress = 0x00567800;
	pvr_write32p<std::uint32_t>(secondRegion, lastPreSortedRegion);
	pvr_write32p<std::uint32_t>(secondRegion + 4, nullPointer);
	pvr_write32p<std::uint32_t>(secondRegion + 12, nullPointer);
	pvr_write32p<std::uint32_t>(secondRegion + 20, opbAddress);
	pvr_write32p<std::uint32_t>(opbAddress, contextAddress);

	std::array<std::uint32_t, 10> addresses {};
	research::PvrTaRenderSelectionTranscript transcript;
	ASSERT_EQ(1, getTAContextAddresses(addresses.data(), &transcript));
	EXPECT_EQ(contextAddress, addresses[0]);
	const std::array<research::PvrTaVramRead, 12> expected {{
			{REGION_BASE + 20, nullPointer},
			{REGION_BASE + 16, nullPointer},
			{REGION_BASE + 12, nullPointer},
			{REGION_BASE + 8, nullPointer},
			{REGION_BASE + 4, nullPointer},
			{secondRegion, lastPreSortedRegion},
			{secondRegion, lastPreSortedRegion},
			{secondRegion, lastPreSortedRegion},
			{secondRegion + 4, nullPointer},
			{secondRegion + 12, nullPointer},
			{secondRegion + 20, opbAddress},
			{opbAddress, contextAddress},
	}};
	ASSERT_EQ(expected.size(), transcript.readCount);
	for (std::size_t index = 0; index < expected.size(); ++index)
	{
		EXPECT_EQ(expected[index].address, transcript.reads[index].address)
				<< "read " << index;
		EXPECT_EQ(expected[index].value, transcript.reads[index].value)
				<< "read " << index;
	}
}
#endif

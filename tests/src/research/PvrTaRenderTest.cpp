#include "PvrTaObservationSupport.h"

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

using pvr_ta_test::PvrSubscription;
using pvr_ta_test::selectionTranscript;

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

TEST(ResearchPvrTaObservation, FrozenRenderWindowRejectsLaterGenerations)
{
	std::vector<research::PvrTaObservation> observed;
	PvrSubscription subscription(research::subscribePvrTaObservations(
			research::PvrTaObservationFilter {},
			[&observed](const research::PvrTaObservation& observation) {
				observed.push_back(observation);
			}));
	const std::uint32_t selected[] {0x00100000};
	const bool available[] {true};
	const auto transcript = selectionTranscript();

	research::observePvrTaListBoundary(false, 0x00100000, 0, 1);
	const std::uint64_t included = research::observePvrTaStartRender(
			selected, available, 1, &transcript, 2);
	ASSERT_TRUE(research::pvrTaRenderGenerationObserved(included));
	research::freezePvrTaObservedRenderGenerationWindow();

	research::observePvrTaListBoundary(false, 0x00100000, 0, 3);
	const std::uint64_t excluded = research::observePvrTaStartRender(
			selected, available, 1, &transcript, 4);
	EXPECT_TRUE(research::pvrTaRenderGenerationObserved(included));
	EXPECT_FALSE(research::pvrTaRenderGenerationObserved(excluded));
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

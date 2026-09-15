#include "PvrTaObservationSupport.h"

#include "research/pvr_ta_observation.h"

#include "hw/sh4/sh4_if.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

using pvr_ta_test::PvrSubscription;
using pvr_ta_test::selectionTranscript;

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

TEST(ResearchPvrTaObservation,
		DelayedObservationSuppressesOrphanContextUntilObservedListInit)
{
	std::vector<research::PvrTaObservation> observed;
	PvrSubscription subscription(research::subscribePvrTaObservations(
			research::PvrTaObservationFilter {},
			[&observed](const research::PvrTaObservation& observation) {
				observed.push_back(observation);
			}));
	const std::uint64_t dropsBefore = research::pvrTaObservationDroppedCount();
	std::array<std::uint8_t, 32> block {};

	// Observation starts after this context's ListInit. Neither its continuation
	// nor its blocks may be serialized with a zero context generation.
	research::observePvrTaListBoundary(true, 0x00100000, 1, 1);
	const research::PvrTaBlockProvenance orphan =
			research::observePvrTaAcceptedBlock(
			research::PvrTaInputSource::Channel2Dma,
			0x0c001000, 0x10000000, block.data(), 0x00100000, 1,
			7, 7, 0, 0, 2);

	EXPECT_TRUE(observed.empty());
	EXPECT_FALSE(orphan.available);
	EXPECT_EQ(0x00100000u, orphan.contextAddress);
	EXPECT_EQ(0u, orphan.contextGeneration);
	EXPECT_EQ(research::PvrTaInputSource::Channel2Dma, orphan.source);
	EXPECT_EQ(0x0c001000u, orphan.sourceAddress);
	EXPECT_EQ(0x10000000u, orphan.taAddress);
	EXPECT_EQ(dropsBefore, research::pvrTaObservationDroppedCount());

	// The next observed ListInit establishes a complete causal context and the
	// following block resumes normal evidence publication.
	research::observePvrTaListBoundary(false, 0x00100000, 0, 3);
	const research::PvrTaBlockProvenance complete =
			research::observePvrTaAcceptedBlock(
			research::PvrTaInputSource::Channel2Dma,
			0x0c001020, 0x10000020, block.data(), 0x00100000, 0,
			7, 7, 0, 0, 4);

	ASSERT_EQ(2u, observed.size());
	EXPECT_EQ(research::PvrTaObservationType::ListInit, observed[0].type);
	EXPECT_EQ(research::PvrTaObservationType::AcceptedBlock, observed[1].type);
	EXPECT_NE(0u, observed[0].contextGeneration);
	EXPECT_EQ(observed[0].contextGeneration, observed[1].contextGeneration);
	EXPECT_EQ(observed[0].emissionOrdinal + 1, observed[1].emissionOrdinal);
	EXPECT_TRUE(complete.available);
	EXPECT_EQ(observed[1].contextGeneration, complete.contextGeneration);
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

	ASSERT_EQ(3u, observed.size());
	EXPECT_EQ(research::PvrTaObservationType::Reset, observed[1].type);
	EXPECT_EQ(research::PvrTaObservationType::ListInit, observed[2].type);
	EXPECT_NE(0u, observed[2].contextGeneration);
	EXPECT_NE(beforeReset, observed[2].contextGeneration);
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

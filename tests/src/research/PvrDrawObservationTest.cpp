#include "research/pvr_draw_observation.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace
{

class DrawSubscription
{
public:
	explicit DrawSubscription(research::PvrDrawObservationSubscription id)
			: id(id) {}
	~DrawSubscription() { research::unsubscribePvrDrawObservations(id); }

private:
	research::PvrDrawObservationSubscription id;
};

research::PvrTaBlockProvenance block(std::uint64_t blockOrdinal,
		std::uint64_t ownerGeneration, std::uint32_t pc)
{
	research::PvrTaBlockProvenance result;
	result.available = true;
	result.contextAddress = 0x00500000;
	result.contextGeneration = 7;
	result.contextBlockOrdinal = blockOrdinal;
	result.renderPass = 0;
	result.sourceAddress = 0xe0000000;
	result.taAddress = 0x10000000;
	result.initiator.valid = true;
	result.initiator.backend = research::Sh4ObservationBackend::Interpreter;
	result.initiator.generation = ownerGeneration;
	result.initiator.tick = 100 + blockOrdinal;
	result.initiator.pc = pc;
	result.initiator.pr = 0x8c020000;
	result.initiator.opcode = 0x2102;
	return result;
}

} // namespace

TEST(ResearchPvrDrawObservation, ClassifiesExactMixedAndUnownedContributors)
{
	using namespace research;
	EXPECT_EQ(PvrPrimitiveOwnerClass::Unowned,
			classifyPvrPrimitiveOwnership({}, {}));
	const auto header = block(0, 9, 0x8c010100);
	auto vertex = block(1, 9, 0x8c010100);
	vertex.initiator.tick = header.initiator.tick;
	EXPECT_EQ(PvrPrimitiveOwnerClass::Exact,
			classifyPvrPrimitiveOwnership({header}, {vertex}));
	vertex.initiator.generation = 10;
	vertex.initiator.pc = 0x8c010120;
	EXPECT_EQ(PvrPrimitiveOwnerClass::Mixed,
			classifyPvrPrimitiveOwnership({header}, {vertex}));
	vertex.available = false;
	EXPECT_EQ(PvrPrimitiveOwnerClass::Mixed,
			classifyPvrPrimitiveOwnership({header}, {vertex}));
}

TEST(ResearchPvrDrawObservation, PreservesPrimitiveAndCommittedDrawBoundary)
{
	using namespace research;
	std::vector<PvrDrawObservation> events;
	DrawSubscription subscription(subscribePvrDrawObservations(
			[&](const PvrDrawObservation& event) { events.push_back(event); }));

	PvrDrawObservation primitive;
	primitive.tick = 200;
	primitive.renderGeneration = 17;
	primitive.primitiveGeneration = allocatePvrPrimitiveGeneration();
	primitive.contextAddress = 0x00500000;
	primitive.contextGeneration = 7;
	primitive.listType = 0;
	primitive.primitiveKind = PvrPrimitiveKind::PolygonStrip;
	primitive.count = 3;
	primitive.parameterBlocks = {block(0, 9, 0x8c010100)};
	primitive.vertexBlocks = {block(1, 10, 0x8c010120)};
	observePvrPrimitiveDecoded(primitive);

	const std::uint64_t rasterGeneration = observePvrDrawConsumed(17,
			{primitive.primitiveGeneration}, PvrDrawBackend::DirectX11,
			PvrDrawPass::Color, 4, 3, true, 201);
	observePvrDrawRenderCompleted(17, true, 202);

	ASSERT_EQ(3u, events.size());
	EXPECT_EQ(PvrDrawObservationType::PrimitiveDecoded, events[0].type);
	EXPECT_EQ(PvrPrimitiveOwnerClass::Mixed, events[0].ownerClass);
	EXPECT_EQ(2u, events[0].parameterBlocks.size()
			+ events[0].vertexBlocks.size());
	EXPECT_EQ(PvrDrawObservationType::DrawConsumed, events[1].type);
	EXPECT_EQ(rasterGeneration, events[1].rasterGeneration);
	EXPECT_EQ((std::vector<std::uint64_t> {primitive.primitiveGeneration}),
			events[1].primitiveGenerations);
	EXPECT_EQ(PvrDrawObservationType::RenderCompleted, events[2].type);
	EXPECT_TRUE(events[2].successful);
}

TEST(ResearchPvrDrawObservation, RecordsCommittedUnownedInternalDraw)
{
	using namespace research;
	std::vector<PvrDrawObservation> events;
	DrawSubscription subscription(subscribePvrDrawObservations(
			[&](const PvrDrawObservation& event) { events.push_back(event); }));
	EXPECT_NE(0u, observePvrDrawConsumed(3, {}, PvrDrawBackend::DirectX11,
			PvrDrawPass::ModifierResolve, 0, 4, true, 40));
	ASSERT_EQ(1u, events.size());
	EXPECT_TRUE(events[0].primitiveGenerations.empty());
}

TEST(ResearchPvrDrawObservation, ZeroPrimitiveSentinelsAreNotPublishedAsOwners)
{
	using namespace research;
	std::vector<PvrDrawObservation> events;
	DrawSubscription subscription(subscribePvrDrawObservations(
			[&](const PvrDrawObservation& event) { events.push_back(event); }));
	EXPECT_NE(0u, observePvrDrawConsumed(3, {0, 7, 0},
			PvrDrawBackend::DirectX11, PvrDrawPass::Color,
			0, 3, true, 40));
	ASSERT_EQ(1u, events.size());
	EXPECT_EQ((std::vector<std::uint64_t> {7}),
			events[0].primitiveGenerations);
	EXPECT_EQ(0u, observePvrDrawConsumed(3, {0, 0},
			PvrDrawBackend::DirectX11, PvrDrawPass::Color,
			0, 3, true, 41));
	EXPECT_EQ(1u, events.size());
}

TEST(ResearchPvrDrawObservation, EvidenceSubscriptionIsExclusive)
{
	using namespace research;
	DrawSubscription evidence(subscribePvrDrawEvidenceObservations(
			[](const PvrDrawObservation&) {}));
	EXPECT_TRUE(pvrDrawEvidenceSubscriptionActive());
	EXPECT_THROW(subscribePvrDrawObservations(
			[](const PvrDrawObservation&) {}), std::logic_error);
}

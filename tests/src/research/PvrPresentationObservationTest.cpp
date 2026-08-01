#include "research/pvr_presentation_observation.h"

#include "hw/sh4/sh4_if.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace
{

class PresentationSubscription
{
public:
	explicit PresentationSubscription(
			research::PvrPresentationObservationSubscription id) : id(id) {}
	~PresentationSubscription()
	{
		research::unsubscribePvrPresentationObservations(id);
	}

private:
	research::PvrPresentationObservationSubscription id;
};

} // namespace

TEST(ResearchPvrPresentationObservation, PreservesTypedOwnershipAndGenerations)
{
	using namespace research;
	std::vector<PvrPresentationObservation> events;
	PresentationSubscription subscription(subscribePvrPresentationObservations(
			[&](const PvrPresentationObservation& event) { events.push_back(event); }));

	Sh4Context context {};
	context.pc = 0x8c010102;
	context.pr = 0x8c020000;
	sh4ObservationInstructionBegin(Sh4ObservationBackend::Interpreter,
			0x8c010100, 0x2102, 100, context);
	observePvrRegisterWrite(0x005f8050, 0x50, 0x01000003, 0,
			0x01000000, PvrRegisterWriteDisposition::MaskedAndStored, 17, 101);
	const std::array<std::uint8_t, 4> write {{0x11, 0x22, 0x33, 0x44}};
	observePvrVramWrite(PvrVramWriteSource::Sh4Area1Mapped, 0x05000020,
			0x40, write.data(), write.size(), 0, 102);
	sh4ObservationInstructionEnd(Sh4ObservationBackend::Interpreter,
			0x8c010100, 0x2102, 103, context);

	observePvrRenderQueued(17, PvrRenderKind::Screen, 0x00200000, 104);
	observePvrRenderCompleted(17, PvrRenderKind::Screen, true, 105);
	PvrFramebufferConfig config;
	config.fbReadSize = 0x001d027f;
	config.fbReadControl = 1;
	const std::array<std::uint8_t, 8> framebuffer {{
			0, 1, 2, 3, 4, 5, 6, 7}};
	const std::uint64_t framebufferGeneration = observePvrFramebufferCaptured(
			PvrFramebufferKind::DreamcastVram, 0, config, 2, 1, 8,
			framebuffer.data(), framebuffer.size(), 106);
	const std::uint64_t presentationGeneration = observePvrPresentation(
			PvrPresentationSource::Framebuffer, framebufferGeneration, true, 107);

	ASSERT_EQ(6u, events.size());
	EXPECT_EQ(PvrPresentationObservationType::RegisterWrite, events[0].type);
	EXPECT_TRUE(events[0].initiator.valid);
	EXPECT_EQ(0x8c010100u, events[0].initiator.pc);
	EXPECT_EQ(PvrRegisterWriteDisposition::MaskedAndStored,
			events[0].registerDisposition);
	EXPECT_EQ(17u, events[0].renderGeneration);

	EXPECT_EQ(PvrPresentationObservationType::VramWrite, events[1].type);
	EXPECT_TRUE(events[1].initiator.valid);
	EXPECT_EQ(std::vector<std::uint8_t>(write.begin(), write.end()),
			events[1].bytes);

	EXPECT_EQ(PvrPresentationObservationType::RenderQueued, events[2].type);
	EXPECT_FALSE(events[2].initiator.valid);
	EXPECT_EQ(PvrPresentationObservationType::RenderCompleted, events[3].type);
	EXPECT_EQ(17u, events[3].renderGeneration);
	EXPECT_TRUE(events[3].successful);

	EXPECT_EQ(PvrPresentationObservationType::FramebufferCaptured, events[4].type);
	EXPECT_EQ(framebufferGeneration, events[4].framebufferGeneration);
	EXPECT_EQ(std::vector<std::uint8_t>(framebuffer.begin(), framebuffer.end()),
			events[4].bytes);
	EXPECT_EQ(PvrPresentationObservationType::Presentation, events[5].type);
	EXPECT_EQ(presentationGeneration, events[5].presentationGeneration);
	EXPECT_EQ(framebufferGeneration, events[5].sourceGeneration);
	EXPECT_TRUE(events[5].successful);
}

TEST(ResearchPvrPresentationObservation, CapturesDirectAreaOneMemoryWrites)
{
	using namespace research;
	std::vector<PvrPresentationObservation> events;
	PresentationSubscription subscription(subscribePvrPresentationObservations(
			[&](const PvrPresentationObservation& event) { events.push_back(event); }));

	Sh4Context context {};
	context.pc = 0x8c100002;
	sh4ObservationInstructionBegin(Sh4ObservationBackend::Interpreter,
			0x8c100000, 0x2102, 500, context);
	sh4ObservationMemoryAccess(Sh4ObservationBackend::Interpreter, 0xa4000020,
			4, Sh4MemoryAccessKind::Write, 0x44332211);
	sh4ObservationInstructionEnd(Sh4ObservationBackend::Interpreter,
			0x8c100000, 0x2102, 501, context);

	ASSERT_EQ(1u, events.size());
	EXPECT_EQ(PvrPresentationObservationType::VramWrite, events[0].type);
	EXPECT_EQ(PvrVramWriteSource::Sh4Area1Direct, events[0].vramSource);
	EXPECT_EQ(0xa4000020u, events[0].logicalAddress);
	EXPECT_EQ(0x20u, events[0].physicalAddress);
	EXPECT_EQ((std::vector<std::uint8_t> {0x11, 0x22, 0x33, 0x44}),
			events[0].bytes);
	EXPECT_TRUE(events[0].initiator.valid);
}

TEST(ResearchPvrPresentationObservation, EvidenceSubscriptionIsExclusive)
{
	using namespace research;
	PresentationSubscription evidence(subscribePvrPresentationEvidenceObservations(
			[](const PvrPresentationObservation&) {}));
	EXPECT_TRUE(pvrPresentationEvidenceSubscriptionActive());
	EXPECT_THROW(subscribePvrPresentationObservations(
			[](const PvrPresentationObservation&) {}), std::logic_error);
}

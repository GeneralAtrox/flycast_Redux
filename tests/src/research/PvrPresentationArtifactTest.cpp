#include "research/pvr_presentation_artifact.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <vector>

namespace
{

class TemporaryArtifact
{
public:
	TemporaryArtifact()
	{
		path = std::filesystem::temp_directory_path()
				/ ("flycast-pvr-presentation-"
						+ std::to_string(++next) + ".bin");
		std::error_code ignored;
		std::filesystem::remove(path, ignored);
	}
	~TemporaryArtifact()
	{
		std::error_code ignored;
		std::filesystem::remove(path, ignored);
	}

	std::filesystem::path path;
	static std::uint64_t next;
};

std::uint64_t TemporaryArtifact::next = 0;

research::PvrPresentationArtifactBinding binding()
{
	research::PvrPresentationArtifactBinding result;
	result.backend = research::Sh4ObservationBackend::Interpreter;
	result.identityDigest.fill(0x11);
	result.replayDigest.fill(0x22);
	return result;
}

research::Sh4InstructionOwnerToken owner()
{
	research::Sh4InstructionOwnerToken result;
	result.valid = true;
	result.backend = research::Sh4ObservationBackend::Interpreter;
	result.generation = 4;
	result.tick = 100;
	result.pc = 0x8c010100;
	result.pr = 0x8c020000;
	result.opcode = 0x2102;
	return result;
}

std::vector<research::PvrPresentationObservation> completeEvents()
{
	using namespace research;
	std::vector<PvrPresentationObservation> events;
	auto make = [&](PvrPresentationObservationType type, std::uint64_t tick) {
		PvrPresentationObservation event;
		event.type = type;
		event.emissionOrdinal = events.size();
		event.tick = tick;
		return event;
	};
	auto initial = make(PvrPresentationObservationType::InitialRegisterState, 99);
	initial.bytes.resize(0x8000);
	events.push_back(initial);

	auto registerWrite = make(PvrPresentationObservationType::RegisterWrite, 100);
	registerWrite.initiator = owner();
	registerWrite.registerPhysicalAddress = 0x005f8050;
	registerWrite.registerAddress = 0x50;
	registerWrite.requestedValue = 0x1003;
	registerWrite.effectiveValue = 0x1000;
	registerWrite.registerDisposition = PvrRegisterWriteDisposition::MaskedAndStored;
	events.push_back(registerWrite);

	auto write = make(PvrPresentationObservationType::VramWrite, 101);
	write.initiator = owner();
	write.vramSource = PvrVramWriteSource::Sh4Area1Direct;
	write.logicalAddress = 0xa4000020;
	write.physicalAddress = 0x20;
	write.bytes = {1, 2, 3, 4};
	events.push_back(write);

	auto framebuffer = make(PvrPresentationObservationType::FramebufferCaptured, 102);
	framebuffer.framebufferGeneration = 7;
	framebuffer.framebufferWidth = 1;
	framebuffer.framebufferHeight = 1;
	framebuffer.framebufferRowBytes = 2;
	framebuffer.framebufferConfig.fbReadControl = 1u << 2;
	framebuffer.bytes = {0xe0, 0x07};
	events.push_back(framebuffer);

	auto queued = make(PvrPresentationObservationType::RenderQueued, 103);
	queued.renderGeneration = 7;
	queued.renderKind = PvrRenderKind::DirectFramebuffer;
	queued.framebufferWriteAddress = 0x20;
	queued.successful = true;
	events.push_back(queued);

	auto completed = make(PvrPresentationObservationType::RenderCompleted, 104);
	completed.renderGeneration = 7;
	completed.renderKind = PvrRenderKind::DirectFramebuffer;
	completed.successful = true;
	events.push_back(completed);

	auto present = make(PvrPresentationObservationType::Presentation, 105);
	present.presentationGeneration = 9;
	present.presentationSource = PvrPresentationSource::Framebuffer;
	present.sourceGeneration = 7;
	present.successful = true;
	events.push_back(present);
	return events;
}

} // namespace

TEST(ResearchPvrPresentationArtifact, IndependentlyValidatesCompleteSlice)
{
	TemporaryArtifact artifact;
	const auto expectedBinding = binding();
	research::PvrPresentationArtifactWriter writer(artifact.path,
			expectedBinding);
	for (const auto& event : completeEvents())
		writer.write(event);
	const auto written = writer.finalize();
	EXPECT_EQ(7u, written.eventCount);

	const auto validated = research::validatePvrPresentationArtifactFile(
			artifact.path, expectedBinding);
	EXPECT_EQ(7u, validated.eventCount);
	EXPECT_EQ(1u, validated.decodedFramebufferCount);
	EXPECT_TRUE(validated.completeVerticalSlice);
}

TEST(ResearchPvrPresentationArtifact, PreservesFailedAttemptBeforeSuccessfulPresentation)
{
	TemporaryArtifact artifact;
	const auto expectedBinding = binding();
	auto events = completeEvents();
	auto failed = events.back();
	failed.presentationGeneration = 8;
	failed.successful = false;
	events.insert(events.end() - 1, failed);
	for (std::size_t index = 0; index < events.size(); ++index)
		events[index].emissionOrdinal = index;
	{
		research::PvrPresentationArtifactWriter writer(artifact.path,
				expectedBinding);
		for (const auto& event : events)
			writer.write(event);
		writer.finalize();
	}

	const auto validated = research::validatePvrPresentationArtifactFile(
			artifact.path, expectedBinding);
	EXPECT_EQ(8u, validated.eventCount);
	EXPECT_TRUE(validated.completeVerticalSlice);
}

TEST(ResearchPvrPresentationArtifact, FailedPresentationDoesNotCompleteSlice)
{
	TemporaryArtifact artifact;
	const auto expectedBinding = binding();
	auto events = completeEvents();
	events.back().successful = false;
	{
		research::PvrPresentationArtifactWriter writer(artifact.path,
				expectedBinding);
		for (const auto& event : events)
			writer.write(event);
		writer.finalize();
	}

	const auto validated = research::validatePvrPresentationArtifactFile(
			artifact.path, expectedBinding);
	EXPECT_FALSE(validated.completeVerticalSlice);
}

TEST(ResearchPvrPresentationArtifact, RejectsPayloadMutation)
{
	TemporaryArtifact artifact;
	const auto expectedBinding = binding();
	{
		research::PvrPresentationArtifactWriter writer(artifact.path,
				expectedBinding);
		for (const auto& event : completeEvents())
			writer.write(event);
		writer.finalize();
	}
	std::fstream file(artifact.path, std::ios::binary | std::ios::in | std::ios::out);
	file.seekg(-1, std::ios::end);
	char value = 0;
	file.read(&value, 1);
	value ^= 1;
	file.seekp(-1, std::ios::end);
	file.write(&value, 1);
	file.close();
	EXPECT_THROW(research::validatePvrPresentationArtifactFile(
			artifact.path, expectedBinding), std::runtime_error);
}

TEST(ResearchPvrPresentationArtifact, RejectsPresentationBeforeCompletion)
{
	TemporaryArtifact artifact;
	const auto expectedBinding = binding();
	auto events = completeEvents();
	std::swap(events[4], events[5]);
	for (std::size_t index = 0; index < events.size(); ++index)
		events[index].emissionOrdinal = index;
	{
		research::PvrPresentationArtifactWriter writer(artifact.path,
				expectedBinding);
		for (const auto& event : events)
			writer.write(event);
		writer.finalize();
	}
	EXPECT_THROW(research::validatePvrPresentationArtifactFile(
			artifact.path, expectedBinding), std::runtime_error);
}

TEST(ResearchPvrPresentationArtifact, RefusesDropsAndExistingOutput)
{
	TemporaryArtifact artifact;
	const auto expectedBinding = binding();
	research::PvrPresentationArtifactWriter writer(artifact.path, expectedBinding);
	for (const auto& event : completeEvents())
		writer.write(event);
	EXPECT_THROW(writer.finalize(1), std::runtime_error);

	{
		std::ofstream existing(artifact.path, std::ios::binary);
		existing.put('x');
	}
	EXPECT_THROW(research::PvrPresentationArtifactWriter(artifact.path,
			expectedBinding), std::runtime_error);
}

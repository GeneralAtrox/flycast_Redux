#include "research/pvr_ta_artifact.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

class TemporaryDirectory
{
public:
	TemporaryDirectory()
	{
		static std::atomic<unsigned> sequence {0};
		path = std::filesystem::temp_directory_path()
				/ ("flycast-pvr-ta-artifact-test-"
						+ std::to_string(sequence.fetch_add(1)));
		std::error_code error;
		std::filesystem::remove_all(path, error);
		std::filesystem::create_directories(path);
	}

	~TemporaryDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	std::filesystem::path file(const char *name) const { return path / name; }

private:
	std::filesystem::path path;
};

research::Sha256Digest digest(const char *text)
{
	return research::sha256(text, std::char_traits<char>::length(text));
}

research::PvrTaArtifactBinding binding()
{
	research::PvrTaArtifactBinding result;
	result.backend = research::Sh4ObservationBackend::Interpreter;
	result.identityDigest = digest("pvr-identity-v2");
	result.replayDigest = digest("pvr-maple-replay");
	result.manifestDigest = digest("pvr-capture-manifest");
	return result;
}

research::Sh4InstructionOwnerToken owner(std::uint64_t generation,
		std::uint64_t tick)
{
	research::Sh4InstructionOwnerToken result;
	result.valid = true;
	result.backend = research::Sh4ObservationBackend::Interpreter;
	result.generation = generation;
	result.tick = tick;
	result.pc = 0x8c010100;
	result.pr = 0x8c020000;
	result.opcode = 0x2102;
	return result;
}

std::vector<research::PvrTaVramRead> renderReads()
{
	return {
		{0x00200010, 0},
		{0x00200000, 0x80000000},
		{0x00200000, 0x80000000},
		{0x00200000, 0x80000000},
		{0x00200004, 0x00300000},
		{0x00300000, 0x00100000},
	};
}

std::vector<research::PvrTaObservation> completeStream(bool corruptRead = false)
{
	using Type = research::PvrTaObservationType;
	std::vector<research::PvrTaObservation> events;

	research::PvrTaObservation init;
	init.type = Type::ListInit;
	init.emissionOrdinal = 40;
	init.tick = 101;
	init.initiator = owner(7, 100);
	init.contextAddress = 0x00100000;
	init.contextGeneration = 12;
	events.push_back(init);

	research::PvrTaObservation block;
	block.type = Type::AcceptedBlock;
	block.emissionOrdinal = 41;
	block.tick = 102;
	block.initiator = owner(7, 100);
	block.contextAddress = init.contextAddress;
	block.contextGeneration = init.contextGeneration;
	block.contextBlockOrdinal = 0;
	block.renderPass = 0;
	block.listTypeBefore = 7;
	block.listTypeAfter = 0;
	block.parserStateBefore = 0;
	block.parserStateAfter = 1;
	block.source = research::PvrTaInputSource::StoreQueue;
	block.sourceAddress = 0xe0000020;
	block.taAddress = 0x10000020;
	for (std::size_t index = 0; index < block.block.size(); ++index)
		block.block[index] = static_cast<std::uint8_t>(index);
	events.push_back(block);

	research::PvrTaObservation start;
	start.type = Type::StartRender;
	start.emissionOrdinal = 42;
	start.tick = 103;
	start.initiator = owner(8, 103);
	start.renderGeneration = 3;
	start.renderContextAvailable = true;
	start.selectedContexts.push_back({init.contextAddress,
			init.contextGeneration, true});
	start.regionBase = 0x00200000;
	start.fpuParamCfg = 0;
	const std::vector<research::PvrTaVramRead> reads = renderReads();
	start.renderSelectionReadCount = reads.size();
	std::copy(reads.begin(), reads.end(), start.renderSelectionReads.begin());
	if (corruptRead)
		start.renderSelectionReads[4].address += 4;
	events.push_back(start);

	research::PvrTaObservation done;
	done.type = Type::RenderDone;
	done.emissionOrdinal = 43;
	done.tick = 200;
	done.renderGeneration = start.renderGeneration;
	events.push_back(done);
	return events;
}

void writeStream(const std::filesystem::path& path,
		const std::vector<research::PvrTaObservation>& events)
{
	research::PvrTaArtifactWriter writer(path, binding());
	for (const auto& event : events)
		writer.write(event);
	writer.finalize();
}

} // namespace

TEST(ResearchPvrTaArtifact, RoundTripsCompleteCausalSlice)
{
	TemporaryDirectory temporary;
	const std::filesystem::path artifact = temporary.file("capture.fcpvr");
	writeStream(artifact, completeStream());

	const research::PvrTaArtifactSummary summary =
			research::validatePvrTaArtifactFile(artifact, binding());
	EXPECT_EQ(4u, summary.eventCount);
	EXPECT_EQ(1u, summary.typeCounts[0]);
	EXPECT_EQ(1u, summary.typeCounts[2]);
	EXPECT_EQ(1u, summary.typeCounts[3]);
	EXPECT_EQ(1u, summary.typeCounts[4]);
	EXPECT_EQ(101u, summary.startTick);
	EXPECT_EQ(200u, summary.endTick);
	EXPECT_EQ(40u, summary.firstEmissionOrdinal);
	EXPECT_EQ(43u, summary.lastEmissionOrdinal);
}

TEST(ResearchPvrTaArtifact, IndependentDecoderRejectsAlteredSelectionRead)
{
	TemporaryDirectory temporary;
	const std::filesystem::path artifact = temporary.file("bad-selection.fcpvr");
	writeStream(artifact, completeStream(true));
	EXPECT_THROW(research::validatePvrTaArtifactFile(artifact, binding()),
			std::runtime_error);
}

TEST(ResearchPvrTaArtifact, RejectsBlockUnrelatedToRenderedContext)
{
	TemporaryDirectory temporary;
	const std::filesystem::path artifact = temporary.file("unrelated-context.fcpvr");
	auto events = completeStream();
	events[0].contextAddress = 0x00400000;
	events[1].contextAddress = events[0].contextAddress;

	research::PvrTaObservation renderedInit = events[0];
	renderedInit.emissionOrdinal = 42;
	renderedInit.tick = 103;
	renderedInit.contextAddress = 0x00100000;
	renderedInit.contextGeneration = 13;
	events[2].emissionOrdinal = 43;
	events[2].tick = 104;
	events[2].selectedContexts[0].generation = renderedInit.contextGeneration;
	events[3].emissionOrdinal = 44;
	events.insert(events.begin() + 2, renderedInit);

	writeStream(artifact, events);
	EXPECT_THROW(research::validatePvrTaArtifactFile(artifact, binding()),
			std::runtime_error);
}

TEST(ResearchPvrTaArtifact, RejectsOverlappingRenderGenerations)
{
	TemporaryDirectory temporary;
	const std::filesystem::path artifact = temporary.file("overlapping-render.fcpvr");
	auto events = completeStream();
	research::PvrTaObservation overlapping = events[2];
	overlapping.emissionOrdinal = 43;
	overlapping.tick = 104;
	overlapping.renderGeneration = 4;
	overlapping.renderContextAvailable = false;
	overlapping.selectedContexts[0].generation = 0;
	overlapping.selectedContexts[0].available = false;
	events[3].emissionOrdinal = 44;
	events[3].renderGeneration = overlapping.renderGeneration;
	events.insert(events.begin() + 3, overlapping);

	writeStream(artifact, events);
	EXPECT_THROW(research::validatePvrTaArtifactFile(artifact, binding()),
			std::runtime_error);
}

TEST(ResearchPvrTaArtifact, IncompleteCandidateIsRejected)
{
	TemporaryDirectory temporary;
	const std::filesystem::path artifact = temporary.file("incomplete.fcpvr");
	{
		research::PvrTaArtifactWriter writer(artifact, binding());
		writer.write(completeStream().front());
		writer.abandon();
	}
	EXPECT_THROW(research::validatePvrTaArtifactFile(artifact, binding()),
			std::runtime_error);
}

TEST(ResearchPvrTaArtifact, WriterRejectsAContextWithoutAnInstructionOwner)
{
	TemporaryDirectory temporary;
	const std::filesystem::path artifact = temporary.file("unowned.fcpvr");
	research::PvrTaArtifactWriter writer(artifact, binding());
	auto events = completeStream();
	events.front().initiator = {};
	EXPECT_THROW(writer.write(events.front()), std::runtime_error);
}

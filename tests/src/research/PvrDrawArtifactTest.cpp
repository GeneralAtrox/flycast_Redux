#include "research/pvr_draw_artifact.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace
{

class TemporaryDirectory
{
public:
	TemporaryDirectory()
	{
		static std::atomic<unsigned> sequence {0};
		path = std::filesystem::temp_directory_path()
				/ ("flycast-pvr-draw-artifact-test-"
						+ std::to_string(sequence.fetch_add(1)));
		std::filesystem::remove_all(path);
		std::filesystem::create_directories(path);
	}

	~TemporaryDirectory() { std::filesystem::remove_all(path); }

	std::filesystem::path path;
};

research::Sha256Digest digest(std::uint8_t seed)
{
	research::Sha256Digest result {};
	for (std::size_t index = 0; index < result.size(); ++index)
		result[index] = static_cast<std::uint8_t>(seed + index);
	return result;
}

research::PvrDrawArtifactBinding binding()
{
	research::PvrDrawArtifactBinding result;
	result.backend = research::Sh4ObservationBackend::Interpreter;
	result.identityDigest = digest(1);
	result.replayDigest = digest(2);
	result.taArtifactDigest = digest(3);
	result.presentationArtifactDigest = digest(4);
	result.rendererConfigurationDigest = digest(5);
	return result;
}

research::PvrTaBlockProvenance block(std::uint64_t ordinal,
		std::uint64_t ownerGeneration, std::uint32_t pc)
{
	research::PvrTaBlockProvenance result;
	result.available = true;
	result.contextAddress = 0x00500000;
	result.contextGeneration = 3;
	result.contextBlockOrdinal = ordinal;
	result.renderPass = 0;
	result.source = research::PvrTaInputSource::StoreQueue;
	result.sourceAddress = 0xe0000000 + static_cast<std::uint32_t>(ordinal * 32);
	result.taAddress = 0x10000000 + static_cast<std::uint32_t>(ordinal * 32);
	result.initiator.valid = true;
	result.initiator.backend = research::Sh4ObservationBackend::Interpreter;
	result.initiator.generation = ownerGeneration;
	result.initiator.tick = 10;
	result.initiator.pc = pc;
	result.initiator.pr = 0x8c020000;
	result.initiator.opcode = 0x2102;
	return result;
}

research::PvrDrawObservation primitive(std::uint64_t emission,
		std::uint64_t generation, research::PvrPrimitiveKind kind)
{
	research::PvrDrawObservation result;
	result.type = research::PvrDrawObservationType::PrimitiveDecoded;
	result.emissionOrdinal = emission;
	result.tick = 10 + emission;
	result.renderGeneration = 7;
	result.primitiveGeneration = generation;
	result.contextAddress = 0x00500000;
	result.contextGeneration = kind == research::PvrPrimitiveKind::Background
			? 0 : 3;
	result.renderPass = 0;
	result.listType = 0;
	result.primitiveKind = kind;
	result.ownerClass = kind == research::PvrPrimitiveKind::Background
			? research::PvrPrimitiveOwnerClass::Unowned
			: research::PvrPrimitiveOwnerClass::Exact;
	result.first = generation == 1 ? 0 : 4;
	result.count = kind == research::PvrPrimitiveKind::Background ? 4 : 3;
	result.vertices.resize(result.count);
	result.bounds.available = true;
	result.bounds.maximumX = 10;
	result.bounds.maximumY = 10;
	result.bounds.maximumZ = 1;
	if (kind != research::PvrPrimitiveKind::Background)
	{
		result.pcw = 0x8;
		result.sampledTexture.available = true;
		result.sampledTexture.sourceAddress = 0x00100000;
		result.sampledTexture.sourceSize = 8;
		result.sampledTexture.maximumLevelAddress = 0x00100000;
		result.sampledTexture.maximumLevelSize = 8;
		result.sampledTexture.width = 2;
		result.sampledTexture.height = 2;
		result.sampledTexture.pixelFormat = 3;
		result.sampledTexture.sourceBytes = {0, 1, 2, 3, 4, 5, 6, 7};
		result.sampledTexture.sourceDigest = research::sha256(
				result.sampledTexture.sourceBytes.data(),
				result.sampledTexture.sourceBytes.size());
		result.parameterBlocks = {block(0, 11, 0x8c010100)};
		result.vertexBlocks = {block(1, 11, 0x8c010100)};
	}
	return result;
}

void writeComplete(const std::filesystem::path& path,
		bool backgroundOnly = false, bool wrongOwnerClass = false)
{
	research::PvrDrawArtifactWriter writer(path, binding());
	auto background = primitive(0, 1, research::PvrPrimitiveKind::Background);
	writer.write(background);
	std::uint64_t consumed = 1;
	std::uint64_t nextEmission = 1;
	if (!backgroundOnly)
	{
		auto polygon = primitive(1, 2,
				research::PvrPrimitiveKind::PolygonStrip);
		if (wrongOwnerClass)
		{
			polygon.vertexBlocks[0].initiator.generation = 12;
			polygon.vertexBlocks[0].initiator.pc = 0x8c010120;
		}
		writer.write(polygon);
		consumed = 2;
		nextEmission = 2;
	}
	research::PvrDrawObservation draw;
	draw.type = research::PvrDrawObservationType::DrawConsumed;
	draw.emissionOrdinal = nextEmission++;
	draw.tick = 10 + draw.emissionOrdinal;
	draw.renderGeneration = 7;
	draw.rasterGeneration = 20;
	draw.primitiveGenerations = {consumed};
	draw.backend = research::PvrDrawBackend::DirectX11;
	draw.drawPass = consumed == 1 ? research::PvrDrawPass::Background
			: research::PvrDrawPass::Color;
	draw.count = consumed == 1 ? 4 : 3;
	draw.indexed = true;
	writer.write(draw);
	research::PvrDrawObservation completed;
	completed.type = research::PvrDrawObservationType::RenderCompleted;
	completed.emissionOrdinal = nextEmission;
	completed.tick = 10 + completed.emissionOrdinal;
	completed.renderGeneration = 7;
	completed.successful = true;
	writer.write(completed);
	writer.finalize();
}

} // namespace

TEST(ResearchPvrDrawArtifact, IndependentlyValidatesConsumedGuestPrimitive)
{
	TemporaryDirectory temporary;
	const auto path = temporary.path / "draw.fcpvrd";
	writeComplete(path);
	const auto summary = research::validatePvrDrawArtifactFile(path, binding());
	EXPECT_TRUE(summary.completeVerticalSlice);
	EXPECT_EQ(4u, summary.eventCount);
	EXPECT_EQ(2u, summary.typeCounts[0]);
}

TEST(ResearchPvrDrawArtifact, RejectsBackgroundOnlyCapture)
{
	TemporaryDirectory temporary;
	const auto path = temporary.path / "background.fcpvrd";
	writeComplete(path, true);
	EXPECT_THROW(research::validatePvrDrawArtifactFile(path, binding()),
			std::runtime_error);
}

TEST(ResearchPvrDrawArtifact, RejectsFalseExactOwnerClassification)
{
	TemporaryDirectory temporary;
	const auto path = temporary.path / "mixed.fcpvrd";
	writeComplete(path, false, true);
	EXPECT_THROW(research::validatePvrDrawArtifactFile(path, binding()),
			std::runtime_error);
}

TEST(ResearchPvrDrawArtifact, RejectsPayloadMutationAndDrops)
{
	TemporaryDirectory temporary;
	const auto path = temporary.path / "mutated.fcpvrd";
	writeComplete(path);
	{
		std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
		file.seekg(research::PvrDrawArtifactHeaderSize + 40);
		char byte = 0;
		file.read(&byte, 1);
		byte ^= 1;
		file.seekp(research::PvrDrawArtifactHeaderSize + 40);
		file.write(&byte, 1);
	}
	EXPECT_THROW(research::validatePvrDrawArtifactFile(path, binding()),
			std::runtime_error);

	const auto dropped = temporary.path / "dropped.fcpvrd";
	research::PvrDrawArtifactWriter writer(dropped, binding());
	writer.write(primitive(0, 1, research::PvrPrimitiveKind::Background));
	EXPECT_THROW(writer.finalize(1), std::runtime_error);
}

TEST(ResearchPvrDrawArtifact, RefusesUnboundLinkedArtifacts)
{
	TemporaryDirectory temporary;
	auto unbound = binding();
	unbound.taArtifactDigest = {};
	unbound.presentationArtifactDigest = {};
	unbound.rendererConfigurationDigest = {};
	const auto path = temporary.path / "unbound.fcpvrd";
	research::PvrDrawArtifactWriter writer(path, unbound);
	writer.write(primitive(0, 1, research::PvrPrimitiveKind::Background));
	EXPECT_THROW(writer.finalize(), std::logic_error);
	EXPECT_THROW(research::validatePvrDrawArtifactFile(path, unbound),
			std::runtime_error);
}

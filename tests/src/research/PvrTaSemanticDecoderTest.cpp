#include "research/pvr_draw_artifact.h"
#include "research/identity_manifest.h"
#include "research/pvr_ta_semantic_decoder.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>

namespace
{

class TemporaryDirectory
{
public:
	TemporaryDirectory()
	{
		static std::atomic<unsigned> sequence {0};
		path = std::filesystem::temp_directory_path()
				/ ("flycast-pvr-ta-semantic-test-"
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
	for (std::size_t i = 0; i < result.size(); ++i)
		result[i] = static_cast<std::uint8_t>(seed + i);
	return result;
}

research::PvrTaArtifactBinding taBinding()
{
	research::PvrTaArtifactBinding result;
	result.backend = research::Sh4ObservationBackend::Interpreter;
	result.identityDigest = digest(1);
	result.replayDigest = digest(2);
	result.manifestDigest = digest(3);
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

void putWord(std::array<std::uint8_t, 32>& bytes, std::size_t offset,
		std::uint32_t value)
{
	for (unsigned i = 0; i < 4; ++i)
		bytes[offset + i] = static_cast<std::uint8_t>(value >> (i * 8));
}

void putFloat(std::array<std::uint8_t, 32>& bytes, std::size_t offset,
		float value)
{
	std::uint32_t bits = 0;
	std::memcpy(&bits, &value, sizeof(bits));
	putWord(bytes, offset, bits);
}

research::PvrTaRenderSelectionTranscript transcript()
{
	research::PvrTaRenderSelectionTranscript result;
	result.initialized = true;
	result.regionBase = 0x00200000;
	result.fpuParamCfg = 0;
	result.record(0x00200010, 0);
	result.record(0x00200000, 0x80000000);
	result.record(0x00200000, 0x80000000);
	result.record(0x00200000, 0x80000000);
	result.record(0x00200004, 0x00300000);
	result.record(0x00300000, 0x00500000);
	return result;
}

struct Slice
{
	std::filesystem::path taPath;
	research::PvrTaArtifactBinding taBinding;
	std::vector<research::PvrTaBlockProvenance> parameterBlocks;
	std::vector<research::PvrTaBlockProvenance> vertexBlocks;
};

Slice writeTa(const TemporaryDirectory& temporary, bool twoBlockVertices)
{
	Slice slice;
	slice.taPath = temporary.path / "raw.fcpvr";
	slice.taBinding = taBinding();
	research::PvrTaArtifactWriter writer(slice.taPath, slice.taBinding,
			1024 * 1024, 100);
	std::uint64_t emission = 1;
	std::uint64_t tick = 1;
	research::PvrTaObservation init;
	init.type = research::PvrTaObservationType::ListInit;
	init.emissionOrdinal = emission++;
	init.tick = tick++;
	init.initiator = owner(1, init.tick);
	init.contextAddress = 0x00500000;
	init.contextGeneration = 3;
	init.renderPass = 0;
	writer.write(init);

	std::vector<std::array<std::uint8_t, 32>> blocks;
	std::array<std::uint8_t, 32> header {};
	putWord(header, 0, 0x80000000u | (twoBlockVertices ? 0x18u : 0u));
	putWord(header, 4, 0x01020304);
	putWord(header, 8, 0);
	putWord(header, 12, 0x11223344);
	blocks.push_back(header);
	for (unsigned vertex = 0; vertex < 3; ++vertex)
	{
		std::array<std::uint8_t, 32> first {};
		putWord(first, 0, (vertex == 2 ? 0xf0000000u : 0xe0000000u));
		putFloat(first, 4, static_cast<float>(vertex * 2));
		putFloat(first, 8, static_cast<float>(vertex + 1));
		putFloat(first, 12, static_cast<float>(vertex + 2));
		blocks.push_back(first);
		if (twoBlockVertices)
		{
			std::array<std::uint8_t, 32> second {};
			blocks.push_back(second);
		}
	}
	blocks.push_back({});

	for (std::size_t index = 0; index < blocks.size(); ++index)
	{
		research::PvrTaObservation accepted;
		accepted.type = research::PvrTaObservationType::AcceptedBlock;
		accepted.emissionOrdinal = emission++;
		accepted.tick = tick++;
		accepted.initiator = owner(2 + index, accepted.tick);
		accepted.contextAddress = 0x00500000;
		accepted.contextGeneration = 3;
		accepted.contextBlockOrdinal = index;
		accepted.renderPass = 0;
		accepted.listTypeBefore = index == 0 ? 7 : 0;
		accepted.listTypeAfter = index + 1 == blocks.size() ? 7 : 0;
		accepted.parserStateBefore = 0;
		accepted.parserStateAfter = 0;
		accepted.source = research::PvrTaInputSource::StoreQueue;
		accepted.sourceAddress = 0xe0000000u
				+ static_cast<std::uint32_t>(index * 32);
		accepted.taAddress = 0x10000000u
				+ static_cast<std::uint32_t>(index * 32);
		accepted.block = blocks[index];
		writer.write(accepted);
		research::PvrTaBlockProvenance provenance;
		provenance.available = true;
		provenance.initiator = accepted.initiator;
		provenance.contextAddress = accepted.contextAddress;
		provenance.contextGeneration = accepted.contextGeneration;
		provenance.contextBlockOrdinal = accepted.contextBlockOrdinal;
		provenance.renderPass = 0;
		provenance.source = accepted.source;
		provenance.sourceAddress = accepted.sourceAddress;
		provenance.taAddress = accepted.taAddress;
		if (index == 0)
			slice.parameterBlocks.push_back(provenance);
		else if (index + 1 != blocks.size())
			slice.vertexBlocks.push_back(provenance);
	}

	research::PvrTaObservation start;
	start.type = research::PvrTaObservationType::StartRender;
	start.emissionOrdinal = emission++;
	start.tick = tick++;
	start.initiator = owner(20, start.tick);
	start.renderGeneration = 7;
	start.renderContextAvailable = true;
	start.selectedContexts.push_back({0x00500000, 3, true});
	const auto reads = transcript();
	start.regionBase = reads.regionBase;
	start.fpuParamCfg = reads.fpuParamCfg;
	start.renderSelectionReads = reads.reads;
	start.renderSelectionReadCount = reads.readCount;
	writer.write(start);
	research::PvrTaObservation done;
	done.type = research::PvrTaObservationType::RenderDone;
	done.emissionOrdinal = emission;
	done.tick = tick;
	done.renderGeneration = 7;
	writer.write(done);
	writer.finalize();
	return slice;
}

research::PvrDrawArtifactBinding drawBinding(const Slice& slice)
{
	research::PvrDrawArtifactBinding result;
	result.backend = slice.taBinding.backend;
	result.identityDigest = slice.taBinding.identityDigest;
	result.replayDigest = slice.taBinding.replayDigest;
	result.taArtifactDigest = research::hashFileExact(slice.taPath, 1024 * 1024);
	result.presentationArtifactDigest = digest(4);
	result.rendererConfigurationDigest = digest(5);
	return result;
}

enum class Mutation { None, Material, Bounds, Provenance };

std::filesystem::path writeDraw(const TemporaryDirectory& temporary,
		const Slice& slice, Mutation mutation)
{
	const auto path = temporary.path / ("draw-" + std::to_string(
			static_cast<int>(mutation)) + ".fcpvrd");
	research::PvrDrawArtifactWriter writer(path, drawBinding(slice));
	research::PvrDrawObservation background;
	background.type = research::PvrDrawObservationType::PrimitiveDecoded;
	background.emissionOrdinal = 1;
	background.tick = 100;
	background.renderGeneration = 7;
	background.primitiveGeneration = 1;
	background.contextAddress = 0x00500000;
	background.listType = 0;
	background.primitiveKind = research::PvrPrimitiveKind::Background;
	background.ownerClass = research::PvrPrimitiveOwnerClass::Unowned;
	background.first = 0;
	background.count = 4;
	background.bounds.available = true;
	background.bounds.maximumX = 1;
	background.bounds.maximumY = 1;
	background.bounds.maximumZ = 1;
	writer.write(background);
	research::PvrDrawObservation primitive;
	primitive.type = research::PvrDrawObservationType::PrimitiveDecoded;
	primitive.emissionOrdinal = 2;
	primitive.tick = 101;
	primitive.renderGeneration = 7;
	primitive.primitiveGeneration = 2;
	primitive.contextAddress = 0x00500000;
	primitive.contextGeneration = 3;
	primitive.renderPass = 0;
	primitive.listType = 0;
	primitive.primitiveKind = research::PvrPrimitiveKind::PolygonStrip;
	primitive.ownerClass = research::PvrPrimitiveOwnerClass::Mixed;
	primitive.pcw = 0x80000000;
	primitive.isp = 0x01020304;
	primitive.tsp = 1u << 29;
	primitive.tcw = 0x11223344;
	primitive.tileClip = (39u << 6) | (14u << 17);
	primitive.first = 4;
	primitive.count = 3;
	primitive.bounds.available = true;
	primitive.bounds.minimumX = 0;
	primitive.bounds.minimumY = 1;
	primitive.bounds.minimumZ = 2;
	primitive.bounds.maximumX = 4;
	primitive.bounds.maximumY = 3;
	primitive.bounds.maximumZ = 4;
	primitive.parameterBlocks = slice.parameterBlocks;
	primitive.vertexBlocks = slice.vertexBlocks;
	if (mutation == Mutation::Material)
		++primitive.tcw;
	if (mutation == Mutation::Bounds)
		primitive.bounds.maximumX = 5;
	if (mutation == Mutation::Provenance)
		++primitive.vertexBlocks.back().contextBlockOrdinal;
	writer.write(primitive);
	research::PvrDrawObservation draw;
	draw.type = research::PvrDrawObservationType::DrawConsumed;
	draw.emissionOrdinal = 3;
	draw.tick = 102;
	draw.renderGeneration = 7;
	draw.rasterGeneration = 1;
	draw.primitiveGenerations = {2};
	draw.backend = research::PvrDrawBackend::DirectX11;
	draw.drawPass = research::PvrDrawPass::Color;
	draw.count = 3;
	draw.indexed = true;
	writer.write(draw);
	research::PvrDrawObservation completed;
	completed.type = research::PvrDrawObservationType::RenderCompleted;
	completed.emissionOrdinal = 4;
	completed.tick = 103;
	completed.renderGeneration = 7;
	completed.successful = true;
	writer.write(completed);
	writer.finalize();
	return path;
}

} // namespace

TEST(ResearchPvrTaSemanticDecoder, ReconstructsAndJoinsExactPolygon)
{
	TemporaryDirectory temporary;
	const Slice slice = writeTa(temporary, false);
	const auto semantics = research::reconstructPvrTaSemantics(slice.taPath,
			slice.taBinding, 1024 * 1024, 100);
	ASSERT_EQ(1u, semantics.primitives.size());
	EXPECT_EQ(3u, semantics.primitives[0].count);
	EXPECT_EQ(4u, semantics.primitives[0].first);
	const auto draw = writeDraw(temporary, slice, Mutation::None);
	EXPECT_TRUE(research::validatePvrDrawArtifactAgainstTaFile(draw,
			drawBinding(slice), slice.taPath, slice.taBinding,
			1024 * 1024, 100, 1024 * 1024, 100).completeVerticalSlice);
}

TEST(ResearchPvrTaSemanticDecoder, RejectsSelfConsistentSemanticMutations)
{
	TemporaryDirectory temporary;
	const Slice slice = writeTa(temporary, false);
	for (const Mutation mutation : {Mutation::Material, Mutation::Bounds,
			Mutation::Provenance})
	{
		const auto draw = writeDraw(temporary, slice, mutation);
		EXPECT_THROW(research::validatePvrDrawArtifactAgainstTaFile(draw,
				drawBinding(slice), slice.taPath, slice.taBinding,
				1024 * 1024, 100, 1024 * 1024, 100), std::runtime_error);
	}
}

TEST(ResearchPvrTaSemanticDecoder, RetainsBothHalvesOfSixtyFourByteVertices)
{
	TemporaryDirectory temporary;
	const Slice slice = writeTa(temporary, true);
	const auto semantics = research::reconstructPvrTaSemantics(slice.taPath,
			slice.taBinding, 1024 * 1024, 100);
	ASSERT_EQ(1u, semantics.primitives.size());
	EXPECT_EQ(6u, semantics.primitives[0].vertexBlocks.size());
}

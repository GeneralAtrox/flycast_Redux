#pragma once

#include "research/pvr_draw_observation.h"
#include "research/pvr_ta_artifact.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace research
{

// Semantic facts reconstructed directly from the accepted 32-byte TA blocks.
// This decoder is intentionally separate from the emulator's TA parser.
struct PvrTaSemanticPrimitive
{
	std::uint64_t renderGeneration = 0;
	std::uint32_t contextAddress = UINT32_MAX;
	std::uint64_t contextGeneration = 0;
	std::uint32_t renderPass = 0;
	std::uint32_t listType = UINT32_MAX;
	PvrPrimitiveKind kind = PvrPrimitiveKind::PolygonStrip;
	std::uint32_t pcw = 0;
	std::uint32_t isp = 0;
	std::uint32_t tsp = 0;
	std::uint32_t tcw = 0;
	std::uint32_t tsp1 = UINT32_MAX;
	std::uint32_t tcw1 = UINT32_MAX;
	std::uint32_t tileClip = 0;
	std::uint32_t first = 0;
	std::uint32_t count = 0;
	PvrPrimitiveBounds bounds;
	std::vector<PvrTaBlockProvenance> parameterBlocks;
	std::vector<PvrTaBlockProvenance> vertexBlocks;
};

struct PvrTaSemanticSummary
{
	std::vector<PvrTaSemanticPrimitive> primitives;
	std::uint64_t renderCount = 0;
};

PvrTaSemanticSummary reconstructPvrTaSemantics(
		const std::filesystem::path& path,
		const PvrTaArtifactBinding& expectedBinding,
		std::uint64_t maximumBytes = DefaultMaximumPvrTaArtifactBytes,
		std::uint64_t maximumEvents = DefaultMaximumPvrTaArtifactEvents);

} // namespace research

#pragma once

#include "research/sha256.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace research
{

constexpr std::size_t MaxIdentityManifestBytes = 16 * 1024 * 1024;
constexpr std::uint64_t MaximumMapleDmaCheckpoint = 10'000'000;

struct IdentityRuntimeConfiguration
{
	std::string cpuBackend;
	bool dynarecObservation = false;
	bool threadedRendering = true;
	bool autoLoadState = true;
	bool autoSaveState = true;
	bool ggpo = true;
	std::uint64_t mapleDmaCheckpoint = 0;
	std::uint64_t sh4ObservationStartDma = 0;
	std::uint64_t pvrTaStartDma = 0;
	std::uint32_t dreamcastRtcSeed = 0;
};

struct IdentityManifest
{
	std::uint32_t schemaVersion = 0;
	std::filesystem::path path;
	std::vector<std::uint8_t> bytes;
	Sha256Digest digest {};
	IdentityRuntimeConfiguration runtimeConfiguration;
	std::string mediaKind;
	std::size_t mediaTrackCount = 0;
	Sha256Digest bootExecutableDigest {};
	bool hasStaticAnalysis = false;
	Sha256Digest staticAnalysisProgramDigest {};
	Sha256Digest staticAnalysisExportDigest {};
	std::uint32_t staticAnalysisImageBase = 0;
	bool hasHookManifestDigest = false;
	Sha256Digest hookManifestDigest {};
	bool hasMapleReplayIdentityDigest = false;
	Sha256Digest mapleReplayIdentityDigest {};
};

IdentityManifest loadIdentityManifest(const std::filesystem::path& path);
void requireCaptureV1Identity(const IdentityManifest& manifest);
void requireSh4EquivalenceIdentityV2(const IdentityManifest& manifest);
Sha256Digest hashFileExact(const std::filesystem::path& path, std::uint64_t maximumBytes);
std::vector<std::uint8_t> readFileExact(const std::filesystem::path& path,
		std::uint64_t maximumBytes);
bool pathsAlias(const std::filesystem::path& lhs, const std::filesystem::path& rhs);

} // namespace research

#pragma once

#include "research/identity_manifest.h"
#include "research/sha256.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace research
{

struct Sh4EventsManifest;
struct MemoryRangesManifest;

constexpr std::size_t MaxGhidraExportBytes = 64 * 1024 * 1024;
constexpr std::size_t MaxGhidraMemoryBlocks = 4096;
constexpr std::size_t MaxGhidraFunctions = 250000;
constexpr std::size_t MaxGhidraSymbols = 1000000;
constexpr std::size_t MaxGhidraDataTypes = 100000;
constexpr std::size_t MaxGhidraFunctionBodyRanges = 1000000;
constexpr std::size_t MaxGhidraFunctionParameters = 64;

struct GhidraExport
{
	std::filesystem::path path;
	std::vector<std::uint8_t> bytes;
	Sha256Digest digest {};
	std::string exportId;
	std::string ghidraVersion;
	Sha256Digest exporterScriptDigest {};
	std::string programName;
	Sha256Digest executableDigest {};
	std::uint64_t executableSize = 0;
	std::uint32_t imageBase = 0;
	std::size_t memoryBlockCount = 0;
	std::size_t functionCount = 0;
	std::size_t symbolCount = 0;
	std::size_t dataTypeCount = 0;
};

GhidraExport loadGhidraExport(const std::filesystem::path& path);
void requireGhidraExportIdentity(const GhidraExport& exportArtifact,
		const IdentityManifest& identity, const std::filesystem::path& executable,
		const std::filesystem::path& exporterScript);
void requireGhidraExportSh4Join(const GhidraExport& exportArtifact,
		const Sh4EventsManifest& manifest);
void requireGhidraExportMemoryRangesJoin(const GhidraExport& exportArtifact,
		const MemoryRangesManifest& manifest);

} // namespace research

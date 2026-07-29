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

constexpr std::size_t MaxMemoryRangesManifestBytes = 1024 * 1024;
constexpr std::size_t MaxMemoryRangeCount = 64;
constexpr std::uint32_t MaxMemoryRangeLength = 16 * 1024 * 1024;
constexpr std::uint64_t MaxMemoryRangesTotalBytes = 1024ull * 1024 * 1024;

struct MemoryRangesBindings
{
	Sha256Digest executableDigest {};
	std::string staticAnalysisId;
	Sha256Digest staticAnalysisDigest {};
	std::string hookManifestId;
	Sha256Digest hookManifestDigest {};
};

struct MemoryRangeDefinition
{
	std::string id;
	std::uint32_t address = 0;
	std::uint32_t length = 0;
	Sha256Digest expectedDigest {};
};

struct MemoryRangesManifest
{
	std::filesystem::path path;
	std::vector<std::uint8_t> bytes;
	Sha256Digest digest {};
	std::string id;
	MemoryRangesBindings bindings;
	std::uint32_t triggerStart = 0;
	std::uint32_t triggerEndExclusive = 0;
	std::vector<MemoryRangeDefinition> ranges;
	std::uint64_t maximumTotalBytes = 0;
};

MemoryRangesManifest loadMemoryRangesManifest(const std::filesystem::path& path);
void requireMemoryRangesIdentity(const MemoryRangesManifest& manifest,
		const IdentityManifest& identity);

} // namespace research

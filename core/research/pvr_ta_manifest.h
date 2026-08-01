#pragma once

#include "research/identity_manifest.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace research
{

constexpr std::size_t MaxPvrTaManifestBytes = 16 * 1024 * 1024;

struct PvrTaManifest
{
	std::filesystem::path path;
	std::vector<std::uint8_t> bytes;
	Sha256Digest digest {};
	std::string manifestId;
	std::uint64_t startDma = 0;
	std::uint64_t renderDoneCount = 0;
	std::uint64_t maximumBytes = 0;
	std::uint64_t maximumEvents = 0;
	Sha256Digest staticAnalysisDigest {};
	Sha256Digest executableDigest {};
};

PvrTaManifest loadPvrTaManifest(const std::filesystem::path& path);
void requirePvrTaManifestIdentity(const PvrTaManifest& manifest,
		const IdentityManifest& identity);

} // namespace research

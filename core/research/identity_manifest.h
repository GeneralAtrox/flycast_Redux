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

struct IdentityRuntimeConfiguration
{
	std::string cpuBackend;
	bool threadedRendering = true;
	bool autoLoadState = true;
	bool autoSaveState = true;
	bool ggpo = true;
};

struct IdentityManifest
{
	std::filesystem::path path;
	std::vector<std::uint8_t> bytes;
	Sha256Digest digest {};
	IdentityRuntimeConfiguration runtimeConfiguration;
	std::string mediaKind;
	std::size_t mediaTrackCount = 0;
};

IdentityManifest loadIdentityManifest(const std::filesystem::path& path);
void requireCaptureV1Identity(const IdentityManifest& manifest);
Sha256Digest hashFileExact(const std::filesystem::path& path, std::uint64_t maximumBytes);
std::vector<std::uint8_t> readFileExact(const std::filesystem::path& path,
		std::uint64_t maximumBytes);
bool pathsAlias(const std::filesystem::path& lhs, const std::filesystem::path& rhs);

} // namespace research

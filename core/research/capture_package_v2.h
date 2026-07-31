#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace research
{

constexpr std::size_t MaxCapturePackageV2JsonBytes = 16 * 1024 * 1024;

struct CapturePackageV2Summary
{
	std::string packageId;
	std::size_t artifactCount = 0;
};

CapturePackageV2Summary validateCapturePackageV2ReadOnly(
		const std::filesystem::path& package);
CapturePackageV2Summary issueCapturePackageV2Receipt(
		const std::filesystem::path& package, const std::filesystem::path& receipt,
		const std::filesystem::path& runningValidator);

} // namespace research

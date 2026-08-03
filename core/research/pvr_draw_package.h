#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace research
{

constexpr std::size_t MaxPvrDrawPackageJsonBytes = 16 * 1024 * 1024;

struct PvrDrawPackageSummary
{
	std::string packageId;
	std::size_t entryCount = 0;
};

PvrDrawPackageSummary validatePvrDrawPackageV1ReadOnly(
		const std::filesystem::path& package);
PvrDrawPackageSummary issuePvrDrawPackageV1Receipt(
		const std::filesystem::path& package,
		const std::filesystem::path& receipt,
		const std::filesystem::path& runningValidator);

} // namespace research

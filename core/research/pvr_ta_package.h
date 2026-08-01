#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace research
{

constexpr std::size_t MaxPvrTaPackageJsonBytes = 16 * 1024 * 1024;

struct PvrTaPackageSummary
{
	std::string packageId;
	std::size_t entryCount = 0;
};

PvrTaPackageSummary validatePvrTaPackageV1ReadOnly(
		const std::filesystem::path& package);
PvrTaPackageSummary issuePvrTaPackageV1Receipt(
		const std::filesystem::path& package,
		const std::filesystem::path& receipt,
		const std::filesystem::path& runningValidator);

} // namespace research

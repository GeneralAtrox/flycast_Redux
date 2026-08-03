#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace research
{

constexpr std::size_t MaxSh4GhidraPackageJsonBytes = 64 * 1024 * 1024;

struct Sh4GhidraPackageSummary
{
	std::string packageId;
	std::size_t lockedEntryCount = 0;
};

Sh4GhidraPackageSummary validateSh4GhidraPackageV1ReadOnly(
		const std::filesystem::path& package);
Sh4GhidraPackageSummary issueSh4GhidraPackageV1Receipt(
		const std::filesystem::path& package,
		const std::filesystem::path& receipt,
		const std::filesystem::path& runningValidator);

} // namespace research

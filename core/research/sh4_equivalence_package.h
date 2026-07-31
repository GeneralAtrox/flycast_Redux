#pragma once

#include <cstddef>
#include <filesystem>
#include <string>

namespace research
{

constexpr std::size_t MaxSh4EquivalencePackageJsonBytes = 16 * 1024 * 1024;

struct Sh4EquivalencePackageSummary
{
	std::string packageId;
	std::size_t lockedEntryCount = 0;
};

Sh4EquivalencePackageSummary validateSh4EquivalencePackageV1ReadOnly(
		const std::filesystem::path& package);
Sh4EquivalencePackageSummary issueSh4EquivalencePackageV1Receipt(
		const std::filesystem::path& package,
		const std::filesystem::path& receipt,
		const std::filesystem::path& runningValidator);

} // namespace research

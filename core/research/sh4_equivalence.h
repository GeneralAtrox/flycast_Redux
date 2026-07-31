#pragma once

#include "research/sh4_observation_compare.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace research
{

constexpr std::size_t MaxSh4EquivalenceJsonBytes = 16 * 1024 * 1024;

struct Sh4EquivalenceReportSummary
{
	std::string jobId;
	bool equivalent = false;
	std::uint64_t matchedEventCount = 0;
	std::optional<Sh4ObservationDivergence> firstDivergence;
};

// Independently authenticates every job input, validates the two v2 backend
// identities and their common replay/manifest bindings, compares both traces,
// then creates a deterministic typed report exclusively.
Sh4EquivalenceReportSummary issueSh4EquivalenceReport(
		const std::filesystem::path& jobPath,
		const std::filesystem::path& reportPath,
		const std::filesystem::path& runningComparator);

// Recomputes the report from the immutable job inputs and requires byte-for-byte
// equality with an existing report.
Sh4EquivalenceReportSummary validateSh4EquivalenceReport(
		const std::filesystem::path& jobPath,
		const std::filesystem::path& reportPath,
		const std::filesystem::path& runningComparator);

} // namespace research

#pragma once

#include "research/sh4_observation_trace.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace research
{

struct Sh4ObservationEquivalenceContract
{
	Sha256Digest interpreterIdentityDigest {};
	Sha256Digest dynarecIdentityDigest {};
	Sha256Digest replayDigest {};
	Sha256Digest manifestSetDigest {};
	std::uint64_t maximumTraceBytes = DefaultMaximumSh4ObservationTraceBytes;
	std::uint64_t maximumEvents = DefaultMaximumSh4ObservationTraceEvents;
};

struct Sh4ObservationDivergence
{
	std::uint64_t ordinal = 0;
	std::string field;
	std::string interpreterValue;
	std::string dynarecValue;
};

struct Sh4ObservationComparison
{
	bool equivalent = false;
	std::uint64_t matchedEventCount = 0;
	Sh4ObservationTraceSummary interpreter;
	Sh4ObservationTraceSummary dynarec;
	std::optional<Sh4ObservationDivergence> firstDivergence;
};

// Independently parses, authenticates and compares both files. Malformed,
// incomplete, dropped or incorrectly bound inputs are rejected with an
// exception. A well-formed semantic difference is returned as firstDivergence.
Sh4ObservationComparison compareSh4ObservationTraces(
		const std::filesystem::path& interpreterPath,
		const std::filesystem::path& dynarecPath,
		const Sh4ObservationEquivalenceContract& contract);

} // namespace research

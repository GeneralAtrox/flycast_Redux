#pragma once

#include "research/sha256.h"

#include <cstdint>
#include <filesystem>

namespace research
{

constexpr std::uint32_t Sh4GhidraSemanticJoinSchemaVersion = 1;
constexpr std::uint64_t DefaultMaximumSh4GhidraSemanticJoinBytes =
		128ull * 1024 * 1024;

struct Sh4GhidraSemanticJoinSummary
{
	Sha256Digest artifactDigest {};
	Sha256Digest identityDigest {};
	Sha256Digest replayDigest {};
	Sha256Digest profileDigest {};
	Sha256Digest profilePayloadDigest {};
	Sha256Digest ghidraExportDigest {};
	Sha256Digest programDigest {};
	std::uint64_t blockCount = 0;
	std::uint64_t edgeCount = 0;
	std::uint64_t edgeOccurrences = 0;
	std::uint64_t authenticatedProgramBlocks = 0;
	std::uint64_t functionOwnedBlocks = 0;
	std::uint64_t staticExecutableUnassignedBlocks = 0;
	std::uint64_t runtimeOnlyBlocks = 0;
};

Sh4GhidraSemanticJoinSummary writeSh4GhidraSemanticJoin(
		const std::filesystem::path& output,
		const std::filesystem::path& profile,
		const std::filesystem::path& identity,
		const std::filesystem::path& replay,
		const std::filesystem::path& ghidraExport,
		const std::filesystem::path& program,
		const std::filesystem::path& exporterScript,
		std::uint64_t maximumBytes = DefaultMaximumSh4GhidraSemanticJoinBytes);

// Revalidates every source artifact, independently reconstructs the canonical
// semantic join and requires byte-for-byte equality with the candidate.
Sh4GhidraSemanticJoinSummary validateSh4GhidraSemanticJoin(
		const std::filesystem::path& candidate,
		const std::filesystem::path& profile,
		const std::filesystem::path& identity,
		const std::filesystem::path& replay,
		const std::filesystem::path& ghidraExport,
		const std::filesystem::path& program,
		const std::filesystem::path& exporterScript,
		std::uint64_t maximumBytes = DefaultMaximumSh4GhidraSemanticJoinBytes);

} // namespace research

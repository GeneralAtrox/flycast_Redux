#pragma once

#include "research/sh4_profile.h"
#include "research/sha256.h"

#include <cstdint>
#include <filesystem>

namespace research
{

constexpr std::uint32_t Sh4DynarecProfileArtifactSchemaVersion = 1;
constexpr std::uint32_t Sh4DynarecProfileArtifactHeaderSize = 256;
constexpr std::uint64_t DefaultMaximumSh4DynarecProfileArtifactBytes =
		256ull * 1024 * 1024;
constexpr std::uint64_t DefaultMaximumSh4DynarecProfileBlocks = 1'000'000;
constexpr std::uint64_t DefaultMaximumSh4DynarecProfileBranches = 4'000'000;

struct Sh4DynarecProfileArtifactBinding
{
	Sha256Digest identityDigest {};
	Sha256Digest replayDigest {};
	Sha256Digest configurationDigest {};
};

struct Sh4DynarecProfileArtifactSummary
{
	Sh4DynarecProfileArtifactBinding binding;
	Sha256Digest payloadDigest {};
	std::uint64_t blockCount = 0;
	std::uint64_t branchCount = 0;
	std::uint64_t enteredExecutions = 0;
	std::uint64_t completedExecutions = 0;
	std::uint64_t abortedExecutions = 0;
	std::uint64_t incompleteByteBlocks = 0;
	std::uint64_t payloadBytes = 0;
	std::vector<Sh4DynarecBlockExecution> blocks;
	std::vector<Sh4DynarecBranchExecution> branches;
};

Sh4DynarecProfileArtifactSummary writeSh4DynarecProfileArtifact(
		const std::filesystem::path& path,
		const Sh4DynarecProfileArtifactBinding& binding,
		const Sh4DynarecProfileSnapshot& profile,
		std::uint64_t maximumBytes =
				DefaultMaximumSh4DynarecProfileArtifactBytes);

Sh4DynarecProfileArtifactSummary validateSh4DynarecProfileArtifact(
		const std::filesystem::path& path,
		const Sh4DynarecProfileArtifactBinding& expectedBinding,
		std::uint64_t maximumBytes =
				DefaultMaximumSh4DynarecProfileArtifactBytes,
		std::uint64_t maximumBlocks =
				DefaultMaximumSh4DynarecProfileBlocks,
		std::uint64_t maximumBranches =
				DefaultMaximumSh4DynarecProfileBranches);

} // namespace research

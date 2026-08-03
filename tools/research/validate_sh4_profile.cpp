#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/sh4_profile_artifact.h"
#include "research/sha256.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace
{

void usage(const char *executable)
{
	std::fprintf(stderr,
			"Usage: %s --artifact <candidate.fcsh4profile> "
			"--identity <identity-v2.json> --replay <maple-replay.fcmt> "
			"[--max-bytes <count>] [--max-blocks <count>] "
			"[--max-branches <count>]\n", executable);
}

bool parseUnsigned(const char *text, std::uint64_t& value)
{
	const char *end = text + std::char_traits<char>::length(text);
	const auto result = std::from_chars(text, end, value);
	return result.ec == std::errc() && result.ptr == end && value != 0;
}

} // namespace

int main(int argc, char *argv[])
{
	std::filesystem::path artifactPath;
	std::filesystem::path identityPath;
	std::filesystem::path replayPath;
	std::uint64_t maximumBytes =
			research::DefaultMaximumSh4DynarecProfileArtifactBytes;
	std::uint64_t maximumBlocks =
			research::DefaultMaximumSh4DynarecProfileBlocks;
	std::uint64_t maximumBranches =
			research::DefaultMaximumSh4DynarecProfileBranches;
	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		if ((argument == "--artifact" || argument == "--identity"
				|| argument == "--replay" || argument == "--max-bytes"
				|| argument == "--max-blocks" || argument == "--max-branches")
				&& index + 1 >= argc)
		{
			usage(argv[0]);
			return 2;
		}
		if (argument == "--artifact")
			artifactPath = argv[++index];
		else if (argument == "--identity")
			identityPath = argv[++index];
		else if (argument == "--replay")
			replayPath = argv[++index];
		else if (argument == "--max-bytes")
		{
			if (!parseUnsigned(argv[++index], maximumBytes))
			{
				usage(argv[0]);
				return 2;
			}
		}
		else if (argument == "--max-blocks")
		{
			if (!parseUnsigned(argv[++index], maximumBlocks))
			{
				usage(argv[0]);
				return 2;
			}
		}
		else if (argument == "--max-branches")
		{
			if (!parseUnsigned(argv[++index], maximumBranches))
			{
				usage(argv[0]);
				return 2;
			}
		}
		else if (argument == "--help" || argument == "-h")
		{
			usage(argv[0]);
			return 0;
		}
		else
		{
			std::fprintf(stderr, "Unknown argument: %s\n", argument.c_str());
			usage(argv[0]);
			return 2;
		}
	}
	if (artifactPath.empty() || identityPath.empty() || replayPath.empty())
	{
		usage(argv[0]);
		return 2;
	}

	try
	{
		const auto identity = research::loadIdentityManifest(identityPath);
		if (identity.schemaVersion == 2)
			research::requireSh4DynarecProfileIdentityV2(identity);
		else
			research::requireSh4DynarecProfileRecordIdentityV3(identity);
		const auto& mapleIdentity = identity.schemaVersion == 2
				? identity.mapleReplayIdentityDigest : identity.digest;
		research::validateProductionMapleTraceFile(replayPath,
				mapleIdentity);
		research::Sh4DynarecProfileArtifactBinding binding;
		binding.identityDigest = identity.digest;
		binding.replayDigest = research::hashFileExact(replayPath,
				research::DefaultMaximumMapleTraceBytes);
		binding.configurationDigest = identity.configurationDigest;
		const auto summary = research::validateSh4DynarecProfileArtifact(
				artifactPath, binding, maximumBytes, maximumBlocks,
				maximumBranches);
		if (summary.incompleteByteBlocks != 0)
			throw std::runtime_error(
					"profile contains blocks without complete exact guest bytes");
		std::printf("ACCEPTED flycast-sh4-dynarec-profile-v1\n");
		std::printf("identity_sha256=%s\n",
				research::sha256ToHex(binding.identityDigest).c_str());
		std::printf("replay_sha256=%s\n",
				research::sha256ToHex(binding.replayDigest).c_str());
		std::printf("configuration_sha256=%s\n",
				research::sha256ToHex(binding.configurationDigest).c_str());
		std::printf("payload_sha256=%s\n",
				research::sha256ToHex(summary.payloadDigest).c_str());
		std::printf("block_count=%llu\n",
				static_cast<unsigned long long>(summary.blockCount));
		std::printf("branch_count=%llu\n",
				static_cast<unsigned long long>(summary.branchCount));
		std::printf("entered_executions=%llu\n",
				static_cast<unsigned long long>(summary.enteredExecutions));
		std::printf("completed_executions=%llu\n",
				static_cast<unsigned long long>(summary.completedExecutions));
		std::printf("aborted_executions=%llu\n",
				static_cast<unsigned long long>(summary.abortedExecutions));
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "REJECTED flycast-sh4-dynarec-profile-v1: %s\n",
				exception.what());
		return 1;
	}
}

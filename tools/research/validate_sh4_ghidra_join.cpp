#include "research/sh4_ghidra_join.h"
#include "research/sha256.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

namespace
{

void usage(const char *program)
{
	std::fprintf(stderr,
			"Usage: %s --artifact <join.json> --profile <profile.fcsh4profile> "
			"--identity <identity.json> --replay <maple.fcmt> "
			"--ghidra-export <export.json> --program <boot-executable> "
			"--script <ExportFlycastResearch.java> [--max-bytes <count>]\n",
			program);
}

bool parseUnsigned(const char *text, std::uint64_t& value)
{
	const char *end = text + std::char_traits<char>::length(text);
	const auto parsed = std::from_chars(text, end, value);
	return parsed.ec == std::errc() && parsed.ptr == end && value != 0;
}

} // namespace

int main(int argc, char **argv)
{
	std::filesystem::path artifact, profile, identity, replay, ghidraExport, program, script;
	std::uint64_t maximumBytes =
			research::DefaultMaximumSh4GhidraSemanticJoinBytes;
	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		if ((argument == "--artifact" || argument == "--profile"
				|| argument == "--identity" || argument == "--replay"
				|| argument == "--ghidra-export" || argument == "--program"
				|| argument == "--script" || argument == "--max-bytes")
				&& index + 1 >= argc)
		{
			usage(argv[0]);
			return 2;
		}
		if (argument == "--artifact") artifact = argv[++index];
		else if (argument == "--profile") profile = argv[++index];
		else if (argument == "--identity") identity = argv[++index];
		else if (argument == "--replay") replay = argv[++index];
		else if (argument == "--ghidra-export") ghidraExport = argv[++index];
		else if (argument == "--program") program = argv[++index];
		else if (argument == "--script") script = argv[++index];
		else if (argument == "--max-bytes")
		{
			if (!parseUnsigned(argv[++index], maximumBytes))
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
	if (artifact.empty() || profile.empty() || identity.empty() || replay.empty()
			|| ghidraExport.empty() || program.empty() || script.empty())
	{
		usage(argv[0]);
		return 2;
	}
	try
	{
		const auto summary = research::validateSh4GhidraSemanticJoin(artifact,
				profile, identity, replay, ghidraExport, program, script, maximumBytes);
		std::printf("ACCEPTED flycast-research-sh4-ghidra-semantic-join-v1\n");
		std::printf("artifact_sha256=%s\n",
				research::sha256ToHex(summary.artifactDigest).c_str());
		std::printf("block_count=%llu\n",
				static_cast<unsigned long long>(summary.blockCount));
		std::printf("edge_count=%llu\n",
				static_cast<unsigned long long>(summary.edgeCount));
		std::printf("edge_occurrences=%llu\n",
				static_cast<unsigned long long>(summary.edgeOccurrences));
		std::printf("authenticated_program_blocks=%llu\n",
				static_cast<unsigned long long>(summary.authenticatedProgramBlocks));
		std::printf("function_owned_blocks=%llu\n",
				static_cast<unsigned long long>(summary.functionOwnedBlocks));
		std::printf("static_executable_unassigned_blocks=%llu\n",
				static_cast<unsigned long long>(
						summary.staticExecutableUnassignedBlocks));
		std::printf("runtime_only_blocks=%llu\n",
				static_cast<unsigned long long>(summary.runtimeOnlyBlocks));
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "REJECTED semantic join: %s\n", exception.what());
		return 1;
	}
}

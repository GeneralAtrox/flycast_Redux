#include "research/identity_manifest.h"
#include "research/memory_ranges_artifact.h"
#include "research/memory_ranges_manifest.h"
#include "research/sha256.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

namespace
{

void usage(const char *executable)
{
	std::fprintf(stderr,
			"Usage: %s --artifact <candidate.fcmr> --identity <identity.json> "
			"--manifest <memory-ranges.json> [--max-bytes <count>]\n",
			executable);
}

bool parseUnsigned(const char *text, std::uint64_t& value)
{
	const char *end = text + std::char_traits<char>::length(text);
	const auto result = std::from_chars(text, end, value);
	return result.ec == std::errc() && result.ptr == end;
}

} // namespace

int main(int argc, char *argv[])
{
	std::filesystem::path artifactPath;
	std::filesystem::path identityPath;
	std::filesystem::path manifestPath;
	std::uint64_t maximumBytes = research::DefaultMaximumMemoryRangesArtifactBytes;
	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		if ((argument == "--artifact" || argument == "--identity"
				|| argument == "--manifest" || argument == "--max-bytes")
				&& index + 1 >= argc)
		{
			usage(argv[0]);
			return 2;
		}
		if (argument == "--artifact")
			artifactPath = argv[++index];
		else if (argument == "--identity")
			identityPath = argv[++index];
		else if (argument == "--manifest")
			manifestPath = argv[++index];
		else if (argument == "--max-bytes")
		{
			if (!parseUnsigned(argv[++index], maximumBytes) || maximumBytes == 0)
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
	if (artifactPath.empty() || identityPath.empty() || manifestPath.empty())
	{
		usage(argv[0]);
		return 2;
	}

	try
	{
		const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
		const research::MemoryRangesManifest manifest =
				research::loadMemoryRangesManifest(manifestPath);
		const research::MemoryRangesArtifactSummary summary =
				research::validateProductionMemoryRangesArtifactFile(
						artifactPath, identity, manifest, maximumBytes);
		std::printf("ACCEPTED flycast-memory-ranges-v1\n");
		std::printf("identity_sha256=%s\n", research::sha256ToHex(identity.digest).c_str());
		std::printf("manifest_sha256=%s\n", research::sha256ToHex(manifest.digest).c_str());
		std::printf("payload_sha256=%s\n",
				research::sha256ToHex(summary.payloadDigest).c_str());
		std::printf("trigger_pc=0x%08x\n", summary.triggerPc);
		std::printf("trigger_tick=%llu\n",
				static_cast<unsigned long long>(summary.startTick));
		std::printf("range_event_count=%llu\n",
				static_cast<unsigned long long>(summary.rangeEventCount));
		std::printf("memory_byte_count=%llu\n",
				static_cast<unsigned long long>(summary.totalMemoryBytes));
		std::printf("dropped_event_count=%llu\n",
				static_cast<unsigned long long>(summary.droppedEvents));
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "REJECTED flycast-memory-ranges-v1: %s\n", exception.what());
		return 1;
	}
}

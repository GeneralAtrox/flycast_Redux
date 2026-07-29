#include "research/identity_manifest.h"
#include "research/sh4_events_artifact.h"
#include "research/sh4_events_manifest.h"
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
			"Usage: %s --artifact <candidate.fcsh4> --identity <identity.json> "
			"--manifest <sh4-events.json> [--max-bytes <count>]\n",
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
	std::uint64_t maximumBytes = research::DefaultMaximumSh4EventsArtifactBytes;
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
		const research::IdentityManifest identity =
				research::loadIdentityManifest(identityPath);
		const research::Sh4EventsManifest manifest =
				research::loadSh4EventsManifest(manifestPath);
		const research::Sh4EventsArtifactSummary summary =
				research::validateProductionSh4EventsArtifactFile(
						artifactPath, identity, manifest, maximumBytes);
		std::printf("ACCEPTED flycast-sh4-events-v1\n");
		std::printf("identity_sha256=%s\n",
				research::sha256ToHex(identity.digest).c_str());
		std::printf("manifest_sha256=%s\n",
				research::sha256ToHex(manifest.digest).c_str());
		std::printf("payload_sha256=%s\n",
				research::sha256ToHex(summary.payloadDigest).c_str());
		std::printf("event_count=%llu\n",
				static_cast<unsigned long long>(summary.eventCount));
		std::printf("call_count=%llu\n",
				static_cast<unsigned long long>(summary.callCount));
		std::printf("return_count=%llu\n",
				static_cast<unsigned long long>(summary.returnCount));
		std::printf("watch_read_count=%llu\n",
				static_cast<unsigned long long>(summary.watchReadCount));
		std::printf("watch_write_count=%llu\n",
				static_cast<unsigned long long>(summary.watchWriteCount));
		std::printf("snapshot_count=%llu\n",
				static_cast<unsigned long long>(summary.snapshotCount));
		std::printf("snapshot_byte_count=%llu\n",
				static_cast<unsigned long long>(summary.snapshotBytes));
		std::printf("exception_count=%llu\n",
				static_cast<unsigned long long>(summary.exceptionCount));
		std::printf("start_tick=%llu\n",
				static_cast<unsigned long long>(summary.startTick));
		std::printf("end_tick=%llu\n",
				static_cast<unsigned long long>(summary.endTick));
		std::printf("dropped_event_count=%llu\n",
				static_cast<unsigned long long>(summary.droppedEvents));
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "REJECTED flycast-sh4-events-v1: %s\n", exception.what());
		return 1;
	}
}

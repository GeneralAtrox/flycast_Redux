#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/pvr_ta_artifact.h"
#include "research/pvr_ta_manifest.h"
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
			"Usage: %s --artifact <candidate.fcpvr> --identity <identity-v2.json> "
			"--replay <maple-replay.fcmt> --manifest <pvr-capture.json> "
			"[--max-bytes <count>] [--max-events <count>]\n", executable);
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
	std::filesystem::path replayPath;
	std::filesystem::path manifestPath;
	std::uint64_t maximumBytes = research::DefaultMaximumPvrTaArtifactBytes;
	std::uint64_t maximumEvents = research::DefaultMaximumPvrTaArtifactEvents;
	bool maximumBytesSpecified = false;
	bool maximumEventsSpecified = false;
	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		if ((argument == "--artifact" || argument == "--identity"
				|| argument == "--replay" || argument == "--manifest"
				|| argument == "--max-bytes" || argument == "--max-events")
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
		else if (argument == "--manifest")
			manifestPath = argv[++index];
		else if (argument == "--max-bytes")
		{
			if (!parseUnsigned(argv[++index], maximumBytes) || maximumBytes == 0)
			{
				usage(argv[0]);
				return 2;
			}
			maximumBytesSpecified = true;
		}
		else if (argument == "--max-events")
		{
			if (!parseUnsigned(argv[++index], maximumEvents) || maximumEvents == 0)
			{
				usage(argv[0]);
				return 2;
			}
			maximumEventsSpecified = true;
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
	if (artifactPath.empty() || identityPath.empty() || replayPath.empty()
			|| manifestPath.empty())
	{
		usage(argv[0]);
		return 2;
	}

	try
	{
		const research::IdentityManifest identity =
				research::loadIdentityManifest(identityPath);
		research::requireSh4EquivalenceIdentityV2(identity);
		research::PvrTaArtifactBinding binding;
		binding.backend = identity.runtimeConfiguration.cpuBackend == "dynarec"
				? research::Sh4ObservationBackend::Dynarec
				: research::Sh4ObservationBackend::Interpreter;
		binding.identityDigest = identity.digest;
		research::validateProductionMapleTraceFile(replayPath,
				identity.mapleReplayIdentityDigest,
				research::DefaultMaximumPvrTaArtifactBytes);
		binding.replayDigest = research::hashFileExact(replayPath,
				research::DefaultMaximumPvrTaArtifactBytes);
		const research::PvrTaManifest manifest =
				research::loadPvrTaManifest(manifestPath);
		research::requirePvrTaManifestIdentity(manifest, identity);
		if (!maximumBytesSpecified)
			maximumBytes = manifest.maximumBytes;
		if (!maximumEventsSpecified)
			maximumEvents = manifest.maximumEvents;
		binding.manifestDigest = manifest.digest;
		const research::PvrTaArtifactSummary summary =
				research::validatePvrTaArtifactFile(artifactPath, binding,
						maximumBytes, maximumEvents);
		if (maximumBytes != manifest.maximumBytes
				|| maximumEvents != manifest.maximumEvents)
			throw std::runtime_error("CLI limits differ from the capture manifest");
		if (summary.typeCounts[4] != manifest.renderDoneCount)
			throw std::runtime_error(
					"render-done count differs from the capture manifest");
		std::printf("ACCEPTED flycast-pvr-ta-v1\n");
		std::printf("identity_sha256=%s\n",
				research::sha256ToHex(binding.identityDigest).c_str());
		std::printf("replay_sha256=%s\n",
				research::sha256ToHex(binding.replayDigest).c_str());
		std::printf("manifest_sha256=%s\n",
				research::sha256ToHex(binding.manifestDigest).c_str());
		std::printf("payload_sha256=%s\n",
				research::sha256ToHex(summary.payloadDigest).c_str());
		std::printf("event_count=%llu\n",
				static_cast<unsigned long long>(summary.eventCount));
		std::printf("accepted_block_count=%llu\n",
				static_cast<unsigned long long>(summary.typeCounts[2]));
		std::printf("start_render_count=%llu\n",
				static_cast<unsigned long long>(summary.typeCounts[3]));
		std::printf("render_done_count=%llu\n",
				static_cast<unsigned long long>(summary.typeCounts[4]));
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "REJECTED flycast-pvr-ta-v1: %s\n", exception.what());
		return 1;
	}
}

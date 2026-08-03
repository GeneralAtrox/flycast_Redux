#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/pvr_draw_artifact.h"
#include "research/pvr_presentation_artifact.h"
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

struct Arguments
{
	std::filesystem::path artifact;
	std::filesystem::path taArtifact;
	std::filesystem::path presentationArtifact;
	std::filesystem::path identity;
	std::filesystem::path replay;
	std::filesystem::path manifest;
	std::uint64_t maximumDrawBytes = research::DefaultMaximumPvrDrawArtifactBytes;
	std::uint64_t maximumDrawEvents = research::DefaultMaximumPvrDrawArtifactEvents;
	std::uint64_t maximumPresentationBytes =
			research::DefaultMaximumPvrPresentationArtifactBytes;
};

void usage(const char* executable)
{
	std::fprintf(stderr,
			"Usage: %s --artifact <draw.fcpvrd> --ta-artifact <ta.fcpvr> "
			"--presentation-artifact <presentation.fcpvrp> --identity <identity-v2.json> "
			"--replay <maple.fcmt> --manifest <pvr-ta.json> "
			"[--max-draw-bytes <count>] [--max-draw-events <count>] "
			"[--max-presentation-bytes <count>]\n", executable);
}

std::uint64_t unsignedValue(const char* text, const std::string& field)
{
	std::uint64_t value = 0;
	const char* end = text + std::char_traits<char>::length(text);
	const auto parsed = std::from_chars(text, end, value);
	if (parsed.ec != std::errc() || parsed.ptr != end || value == 0)
		throw std::invalid_argument(field + " must be a positive integer");
	return value;
}

Arguments arguments(int argc, char** argv)
{
	Arguments result;
	for (int i = 1; i < argc; ++i)
	{
		const std::string option = argv[i];
		if (option == "--help" || option == "-h")
		{
			usage(argv[0]);
			std::exit(0);
		}
		if (i + 1 >= argc)
			throw std::invalid_argument("missing value for " + option);
		const char* value = argv[++i];
		if (option == "--artifact") result.artifact = value;
		else if (option == "--ta-artifact") result.taArtifact = value;
		else if (option == "--presentation-artifact") result.presentationArtifact = value;
		else if (option == "--identity") result.identity = value;
		else if (option == "--replay") result.replay = value;
		else if (option == "--manifest") result.manifest = value;
		else if (option == "--max-draw-bytes")
			result.maximumDrawBytes = unsignedValue(value, option);
		else if (option == "--max-draw-events")
			result.maximumDrawEvents = unsignedValue(value, option);
		else if (option == "--max-presentation-bytes")
			result.maximumPresentationBytes = unsignedValue(value, option);
		else
			throw std::invalid_argument("unknown argument: " + option);
	}
	if (result.artifact.empty() || result.taArtifact.empty()
			|| result.presentationArtifact.empty() || result.identity.empty()
			|| result.replay.empty() || result.manifest.empty())
		throw std::invalid_argument("required argument is missing");
	return result;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const Arguments args = arguments(argc, argv);
		const research::IdentityManifest identity =
				research::loadIdentityManifest(args.identity);
		research::requireSh4EquivalenceIdentityV2(identity);
		if (!identity.runtimeConfiguration.pvrDrawConfiguration.available)
			throw std::runtime_error(
					"identity has no authenticated PowerVR draw configuration");
		constexpr std::uint64_t MaximumReplayBytes = 512ull * 1024 * 1024;
		research::validateProductionMapleTraceFile(args.replay,
				identity.mapleReplayIdentityDigest, MaximumReplayBytes);
		const research::Sha256Digest replayDigest = research::hashFileExact(
				args.replay, MaximumReplayBytes);
		const research::PvrTaManifest manifest =
				research::loadPvrTaManifest(args.manifest);
		research::requirePvrTaManifestIdentity(manifest, identity);
		const research::Sh4ObservationBackend backend =
				identity.runtimeConfiguration.cpuBackend == "dynarec"
				? research::Sh4ObservationBackend::Dynarec
				: research::Sh4ObservationBackend::Interpreter;
		research::PvrTaArtifactBinding taBinding;
		taBinding.backend = backend;
		taBinding.identityDigest = identity.digest;
		taBinding.replayDigest = replayDigest;
		taBinding.manifestDigest = manifest.digest;
		const auto taSummary = research::validatePvrTaArtifactFile(args.taArtifact,
				taBinding, manifest.maximumBytes, manifest.maximumEvents);
		if (taSummary.typeCounts[4] != manifest.renderDoneCount)
			throw std::runtime_error("TA render-done count differs from manifest");
		research::PvrPresentationArtifactBinding presentationBinding;
		presentationBinding.backend = backend;
		presentationBinding.identityDigest = identity.digest;
		presentationBinding.replayDigest = replayDigest;
		const auto presentationSummary =
				research::validatePvrPresentationArtifactFile(
						args.presentationArtifact, presentationBinding,
						args.maximumPresentationBytes,
						research::DefaultMaximumPvrPresentationArtifactEvents);
		if (!presentationSummary.completeVerticalSlice)
			throw std::runtime_error("presentation artifact lacks a complete vertical slice");
		research::PvrDrawArtifactBinding drawBinding;
		drawBinding.backend = backend;
		drawBinding.identityDigest = identity.digest;
		drawBinding.replayDigest = replayDigest;
		drawBinding.taArtifactDigest = research::hashFileExact(args.taArtifact,
				manifest.maximumBytes);
		drawBinding.presentationArtifactDigest = research::hashFileExact(
				args.presentationArtifact, args.maximumPresentationBytes);
		drawBinding.rendererConfigurationDigest =
				research::pvrDrawConfigurationDigest(
						identity.runtimeConfiguration.pvrDrawConfiguration);
		const auto drawSummary = research::validatePvrDrawArtifactAgainstTaFile(
				args.artifact, drawBinding, args.taArtifact, taBinding,
				args.maximumDrawBytes, args.maximumDrawEvents,
				manifest.maximumBytes, manifest.maximumEvents);
		std::printf("ACCEPTED flycast-pvr-draw-v1\n");
		std::printf("identity_sha256=%s\n",
				research::sha256ToHex(identity.digest).c_str());
		std::printf("replay_sha256=%s\n",
				research::sha256ToHex(replayDigest).c_str());
		std::printf("ta_artifact_sha256=%s\n",
				research::sha256ToHex(drawBinding.taArtifactDigest).c_str());
		std::printf("presentation_artifact_sha256=%s\n",
				research::sha256ToHex(
					drawBinding.presentationArtifactDigest).c_str());
		std::printf("renderer_configuration_sha256=%s\n",
				research::sha256ToHex(
					drawBinding.rendererConfigurationDigest).c_str());
		std::printf("payload_sha256=%s\n",
				research::sha256ToHex(drawSummary.payloadDigest).c_str());
		std::printf("event_count=%llu\n",
				static_cast<unsigned long long>(drawSummary.eventCount));
		std::printf("primitive_count=%llu\n",
				static_cast<unsigned long long>(drawSummary.typeCounts[0]));
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "REJECTED flycast-pvr-draw-v1: %s\n",
				exception.what());
		return 1;
	}
}

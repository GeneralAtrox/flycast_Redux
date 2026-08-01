#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/pvr_presentation_artifact.h"
#include "research/sha256.h"

#include "json.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using json = nlohmann::json;

struct Arguments
{
	std::filesystem::path artifact;
	std::filesystem::path comparison;
	std::filesystem::path identity;
	std::filesystem::path replay;
};

Arguments parseArguments(int argc, char** argv)
{
	Arguments result;
	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		if (index + 1 >= argc)
			throw std::invalid_argument("missing value for " + argument);
		const std::filesystem::path value =
				std::filesystem::u8path(argv[++index]);
		if (argument == "--artifact")
			result.artifact = value;
		else if (argument == "--compare")
			result.comparison = value;
		else if (argument == "--identity")
			result.identity = value;
		else if (argument == "--replay")
			result.replay = value;
		else
			throw std::invalid_argument("unknown argument: " + argument);
	}
	if (result.artifact.empty() || result.identity.empty() || result.replay.empty())
		throw std::invalid_argument(
				"usage: --artifact PATH --identity PATH --replay PATH [--compare PATH]");
	return result;
}

research::PvrPresentationArtifactBinding bindingFor(
		const research::IdentityManifest& identity,
		const research::Sha256Digest& replayDigest)
{
	research::PvrPresentationArtifactBinding binding;
	binding.backend = identity.runtimeConfiguration.cpuBackend == "dynarec"
			? research::Sh4ObservationBackend::Dynarec
			: research::Sh4ObservationBackend::Interpreter;
	binding.identityDigest = identity.digest;
	binding.replayDigest = replayDigest;
	return binding;
}

bool sameConfig(const research::PvrFramebufferConfig& lhs,
		const research::PvrFramebufferConfig& rhs)
{
	return lhs.fbReadSize == rhs.fbReadSize
			&& lhs.fbReadControl == rhs.fbReadControl
			&& lhs.spgControl == rhs.spgControl
			&& lhs.spgStatus == rhs.spgStatus
			&& lhs.fbReadSof1 == rhs.fbReadSof1
			&& lhs.fbReadSof2 == rhs.fbReadSof2
			&& lhs.videoControl == rhs.videoControl
			&& lhs.borderColor == rhs.borderColor;
}

} // namespace

int main(int argc, char** argv)
{
	try
	{
		const Arguments arguments = parseArguments(argc, argv);
		const auto identity = research::loadIdentityManifest(arguments.identity);
		research::requireSh4EquivalenceIdentityV2(identity);
		constexpr std::uint64_t MaximumReplayBytes = 512ull * 1024 * 1024;
		research::validateProductionMapleTraceFile(arguments.replay,
				identity.mapleReplayIdentityDigest, MaximumReplayBytes);
		const auto replayDigest = research::hashFileExact(arguments.replay,
				MaximumReplayBytes);
		const auto binding = bindingFor(identity, replayDigest);

		std::vector<research::PvrValidatedFramebuffer> frames;
		const auto summary = research::validatePvrPresentationArtifactFile(
				arguments.artifact, binding,
				research::DefaultMaximumPvrPresentationArtifactBytes,
				research::DefaultMaximumPvrPresentationArtifactEvents, &frames);
		if (!summary.completeVerticalSlice)
			throw std::runtime_error(
					"artifact does not contain a complete register/VRAM/render/framebuffer/presentation slice");
		json report {
			{"schema", "flycast-research-pvr-presentation-validation"},
			{"schema_version", 1},
			{"valid", true},
			{"event_count", summary.eventCount},
			{"framebuffer_count", summary.decodedFramebufferCount},
			{"complete_vertical_slice", summary.completeVerticalSlice},
			{"payload_sha256", research::sha256ToHex(summary.payloadDigest)},
		};

		bool exact = true;
		if (!arguments.comparison.empty())
		{
			std::vector<research::PvrValidatedFramebuffer> candidates;
			const auto candidateSummary =
					research::validatePvrPresentationArtifactFile(
							arguments.comparison, binding,
							research::DefaultMaximumPvrPresentationArtifactBytes,
							research::DefaultMaximumPvrPresentationArtifactEvents,
							&candidates);
			if (!candidateSummary.completeVerticalSlice)
				throw std::runtime_error(
						"comparison artifact does not contain a complete vertical slice");
			report["comparison_payload_sha256"] =
					research::sha256ToHex(candidateSummary.payloadDigest);
			report["comparison_framebuffer_count"] = candidates.size();
			json differences = json::array();
			if (frames.size() != candidates.size())
				exact = false;
			const std::size_t count = std::min(frames.size(), candidates.size());
			for (std::size_t index = 0; index < count; ++index)
			{
				const bool configurationExact = frames[index].generation
						== candidates[index].generation
						&& frames[index].sourceRenderGeneration
								== candidates[index].sourceRenderGeneration
						&& frames[index].kind == candidates[index].kind
						&& sameConfig(frames[index].config,
						candidates[index].config)
						&& frames[index].rowBytes == candidates[index].rowBytes;
				research::ExactPvrFrameDifference difference;
				if (configurationExact)
					difference = research::comparePvrFramebuffersExact(
							frames[index].decoded, candidates[index].decoded);
				else
					exact = false;
				exact = exact && configurationExact && difference.exact;
				differences.push_back({
					{"ordinal", index},
					{"configuration_exact", configurationExact},
					{"pixels_exact", configurationExact && difference.exact},
					{"compared_pixels", difference.comparedPixels},
					{"differing_pixels", difference.differingPixels},
					{"maximum_channel_difference",
							difference.maximumChannelDifference},
					{"absolute_channel_difference",
							difference.absoluteChannelDifference},
					{"minimum_x", difference.minimumX},
					{"minimum_y", difference.minimumY},
					{"maximum_x", difference.maximumX},
					{"maximum_y", difference.maximumY},
				});
			}
			report["frames_exact"] = exact;
			report["frames"] = std::move(differences);
		}
		std::cout << report.dump(2) << '\n';
		return exact ? 0 : 1;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "PowerVR presentation validation failed: "
				<< exception.what() << '\n';
		return 2;
	}
}

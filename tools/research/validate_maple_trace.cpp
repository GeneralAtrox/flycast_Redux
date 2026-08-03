#include "research/identity_manifest.h"
#include "research/maple_trace.h"
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
			"Usage: %s --trace <candidate.fcmr> --identity <identity.json> "
			"[--max-bytes <count>]\n",
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
	std::filesystem::path tracePath;
	std::filesystem::path identityPath;
	std::uint64_t maximumBytes = research::DefaultMaximumMapleTraceBytes;

	for (int i = 1; i < argc; ++i)
	{
		const std::string argument = argv[i];
		if ((argument == "--trace" || argument == "--identity" || argument == "--max-bytes")
				&& i + 1 >= argc)
		{
			usage(argv[0]);
			return 2;
		}
		if (argument == "--trace")
			tracePath = std::filesystem::path(argv[++i]);
		else if (argument == "--identity")
			identityPath = std::filesystem::path(argv[++i]);
		else if (argument == "--max-bytes")
		{
			if (!parseUnsigned(argv[++i], maximumBytes) || maximumBytes == 0)
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

	if (tracePath.empty() || identityPath.empty())
	{
		usage(argv[0]);
		return 2;
	}

	try
	{
		const research::IdentityManifest identity = research::loadIdentityManifest(identityPath);
		if (identity.schemaVersion == 2)
			research::requireSh4EquivalenceIdentityV2(identity);
		else if (identity.schemaVersion == 1)
			research::requireCaptureV1Identity(identity);
		else
			research::requireMapleRecordIdentityV3(identity);
		const research::Sha256Digest& traceIdentity = identity.hasMapleReplayIdentityDigest
				? identity.mapleReplayIdentityDigest : identity.digest;
		const research::MapleTraceSummary summary = research::validateProductionMapleTraceFile(
				tracePath, traceIdentity, maximumBytes);
		std::printf("ACCEPTED flycast-maple-trace-v%u\n", summary.schemaVersion);
		std::printf("schema_version=%u\n", summary.schemaVersion);
		std::printf("identity_sha256=%s\n", research::sha256ToHex(traceIdentity).c_str());
		std::printf("payload_sha256=%s\n",
				research::sha256ToHex(summary.payloadDigest).c_str());
		std::printf("dma_count=%llu\n",
				static_cast<unsigned long long>(summary.dmaCount));
		std::printf("transaction_count=%llu\n",
				static_cast<unsigned long long>(summary.transactionCount));
		std::printf("control_descriptor_count=%llu\n",
				static_cast<unsigned long long>(summary.controlDescriptorCount));
		std::printf("event_count=%llu\n",
				static_cast<unsigned long long>(summary.eventCount));
		std::printf("start_tick=%llu\n",
				static_cast<unsigned long long>(summary.startTick));
		std::printf("end_tick=%llu\n",
				static_cast<unsigned long long>(summary.endTick));
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "REJECTED flycast-maple-trace: %s\n", exception.what());
		return 1;
	}
}

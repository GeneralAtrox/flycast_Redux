#include "research/ghidra_export.h"
#include "research/identity_manifest.h"
#include "research/sh4_events_manifest.h"
#include "research/sha256.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace
{

void usage(const char *executable)
{
	std::fprintf(stderr,
			"Usage: %s --export <ghidra-export.json> --identity <identity.json> "
			"--program <boot-executable> --script <ExportFlycastResearch.java> "
			"[--sh4-manifest <sh4-events.json>]\n",
			executable);
}

} // namespace

int main(int argc, char *argv[])
{
	std::filesystem::path exportPath;
	std::filesystem::path identityPath;
	std::filesystem::path programPath;
	std::filesystem::path scriptPath;
	std::filesystem::path sh4ManifestPath;
	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		if ((argument == "--export" || argument == "--identity" || argument == "--program"
				|| argument == "--script" || argument == "--sh4-manifest")
				&& index + 1 >= argc)
		{
			usage(argv[0]);
			return 2;
		}
		if (argument == "--export")
			exportPath = argv[++index];
		else if (argument == "--identity")
			identityPath = argv[++index];
		else if (argument == "--program")
			programPath = argv[++index];
		else if (argument == "--script")
			scriptPath = argv[++index];
		else if (argument == "--sh4-manifest")
			sh4ManifestPath = argv[++index];
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
	if (exportPath.empty() || identityPath.empty() || programPath.empty() || scriptPath.empty())
	{
		usage(argv[0]);
		return 2;
	}

	try
	{
		const research::IdentityManifest identity =
				research::loadIdentityManifest(identityPath);
		const research::GhidraExport exportArtifact =
				research::loadGhidraExport(exportPath);
		research::requireGhidraExportIdentity(exportArtifact, identity, programPath, scriptPath);
		if (!sh4ManifestPath.empty())
		{
			const research::Sh4EventsManifest manifest =
					research::loadSh4EventsManifest(sh4ManifestPath);
			research::requireSh4EventsIdentity(manifest, identity);
			research::requireGhidraExportSh4Join(exportArtifact, manifest);
		}
		std::printf("ACCEPTED flycast-research-ghidra-export-v1\n");
		std::printf("export_id=%s\n", exportArtifact.exportId.c_str());
		std::printf("export_sha256=%s\n",
				research::sha256ToHex(exportArtifact.digest).c_str());
		std::printf("program_sha256=%s\n",
				research::sha256ToHex(exportArtifact.executableDigest).c_str());
		std::printf("image_base=0x%08x\n", exportArtifact.imageBase);
		std::printf("memory_block_count=%zu\n", exportArtifact.memoryBlockCount);
		std::printf("function_count=%zu\n", exportArtifact.functionCount);
		std::printf("symbol_count=%zu\n", exportArtifact.symbolCount);
		std::printf("data_type_count=%zu\n", exportArtifact.dataTypeCount);
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "REJECTED flycast-research-ghidra-export-v1: %s\n",
				exception.what());
		return 1;
	}
}

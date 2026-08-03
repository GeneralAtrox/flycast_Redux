#include "research/sh4_ghidra_package.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace
{
void usage(const char *program)
{
	std::fprintf(stderr,
			"Usage: %s --package <directory> [--receipt <package-validation.json>]\n",
			program);
}
}

int main(int argc, char **argv)
{
	std::filesystem::path package;
	std::filesystem::path receipt;
	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		if (argument == "--help" || argument == "-h")
		{
			usage(argv[0]);
			return 0;
		}
		if ((argument != "--package" && argument != "--receipt")
				|| index + 1 >= argc)
		{
			usage(argv[0]);
			return 2;
		}
		if (argument == "--package")
			package = argv[++index];
		else
			receipt = argv[++index];
	}
	if (package.empty())
	{
		usage(argv[0]);
		return 2;
	}
	try
	{
		const auto summary = receipt.empty()
				? research::validateSh4GhidraPackageV1ReadOnly(package)
				: research::issueSh4GhidraPackageV1Receipt(package, receipt, argv[0]);
		std::printf("ACCEPTED flycast-research-sh4-ghidra-package-v1\n");
		std::printf("package_id=%s\n", summary.packageId.c_str());
		std::printf("locked_entry_count=%zu\n", summary.lockedEntryCount);
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "REJECTED flycast-research-sh4-ghidra-package-v1: %s\n",
				exception.what());
		return 1;
	}
}

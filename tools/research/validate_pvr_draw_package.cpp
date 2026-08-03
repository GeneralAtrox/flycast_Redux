#include "research/pvr_draw_package.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace
{
void usage(const char* executable)
{
	std::fprintf(stderr,
			"Usage: %s --package <directory> [--receipt <package-validation.json>]\n",
			executable);
}
}

int main(int argc, char** argv)
{
	std::filesystem::path package;
	std::filesystem::path receipt;
	for (int i = 1; i < argc; ++i)
	{
		const std::string argument = argv[i];
		if (argument == "--help" || argument == "-h")
		{
			usage(argv[0]);
			return 0;
		}
		if ((argument != "--package" && argument != "--receipt") || i + 1 >= argc)
		{
			usage(argv[0]);
			return 2;
		}
		if (argument == "--package") package = argv[++i];
		else receipt = argv[++i];
	}
	if (package.empty())
	{
		usage(argv[0]);
		return 2;
	}
	try
	{
		const auto summary = receipt.empty()
				? research::validatePvrDrawPackageV1ReadOnly(package)
				: research::issuePvrDrawPackageV1Receipt(package, receipt, argv[0]);
		std::printf("ACCEPTED flycast-research-pvr-draw-package-v1\n");
		std::printf("package_id=%s\n", summary.packageId.c_str());
		std::printf("entry_count=%zu\n", summary.entryCount);
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "REJECTED flycast-research-pvr-draw-package-v1: %s\n",
				exception.what());
		return 1;
	}
}

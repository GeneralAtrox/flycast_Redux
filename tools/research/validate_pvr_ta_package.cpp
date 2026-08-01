#include "research/pvr_ta_package.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace
{

void usage(const char *executable)
{
	std::fprintf(stderr,
			"Usage: %s --package <directory> [--receipt <package-validation.json>]\n",
			executable);
}

} // namespace

int main(int argc, char *argv[])
{
	std::filesystem::path package;
	std::filesystem::path receipt;
	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		if ((argument == "--package" || argument == "--receipt")
				&& index + 1 >= argc)
		{
			usage(argv[0]);
			return 2;
		}
		if (argument == "--package")
			package = argv[++index];
		else if (argument == "--receipt")
			receipt = argv[++index];
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
	if (package.empty())
	{
		usage(argv[0]);
		return 2;
	}
	try
	{
		const research::PvrTaPackageSummary summary = receipt.empty()
				? research::validatePvrTaPackageV1ReadOnly(package)
				: research::issuePvrTaPackageV1Receipt(package, receipt, argv[0]);
		std::printf("ACCEPTED flycast-research-pvr-ta-package-v1\n");
		std::printf("package_id=%s\n", summary.packageId.c_str());
		std::printf("entry_count=%zu\n", summary.entryCount);
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "REJECTED flycast-research-pvr-ta-package-v1: %s\n",
				exception.what());
		return 1;
	}
}

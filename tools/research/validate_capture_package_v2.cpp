#include "research/capture_package_v2.h"

#include <cstdio>
#include <filesystem>
#include <string>

namespace
{

void usage(const char *executable)
{
	std::fprintf(stderr,
			"Usage: %s --package <capture-package-v2-directory> "
			"[--receipt <capture-package-validation.json>]\n",
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
		research::CapturePackageV2Summary summary;
		if (receipt.empty())
			summary = research::validateCapturePackageV2ReadOnly(package);
		else
			summary = research::issueCapturePackageV2Receipt(package, receipt,
					std::filesystem::absolute(argv[0]));
		std::printf("ACCEPTED flycast-research-capture-package-v2\n");
		std::printf("package_id=%s\n", summary.packageId.c_str());
		std::printf("artifact_count=%zu\n", summary.artifactCount);
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "REJECTED flycast-research-capture-package-v2: %s\n",
				exception.what());
		return 1;
	}
}

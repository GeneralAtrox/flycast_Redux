#include "research/sh4_equivalence_package.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace
{

void usage(const char *executable)
{
	std::fprintf(stderr,
			"Usage: %s --package <equivalence-package-v1-directory> "
			"[--receipt <package-validation.json>]\n", executable);
}

std::filesystem::path runningExecutable()
{
#ifdef _WIN32
	std::vector<wchar_t> buffer(1024);
	while (buffer.size() <= 32768)
	{
		const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
				static_cast<DWORD>(buffer.size()));
		if (length == 0)
			throw std::runtime_error("cannot resolve the running package validator");
		if (length < buffer.size())
			return std::filesystem::path(std::wstring(buffer.data(), length));
		buffer.resize(buffer.size() * 2);
	}
	throw std::runtime_error("running package-validator path is too long");
#elif defined(__APPLE__)
	std::uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::vector<char> buffer(size);
	if (_NSGetExecutablePath(buffer.data(), &size) != 0)
		throw std::runtime_error("cannot resolve the running package validator");
	return std::filesystem::weakly_canonical(buffer.data());
#elif defined(__linux__)
	return std::filesystem::read_symlink("/proc/self/exe");
#elif defined(__FreeBSD__)
	return std::filesystem::read_symlink("/proc/curproc/file");
#else
	throw std::runtime_error("running executable discovery is unsupported");
#endif
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
		{
			if (!package.empty())
			{
				usage(argv[0]);
				return 2;
			}
			package = argv[++index];
		}
		else if (argument == "--receipt")
		{
			if (!receipt.empty())
			{
				usage(argv[0]);
				return 2;
			}
			receipt = argv[++index];
		}
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
		const research::Sh4EquivalencePackageSummary summary = receipt.empty()
				? research::validateSh4EquivalencePackageV1ReadOnly(package)
				: research::issueSh4EquivalencePackageV1Receipt(package, receipt,
						runningExecutable());
		std::printf("ACCEPTED flycast-research-sh4-equivalence-package-v1\n");
		std::printf("package_id=%s\n", summary.packageId.c_str());
		std::printf("locked_entry_count=%zu\n", summary.lockedEntryCount);
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr,
				"REJECTED flycast-research-sh4-equivalence-package-v1: %s\n",
				exception.what());
		return 1;
	}
}

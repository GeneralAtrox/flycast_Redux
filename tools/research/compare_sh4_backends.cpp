#include "research/sh4_equivalence.h"

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
			"Usage: %s --job <equivalence-job.json> "
			"(--report <new-report.json> | --validate-report <report.json>)\n",
			executable);
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
			throw std::runtime_error("cannot resolve the running comparator executable");
		if (length < buffer.size())
			return std::filesystem::path(std::wstring(buffer.data(), length));
		buffer.resize(buffer.size() * 2);
	}
	throw std::runtime_error("running comparator executable path is too long");
#elif defined(__APPLE__)
	std::uint32_t size = 0;
	_NSGetExecutablePath(nullptr, &size);
	std::vector<char> buffer(size);
	if (_NSGetExecutablePath(buffer.data(), &size) != 0)
		throw std::runtime_error("cannot resolve the running comparator executable");
	return std::filesystem::weakly_canonical(buffer.data());
#elif defined(__linux__)
	return std::filesystem::read_symlink("/proc/self/exe");
#elif defined(__FreeBSD__)
	return std::filesystem::read_symlink("/proc/curproc/file");
#else
	throw std::runtime_error("running executable discovery is unsupported on this platform");
#endif
}

} // namespace

int main(int argc, char *argv[])
{
	std::filesystem::path job;
	std::filesystem::path report;
	bool validate = false;
	for (int index = 1; index < argc; ++index)
	{
		const std::string argument = argv[index];
		if ((argument == "--job" || argument == "--report"
				|| argument == "--validate-report") && index + 1 >= argc)
		{
			usage(argv[0]);
			return 2;
		}
		if (argument == "--job")
		{
			if (!job.empty())
			{
				usage(argv[0]);
				return 2;
			}
			job = argv[++index];
		}
		else if (argument == "--report" || argument == "--validate-report")
		{
			if (!report.empty())
			{
				usage(argv[0]);
				return 2;
			}
			validate = argument == "--validate-report";
			report = argv[++index];
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
	if (job.empty() || report.empty())
	{
		usage(argv[0]);
		return 2;
	}
	try
	{
		const std::filesystem::path running = runningExecutable();
		const research::Sh4EquivalenceReportSummary summary = validate
				? research::validateSh4EquivalenceReport(job, report, running)
				: research::issueSh4EquivalenceReport(job, report, running);
		std::printf("%s flycast-research-sh4-equivalence-report-v1\n",
				summary.equivalent ? "EQUIVALENT" : "DIVERGENT");
		std::printf("job_id=%s\n", summary.jobId.c_str());
		std::printf("matched_event_count=%llu\n",
				static_cast<unsigned long long>(summary.matchedEventCount));
		if (summary.firstDivergence.has_value())
		{
			std::printf("first_divergence_ordinal=%llu\n",
					static_cast<unsigned long long>(
							summary.firstDivergence->ordinal));
			std::printf("first_divergence_field=%s\n",
					summary.firstDivergence->field.c_str());
		}
		return summary.equivalent ? 0 : 3;
	}
	catch (const std::exception& exception)
	{
		std::fprintf(stderr, "REJECTED flycast-research-sh4-equivalence-report-v1: %s\n",
				exception.what());
		return 1;
	}
}

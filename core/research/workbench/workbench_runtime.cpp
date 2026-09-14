#include "research/workbench/workbench_runtime.h"

#include "cfg/option.h"
#include "types.h"
#include "version.h"

#include <cstring>
#include <mutex>
#include <stdexcept>

namespace research::workbench
{
namespace
{

WorkbenchRecorder recorder;
std::mutex controlMutex;
bool launchConfigured = false;

std::filesystem::path optionPath(const std::string& value)
{
#ifdef __cpp_char8_t
	std::u8string utf8(value.size(), u8'\0');
	std::memcpy(utf8.data(), value.data(), value.size());
	return std::filesystem::path(utf8);
#else
	return std::filesystem::u8path(value);
#endif
}

void applyRange(bool& enabled, std::uint32_t& start, std::uint64_t& endExclusive,
		std::int64_t optionStart, std::int64_t optionEnd, const char *what)
{
	if (optionStart < 0 && optionEnd < 0)
		return;
	if (optionStart < 0 || optionEnd < 0 || optionStart > 0xffffffffll
			|| optionEnd > 0xffffffffll || optionEnd < optionStart)
		throw FlycastException(std::string("research.Workbench") + what
				+ "Start/End must form an inclusive 32-bit range");
	enabled = true;
	start = static_cast<std::uint32_t>(optionStart);
	endExclusive = static_cast<std::uint64_t>(optionEnd) + 1;
}

} // namespace

RecorderConfig recorderConfigFromOptions()
{
	RecorderConfig config = defaultRecorderConfig();
	try
	{
		if (!config::ResearchWorkbenchBuses.get().empty())
			config.buses = parseBusList(config::ResearchWorkbenchBuses.get());
		if (!config::ResearchWorkbenchSh4Types.get().empty())
			config.sh4.typeMask = parseSh4TypeList(config::ResearchWorkbenchSh4Types.get());
	}
	catch (const std::invalid_argument& exception)
	{
		throw FlycastException(std::string("research.Workbench option: ") + exception.what());
	}
	applyRange(config.sh4.hasInstructionPcRange, config.sh4.instructionPcStart,
			config.sh4.instructionPcEndExclusive, config::ResearchWorkbenchSh4PcStart.get(),
			config::ResearchWorkbenchSh4PcEnd.get(), "Sh4Pc");
	applyRange(config.sh4.hasMemoryRange, config.sh4.memoryStart,
			config.sh4.memoryEndExclusive, config::ResearchWorkbenchSh4MemStart.get(),
			config::ResearchWorkbenchSh4MemEnd.get(), "Sh4Mem");
	config.rows.recordVramWrites = config::ResearchWorkbenchVramWrites.get();
	config.rows.recordSampleFrames = config::ResearchWorkbenchSampleFrames.get();
	config.rows.storeTextureBytes = config::ResearchWorkbenchTextureBytes.get();
	config.note = config::ResearchWorkbenchNote.get();
	return config;
}

RunInfo currentRunInfo()
{
	RunInfo run;
	run.gameId = settings.content.gameId;
	run.mediaPath = settings.content.path;
	run.flycastVersion = GIT_VERSION;
	run.cpuBackend = config::DynarecEnabled.get() ? "dynarec" : "interpreter";
	run.dynarecObservation = config::ResearchDynarecObservation.get();
	run.threadedRendering = config::ThreadedRendering.get();
	run.rtcSeed = config::ResearchDreamcastRtcSeed.get();
	return run;
}

void configureWorkbenchRuntime()
{
	launchConfigured = false;
	if (config::ResearchWorkbenchRecordPath.get().empty())
		return;
	const RecorderConfig config = recorderConfigFromOptions();
	if (config.buses == 0)
		throw FlycastException("research.WorkbenchBuses selects no bus");
	launchConfigured = true;
}

void startWorkbenchRuntime()
{
	if (!launchConfigured)
		return;
	std::string error;
	if (!workbenchRecordStart(optionPath(config::ResearchWorkbenchRecordPath.get()),
			recorderConfigFromOptions(), error))
		throw FlycastException("workbench recording failed to start: " + error);
	NOTICE_LOG(COMMON, "Workbench recording to %s",
			config::ResearchWorkbenchRecordPath.get().c_str());
}

void stopWorkbenchRuntime() noexcept
{
	std::string error;
	if (!workbenchRecordStop(error) && !error.empty())
		ERROR_LOG(COMMON, "Workbench recording did not finalize cleanly: %s", error.c_str());
}

bool workbenchRecordStart(const std::filesystem::path& database,
		const RecorderConfig& config, std::string& error)
{
	std::lock_guard<std::mutex> lock(controlMutex);
	try
	{
		recorder.start(database, config, currentRunInfo());
		return true;
	}
	catch (const std::exception& exception)
	{
		error = exception.what();
		return false;
	}
}

bool workbenchRecordStop(std::string& error)
{
	std::lock_guard<std::mutex> lock(controlMutex);
	if (!recorder.active())
		return false;
	try
	{
		recorder.stop();
		return true;
	}
	catch (const std::exception& exception)
	{
		error = exception.what();
		return false;
	}
}

RecorderStatus workbenchStatus()
{
	return recorder.status();
}

} // namespace research::workbench

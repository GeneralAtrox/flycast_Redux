#pragma once

// Emulator-side glue for the workbench recorder: launch-time configuration
// from research.* options, lifecycle calls from Emulator::loadGame/unloadGame,
// and the entry points the control endpoint uses at runtime.

#include "research/workbench/workbench_config.h"
#include "research/workbench/workbench_recorder.h"

#include <filesystem>
#include <string>

namespace research::workbench
{

#ifdef LIBRETRO

inline void configureWorkbenchRuntime() {}
inline void startWorkbenchRuntime() {}
inline void stopWorkbenchRuntime() noexcept {}

#else

// Validates research.Workbench* options. Throws FlycastException on bad input.
void configureWorkbenchRuntime();
// Starts a recording if research.WorkbenchRecord names a database.
void startWorkbenchRuntime();
// Finalizes any active recording. Errors are logged, never thrown.
void stopWorkbenchRuntime() noexcept;

// Runtime control. Thread-safe; callable from the control server thread.
// On failure they return false and fill `error`.
bool workbenchRecordStart(const std::filesystem::path& database,
		const RecorderConfig& config, std::string& error);
bool workbenchRecordStop(std::string& error);
RecorderStatus workbenchStatus();
RunInfo currentRunInfo();
// Parses the launch-time options into a config (also used by `status`).
RecorderConfig recorderConfigFromOptions();

#endif

} // namespace research::workbench

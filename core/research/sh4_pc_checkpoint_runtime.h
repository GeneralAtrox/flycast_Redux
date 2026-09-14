#pragma once

#include "research/sh4_observation.h"

#include <cstdint>
#include <functional>

namespace research
{

#ifdef LIBRETRO

inline void configureSh4PcCheckpointRuntime() {}
inline void startSh4PcCheckpointRuntime(std::function<void()>) {}
inline void stopSh4PcCheckpointRuntime() noexcept {}
inline void sh4PcCheckpointInstructionEnd(Sh4ObservationBackend,
		std::uint32_t) noexcept {}
inline bool sh4PcCheckpointRuntimeActive() noexcept { return false; }
inline bool sh4PcCheckpointRuntimeTriggered() noexcept { return false; }
inline std::uint32_t sh4PcCheckpointTarget() noexcept { return 0; }

#else

// Configures one launch-time, read-only instruction boundary. The callback is
// invoked once, after the configured top-level guest instruction has completed.
// It must request Flycast's ordinary stop/unload path; this runtime never
// finalizes an evidence writer directly.
void configureSh4PcCheckpointRuntime();
void startSh4PcCheckpointRuntime(std::function<void()> cleanExitCallback);
void stopSh4PcCheckpointRuntime() noexcept;
void sh4PcCheckpointInstructionEnd(Sh4ObservationBackend backend,
		std::uint32_t pc) noexcept;
bool sh4PcCheckpointRuntimeActive() noexcept;
bool sh4PcCheckpointRuntimeTriggered() noexcept;
std::uint32_t sh4PcCheckpointTarget() noexcept;

#endif

} // namespace research

#pragma once

#include "research/sh4_observation.h"

#include <cstdint>
#include <functional>

namespace research
{

#ifdef LIBRETRO

inline void configureSh4PcCheckpointRuntime() {}
inline void startSh4PcCheckpointRuntime(std::function<void()>) {}
inline void armSh4PcCheckpointRuntime(std::uint32_t, std::uint32_t, std::uint32_t,
		std::function<void()>) {}
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
// Runtime arming from the control endpoint: validates like the launch-time
// path, replaces any armed checkpoint, and invokes `callback` once when the
// top-level instruction at `pc` completes (optionally only while the U32 at
// gateAddress equals gateValue; gateAddress 0 disables the gate).
void armSh4PcCheckpointRuntime(std::uint32_t pc, std::uint32_t gateAddress,
		std::uint32_t gateValue, std::function<void()> callback);
void stopSh4PcCheckpointRuntime() noexcept;
void sh4PcCheckpointInstructionEnd(Sh4ObservationBackend backend,
		std::uint32_t pc) noexcept;
bool sh4PcCheckpointRuntimeActive() noexcept;
bool sh4PcCheckpointRuntimeTriggered() noexcept;
std::uint32_t sh4PcCheckpointTarget() noexcept;

#endif

} // namespace research

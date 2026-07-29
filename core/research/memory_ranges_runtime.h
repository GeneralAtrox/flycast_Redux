#pragma once

#include <cstdint>

namespace research
{

#ifdef LIBRETRO

inline void configureMemoryRangesRuntime() {}
inline void startMemoryRangesRuntime() {}
inline void stopMemoryRangesRuntime(bool) {}
inline void abortMemoryRangesRuntime() noexcept {}
inline void memoryRangesInstructionBoundary(std::uint32_t, std::uint64_t) {}
inline bool memoryRangesRuntimeActive() { return false; }

#else

void configureMemoryRangesRuntime();
void startMemoryRangesRuntime();
void stopMemoryRangesRuntime(bool clean);
void abortMemoryRangesRuntime() noexcept;
void memoryRangesInstructionBoundary(std::uint32_t executedPc, std::uint64_t tick);
bool memoryRangesRuntimeActive();

#endif

} // namespace research

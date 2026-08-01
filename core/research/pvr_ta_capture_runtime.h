#pragma once

namespace research
{

#ifdef LIBRETRO

inline void configurePvrTaCaptureRuntime() {}
inline void startPvrTaCaptureRuntime() {}
inline void stopPvrTaCaptureRuntime(bool) {}
inline void abortPvrTaCaptureRuntime() noexcept {}
inline bool pvrTaCaptureRuntimeActive() { return false; }

#else

void configurePvrTaCaptureRuntime();
void startPvrTaCaptureRuntime();
void stopPvrTaCaptureRuntime(bool clean);
void abortPvrTaCaptureRuntime() noexcept;
bool pvrTaCaptureRuntimeActive();

#endif

} // namespace research

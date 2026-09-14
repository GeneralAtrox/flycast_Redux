#pragma once

namespace research
{

#ifdef LIBRETRO

inline void configurePvrTaCaptureRuntime() {}
inline void startPvrTaCaptureRuntime() {}
inline void stopPvrTaCaptureRuntime(bool) {}
inline void abortPvrTaCaptureRuntime() noexcept {}
inline bool pvrTaCaptureRuntimeActive() { return false; }
inline bool pvrTaCaptureWindowComplete() { return false; }

#else

void configurePvrTaCaptureRuntime();
void startPvrTaCaptureRuntime();
void stopPvrTaCaptureRuntime(bool clean);
void abortPvrTaCaptureRuntime() noexcept;
bool pvrTaCaptureRuntimeActive();
bool pvrTaCaptureWindowComplete();

#endif

} // namespace research

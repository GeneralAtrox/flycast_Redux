#pragma once

namespace research
{
#ifdef LIBRETRO
inline void configureGdromCaptureRuntime() {}
inline void startGdromCaptureRuntime() {}
inline void stopGdromCaptureRuntime(bool) {}
inline void abortGdromCaptureRuntime() noexcept {}
inline bool gdromCaptureRuntimeActive() { return false; }
#else
void configureGdromCaptureRuntime();
void startGdromCaptureRuntime();
void stopGdromCaptureRuntime(bool clean);
void abortGdromCaptureRuntime() noexcept;
bool gdromCaptureRuntimeActive();
#endif
} // namespace research

#pragma once

namespace research
{
#ifdef LIBRETRO
inline void configureAicaCaptureRuntime() {}
inline void startAicaCaptureRuntime() {}
inline void stopAicaCaptureRuntime(bool) {}
inline void abortAicaCaptureRuntime() noexcept {}
inline bool aicaCaptureRuntimeActive() { return false; }
#else
void configureAicaCaptureRuntime();
void startAicaCaptureRuntime();
void stopAicaCaptureRuntime(bool clean);
void abortAicaCaptureRuntime() noexcept;
bool aicaCaptureRuntimeActive();
#endif
}

#pragma once

namespace research
{
#ifdef LIBRETRO
inline void configureCddaCaptureRuntime() {}
inline void startCddaCaptureRuntime() {}
inline void stopCddaCaptureRuntime(bool) {}
inline void abortCddaCaptureRuntime() noexcept {}
inline bool cddaCaptureRuntimeActive() { return false; }
#else
void configureCddaCaptureRuntime();
void startCddaCaptureRuntime();
void stopCddaCaptureRuntime(bool clean);
void abortCddaCaptureRuntime() noexcept;
bool cddaCaptureRuntimeActive();
#endif
}

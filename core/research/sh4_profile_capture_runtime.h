#pragma once

namespace research
{

#ifdef LIBRETRO
inline void configureSh4DynarecProfileCaptureRuntime() {}
inline void startSh4DynarecProfileCaptureRuntime() {}
inline void stopSh4DynarecProfileCaptureRuntime(bool) {}
inline void abortSh4DynarecProfileCaptureRuntime() noexcept {}
inline bool sh4DynarecProfileCaptureRuntimeActive() { return false; }
#else
void configureSh4DynarecProfileCaptureRuntime();
void startSh4DynarecProfileCaptureRuntime();
void stopSh4DynarecProfileCaptureRuntime(bool clean);
void abortSh4DynarecProfileCaptureRuntime() noexcept;
bool sh4DynarecProfileCaptureRuntimeActive();
#endif

} // namespace research

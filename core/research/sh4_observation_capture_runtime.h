#pragma once

namespace research
{

#ifdef LIBRETRO

inline void configureSh4ObservationCaptureRuntime() {}
inline void startSh4ObservationCaptureRuntime() {}
inline void stopSh4ObservationCaptureRuntime(bool) {}
inline void abortSh4ObservationCaptureRuntime() noexcept {}
inline bool sh4ObservationCaptureRuntimeActive() { return false; }

#else

void configureSh4ObservationCaptureRuntime();
void startSh4ObservationCaptureRuntime();
void stopSh4ObservationCaptureRuntime(bool clean);
void abortSh4ObservationCaptureRuntime() noexcept;
bool sh4ObservationCaptureRuntimeActive();

#endif

} // namespace research

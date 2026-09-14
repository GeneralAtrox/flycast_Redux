#pragma once

// Lifecycle of the research control endpoint: a loopback TCP JSON-lines
// server (see research/control/control_protocol.h) enabled by
// research.ControlPort and optionally authenticated by research.ControlToken.

#include <functional>

namespace research
{

#ifdef LIBRETRO
inline void configureResearchControl() {}
inline void startResearchControl(std::function<void()>) {}
inline void stopResearchControl() noexcept {}
inline void pollResearchControl() noexcept {}
#else
// Validates the options. Throws FlycastException on bad input.
void configureResearchControl();
// Starts the server if a port is configured. The callback is kept for API
// compatibility with the emulator lifecycle; "exit" requests the frontend's
// ordinary shutdown path directly.
void startResearchControl(std::function<void()> cleanExitCallback);
void stopResearchControl() noexcept;
// UI thread, once per frame: runs lifecycle work requested by control clients.
void pollResearchControl() noexcept;
#endif

} // namespace research

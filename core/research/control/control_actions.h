#pragma once

// Binds the control protocol to the running emulator.

#include "research/control/control_protocol.h"
#include "research/control/control_ui_tasks.h"

namespace research::control
{

// Builds the action set. Lifecycle calls (pause, resume, save/load state,
// exit) are marshalled onto the UI thread through `uiTasks`; recorder and
// checkpoint calls are thread-safe and run inline; memory and register reads
// run inline and are documented as unsynchronized while the emulator runs.
ControlActions makeEmulatorActions(UiTaskQueue& uiTasks);

// EventManager listeners that keep the "game loaded" flag current. Call from
// the UI thread around the server's lifetime.
void installLifecycleListeners();
void removeLifecycleListeners() noexcept;

} // namespace research::control

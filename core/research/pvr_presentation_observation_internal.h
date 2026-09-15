#pragma once

// Private PowerVR presentation observation bus state shared between
// pvr_presentation_observation.cpp (subscription and publish skeleton) and
// pvr_presentation_observation_events.cpp (scope guards and observe*/reset*
// emitters). Every definition lives in pvr_presentation_observation.cpp.

#include "research/pvr_presentation_observation.h"

#include <atomic>
#include <cstdint>

namespace research
{
namespace detail_pvr_presentation
{

extern std::atomic<std::uint64_t> nextFramebufferGeneration;
extern std::atomic<std::uint64_t> nextPresentationGeneration;

// Ambient render/VRAM attribution carried by the scope guards.
extern thread_local std::uint64_t activeRenderGeneration;
extern thread_local PvrRenderKind activeRenderKind;
extern thread_local PvrVramWriteSource activeVramWriteSource;

void noteDroppedObservation() noexcept;
void publish(PvrPresentationObservation observation) noexcept;

} // namespace detail_pvr_presentation
} // namespace research

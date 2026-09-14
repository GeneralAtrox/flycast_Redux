#pragma once

#include "research/sh4_events_artifact.h"

#include <cstdint>
#include <functional>

struct Sh4Context;

namespace research
{

#ifdef LIBRETRO

inline void configureSh4EventsRuntime() {}
inline void startSh4EventsRuntime(std::function<void()> = {}) {}
inline void sh4EventsInitialStateLoaded() {}
inline void stopSh4EventsRuntime(bool) {}
inline void abortSh4EventsRuntime() noexcept {}
inline void sh4EventsInstructionBegin(std::uint32_t, std::uint16_t, std::uint64_t,
		const Sh4Context&) {}
inline void sh4EventsInstructionEnd(std::uint32_t, std::uint16_t, std::uint64_t,
		const Sh4Context&) {}
inline void sh4EventsInstructionAbort() noexcept {}
inline void sh4EventsMemoryAccess(std::uint32_t, std::uint8_t, Sh4MemoryAccessKind,
		std::uint64_t) {}
inline void sh4EventsException(std::uint32_t, std::uint32_t, std::uint32_t,
		std::uint64_t, const Sh4Context&) {}
inline bool sh4EventsRuntimeActive() { return false; }

#else

void configureSh4EventsRuntime();
void startSh4EventsRuntime(std::function<void()> completion = {});
void sh4EventsInitialStateLoaded();
void stopSh4EventsRuntime(bool clean);
void abortSh4EventsRuntime() noexcept;
void sh4EventsInstructionBegin(std::uint32_t pc, std::uint16_t opcode,
		std::uint64_t tick, const Sh4Context& context);
void sh4EventsInstructionEnd(std::uint32_t pc, std::uint16_t opcode,
		std::uint64_t tick, const Sh4Context& context);
void sh4EventsInstructionAbort() noexcept;
void sh4EventsMemoryAccess(std::uint32_t address, std::uint8_t width,
		Sh4MemoryAccessKind kind, std::uint64_t value);
void sh4EventsException(std::uint32_t exceptionPc, std::uint32_t vectorPc,
		std::uint32_t exceptionCode, std::uint64_t tick, const Sh4Context& context);
bool sh4EventsRuntimeActive();

#endif

} // namespace research

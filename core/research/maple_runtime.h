#pragma once

#include "research/maple_trace.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace research
{

#ifdef LIBRETRO

inline void configureRuntime() {}
inline void startRuntime() {}
inline void stopRuntime(bool) {}
inline void abortRuntime() noexcept {}

inline bool runtimeActive() { return false; }
inline bool mapleRecording() { return false; }
inline bool mapleReplaying() { return false; }

inline std::uint64_t mapleBeginDma(MapleDmaBeginEvent) { return UINT64_MAX; }
inline std::vector<std::uint8_t> mapleTransaction(MapleTransactionEvent event)
{
	return std::move(event.response);
}
inline void mapleScheduleDma(MapleDmaScheduleEvent) {}
inline void mapleCommitDma(MapleDmaCommitEvent) {}
inline void mapleAbortDma(MapleDmaAbortEvent) {}

#else

void configureRuntime();
void startRuntime();
void stopRuntime(bool clean);
void abortRuntime() noexcept;

bool runtimeActive();
bool mapleRecording();
bool mapleReplaying();

std::uint64_t mapleBeginDma(MapleDmaBeginEvent event);
std::vector<std::uint8_t> mapleTransaction(MapleTransactionEvent event);
void mapleScheduleDma(MapleDmaScheduleEvent event);
void mapleCommitDma(MapleDmaCommitEvent event);
void mapleAbortDma(MapleDmaAbortEvent event);

#endif

} // namespace research

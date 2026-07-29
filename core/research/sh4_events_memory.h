#pragma once

#include "hw/sh4/sh4_mem.h"
#include "research/sh4_events_runtime.h"

namespace research
{

inline u8 observedSh4Read8(u32 address)
{
	const u8 value = ::ReadMem8(address);
	sh4EventsMemoryAccess(address, 1, Sh4MemoryAccessKind::Read, value);
	return value;
}

inline u16 observedSh4Read16(u32 address)
{
	const u16 value = ::ReadMem16(address);
	sh4EventsMemoryAccess(address, 2, Sh4MemoryAccessKind::Read, value);
	return value;
}

inline u32 observedSh4Read32(u32 address)
{
	const u32 value = ::ReadMem32(address);
	sh4EventsMemoryAccess(address, 4, Sh4MemoryAccessKind::Read, value);
	return value;
}

inline u64 observedSh4Read64(u32 address)
{
	const u64 value = ::ReadMem64(address);
	sh4EventsMemoryAccess(address, 8, Sh4MemoryAccessKind::Read, value);
	return value;
}

inline void observedSh4Write8(u32 address, u8 value)
{
	::WriteMem8(address, value);
	sh4EventsMemoryAccess(address, 1, Sh4MemoryAccessKind::Write, value);
}

inline void observedSh4Write16(u32 address, u16 value)
{
	::WriteMem16(address, value);
	sh4EventsMemoryAccess(address, 2, Sh4MemoryAccessKind::Write, value);
}

inline void observedSh4Write32(u32 address, u32 value)
{
	::WriteMem32(address, value);
	sh4EventsMemoryAccess(address, 4, Sh4MemoryAccessKind::Write, value);
}

inline void observedSh4Write64(u32 address, u64 value)
{
	::WriteMem64(address, value);
	sh4EventsMemoryAccess(address, 8, Sh4MemoryAccessKind::Write, value);
}

} // namespace research

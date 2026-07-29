#include "cfg/option.h"
#include "log/Log.h"
#include "ResearchRuntimeStubs.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <limits>

namespace config
{

Option<bool> DynarecEnabled("Dynarec.Enabled", true);
Option<bool> ThreadedRendering("rend.ThreadedRendering", true);
Option<bool> AutoLoadState("Dreamcast.AutoLoadState");
Option<bool> AutoSaveState("Dreamcast.AutoSaveState");
Option<bool> GGPOEnable("GGPO", false, "network");
Option<std::string, false> ResearchIdentityManifestPath("IdentityManifest", "", "research");
Option<std::string, false> ResearchMapleRecordPath("MapleRecord", "", "research");
Option<std::string, false> ResearchMapleReplayPath("MapleReplay", "", "research");
Option<int64_t, false> ResearchMapleTraceMaxBytes("MapleTraceMaxBytes", 512_MB, "research");
Option<std::string, false> ResearchMemoryRangesManifestPath("MemoryRangesManifest", "", "research");
Option<std::string, false> ResearchMemoryRangesRecordPath("MemoryRangesRecord", "", "research");
Option<int64_t, false> ResearchMemoryRangesMaxBytes("MemoryRangesMaxBytes",
		1025ll * 1024 * 1024, "research");

bool open() { return true; }
int loadInt(const std::string&, const std::string&, int value) { return value; }
void saveInt(const std::string&, const std::string&, int) {}
int64_t loadInt64(const std::string&, const std::string&, int64_t value) { return value; }
void saveInt64(const std::string&, const std::string&, int64_t) {}
std::string loadStr(const std::string&, const std::string&, const std::string& value)
{
	return value;
}
void saveStr(const std::string&, const std::string&, const std::string&) {}
bool loadBool(const std::string&, const std::string&, bool value) { return value; }
void saveBool(const std::string&, const std::string&, bool) {}
float loadFloat(const std::string&, const std::string&, float value) { return value; }
void saveFloat(const std::string&, const std::string&, float) {}
void setTransient(const std::string&, const std::string&, const std::string&) {}
bool isTransient(const std::string&, const std::string&) { return true; }
void setAutoSave(bool) {}
bool hasSection(const std::string&) { return false; }
void deleteSection(const std::string&) {}
void deleteEntry(const std::string&, const std::string&) {}
std::vector<std::string> getEntries(const std::string&) { return {}; }

} // namespace config

void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char *, int, const char *, ...)
{
}

namespace
{

constexpr std::uint32_t TestRamSize = 16 * 1024 * 1024;
constexpr std::uint32_t TestRamMask = TestRamSize - 1;
std::array<std::uint8_t, TestRamSize> guestRam {};

} // namespace

u8* GetMemPtr(u32 address, u32 size)
{
	if (((address >> 29) & 7) == 7 || ((address >> 26) & 7) != 3)
		return nullptr;
	const std::uint32_t offset = address & TestRamMask;
	if (offset > TestRamSize || size > TestRamSize - offset)
		return nullptr;
	return guestRam.data() + offset;
}

namespace research_test
{

void clearGuestRam()
{
	guestRam.fill(0);
}

bool writeGuestRam(std::uint32_t address, const std::vector<std::uint8_t>& bytes)
{
	if (bytes.size() > std::numeric_limits<std::uint32_t>::max())
		return false;
	std::uint8_t *destination = GetMemPtr(address, static_cast<std::uint32_t>(bytes.size()));
	if (destination == nullptr)
		return false;
	std::copy(bytes.begin(), bytes.end(), destination);
	return true;
}

} // namespace research_test

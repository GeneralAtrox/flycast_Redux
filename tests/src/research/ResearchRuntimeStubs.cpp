#include "cfg/option.h"
#include "log/Log.h"

#include <cstdarg>

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

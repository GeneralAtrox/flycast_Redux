#include "cfg/option.h"
#include "hw/sh4/sh4_cycles.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "log/Log.h"
#include "ResearchRuntimeStubs.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <limits>
#include <stdexcept>

namespace
{
Sh4RCB researchTestSh4Rcb {};
constexpr std::size_t DreamcastBiosBytes = 2 * 1024 * 1024;
constexpr std::size_t DreamcastFlashBytes = 128 * 1024;
}

Sh4RCB *p_sh4rcb = &researchTestSh4Rcb;

namespace nvmem
{
namespace
{
std::uint8_t bios[DreamcastBiosBytes] {};
std::uint8_t flash[DreamcastFlashBytes] {};
std::uint8_t initialFlash[DreamcastFlashBytes] {};
}
std::uint8_t *getBiosData() { return bios; }
std::uint8_t *getFlashData() { return flash; }
const std::uint8_t *getInitialFlashData() { return initialFlash; }
size_t getInitialFlashSize() { return sizeof(initialFlash); }
}

namespace config
{

Option<bool> DynarecEnabled("Dynarec.Enabled", true);
Option<bool> UseReios("Dreamcast.UseReios", true);
Option<bool> ThreadedRendering("rend.ThreadedRendering", true);
Option<bool> AutoLoadState("Dreamcast.AutoLoadState");
Option<int, false> SavestateSlot("Dreamcast.SavestateSlot");
Option<bool> AutoSaveState("Dreamcast.AutoSaveState");
Option<bool> GGPOEnable("GGPO", false, "network");
RendererOption RendererType;
Option<bool> TranslucentPolygonDepthMask("rend.TranslucentPolygonDepthMask");
Option<bool> ModifierVolumes("rend.ModifierVolumes", true);
Option<bool> PerStripSorting("rend.PerStripSorting");
Option<int> RenderResolution("rend.Resolution", 480);
Option<bool> EmulateFramebuffer("rend.EmulateFramebuffer", false);
Option<bool> FixUpscaleBleedingEdge("rend.FixUpscaleBleedingEdge", true);
Option<std::string, false> ResearchMapleRecordPath("MapleRecord", "", "research");
Option<std::string, false> ResearchMapleReplayPath("MapleReplay", "", "research");
Option<int64_t, false> ResearchMapleTraceMaxBytes("MapleTraceMaxBytes", 512_MB, "research");
Option<int64_t, false> ResearchMapleDmaCheckpoint("MapleDmaCheckpoint", 0, "research");
Option<bool, false> ResearchDynarecObservation("DynarecObservation", false,
		"research");
Option<int64_t, false> ResearchSh4PcCheckpoint(
		"Sh4PcCheckpoint", 0, "research");
Option<int64_t, false> ResearchSh4PcCheckpointU32Address(
		"Sh4PcCheckpointU32Address", 0, "research");
Option<int64_t, false> ResearchSh4PcCheckpointU32Value(
		"Sh4PcCheckpointU32Value", 0, "research");
Option<int64_t, false> ResearchDreamcastRtcSeed(
		"DreamcastRtcSeed", -1, "research");
Option<std::string, false> ResearchWorkbenchRecordPath("WorkbenchRecord", "", "research");
Option<std::string, false> ResearchWorkbenchBuses("WorkbenchBuses", "", "research");
Option<std::string, false> ResearchWorkbenchSh4Types("WorkbenchSh4Types", "", "research");
Option<int64_t, false> ResearchWorkbenchSh4PcStart("WorkbenchSh4PcStart", -1, "research");
Option<int64_t, false> ResearchWorkbenchSh4PcEnd("WorkbenchSh4PcEnd", -1, "research");
Option<int64_t, false> ResearchWorkbenchSh4MemStart("WorkbenchSh4MemStart", -1, "research");
Option<int64_t, false> ResearchWorkbenchSh4MemEnd("WorkbenchSh4MemEnd", -1, "research");
Option<bool, false> ResearchWorkbenchVramWrites("WorkbenchVramWrites", false, "research");
Option<bool, false> ResearchWorkbenchSampleFrames("WorkbenchSampleFrames", false, "research");
Option<bool, false> ResearchWorkbenchTextureBytes("WorkbenchTextureBytes", false, "research");
Option<std::string, false> ResearchWorkbenchNote("WorkbenchNote", "", "research");

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

namespace hostfs
{

std::string getSavestatePath(int index, bool)
{
	return "research-test-state-" + std::to_string(index) + ".state";
}

} // namespace hostfs

u64 sh4_sched_now64()
{
	return 1000;
}

void GenericLog(LogTypes::LOG_LEVELS, LogTypes::LOG_TYPE, const char *, int, const char *, ...)
{
}

// The standalone research tests link the observation runtime without the full
// SH-4 decoder. Precise dynarec marker timing is exercised by the real-emulator
// differential test; these definitions only keep the synthetic runtime target
// self-contained and safe if a marker is invoked accidentally.
namespace
{

sh4_opcodelistentry researchTestOpcode {
		nullptr, nullptr, 0, 0, Normal, "research-test", 1, 1, CO, 0, 0};

struct ResearchOpcodeTableInitializer
{
	ResearchOpcodeTableInitializer()
	{
		std::fill(std::begin(OpDesc), std::end(OpDesc), &researchTestOpcode);
	}
};

ResearchOpcodeTableInitializer researchOpcodeTableInitializer;

} // namespace

sh4_opcodelistentry* OpDesc[0x10000];

int Sh4Cycles::countCycles(u16)
{
	return 1;
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

void setInitialFlashData(const std::vector<std::uint8_t>& bytes)
{
	if (bytes.size() != DreamcastFlashBytes)
		throw std::invalid_argument("initial Dreamcast flash test bytes have the wrong size");
	std::copy(bytes.begin(), bytes.end(), nvmem::initialFlash);
}

} // namespace research_test

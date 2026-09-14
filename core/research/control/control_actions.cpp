#include "research/control/control_actions.h"

#include "cfg/option.h"
#include "emulator.h"
#include "hw/mem/addrspace.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "research/aica_observation.h"
#include "research/gdrom_hardware_observation.h"
#include "research/gdrom_observation.h"
#include "research/maple_observation.h"
#include "research/maple_runtime.h"
#include "research/pvr_draw_observation.h"
#include "research/pvr_presentation_observation.h"
#include "research/pvr_ta_observation.h"
#include "research/sh4_observation.h"
#include "research/sh4_pc_checkpoint_runtime.h"
#include "research/workbench/workbench_runtime.h"
#include "types.h"
#include "ui/mainui.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>

namespace research::control
{
namespace
{

using namespace std::chrono_literals;

std::atomic<bool> gameLoaded {false};

void lifecycleEvent(Event event, void *)
{
	if (event == Event::Start)
		gameLoaded.store(true, std::memory_order_release);
	else if (event == Event::Terminate)
		gameLoaded.store(false, std::memory_order_release);
}

json recorderStatusJson()
{
	const workbench::RecorderStatus status = workbench::workbenchStatus();
	return json {
		{"active", status.active}, {"path", status.path}, {"run_id", status.runId},
		{"written", status.written}, {"queued", status.queued},
		{"dropped", status.dropped}, {"error", status.error},
	};
}

json statusJson()
{
	const bool loaded = gameLoaded.load(std::memory_order_acquire);
	const bool running = emu.running();
	json status {
		{"game_loaded", loaded},
		{"running", running},
		{"paused", loaded && !running},
		{"game_id", settings.content.gameId},
		{"media_path", settings.content.path},
		{"cpu_backend", config::DynarecEnabled.get() ? "dynarec" : "interpreter"},
		{"dynarec_observation", config::ResearchDynarecObservation.get()},
		{"threaded_rendering", config::ThreadedRendering.get()},
		{"rtc_seed", config::ResearchDreamcastRtcSeed.get()},
		{"maple_recording", mapleRecording()},
		{"maple_replaying", mapleReplaying()},
		{"recorder", recorderStatusJson()},
		{"checkpoint", {
			{"active", sh4PcCheckpointRuntimeActive()},
			{"target_pc", sh4PcCheckpointTarget()},
			{"triggered", sh4PcCheckpointRuntimeTriggered()},
		}},
		{"buses", {
			{"sh4_subscribers", sh4ObservationSubscriberCount()},
			{"maple_subscribers", mapleObservationSubscriberCount()},
			{"pvr_ta_dropped", pvrTaObservationDroppedCount()},
			{"pvr_draw_dropped", pvrDrawObservationDroppedCount()},
			{"pvr_present_dropped", pvrPresentationObservationDroppedCount()},
			{"gdrom_dropped", gdromObservationDroppedCount()},
			{"gdrom_hw_dropped", gdromHardwareObservationDroppedCount()},
			{"aica_dropped", aicaObservationDroppedCount()},
		}},
	};
	if (loaded && p_sh4rcb != nullptr)
	{
		status["tick"] = sh4_sched_now64();
		status["pc"] = Sh4cntx.pc;
	}
	return status;
}

std::vector<std::uint8_t> readGuestMemory(std::uint32_t address, std::uint32_t length)
{
	std::vector<std::uint8_t> bytes;
	if (length == 0)
		return bytes;
	if (!gameLoaded.load(std::memory_order_acquire))
		throw std::runtime_error("no game is loaded");
	void *ramBase = nullptr;
	void *ram = nullptr;
	void *vram = nullptr;
	void *aica = nullptr;
	addrspace::getAddress(&ramBase, &ram, &vram, &aica);
	const std::uint32_t physical = address & 0x1fffffffu;
	const std::uint8_t *source = nullptr;
	std::uint32_t regionSize = 0;
	std::uint32_t offset = 0;
	switch (physical >> 26)
	{
	case 3: // system RAM, mirrored through the whole area
		source = static_cast<const std::uint8_t *>(ram);
		regionSize = RAM_SIZE;
		offset = physical & (RAM_SIZE - 1);
		break;
	case 1: // 64-bit VRAM path at 0x04000000 (0x05000000 is the 32-bit interleaved view)
		if ((physical & 0x01000000u) == 0)
		{
			source = static_cast<const std::uint8_t *>(vram);
			regionSize = VRAM_SIZE;
			offset = physical & (VRAM_SIZE - 1);
		}
		break;
	case 0: // AICA wave RAM at 0x00800000
		if (physical >= 0x00800000u && physical < 0x01000000u)
		{
			source = static_cast<const std::uint8_t *>(aica);
			regionSize = ARAM_SIZE;
			offset = (physical - 0x00800000u) & (ARAM_SIZE - 1);
		}
		break;
	default:
		break;
	}
	if (source == nullptr)
		throw std::invalid_argument(
				"only system RAM (0x8c000000), VRAM (0xa4000000), and AICA RAM (0x00800000) are readable");
	if (offset + length > regionSize)
		throw std::invalid_argument("read crosses the end of the memory region");
	bytes.resize(length);
	std::memcpy(bytes.data(), source + offset, length);
	return bytes;
}

json registersJson()
{
	if (!gameLoaded.load(std::memory_order_acquire) || p_sh4rcb == nullptr)
		throw std::runtime_error("no game is loaded");
	const Sh4Context& context = Sh4cntx;
	json r = json::array();
	for (int i = 0; i < 16; ++i)
		r.push_back(context.r[i]);
	json rBank = json::array();
	for (int i = 0; i < 8; ++i)
		rBank.push_back(context.r_bank[i]);
	json fr = json::array();
	for (int i = 0; i < 16; ++i)
	{
		std::uint32_t bits;
		std::memcpy(&bits, &context.fr[i], sizeof(bits));
		fr.push_back(bits);
	}
	return json {
		{"tick", sh4_sched_now64()},
		{"pc", context.pc}, {"pr", context.pr}, {"gbr", context.gbr}, {"vbr", context.vbr},
		{"ssr", context.ssr}, {"spc", context.spc}, {"sgr", context.sgr}, {"dbr", context.dbr},
		{"mach", context.mac.h}, {"macl", context.mac.l},
		{"sr", context.sr.getFull()}, {"fpul", context.fpul}, {"fpscr", context.fpscr.full},
		{"r", r}, {"r_bank", rBank}, {"fr_bits", fr},
	};
}

void withEmulatorPaused(const std::function<void()>& action)
{
	const bool wasRunning = emu.running();
	if (wasRunning)
		emu.stop();
	try
	{
		action();
	}
	catch (...)
	{
		if (wasRunning)
			emu.start();
		throw;
	}
	if (wasRunning)
		emu.start();
}

} // namespace

void installLifecycleListeners()
{
	gameLoaded.store(false, std::memory_order_release);
	EventManager::listen(Event::Start, lifecycleEvent);
	EventManager::listen(Event::Terminate, lifecycleEvent);
}

void removeLifecycleListeners() noexcept
{
	EventManager::unlisten(Event::Start, lifecycleEvent);
	EventManager::unlisten(Event::Terminate, lifecycleEvent);
	gameLoaded.store(false, std::memory_order_release);
}

ControlActions makeEmulatorActions(UiTaskQueue& uiTasks)
{
	ControlActions actions;
	actions.status = statusJson;
	actions.pause = [&uiTasks] {
		uiTasks.run([] { if (emu.running()) emu.stop(); }, 5s);
	};
	actions.resume = [&uiTasks] {
		uiTasks.run([] {
			if (!gameLoaded.load(std::memory_order_acquire))
				throw std::runtime_error("no game is loaded");
			if (!emu.running())
				emu.start();
		}, 5s);
	};
	actions.saveState = [&uiTasks](int slot) {
		uiTasks.run([slot] {
			if (!dc_savestateAllowed())
				throw std::runtime_error("save states are not allowed for this content");
			withEmulatorPaused([slot] { dc_savestate(slot); });
		}, 30s);
	};
	actions.loadState = [&uiTasks](int slot) {
		uiTasks.run([slot] {
			if (!gameLoaded.load(std::memory_order_acquire))
				throw std::runtime_error("no game is loaded");
			withEmulatorPaused([slot] { dc_loadstate(slot); });
		}, 30s);
	};
	actions.readMemory = readGuestMemory;
	actions.readRegisters = registersJson;
	actions.recordStart = [](const std::string& path, const json& configJson) {
		std::string error;
		const workbench::RecorderConfig config = configJson.empty()
				? workbench::defaultRecorderConfig() : workbench::recorderConfigFromJson(configJson);
		if (!workbench::workbenchRecordStart(std::filesystem::u8path(path), config, error))
			throw std::runtime_error(error);
	};
	actions.recordStop = [] {
		std::string error;
		if (!workbench::workbenchRecordStop(error))
			throw std::runtime_error(error.empty() ? "no recording is active" : error);
	};
	actions.recordStatus = recorderStatusJson;
	actions.checkpointSet = [](std::uint32_t pc, std::uint32_t gateAddress, std::uint32_t gateValue) {
		armSh4PcCheckpointRuntime(pc, gateAddress, gateValue, [] { emu.stop(); });
	};
	actions.checkpointClear = [] { stopSh4PcCheckpointRuntime(); };
	actions.requestExit = [] { mainui_stop(); };
	return actions;
}

} // namespace research::control

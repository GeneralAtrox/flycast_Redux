/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "lua.h"

#ifdef USE_LUA
#include <lua.hpp>
#include <LuaBridge/LuaBridge.h>
#include "ui/gui.h"
#include "ui/gui_util.h"
#include "ui/mainui.h"
#include "hw/mem/addrspace.h"
#include "cfg/option.h"
#include "emulator.h"
#include "input/gamepad_device.h"
#include "input/mouse.h"
#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_if.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_sched.h"
#include "research/maple_observation.h"
#include "research/sha256.h"
#include "research/sh4_lua_subscriptions.h"
#include "stdclass.h"
#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace lua
{
const char *CallbackTable = "flycast_callbacks";
static lua_State *L;
using namespace luabridge;

static std::recursive_mutex mutex;
using lock_guard = std::lock_guard<std::recursive_mutex>;

static std::unique_ptr<research::Sh4LuaSubscriptionQueue> researchSubscriptions;
static std::unordered_map<research::Sh4LuaSubscriptionQueue::Token, int>
		researchCallbackRefs;
static bool researchSubscriptionsAllowed;

static void clearResearchSubscriptions()
{
	if (researchSubscriptions != nullptr)
		researchSubscriptions->clear();
	if (L != nullptr)
	{
		for (const auto& callback : researchCallbackRefs)
			luaL_unref(L, LUA_REGISTRYINDEX, callback.second);
	}
	researchCallbackRefs.clear();
}

static void emuEventCallback(Event event, void *)
{
	if (L == nullptr)
		return;
	lock_guard lock(mutex);
	if (event == Event::Terminate)
	{
		researchSubscriptionsAllowed = false;
		clearResearchSubscriptions();
	}
	else if (event == Event::Start)
	{
		researchSubscriptionsAllowed = true;
	}
	if (settings.raHardcoreMode)
		return;
	try {
		LuaRef v = LuaRef::getGlobal(L, CallbackTable);
		if (!v.isTable())
			return;
		const char *key = nullptr;
		switch (event)
		{
		case Event::Start:
			key = "start";
			break;
		case Event::Resume:
			key = "resume";
			break;
		case Event::Pause:
			key = "pause";
			break;
		case Event::Terminate:
			key = "terminate";
			break;
		case Event::LoadState:
			key = "loadState";
			break;
		case Event::VBlank:
			key = "vblank";
			break;
		case Event::Network:
			key = "network";
			break;
		case Event::DiskChange:
			key = "diskChange";
			break;
		case Event::LocaleChange:
			key = "localeChange";
			break;
		}
		if (v[key].isFunction())
			v[key]();
	} catch (const LuaException& e) {
		WARN_LOG(COMMON, "Lua exception: %s", e.what());
	}
}

static void eventCallback(const char *tag)
{
	if (L == nullptr)
		return;
	lock_guard lock(mutex);
	try {
		LuaRef v = LuaRef::getGlobal(L, CallbackTable);
		if (v.isTable() && v[tag].isFunction())
			v[tag]();
	} catch (const LuaException& e) {
		WARN_LOG(COMMON, "Lua exception[%s]: %s", tag, e.what());
	}
}

void overlay()
{
	if (L != nullptr && researchSubscriptions != nullptr)
	{
		lock_guard lock(mutex);
		try
		{
			researchSubscriptions->drain();
		}
		catch (const std::exception& exception)
		{
			WARN_LOG(COMMON, "Lua research delivery failed: %s", exception.what());
		}
	}
	eventCallback("overlay");
}

template<typename T>
static LuaRef readMemoryTable(u32 address, int count, lua_State* L)
{
	LuaRef t(L);
	t = newTable(L);
	while (count > 0)
	{
		t[address] = addrspace::readt<T>(address);
		address += sizeof(T);
		count--;
	}

	return t;
}

#define CONFIG_ACCESSORS(Config) 	\
template<typename T>				\
static T get ## Config() {			\
	return config::Config.get();	\
}									\
template<typename T>				\
static void set ## Config(T v)		\
{									\
	config::Config.set(v);			\
}

// General
CONFIG_ACCESSORS(Cable);
CONFIG_ACCESSORS(Region);
CONFIG_ACCESSORS(Broadcast);
CONFIG_ACCESSORS(Language);
CONFIG_ACCESSORS(AutoLoadState);
CONFIG_ACCESSORS(AutoSaveState);
CONFIG_ACCESSORS(SavestateSlot);
// TODO Option<std::vector<std::string>, false> ContentPath;
CONFIG_ACCESSORS(HideLegacyNaomiRoms)

// Video
CONFIG_ACCESSORS(RendererType)
CONFIG_ACCESSORS(Widescreen)
CONFIG_ACCESSORS(UseMipmaps)
CONFIG_ACCESSORS(SuperWidescreen)
CONFIG_ACCESSORS(ShowFPS)
CONFIG_ACCESSORS(RenderToTextureBuffer)
CONFIG_ACCESSORS(TranslucentPolygonDepthMask)
CONFIG_ACCESSORS(ModifierVolumes)
CONFIG_ACCESSORS(TextureUpscale)
CONFIG_ACCESSORS(MaxFilteredTextureSize)
CONFIG_ACCESSORS(ExtraDepthScale)
CONFIG_ACCESSORS(CustomTextures)
CONFIG_ACCESSORS(DumpTextures)
CONFIG_ACCESSORS(ScreenStretching)
CONFIG_ACCESSORS(Fog)
CONFIG_ACCESSORS(FloatVMUs)
CONFIG_ACCESSORS(Rotate90)
CONFIG_ACCESSORS(PerStripSorting)
CONFIG_ACCESSORS(DelayFrameSwapping)
CONFIG_ACCESSORS(WidescreenGameHacks)
//TODO CrosshairColor;
CONFIG_ACCESSORS(SkipFrame)
CONFIG_ACCESSORS(MaxThreads)
CONFIG_ACCESSORS(AutoSkipFrame)
CONFIG_ACCESSORS(RenderResolution)
CONFIG_ACCESSORS(VSync)
CONFIG_ACCESSORS(PixelBufferSize)
CONFIG_ACCESSORS(AnisotropicFiltering)
CONFIG_ACCESSORS(TextureFiltering)
CONFIG_ACCESSORS(ThreadedRendering)

// Audio
CONFIG_ACCESSORS(DSPEnabled)
CONFIG_ACCESSORS(AudioBufferSize)
CONFIG_ACCESSORS(AutoLatency)
CONFIG_ACCESSORS(AudioBackend)
CONFIG_ACCESSORS(AudioVolume)

// Advanced
CONFIG_ACCESSORS(DynarecEnabled)
CONFIG_ACCESSORS(SerialConsole)
CONFIG_ACCESSORS(SerialPTY)
CONFIG_ACCESSORS(UseReios)
CONFIG_ACCESSORS(FastGDRomLoad)
CONFIG_ACCESSORS(OpenGlChecks)

// Network
CONFIG_ACCESSORS(NetworkEnable)
CONFIG_ACCESSORS(ActAsServer)
CONFIG_ACCESSORS(DNS)
CONFIG_ACCESSORS(NetworkServer)
CONFIG_ACCESSORS(EmulateBBA)
CONFIG_ACCESSORS(GGPOEnable)
CONFIG_ACCESSORS(GGPODelay)
CONFIG_ACCESSORS(NetworkStats)
CONFIG_ACCESSORS(GGPOAnalogAxes)

// Maple devices

static int getMapleType(int bus, lua_State *L)
{
	luaL_argcheck(L, bus >= 1 && bus <= 4, 1, "bus must be between 1 and 4");
	if (MapleDevices[bus - 1][5] == nullptr)
		return MDT_None;
	return MapleDevices[bus - 1][5]->get_device_type();
}

static int getMapleSubType(int bus, int port, lua_State *L)
{
	luaL_argcheck(L, bus >= 1 && bus <= 4, 1, "bus must be between 1 and 4");
	luaL_argcheck(L, port >= 1 && port <= 2, 2, "port must be between 1 and 2");
	if (MapleDevices[bus - 1][port - 1] == nullptr)
		return MDT_None;
	return MapleDevices[bus - 1][port - 1]->get_device_type();
}

static void setMapleType(int bus, int type, lua_State *L)
{
	luaL_argcheck(L, bus >= 1 && bus <= 4, 1, "bus must be between 1 and 4");
	switch ((MapleDeviceType)type) {
	case MDT_SegaController:
	case MDT_AsciiStick:
	case MDT_Keyboard:
	case MDT_Mouse:
	case MDT_LightGun:
	case MDT_TwinStick:
	case MDT_MaracasController:
	case MDT_FishingController:
	case MDT_PopnMusicController:
	case MDT_RacingController:
	case MDT_DenshaDeGoController:
	case MDT_SegaControllerXL:
	case MDT_DreamParaParaController:
	case MDT_None:
		config::MapleMainDevices[bus - 1] = (MapleDeviceType)type;
		maple_ReconnectDevices();
		break;
	default:
		luaL_argerror(L, 2, "Invalid device type");
		break;
	}
}

static void setMapleSubType(int bus, int port, int type, lua_State *L)
{
	luaL_argcheck(L, bus >= 1 && bus <= 4, 1, "bus must be between 1 and 4");
	luaL_argcheck(L, port >= 1 && port <= 2, 2, "port must be between 1 and 2");
	switch ((MapleDeviceType)type) {
	case MDT_SegaVMU:
	case MDT_PurupuruPack:
	case MDT_Microphone:
	case MDT_None:
		config::MapleExpansionDevices[bus - 1][port - 1] = (MapleDeviceType)type;
		maple_ReconnectDevices();
		break;
	default:
		luaL_argerror(L, 3, "Invalid device type");
		break;
	}
}

// Inputs

static void checkPlayerNum(lua_State *L, int player) {
	luaL_argcheck(L, player >= 1 && player <= 4, 1, "player must be between 1 and 4");
}

static u32 getButtons(int player, lua_State *L)
{
	checkPlayerNum(L, player);
	return kcode[player - 1];
}

static void pressButtons(int player, u32 buttons, lua_State *L)
{
	checkPlayerNum(L, player);
	kcode[player - 1] &= ~buttons;
}

static void releaseButtons(int player, u32 buttons, lua_State *L)
{
	checkPlayerNum(L, player);
	kcode[player - 1] |= buttons;
}

static int getAxis(int player, int axis, lua_State *L)
{
	checkPlayerNum(L, player);
	luaL_argcheck(L, axis >= 1 && axis <= 6, 2, "axis must be between 1 and 6");
	switch (axis - 1)
	{
	case 0:
		return joyx[player - 1];
	case 1:
		return joyy[player - 1];
	case 2:
		return joyrx[player - 1];
	case 3:
		return joyry[player - 1];
	case 4:
		return lt[player - 1];
	case 5:
		return rt[player - 1];
	default:
		return 0;
	}
}

static void setAxis(int player, int axis, int value, lua_State *L)
{
	checkPlayerNum(L, player);
	luaL_argcheck(L, axis >= 1 && axis <= 6, 2, "axis must be between 1 and 6");
	switch (axis - 1)
	{
	case 0:
		joyx[player - 1] = value;
		break;
	case 1:
		joyy[player - 1] = value;
		break;
	case 2:
		joyrx[player - 1] = value;
		break;
	case 3:
		joyry[player - 1] = value;
		break;
	case 4:
		lt[player - 1] = value;
		break;
	case 5:
		rt[player - 1] = value;
		break;
	default:
		break;
	}
}

static int getAbsCoordinates(lua_State *L)
{
	int player = luaL_checkinteger(L, 1);
	checkPlayerNum(L, player);
	lua_pushnumber(L, mo_x_abs[player - 1]);
	lua_pushnumber(L, mo_y_abs[player - 1]);
	return 2;
}

static void setAbsCoordinates(int player, int x, int y, lua_State *L)
{
	checkPlayerNum(L, player);
	SetMousePosition(x, y, settings.display.width, settings.display.height, player - 1);
}

static int getRelCoordinates(lua_State *L)
{
	int player = luaL_checkinteger(L, 1);
	checkPlayerNum(L, player);
	lua_pushnumber(L, mo_x_delta[player - 1]);
	lua_pushnumber(L, mo_y_delta[player - 1]);
	return 2;
}

static void setRelCoordinates(int player, float x, float y, lua_State *L)
{
	checkPlayerNum(L, player);
	SetRelativeMousePosition(x, y, player - 1);
}

// UI

static void beginWindow(const char *title, int x, int y, int w, int h)
{
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
	ImGui::SetNextWindowPos(ImVec2(x, y));
	ImGui::SetNextWindowSize(ScaledVec2(w, h));
	ImGui::SetNextWindowBgAlpha(0.7f);
	ImGui::Begin(title, NULL, ImGuiWindowFlags_AlwaysAutoResize |  ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoNavFocus);
	ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.557f, 0.268f, 0.965f, 1.f));
}

static void endWindow()
{
	ImGui::PopStyleColor();
	ImGui::End();
	ImGui::PopStyleVar(2);
}

static void uiText(const std::string& text)
{
	ImGui::Text("%s", text.c_str());
}

static void uiTextRightAligned(const std::string& text)
{
	ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(text.c_str()).x);
	uiText(text);
}

static void uiBargraph(float v)
{
	ImGui::ProgressBar(v, ImVec2(-1, uiScaled(10.f)), "");
}

static int uiButton(lua_State *L)
{
	const char *label = luaL_checkstring(L, 1);
	if (ImGui::Button(label))
	{
		LuaRef callback = LuaRef::fromStack(L, 2);
		if (callback.isFunction())
			callback();
	}
	return 0;
}

static const char *observationTypeName(research::Sh4ObservationType type)
{
	switch (type)
	{
	case research::Sh4ObservationType::InstructionBegin:
		return "instruction-begin";
	case research::Sh4ObservationType::InstructionEnd:
		return "instruction";
	case research::Sh4ObservationType::InstructionAbort:
		return "instruction-abort";
	case research::Sh4ObservationType::MemoryRead:
		return "memory-read";
	case research::Sh4ObservationType::MemoryWrite:
		return "memory-write";
	case research::Sh4ObservationType::Exception:
		return "exception";
	case research::Sh4ObservationType::Call:
		return "call";
	case research::Sh4ObservationType::Return:
		return "return";
	}
	return "unknown";
}

static const char *backendName(research::Sh4ObservationBackend backend)
{
	return backend == research::Sh4ObservationBackend::Interpreter
			? "interpreter" : "dynarec";
}

static void setStringField(lua_State *state, const char *name,
		const std::string& value)
{
	lua_pushlstring(state, value.data(), value.size());
	lua_setfield(state, -2, name);
}

static void setStringField(lua_State *state, const char *name, const char *value)
{
	lua_pushstring(state, value);
	lua_setfield(state, -2, name);
}

static void setNumberField(lua_State *state, const char *name, std::uint64_t value)
{
	lua_pushnumber(state, static_cast<lua_Number>(value));
	lua_setfield(state, -2, name);
}

static void setSignedNumberField(lua_State *state, const char *name,
		std::int64_t value)
{
	lua_pushnumber(state, static_cast<lua_Number>(value));
	lua_setfield(state, -2, name);
}

static void setFloatField(lua_State *state, const char *name, float value)
{
	lua_pushnumber(state, static_cast<lua_Number>(value));
	lua_setfield(state, -2, name);
}

static void setBooleanField(lua_State *state, const char *name, bool value)
{
	lua_pushboolean(state, value ? 1 : 0);
	lua_setfield(state, -2, name);
}

static std::string hexadecimal64(std::uint64_t value)
{
	char text[19] {};
	std::snprintf(text, sizeof(text), "0x%016llx",
			static_cast<unsigned long long>(value));
	return text;
}

static std::string hexadecimalBytes(const std::uint8_t *bytes, std::size_t size)
{
	static constexpr char Digits[] = "0123456789abcdef";
	std::string text(size * 2, '0');
	for (std::size_t index = 0; index < size; ++index)
	{
		text[index * 2] = Digits[bytes[index] >> 4];
		text[index * 2 + 1] = Digits[bytes[index] & 0x0f];
	}
	return text;
}

static std::string hexadecimalBytes(const std::vector<std::uint8_t>& bytes)
{
	return hexadecimalBytes(bytes.data(), bytes.size());
}

static void setBytesFields(lua_State *state, const char *bytesName,
		const char *hexName, const std::uint8_t *bytes, std::size_t size)
{
	const char *payload = size == 0 ? "" : reinterpret_cast<const char *>(bytes);
	lua_pushlstring(state, payload, size);
	lua_setfield(state, -2, bytesName);
	setStringField(state, hexName, hexadecimalBytes(bytes, size));
}

static void pushSh4Owner(lua_State *state,
		const research::Sh4InstructionOwnerToken& owner)
{
	lua_newtable(state);
	setBooleanField(state, "available", owner.valid);
	if (!owner.valid)
		return;
	setStringField(state, "backend", backendName(owner.backend));
	setNumberField(state, "generation", owner.generation);
	setStringField(state, "generation_decimal", std::to_string(owner.generation));
	setNumberField(state, "tick", owner.tick);
	setStringField(state, "tick_decimal", std::to_string(owner.tick));
	setNumberField(state, "pc", owner.pc);
	setNumberField(state, "pr", owner.pr);
	setNumberField(state, "opcode", owner.opcode);
	setNumberField(state, "delay_slot_depth", owner.delaySlotDepth);
}

static void pushRegisterSnapshot(lua_State *state,
		const research::Sh4RegisterSnapshot& registers)
{
	lua_newtable(state);
	lua_newtable(state);
	for (std::size_t index = 0; index < registers.r.size(); ++index)
	{
		lua_pushnumber(state, static_cast<lua_Number>(registers.r[index]));
		lua_rawseti(state, -2, static_cast<int>(index + 1));
	}
	lua_setfield(state, -2, "r");
	setNumberField(state, "pr", registers.pr);
	setNumberField(state, "gbr", registers.gbr);
	setNumberField(state, "vbr", registers.vbr);
	setNumberField(state, "mach", registers.mach);
	setNumberField(state, "macl", registers.macl);
	setNumberField(state, "sr", registers.sr);
	setNumberField(state, "fpul", registers.fpul);
	setNumberField(state, "fpscr", registers.fpscr);
}

static void pushResearchEvent(lua_State *state,
		const research::Sh4Observation& observation)
{
	lua_newtable(state);
	setBooleanField(state, "discovery", true);
	setBooleanField(state, "authoritative_evidence", false);
	setNumberField(state, "schema_version", observation.schemaVersion);
	setStringField(state, "event", observationTypeName(observation.type));
	setStringField(state, "backend", backendName(observation.backend));
	setNumberField(state, "ordinal", observation.emissionOrdinal);
	setStringField(state, "ordinal_decimal",
			std::to_string(observation.emissionOrdinal));
	setNumberField(state, "tick", observation.tick);
	setStringField(state, "tick_decimal", std::to_string(observation.tick));
	setNumberField(state, "pc", observation.instructionPc);
	setNumberField(state, "opcode", observation.opcode);
	setNumberField(state, "delay_slot_depth", observation.delaySlotDepth);
	if ((observation.availableFields & research::Sh4Observation::HasNextPc) != 0)
		setNumberField(state, "next_pc", observation.nextPc);
	if ((observation.availableFields & research::Sh4Observation::HasRegisters) != 0)
	{
		pushRegisterSnapshot(state, observation.registers);
		lua_setfield(state, -2, "registers");
	}
	if (observation.type == research::Sh4ObservationType::MemoryRead
			|| observation.type == research::Sh4ObservationType::MemoryWrite)
	{
		setNumberField(state, "address", observation.memoryAddress);
		setNumberField(state, "width", observation.memoryWidth);
		setNumberField(state, "value", observation.memoryValue);
		setStringField(state, "value_hex", hexadecimal64(observation.memoryValue));
	}
	if (observation.type == research::Sh4ObservationType::Exception)
	{
		setNumberField(state, "exception_pc", observation.exceptionPc);
		setNumberField(state, "vector_pc", observation.vectorPc);
		setNumberField(state, "exception_code", observation.exceptionCode);
	}
	if (observation.type == research::Sh4ObservationType::Call
			|| observation.type == research::Sh4ObservationType::Return)
	{
		if (observation.type == research::Sh4ObservationType::Call)
		{
			const char *kind = observation.callKind == research::Sh4CallKind::Bsr
					? "bsr" : observation.callKind == research::Sh4CallKind::Bsrf
							? "bsrf" : "jsr";
			setStringField(state, "call_kind", kind);
		}
		setNumberField(state, "target_pc", observation.targetPc);
		setNumberField(state, "return_pc", observation.returnPc);
		setNumberField(state, "delay_slot_pc", observation.delaySlotPc);
	}
}

static const char *mapleObservationTypeName(research::MapleObservationType type)
{
	return type == research::MapleObservationType::Request
			? "maple-request" : "maple-response";
}

static void pushMapleResearchEvent(lua_State *state,
		const research::MapleObservation& observation)
{
	lua_newtable(state);
	setBooleanField(state, "discovery", true);
	setBooleanField(state, "authoritative_evidence", false);
	setNumberField(state, "schema_version", observation.schemaVersion);
	setStringField(state, "event", mapleObservationTypeName(observation.type));
	setNumberField(state, "ordinal", observation.emissionOrdinal);
	setStringField(state, "ordinal_decimal",
			std::to_string(observation.emissionOrdinal));
	setNumberField(state, "tick", observation.tick);
	setStringField(state, "tick_decimal", std::to_string(observation.tick));
	setNumberField(state, "dma_ordinal", observation.dmaOrdinal);
	setStringField(state, "dma_ordinal_decimal",
			std::to_string(observation.dmaOrdinal));
	setNumberField(state, "transaction_ordinal", observation.transactionOrdinal);
	setStringField(state, "transaction_ordinal_decimal",
			std::to_string(observation.transactionOrdinal));
	setNumberField(state, "descriptor_address", observation.descriptorAddress);
	setNumberField(state, "destination_address", observation.destinationAddress);
	setNumberField(state, "descriptor_header_1", observation.descriptorHeader1);
	setNumberField(state, "descriptor_header_2", observation.descriptorHeader2);
	setNumberField(state, "bus", observation.bus);
	setNumberField(state, "port", observation.port);
	setNumberField(state, "command", observation.command);
	const bool devicePresent = (observation.flags
			& research::MapleTransactionDevicePresent) != 0;
	setBooleanField(state, "device_present", devicePresent);
	if (devicePresent)
		setNumberField(state, "device_type", observation.deviceType);
	setNumberField(state, "byte_count", observation.payload.size());
	const char *payload = observation.payload.empty() ? ""
			: reinterpret_cast<const char *>(observation.payload.data());
	lua_pushlstring(state, payload,
			observation.payload.size());
	lua_setfield(state, -2, "payload");
	setStringField(state, "payload_hex", hexadecimalBytes(observation.payload));
	if (!observation.payload.empty())
	{
		setNumberField(state, "frame_code", observation.payload[0]);
		if (observation.type == research::MapleObservationType::Response)
			setNumberField(state, "response_code", observation.payload[0]);
	}
}

static void pushDiscoveryBase(lua_State *state, const char *event,
		std::uint32_t schemaVersion, std::uint64_t ordinal, std::uint64_t tick)
{
	lua_newtable(state);
	setBooleanField(state, "discovery", true);
	setBooleanField(state, "authoritative_evidence", false);
	setNumberField(state, "schema_version", schemaVersion);
	setStringField(state, "event", event);
	setNumberField(state, "ordinal", ordinal);
	setStringField(state, "ordinal_decimal", std::to_string(ordinal));
	setNumberField(state, "tick", tick);
	setStringField(state, "tick_decimal", std::to_string(tick));
}

static const char *pvrTaEventName(research::PvrTaObservationType type)
{
	switch (type)
	{
	case research::PvrTaObservationType::ListInit: return "pvr-ta-list-init";
	case research::PvrTaObservationType::ListContinue: return "pvr-ta-list-continue";
	case research::PvrTaObservationType::AcceptedBlock: return "pvr-ta-block";
	case research::PvrTaObservationType::StartRender: return "pvr-start-render";
	case research::PvrTaObservationType::RenderDone: return "pvr-render-done";
	case research::PvrTaObservationType::Reset: return "pvr-ta-reset";
	}
	return "pvr-ta-unknown";
}

static void pushPvrTaResearchEvent(lua_State *state,
		const research::Sh4LuaSubscriptionQueue::PvrTaLuaObservation& queued)
{
	const research::PvrTaObservation& observation = queued.observation;
	pushDiscoveryBase(state, pvrTaEventName(observation.type),
			observation.schemaVersion, observation.emissionOrdinal, observation.tick);
	setStringField(state, "guest_snapshot_tick_decimal",
			std::to_string(queued.guestSnapshotTick));
	lua_newtable(state);
	for (std::size_t index = 0; index < queued.guestU32Snapshot.size(); ++index)
	{
		const auto& value = queued.guestU32Snapshot[index];
		lua_newtable(state);
		setNumberField(state, "address", value.address);
		setBooleanField(state, "available", value.available);
		if (value.available)
			setNumberField(state, "value", value.value);
		lua_rawseti(state, -2, static_cast<int>(index + 1));
	}
	lua_setfield(state, -2, "guest_u32_snapshot");
	pushSh4Owner(state, observation.initiator);
	lua_setfield(state, -2, "initiator");
	setNumberField(state, "context_address", observation.contextAddress);
	setNumberField(state, "context_generation", observation.contextGeneration);
	setNumberField(state, "context_block_ordinal", observation.contextBlockOrdinal);
	setNumberField(state, "render_pass", observation.renderPass);
	setNumberField(state, "list_type_before", observation.listTypeBefore);
	setNumberField(state, "list_type_after", observation.listTypeAfter);
	setNumberField(state, "parser_state_before", observation.parserStateBefore);
	setNumberField(state, "parser_state_after", observation.parserStateAfter);
	if (observation.type == research::PvrTaObservationType::AcceptedBlock)
	{
		const char *source = observation.source == research::PvrTaInputSource::StoreQueue
				? "store-queue" : observation.source
						== research::PvrTaInputSource::Channel2Dma
						? "channel2-dma" : "sort-dma";
		setStringField(state, "source", source);
		setNumberField(state, "source_address", observation.sourceAddress);
		setNumberField(state, "ta_address", observation.taAddress);
		setBytesFields(state, "block", "block_hex", observation.block.data(),
				observation.block.size());
	}
	if (observation.type == research::PvrTaObservationType::StartRender
			|| observation.type == research::PvrTaObservationType::RenderDone)
		setNumberField(state, "render_generation", observation.renderGeneration);
	if (observation.type == research::PvrTaObservationType::StartRender)
	{
		setBooleanField(state, "render_context_available",
				observation.renderContextAvailable);
		setNumberField(state, "region_base", observation.regionBase);
		setNumberField(state, "fpu_param_cfg", observation.fpuParamCfg);
		lua_newtable(state);
		for (std::size_t index = 0; index < observation.selectedContexts.size(); ++index)
		{
			const auto& context = observation.selectedContexts[index];
			lua_newtable(state);
			setNumberField(state, "address", context.address);
			setNumberField(state, "generation", context.generation);
			setBooleanField(state, "available", context.available);
			lua_rawseti(state, -2, static_cast<int>(index + 1));
		}
		lua_setfield(state, -2, "selected_contexts");
	}
}

static const char *pvrPresentationEventName(
		research::PvrPresentationObservationType type)
{
	switch (type)
	{
	case research::PvrPresentationObservationType::RegisterWrite: return "pvr-register-write";
	case research::PvrPresentationObservationType::VramWrite: return "pvr-vram-write";
	case research::PvrPresentationObservationType::RenderQueued: return "pvr-render-queued";
	case research::PvrPresentationObservationType::RenderCompleted: return "pvr-render-completed";
	case research::PvrPresentationObservationType::FramebufferCaptured: return "pvr-framebuffer";
	case research::PvrPresentationObservationType::Presentation: return "pvr-presentation";
	case research::PvrPresentationObservationType::Reset: return "pvr-presentation-reset";
	case research::PvrPresentationObservationType::InitialRegisterState:
		return "pvr-initial-register-state";
	}
	return "pvr-presentation-unknown";
}

static void pushPvrPresentationResearchEvent(lua_State *state,
		const research::Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation& queued)
{
	const research::PvrPresentationObservation& observation = queued.observation;
	pushDiscoveryBase(state, pvrPresentationEventName(observation.type),
			observation.schemaVersion, observation.emissionOrdinal, observation.tick);
	setStringField(state, "guest_snapshot_tick_decimal",
			std::to_string(queued.guestSnapshotTick));
	lua_newtable(state);
	for (std::size_t index = 0; index < queued.guestU32Snapshot.size(); ++index)
	{
		const auto& value = queued.guestU32Snapshot[index];
		lua_newtable(state);
		setNumberField(state, "address", value.address);
		setBooleanField(state, "available", value.available);
		if (value.available)
			setNumberField(state, "value", value.value);
		lua_rawseti(state, -2, static_cast<int>(index + 1));
	}
	lua_setfield(state, -2, "guest_u32_snapshot");
	setBooleanField(state, "guest_r15_available", queued.guestR15Available);
	if (queued.guestR15Available)
		setNumberField(state, "guest_r15", queued.guestR15);
	lua_newtable(state);
	for (std::size_t index = 0; index < queued.guestR15U32Snapshot.size(); ++index)
	{
		const auto& value = queued.guestR15U32Snapshot[index];
		lua_newtable(state);
		setNumberField(state, "address", value.address);
		setBooleanField(state, "available", value.available);
		if (value.available)
			setNumberField(state, "value", value.value);
		lua_rawseti(state, -2, static_cast<int>(index + 1));
	}
	lua_setfield(state, -2, "guest_r15_u32_snapshot");
	pushSh4Owner(state, observation.initiator);
	lua_setfield(state, -2, "initiator");
	setNumberField(state, "render_generation", observation.renderGeneration);
	setNumberField(state, "framebuffer_generation", observation.framebufferGeneration);
	setNumberField(state, "presentation_generation", observation.presentationGeneration);
	setNumberField(state, "source_generation", observation.sourceGeneration);
	setBooleanField(state, "successful", observation.successful);
	if (observation.type == research::PvrPresentationObservationType::RegisterWrite)
	{
		setNumberField(state, "physical_address", observation.registerPhysicalAddress);
		setNumberField(state, "register_address", observation.registerAddress);
		setNumberField(state, "requested_value", observation.requestedValue);
		setNumberField(state, "previous_value", observation.previousValue);
		setNumberField(state, "effective_value", observation.effectiveValue);
		setNumberField(state, "disposition", static_cast<unsigned>(observation.registerDisposition));
	}
	else if (observation.type == research::PvrPresentationObservationType::RenderQueued
			|| observation.type
					== research::PvrPresentationObservationType::RenderCompleted)
	{
		setNumberField(state, "render_kind",
				static_cast<unsigned>(observation.renderKind));
		setNumberField(state, "framebuffer_write_address",
				observation.framebufferWriteAddress);
	}
	else if (observation.type == research::PvrPresentationObservationType::VramWrite)
	{
		setNumberField(state, "source", static_cast<unsigned>(observation.vramSource));
		setNumberField(state, "logical_address", observation.logicalAddress);
		setNumberField(state, "physical_address", observation.physicalAddress);
		setBytesFields(state, "bytes", "bytes_hex", observation.bytes.data(),
				observation.bytes.size());
	}
	else if (observation.type
			== research::PvrPresentationObservationType::FramebufferCaptured)
	{
		setNumberField(state, "source_render_generation",
				observation.framebufferSourceRenderGeneration);
		setNumberField(state, "framebuffer_kind",
				static_cast<unsigned>(observation.framebufferKind));
		setNumberField(state, "width", observation.framebufferWidth);
		setNumberField(state, "height", observation.framebufferHeight);
		setNumberField(state, "row_bytes", observation.framebufferRowBytes);
		setNumberField(state, "fb_read_size",
				observation.framebufferConfig.fbReadSize);
		setNumberField(state, "fb_read_control",
				observation.framebufferConfig.fbReadControl);
		setNumberField(state, "spg_control",
				observation.framebufferConfig.spgControl);
		setNumberField(state, "spg_status",
				observation.framebufferConfig.spgStatus);
		setNumberField(state, "fb_read_sof1",
				observation.framebufferConfig.fbReadSof1);
		setNumberField(state, "fb_read_sof2",
				observation.framebufferConfig.fbReadSof2);
		setNumberField(state, "video_control",
				observation.framebufferConfig.videoControl);
		setNumberField(state, "border_color",
				observation.framebufferConfig.borderColor);
		if (observation.framebufferDigestAvailable)
			setStringField(state, "bytes_sha256",
					research::sha256ToHex(observation.framebufferDigest));
		else
			setBytesFields(state, "bytes", "bytes_hex", observation.bytes.data(),
					observation.bytes.size());
	}
	else if (observation.type == research::PvrPresentationObservationType::Presentation)
		setNumberField(state, "presentation_source",
				static_cast<unsigned>(observation.presentationSource));
}

static const char *pvrDrawEventName(research::PvrDrawObservationType type)
{
	switch (type)
	{
	case research::PvrDrawObservationType::PrimitiveDecoded: return "pvr-primitive";
	case research::PvrDrawObservationType::DrawConsumed: return "pvr-draw";
	case research::PvrDrawObservationType::RenderCompleted: return "pvr-draw-render-completed";
	case research::PvrDrawObservationType::Reset: return "pvr-draw-reset";
	}
	return "pvr-draw-unknown";
}

static void pushPvrTaBlockProvenance(lua_State *state,
		const std::vector<research::PvrTaBlockProvenance>& blocks)
{
	lua_newtable(state);
	for (std::size_t index = 0; index < blocks.size(); ++index)
	{
		const auto& block = blocks[index];
		lua_newtable(state);
		setBooleanField(state, "available", block.available);
		pushSh4Owner(state, block.initiator);
		lua_setfield(state, -2, "initiator");
		setNumberField(state, "context_address", block.contextAddress);
		setNumberField(state, "context_generation", block.contextGeneration);
		setNumberField(state, "context_block_ordinal", block.contextBlockOrdinal);
		setNumberField(state, "render_pass", block.renderPass);
		setNumberField(state, "source", static_cast<unsigned>(block.source));
		setNumberField(state, "source_address", block.sourceAddress);
		setNumberField(state, "ta_address", block.taAddress);
		lua_rawseti(state, -2, static_cast<int>(index + 1));
	}
}

static void pushPvrDrawResearchEvent(lua_State *state,
		const research::PvrDrawObservation& observation)
{
	pushDiscoveryBase(state, pvrDrawEventName(observation.type),
			observation.schemaVersion, observation.emissionOrdinal, observation.tick);
	setNumberField(state, "render_generation", observation.renderGeneration);
	setNumberField(state, "primitive_generation", observation.primitiveGeneration);
	setNumberField(state, "raster_generation", observation.rasterGeneration);
	setNumberField(state, "context_address", observation.contextAddress);
	setNumberField(state, "context_generation", observation.contextGeneration);
	setNumberField(state, "render_pass", observation.renderPass);
	setNumberField(state, "list_type", observation.listType);
	setNumberField(state, "primitive_kind", static_cast<unsigned>(observation.primitiveKind));
	setNumberField(state, "owner_class", static_cast<unsigned>(observation.ownerClass));
	setNumberField(state, "pcw", observation.pcw);
	setNumberField(state, "isp", observation.isp);
	setNumberField(state, "tsp", observation.tsp);
	setNumberField(state, "tcw", observation.tcw);
	setNumberField(state, "tsp1", observation.tsp1);
	setNumberField(state, "tcw1", observation.tcw1);
	setNumberField(state, "tile_clip", observation.tileClip);
	setNumberField(state, "first", observation.first);
	setNumberField(state, "count", observation.count);
	setNumberField(state, "backend", static_cast<unsigned>(observation.backend));
	setNumberField(state, "draw_pass", static_cast<unsigned>(observation.drawPass));
	setBooleanField(state, "indexed", observation.indexed);
	setBooleanField(state, "successful", observation.successful);
	if (observation.bounds.available)
	{
		lua_newtable(state);
		setFloatField(state, "minimum_x", observation.bounds.minimumX);
		setFloatField(state, "minimum_y", observation.bounds.minimumY);
		setFloatField(state, "minimum_z", observation.bounds.minimumZ);
		setFloatField(state, "maximum_x", observation.bounds.maximumX);
		setFloatField(state, "maximum_y", observation.bounds.maximumY);
		setFloatField(state, "maximum_z", observation.bounds.maximumZ);
		lua_setfield(state, -2, "bounds");
	}
	if (observation.type == research::PvrDrawObservationType::PrimitiveDecoded)
	{
		pushPvrTaBlockProvenance(state, observation.parameterBlocks);
		lua_setfield(state, -2, "parameter_blocks");
		pushPvrTaBlockProvenance(state, observation.vertexBlocks);
		lua_setfield(state, -2, "vertex_blocks");
		lua_newtable(state);
		for (std::size_t index = 0; index < observation.vertices.size(); ++index)
		{
			const auto& vertex = observation.vertices[index];
			lua_newtable(state);
			setNumberField(state, "x_bits", vertex.xBits);
			setNumberField(state, "y_bits", vertex.yBits);
			setNumberField(state, "z_bits", vertex.zBits);
			setNumberField(state, "u_bits", vertex.uBits);
			setNumberField(state, "v_bits", vertex.vBits);
			setNumberField(state, "u1_bits", vertex.u1Bits);
			setNumberField(state, "v1_bits", vertex.v1Bits);
			setNumberField(state, "nx_bits", vertex.nxBits);
			setNumberField(state, "ny_bits", vertex.nyBits);
			setNumberField(state, "nz_bits", vertex.nzBits);
			setBytesFields(state, "base_color", "base_color_hex",
					vertex.baseColor.data(), vertex.baseColor.size());
			setBytesFields(state, "offset_color", "offset_color_hex",
					vertex.offsetColor.data(), vertex.offsetColor.size());
			setBytesFields(state, "base_color1", "base_color1_hex",
					vertex.baseColor1.data(), vertex.baseColor1.size());
			setBytesFields(state, "offset_color1", "offset_color1_hex",
					vertex.offsetColor1.data(), vertex.offsetColor1.size());
			lua_rawseti(state, -2, static_cast<int>(index + 1));
		}
		lua_setfield(state, -2, "vertices");
		if (observation.sampledTexture.available)
		{
			const auto& texture = observation.sampledTexture;
			lua_newtable(state);
			setNumberField(state, "source_address", texture.sourceAddress);
			setNumberField(state, "source_size", texture.sourceSize);
			setNumberField(state, "maximum_level_address",
					texture.maximumLevelAddress);
			setNumberField(state, "maximum_level_size", texture.maximumLevelSize);
			setNumberField(state, "width", texture.width);
			setNumberField(state, "height", texture.height);
			setNumberField(state, "pixel_format", texture.pixelFormat);
			setNumberField(state, "palette_first_entry", texture.paletteFirstEntry);
			setNumberField(state, "palette_entry_count", texture.paletteEntryCount);
			setNumberField(state, "cache_updates", texture.cacheUpdates);
			setBooleanField(state, "gpu_palette", texture.gpuPalette);
			setBooleanField(state, "custom_replacement", texture.customReplacement);
			setStringField(state, "source_sha256",
					research::sha256ToHex(texture.sourceDigest));
			setStringField(state, "palette_sha256",
					research::sha256ToHex(texture.paletteDigest));
			if (!texture.sourceBytes.empty())
			{
				lua_pushlstring(state,
						reinterpret_cast<const char*>(texture.sourceBytes.data()),
						texture.sourceBytes.size());
				lua_setfield(state, -2, "source_bytes");
			}
			lua_setfield(state, -2, "sampled_texture");
		}
	}
}

static const char *gdromEventName(research::GdromObservationType type)
{
	switch (type)
	{
	case research::GdromObservationType::CommandBegin: return "gdrom-command";
	case research::GdromObservationType::TransferChunk: return "gdrom-transfer";
	case research::GdromObservationType::Complete: return "gdrom-complete";
	case research::GdromObservationType::Abort: return "gdrom-abort";
	case research::GdromObservationType::Reset: return "gdrom-reset";
	}
	return "gdrom-unknown";
}

static void pushGdromResearchEvent(lua_State *state,
		const research::GdromObservation& observation)
{
	pushDiscoveryBase(state, gdromEventName(observation.type),
			observation.schemaVersion, observation.emissionOrdinal, observation.tick);
	setNumberField(state, "command_generation", observation.commandGeneration);
	setNumberField(state, "path", static_cast<unsigned>(observation.path));
	pushSh4Owner(state, observation.initiator);
	lua_setfield(state, -2, "initiator");
	setNumberField(state, "request_id", observation.requestId);
	setNumberField(state, "command", observation.command);
	if (observation.type == research::GdromObservationType::CommandBegin)
	{
		lua_newtable(state);
		for (std::size_t index = 0; index < observation.parameters.size(); ++index)
		{
			lua_pushnumber(state, observation.parameters[index]);
			lua_rawseti(state, -2, static_cast<int>(index + 1));
		}
		lua_setfield(state, -2, "parameters");
	}
	if (observation.type == research::GdromObservationType::TransferChunk)
	{
		setNumberField(state, "chunk_ordinal", observation.chunkOrdinal);
		setNumberField(state, "fad", observation.fad);
		setNumberField(state, "sector_count", observation.sectorCount);
		setNumberField(state, "destination", observation.destination);
		setBytesFields(state, "bytes", "bytes_hex", observation.bytes.data(),
				observation.bytes.size());
	}
	setNumberField(state, "completion", static_cast<unsigned>(observation.completion));
	setNumberField(state, "transferred_bytes", observation.transferredBytes);
}

static const char *cddaEventName(research::CddaObservationType type)
{
	switch (type)
	{
	case research::CddaObservationType::ControlAccepted: return "cdda-control-accepted";
	case research::CddaObservationType::ControlApplied: return "cdda-control-applied";
	case research::CddaObservationType::Sector: return "cdda-sector";
	case research::CddaObservationType::Reset: return "cdda-reset";
	}
	return "cdda-unknown";
}

static void pushCddaDriveState(lua_State *state,
		const research::CddaDriveState& drive)
{
	lua_newtable(state);
	setNumberField(state, "status", drive.status);
	setNumberField(state, "repeats", drive.repeats);
	setNumberField(state, "current_fad", drive.currentFad);
	setNumberField(state, "start_fad", drive.startFad);
	setNumberField(state, "end_fad", drive.endFad);
}

static void pushCddaResearchEvent(lua_State *state,
		const research::CddaObservation& observation)
{
	pushDiscoveryBase(state, cddaEventName(observation.type),
			observation.schemaVersion, observation.emissionOrdinal, observation.tick);
	setNumberField(state, "control_generation", observation.controlGeneration);
	setNumberField(state, "path", static_cast<unsigned>(observation.path));
	setNumberField(state, "request_id", observation.requestId);
	setNumberField(state, "command", observation.command);
	if (observation.type == research::CddaObservationType::ControlAccepted
			|| observation.type == research::CddaObservationType::ControlApplied)
	{
		pushSh4Owner(state, observation.initiator);
		lua_setfield(state, -2, "initiator");
		lua_newtable(state);
		for (std::size_t index = 0; index < observation.parameters.size(); ++index)
		{
			lua_pushnumber(state, observation.parameters[index]);
			lua_rawseti(state, -2, static_cast<int>(index + 1));
		}
		lua_setfield(state, -2, "parameters");
	}
	if (observation.type == research::CddaObservationType::ControlApplied
			|| observation.type == research::CddaObservationType::Sector)
	{
		pushCddaDriveState(state, observation.before);
		lua_setfield(state, -2, "before");
		pushCddaDriveState(state, observation.after);
		lua_setfield(state, -2, "after");
	}
	setBooleanField(state, "applied_successfully", observation.appliedSuccessfully);
	setNumberField(state, "aica_generation", observation.aicaGeneration);
	setNumberField(state, "fad", observation.fad);
	setBooleanField(state, "read_successful", observation.readSuccessful);
	if (!observation.bytes.empty())
		setBytesFields(state, "bytes", "bytes_hex", observation.bytes.data(),
				observation.bytes.size());
}

static const char *aicaEventName(research::AicaObservationType type)
{
	switch (type)
	{
	case research::AicaObservationType::RegisterWrite: return "aica-register-write";
	case research::AicaObservationType::RamWrite: return "aica-ram-write";
	case research::AicaObservationType::G2DmaBegin: return "aica-dma-begin";
	case research::AicaObservationType::G2DmaTransfer: return "aica-dma-transfer";
	case research::AicaObservationType::G2DmaComplete: return "aica-dma-complete";
	case research::AicaObservationType::KeyOn: return "aica-key-on";
	case research::AicaObservationType::KeyOff: return "aica-key-off";
	case research::AicaObservationType::SampleFrame: return "aica-sample";
	case research::AicaObservationType::Reset: return "aica-reset";
	case research::AicaObservationType::KeyBatchComplete: return "aica-key-batch";
	case research::AicaObservationType::CddaSector: return "aica-cdda-sector";
	case research::AicaObservationType::SampleSuppressed: return "aica-sample-suppressed";
	case research::AicaObservationType::KeyBatchBegin: return "aica-key-batch-begin";
	}
	return "aica-unknown";
}

static void pushAicaResearchEvent(lua_State *state,
		const research::AicaObservation& observation)
{
	pushDiscoveryBase(state, aicaEventName(observation.type),
			observation.schemaVersion, observation.emissionOrdinal, observation.tick);
	setNumberField(state, "writer", static_cast<unsigned>(observation.owner.writer));
	pushSh4Owner(state, observation.owner.sh4);
	lua_setfield(state, -2, "sh4_owner");
	setNumberField(state, "address", observation.address);
	setNumberField(state, "width", observation.width);
	setNumberField(state, "value", observation.value);
	if (!observation.bytes.empty())
		setBytesFields(state, "bytes", "bytes_hex", observation.bytes.data(),
				observation.bytes.size());
	setNumberField(state, "dma_generation", observation.dmaGeneration);
	setNumberField(state, "source_address", observation.sourceAddress);
	setNumberField(state, "destination_address", observation.destinationAddress);
	setNumberField(state, "transfer_length", observation.transferLength);
	setBooleanField(state, "aica_ram_is_destination", observation.aicaRamIsDestination);
	setNumberField(state, "channel", observation.channel);
	if (observation.type == research::AicaObservationType::KeyOn
			|| observation.type == research::AicaObservationType::KeyOff)
		setBytesFields(state, "channel_registers", "channel_registers_hex",
				observation.channelRegisters.data(), observation.channelRegisters.size());
	setStringField(state, "key_on_mask_hex", hexadecimal64(observation.keyOnMask));
	setStringField(state, "key_off_mask_hex", hexadecimal64(observation.keyOffMask));
	setNumberField(state, "sample_cut_ordinal", observation.sampleCutOrdinal);
	setNumberField(state, "cdda_generation", observation.cddaGeneration);
	setNumberField(state, "cdda_fad", observation.cddaFad);
	setNumberField(state, "cdda_status", observation.cddaStatus);
	setNumberField(state, "cdda_repeats", observation.cddaRepeats);
	setBooleanField(state, "cdda_read_successful", observation.cddaReadSuccessful);
	setNumberField(state, "cdda_frame_index", observation.cddaFrameIndex);
	setNumberField(state, "suppression", static_cast<unsigned>(observation.suppression));
	setNumberField(state, "sample_ordinal", observation.sampleOrdinal);
	setStringField(state, "active_channel_mask_hex",
			hexadecimal64(observation.activeChannelMask));
	setSignedNumberField(state, "dry_left", observation.dryLeft);
	setSignedNumberField(state, "dry_right", observation.dryRight);
	setSignedNumberField(state, "cdda_input_left", observation.cddaInputLeft);
	setSignedNumberField(state, "cdda_input_right", observation.cddaInputRight);
	setSignedNumberField(state, "cdda_contribution_left", observation.cddaContributionLeft);
	setSignedNumberField(state, "cdda_contribution_right", observation.cddaContributionRight);
	setBooleanField(state, "dsp_enabled", observation.dspEnabled);
	setSignedNumberField(state, "dsp_contribution_left", observation.dspContributionLeft);
	setSignedNumberField(state, "dsp_contribution_right", observation.dspContributionRight);
	setSignedNumberField(state, "final_left", observation.finalLeft);
	setSignedNumberField(state, "final_right", observation.finalRight);
}

static std::optional<std::uint64_t> optionalUnsignedTableField(lua_State *state,
		int tableIndex, const char *name)
{
	tableIndex = lua_absindex(state, tableIndex);
	lua_getfield(state, tableIndex, name);
	if (lua_isnil(state, -1))
	{
		lua_pop(state, 1);
		return std::nullopt;
	}
	if (!lua_isnumber(state, -1))
	{
		lua_pop(state, 1);
		throw std::invalid_argument(std::string(name) + " must be an unsigned integer");
	}
	const lua_Number number = lua_tonumber(state, -1);
	lua_pop(state, 1);
	constexpr lua_Number MaximumExactLuaInteger = 9007199254740991.0;
	if (!std::isfinite(number) || number < 0
			|| std::floor(number) != number
			|| number > MaximumExactLuaInteger)
		throw std::invalid_argument(std::string(name) + " must be an unsigned integer");
	return static_cast<std::uint64_t>(number);
}

static std::optional<bool> optionalBooleanTableField(lua_State *state,
		int tableIndex, const char *name)
{
	tableIndex = lua_absindex(state, tableIndex);
	lua_getfield(state, tableIndex, name);
	if (lua_isnil(state, -1))
	{
		lua_pop(state, 1);
		return std::nullopt;
	}
	if (!lua_isboolean(state, -1))
	{
		lua_pop(state, 1);
		throw std::invalid_argument(std::string(name) + " must be a boolean");
	}
	const bool value = lua_toboolean(state, -1) != 0;
	lua_pop(state, 1);
	return value;
}

static std::optional<std::string> optionalStringTableField(lua_State *state,
		int tableIndex, const char *name)
{
	tableIndex = lua_absindex(state, tableIndex);
	lua_getfield(state, tableIndex, name);
	if (lua_isnil(state, -1))
	{
		lua_pop(state, 1);
		return std::nullopt;
	}
	if (!lua_isstring(state, -1))
	{
		lua_pop(state, 1);
		throw std::invalid_argument(std::string(name) + " must be a string");
	}
	size_t length = 0;
	const char *text = lua_tolstring(state, -1, &length);
	std::string value(text, length);
	lua_pop(state, 1);
	return value;
}

static std::vector<std::uint32_t> optionalU32ArrayTableField(lua_State *state,
		int tableIndex, const char *name, std::size_t maximumCount)
{
	tableIndex = lua_absindex(state, tableIndex);
	lua_getfield(state, tableIndex, name);
	if (lua_isnil(state, -1))
	{
		lua_pop(state, 1);
		return {};
	}
	if (!lua_istable(state, -1))
	{
		lua_pop(state, 1);
		throw std::invalid_argument(std::string(name) + " must be an array");
	}
	const std::size_t count = lua_rawlen(state, -1);
	if (count > maximumCount)
	{
		lua_pop(state, 1);
		throw std::invalid_argument(std::string(name) + " has too many entries");
	}
	std::vector<std::uint32_t> result;
	result.reserve(count);
	for (std::size_t index = 1; index <= count; ++index)
	{
		lua_rawgeti(state, -1, static_cast<lua_Integer>(index));
		if (!lua_isnumber(state, -1))
		{
			lua_pop(state, 2);
			throw std::invalid_argument(
					std::string(name) + " entries must be unsigned U32 integers");
		}
		const lua_Number number = lua_tonumber(state, -1);
		lua_pop(state, 1);
		if (!std::isfinite(number) || number < 0
				|| std::floor(number) != number || number > 4294967295.0)
		{
			lua_pop(state, 1);
			throw std::invalid_argument(
					std::string(name) + " entries must be unsigned U32 integers");
		}
		result.push_back(static_cast<std::uint32_t>(number));
	}
	lua_pop(state, 1);
	return result;
}

static bool tableFieldPresent(lua_State *state, int tableIndex, const char *name)
{
	tableIndex = lua_absindex(state, tableIndex);
	lua_getfield(state, tableIndex, name);
	const bool present = !lua_isnil(state, -1);
	lua_pop(state, 1);
	return present;
}

static research::Sh4ObservationFilter researchFilterFromLua(lua_State *state,
		std::size_t& capacity)
{
	const std::optional<std::string> event = optionalStringTableField(state, 1,
			"event");
	if (!event.has_value())
		throw std::invalid_argument("event is required");
	research::Sh4ObservationType type;
	if (*event == "instruction-begin")
		type = research::Sh4ObservationType::InstructionBegin;
	else if (*event == "instruction")
		type = research::Sh4ObservationType::InstructionEnd;
	else if (*event == "instruction-abort")
		type = research::Sh4ObservationType::InstructionAbort;
	else if (*event == "call")
		type = research::Sh4ObservationType::Call;
	else if (*event == "return")
		type = research::Sh4ObservationType::Return;
	else if (*event == "memory-read")
		type = research::Sh4ObservationType::MemoryRead;
	else if (*event == "memory-write")
		type = research::Sh4ObservationType::MemoryWrite;
	else if (*event == "exception")
		type = research::Sh4ObservationType::Exception;
	else
		throw std::invalid_argument("unsupported research event");
	if (tableFieldPresent(state, 1, "bus")
			|| tableFieldPresent(state, 1, "port")
			|| tableFieldPresent(state, 1, "command"))
		throw std::invalid_argument("Maple filters are only valid for Maple events");

	research::Sh4ObservationFilter filter;
	filter.typeMask = research::sh4ObservationTypeBit(type);
	const std::optional<std::string> backend = optionalStringTableField(state, 1,
			"backend");
	if (!backend.has_value() || *backend == "any")
		filter.backendMask = research::AllSh4ObservationBackends;
	else if (*backend == "interpreter")
		filter.backendMask = research::sh4ObservationBackendBit(
				research::Sh4ObservationBackend::Interpreter);
	else if (*backend == "dynarec")
		filter.backendMask = research::sh4ObservationBackendBit(
				research::Sh4ObservationBackend::Dynarec);
	else
		throw std::invalid_argument("backend must be any, interpreter, or dynarec");

	const std::optional<std::uint64_t> startPc = optionalUnsignedTableField(state, 1,
			"start_pc");
	const std::optional<std::uint64_t> endPc = optionalUnsignedTableField(state, 1,
			"end_pc");
	if (startPc.has_value() != endPc.has_value())
		throw std::invalid_argument("start_pc and end_pc must be provided together");
	if (startPc.has_value())
	{
		if (*startPc > 0xffffffffull || *endPc > 0xffffffffull || *startPc > *endPc)
			throw std::invalid_argument("invalid inclusive SH-4 PC range");
		filter.hasInstructionPcRange = true;
		filter.instructionPcStart = static_cast<std::uint32_t>(*startPc);
		filter.instructionPcEndExclusive = *endPc + 1;
	}

	const std::optional<std::uint64_t> start = optionalUnsignedTableField(state, 1,
			"start_address");
	const std::optional<std::uint64_t> end = optionalUnsignedTableField(state, 1,
			"end_address");
	if (start.has_value() != end.has_value())
		throw std::invalid_argument(
				"start_address and end_address must be provided together");
	const bool memoryEvent = type == research::Sh4ObservationType::MemoryRead
			|| type == research::Sh4ObservationType::MemoryWrite;
	if (start.has_value())
	{
		if (!memoryEvent)
			throw std::invalid_argument("address range is only valid for memory events");
		if (*start > 0xffffffffull || *end > 0xffffffffull || *start > *end)
			throw std::invalid_argument("invalid inclusive SH-4 address range");
		filter.hasMemoryRange = true;
		filter.memoryStart = static_cast<std::uint32_t>(*start);
		filter.memoryEndExclusive = *end + 1;
	}

	const std::optional<std::uint64_t> requestedCapacity = optionalUnsignedTableField(
			state, 1, "queue_capacity");
	capacity = requestedCapacity.has_value()
			? static_cast<std::size_t>(*requestedCapacity)
			: research::Sh4LuaSubscriptionQueue::DefaultCapacity;
	return filter;
}

static research::MapleObservationFilter mapleResearchFilterFromLua(lua_State *state,
		const std::string& event, std::size_t& capacity)
{
	if (tableFieldPresent(state, 1, "backend")
			|| tableFieldPresent(state, 1, "start_pc")
			|| tableFieldPresent(state, 1, "end_pc")
			|| tableFieldPresent(state, 1, "start_address")
			|| tableFieldPresent(state, 1, "end_address"))
		throw std::invalid_argument("SH-4 filters are only valid for SH-4 events");
	research::MapleObservationFilter filter;
	filter.typeMask = event == "maple-request"
			? research::mapleObservationTypeBit(research::MapleObservationType::Request)
			: research::mapleObservationTypeBit(research::MapleObservationType::Response);
	const std::optional<std::uint64_t> bus = optionalUnsignedTableField(state, 1,
			"bus");
	if (bus.has_value())
	{
		if (*bus > 3)
			throw std::invalid_argument("Maple bus must be in [0, 3]");
		filter.busMask = static_cast<std::uint8_t>(1u << *bus);
	}
	const std::optional<std::uint64_t> port = optionalUnsignedTableField(state, 1,
			"port");
	if (port.has_value())
	{
		if (*port > 5)
			throw std::invalid_argument("Maple port must be in [0, 5]");
		filter.portMask = static_cast<std::uint8_t>(1u << *port);
	}
	const std::optional<std::uint64_t> command = optionalUnsignedTableField(state, 1,
			"command");
	if (command.has_value())
	{
		if (*command > 0xff)
			throw std::invalid_argument("Maple command must be in [0, 255]");
		filter.hasCommand = true;
		filter.command = static_cast<std::uint8_t>(*command);
	}
	const std::optional<std::uint64_t> requestedCapacity = optionalUnsignedTableField(
			state, 1, "queue_capacity");
	capacity = requestedCapacity.has_value()
			? static_cast<std::size_t>(*requestedCapacity)
			: research::Sh4LuaSubscriptionQueue::DefaultCapacity;
	return filter;
}

static std::size_t researchQueueCapacity(lua_State *state)
{
	const auto requested = optionalUnsignedTableField(state, 1, "queue_capacity");
	return requested.has_value() ? static_cast<std::size_t>(*requested)
			: research::Sh4LuaSubscriptionQueue::DefaultCapacity;
}

static research::Sh4LuaSubscriptionQueue::PvrTaFilter pvrTaResearchFilterFromLua(
		lua_State *state, const std::string& event, std::size_t& capacity)
{
	research::Sh4LuaSubscriptionQueue::PvrTaFilter filter;
	auto& observation = filter.observation;
	if (event == "pvr-ta-list-init")
		observation.typeMask = research::pvrTaObservationTypeBit(
				research::PvrTaObservationType::ListInit);
	else if (event == "pvr-ta-list-continue")
		observation.typeMask = research::pvrTaObservationTypeBit(
				research::PvrTaObservationType::ListContinue);
	else if (event == "pvr-ta-block")
		observation.typeMask = research::pvrTaObservationTypeBit(
				research::PvrTaObservationType::AcceptedBlock);
	else if (event == "pvr-start-render")
		observation.typeMask = research::pvrTaObservationTypeBit(
				research::PvrTaObservationType::StartRender);
	else if (event == "pvr-render-done")
		observation.typeMask = research::pvrTaObservationTypeBit(
				research::PvrTaObservationType::RenderDone);
	else if (event == "pvr-ta-reset")
		observation.typeMask = research::pvrTaObservationTypeBit(
				research::PvrTaObservationType::Reset);
	else
		throw std::invalid_argument("unsupported PowerVR TA event");
	const auto source = optionalStringTableField(state, 1, "source");
	if (source.has_value())
	{
		if (event != "pvr-ta-block")
			throw std::invalid_argument("source is only valid for pvr-ta-block");
		if (*source == "store-queue")
			observation.sourceMask = research::pvrTaInputSourceBit(
					research::PvrTaInputSource::StoreQueue);
		else if (*source == "channel2-dma")
			observation.sourceMask = research::pvrTaInputSourceBit(
					research::PvrTaInputSource::Channel2Dma);
		else if (*source == "sort-dma")
			observation.sourceMask = research::pvrTaInputSourceBit(
					research::PvrTaInputSource::SortDma);
		else
			throw std::invalid_argument("unsupported PowerVR TA source");
	}
	filter.guestU32Addresses = optionalU32ArrayTableField(state, 1,
			"guest_u32_addresses",
			research::Sh4LuaSubscriptionQueue::MaximumGuestU32Snapshot);
	if (!filter.guestU32Addresses.empty() && event != "pvr-ta-block")
		throw std::invalid_argument(
				"guest_u32_addresses is only valid for pvr-ta-block");
	capacity = researchQueueCapacity(state);
	return filter;
}

static research::Sh4LuaSubscriptionQueue::PvrPresentationFilter
pvrPresentationResearchFilterFromLua(lua_State *state, const std::string& event,
		std::size_t& capacity)
{
	using Type = research::PvrPresentationObservationType;
	Type type;
	if (event == "pvr-register-write") type = Type::RegisterWrite;
	else if (event == "pvr-vram-write") type = Type::VramWrite;
	else if (event == "pvr-render-queued") type = Type::RenderQueued;
	else if (event == "pvr-render-completed") type = Type::RenderCompleted;
	else if (event == "pvr-framebuffer") type = Type::FramebufferCaptured;
	else if (event == "pvr-presentation") type = Type::Presentation;
	else if (event == "pvr-presentation-reset") type = Type::Reset;
	else throw std::invalid_argument("unsupported PowerVR presentation event");
	research::Sh4LuaSubscriptionQueue::PvrPresentationFilter filter;
	filter.typeMask = std::uint32_t {1} << (static_cast<unsigned>(type) - 1u);
	const auto start = optionalUnsignedTableField(state, 1, "start_address");
	const auto end = optionalUnsignedTableField(state, 1, "end_address");
	if (start.has_value() != end.has_value())
		throw std::invalid_argument("start_address and end_address must be provided together");
	if (start.has_value())
	{
		if (type != Type::RegisterWrite && type != Type::VramWrite
				|| *start > 0xffffffffull || *end > 0xffffffffull || *start > *end)
			throw std::invalid_argument("invalid PowerVR address filter");
		filter.hasAddressRange = true;
		filter.addressStart = static_cast<std::uint32_t>(*start);
		filter.addressEndExclusive = *end + 1;
	}
	const auto payload = optionalStringTableField(state, 1, "payload");
	if (payload.has_value())
	{
		if (type != Type::FramebufferCaptured)
			throw std::invalid_argument("payload is only valid for pvr-framebuffer");
		if (*payload == "full")
			filter.framebufferDigestOnly = false;
		else if (*payload == "sha256")
			filter.framebufferDigestOnly = true;
		else
			throw std::invalid_argument("unsupported PowerVR framebuffer payload");
	}
	filter.guestU32Addresses = optionalU32ArrayTableField(state, 1,
			"guest_u32_addresses",
			research::Sh4LuaSubscriptionQueue::MaximumGuestU32Snapshot);
	if (!filter.guestU32Addresses.empty() && type != Type::RegisterWrite
			&& type != Type::VramWrite)
		throw std::invalid_argument(
				"guest_u32_addresses is only valid for synchronous PVR writes");
	filter.guestR15U32Offsets = optionalU32ArrayTableField(state, 1,
			"guest_r15_u32_offsets",
			research::Sh4LuaSubscriptionQueue::MaximumGuestR15U32Snapshot);
	if (!filter.guestR15U32Offsets.empty() && type != Type::RegisterWrite
			&& type != Type::VramWrite)
		throw std::invalid_argument(
				"guest_r15_u32_offsets is only valid for synchronous PVR writes");
	capacity = researchQueueCapacity(state);
	return filter;
}

static research::Sh4LuaSubscriptionQueue::PvrDrawFilter
pvrDrawResearchFilterFromLua(lua_State *state, const std::string& event,
		std::size_t& capacity)
{
	using Type = research::PvrDrawObservationType;
	Type type;
	if (event == "pvr-primitive") type = Type::PrimitiveDecoded;
	else if (event == "pvr-draw") type = Type::DrawConsumed;
	else if (event == "pvr-draw-render-completed") type = Type::RenderCompleted;
	else if (event == "pvr-draw-reset") type = Type::Reset;
	else throw std::invalid_argument("unsupported PowerVR draw event");
	research::Sh4LuaSubscriptionQueue::PvrDrawFilter filter;
	filter.typeMask = std::uint32_t {1} << (static_cast<unsigned>(type) - 1u);
	const auto generation = optionalUnsignedTableField(state, 1, "render_generation");
	if (generation.has_value())
	{
		filter.hasRenderGeneration = true;
		filter.renderGeneration = *generation;
	}
	const auto payload = optionalStringTableField(state, 1, "payload");
	if (payload.has_value())
	{
		if (type != Type::PrimitiveDecoded)
			throw std::invalid_argument("payload is only valid for pvr-primitive");
		if (*payload == "full")
			filter.sampledTextureDigestOnly = false;
		else if (*payload == "sha256")
			filter.sampledTextureDigestOnly = true;
		else
			throw std::invalid_argument("unsupported PowerVR primitive payload");
	}
	capacity = researchQueueCapacity(state);
	return filter;
}

static research::Sh4LuaSubscriptionQueue::GdromFilter gdromResearchFilterFromLua(
		lua_State *state, const std::string& event, std::size_t& capacity)
{
	using Type = research::GdromObservationType;
	Type type;
	if (event == "gdrom-command") type = Type::CommandBegin;
	else if (event == "gdrom-transfer") type = Type::TransferChunk;
	else if (event == "gdrom-complete") type = Type::Complete;
	else if (event == "gdrom-abort") type = Type::Abort;
	else if (event == "gdrom-reset") type = Type::Reset;
	else throw std::invalid_argument("unsupported GD-ROM event");
	research::Sh4LuaSubscriptionQueue::GdromFilter filter;
	filter.typeMask = std::uint32_t {1} << (static_cast<unsigned>(type) - 1u);
	const auto start = optionalUnsignedTableField(state, 1, "start_fad");
	const auto end = optionalUnsignedTableField(state, 1, "end_fad");
	if (start.has_value() != end.has_value())
		throw std::invalid_argument("start_fad and end_fad must be provided together");
	if (start.has_value())
	{
		if (type != Type::TransferChunk || *start > 0xffffffffull
				|| *end > 0xffffffffull || *start > *end)
			throw std::invalid_argument("invalid GD-ROM FAD filter");
		filter.hasFadRange = true;
		filter.fadStart = static_cast<std::uint32_t>(*start);
		filter.fadEndExclusive = *end + 1;
	}
	capacity = researchQueueCapacity(state);
	return filter;
}

static research::Sh4LuaSubscriptionQueue::AicaFilter aicaResearchFilterFromLua(
		lua_State *state, const std::string& event, std::size_t& capacity)
{
	using Type = research::AicaObservationType;
	Type type;
	if (event == "aica-register-write") type = Type::RegisterWrite;
	else if (event == "aica-ram-write") type = Type::RamWrite;
	else if (event == "aica-dma-begin") type = Type::G2DmaBegin;
	else if (event == "aica-dma-transfer") type = Type::G2DmaTransfer;
	else if (event == "aica-dma-complete") type = Type::G2DmaComplete;
	else if (event == "aica-key-on") type = Type::KeyOn;
	else if (event == "aica-key-off") type = Type::KeyOff;
	else if (event == "aica-sample") type = Type::SampleFrame;
	else if (event == "aica-reset") type = Type::Reset;
	else if (event == "aica-key-batch") type = Type::KeyBatchComplete;
	else if (event == "aica-cdda-sector") type = Type::CddaSector;
	else if (event == "aica-sample-suppressed") type = Type::SampleSuppressed;
	else if (event == "aica-key-batch-begin") type = Type::KeyBatchBegin;
	else throw std::invalid_argument("unsupported AICA event");
	research::Sh4LuaSubscriptionQueue::AicaFilter filter;
	filter.typeMask = std::uint32_t {1} << (static_cast<unsigned>(type) - 1u);
	const auto writer = optionalStringTableField(state, 1, "writer");
	if (writer.has_value())
	{
		unsigned value = 0;
		if (*writer == "sh4-direct") value = 1;
		else if (*writer == "sh4-g2-dma") value = 2;
		else if (*writer == "arm7") value = 3;
		else if (*writer == "aica-dma") value = 4;
		else if (*writer == "mixer") value = 5;
		else if (*writer == "reios") value = 6;
		else throw std::invalid_argument("unsupported AICA writer");
		filter.hasWriter = true;
		filter.writerMask = std::uint32_t {1} << (value - 1u);
	}
	const auto start = optionalUnsignedTableField(state, 1, "start_address");
	const auto end = optionalUnsignedTableField(state, 1, "end_address");
	if (start.has_value() != end.has_value())
		throw std::invalid_argument("start_address and end_address must be provided together");
	if (start.has_value())
	{
		if ((type != Type::RegisterWrite && type != Type::RamWrite)
				|| *start > 0xffffffffull || *end > 0xffffffffull || *start > *end)
			throw std::invalid_argument("invalid AICA address filter");
		filter.hasAddressRange = true;
		filter.addressStart = static_cast<std::uint32_t>(*start);
		filter.addressEndExclusive = *end + 1;
	}
	const auto channel = optionalUnsignedTableField(state, 1, "channel");
	if (channel.has_value())
	{
		if (*channel >= 64 || (type != Type::KeyOn && type != Type::KeyOff
				&& type != Type::KeyBatchComplete))
			throw std::invalid_argument("invalid AICA channel filter");
		filter.hasChannel = true;
		filter.channel = static_cast<std::uint8_t>(*channel);
	}
	const auto nonzeroCdda = optionalBooleanTableField(state, 1,
			"nonzero_cdda_contribution");
	if (nonzeroCdda.has_value())
	{
		if (type != Type::SampleFrame || !*nonzeroCdda)
			throw std::invalid_argument("invalid nonzero CD-DA contribution filter");
		filter.requireNonzeroCddaContribution = true;
	}
	capacity = researchQueueCapacity(state);
	return filter;
}

static research::Sh4LuaSubscriptionQueue::CddaFilter cddaResearchFilterFromLua(
		lua_State *state, const std::string& event, std::size_t& capacity)
{
	using Type = research::CddaObservationType;
	Type type;
	if (event == "cdda-control-accepted") type = Type::ControlAccepted;
	else if (event == "cdda-control-applied") type = Type::ControlApplied;
	else if (event == "cdda-sector") type = Type::Sector;
	else if (event == "cdda-reset") type = Type::Reset;
	else throw std::invalid_argument("unsupported CD-DA event");
	research::Sh4LuaSubscriptionQueue::CddaFilter filter;
	filter.typeMask = std::uint32_t {1} << (static_cast<unsigned>(type) - 1u);
	const auto command = optionalUnsignedTableField(state, 1, "command");
	if (command.has_value())
	{
		if ((type != Type::ControlAccepted && type != Type::ControlApplied)
				|| *command > 0xffffffffull
				|| !research::isCddaControlCommand(static_cast<std::uint32_t>(*command)))
			throw std::invalid_argument("invalid CD-DA command filter");
		filter.hasCommand = true;
		filter.command = static_cast<std::uint32_t>(*command);
	}
	const auto successful = optionalBooleanTableField(state, 1, "successful");
	if (successful.has_value())
	{
		if (type != Type::ControlApplied && type != Type::Sector)
			throw std::invalid_argument("successful is valid only for applied controls and sectors");
		filter.hasSuccessful = true;
		filter.successful = *successful;
	}
	const auto start = optionalUnsignedTableField(state, 1, "start_fad");
	const auto end = optionalUnsignedTableField(state, 1, "end_fad");
	if (start.has_value() != end.has_value())
		throw std::invalid_argument("start_fad and end_fad must be provided together");
	if (start.has_value())
	{
		if (type != Type::Sector || *start > 0xffffffffull
				|| *end > 0xffffffffull || *start > *end)
			throw std::invalid_argument("invalid CD-DA FAD filter");
		filter.hasFadRange = true;
		filter.fadStart = static_cast<std::uint32_t>(*start);
		filter.fadEndExclusive = *end + 1;
	}
	capacity = researchQueueCapacity(state);
	return filter;
}

static void deliverResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::Sh4Observation& observation)
{
	const auto found = researchCallbackRefs.find(token);
	if (found == researchCallbackRefs.end())
		return;
	lua_rawgeti(L, LUA_REGISTRYINDEX, found->second);
	pushResearchEvent(L, observation);
	if (lua_pcall(L, 1, 0, 0) != 0)
	{
		const char *message = lua_tostring(L, -1);
		const std::string failure = message == nullptr
				? "unknown Lua callback error" : message;
		lua_pop(L, 1);
		throw std::runtime_error(failure);
	}
}

static void deliverMapleResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::MapleObservation& observation)
{
	const auto found = researchCallbackRefs.find(token);
	if (found == researchCallbackRefs.end())
		return;
	lua_rawgeti(L, LUA_REGISTRYINDEX, found->second);
	pushMapleResearchEvent(L, observation);
	if (lua_pcall(L, 1, 0, 0) != 0)
	{
		const char *message = lua_tostring(L, -1);
		const std::string failure = message == nullptr
				? "unknown Lua callback error" : message;
		lua_pop(L, 1);
		throw std::runtime_error(failure);
	}
}

template<typename Observation, typename Push>
static void deliverTypedResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const Observation& observation, Push push)
{
	const auto found = researchCallbackRefs.find(token);
	if (found == researchCallbackRefs.end())
		return;
	lua_rawgeti(L, LUA_REGISTRYINDEX, found->second);
	push(L, observation);
	if (lua_pcall(L, 1, 0, 0) != 0)
	{
		const char *message = lua_tostring(L, -1);
		const std::string failure = message == nullptr
				? "unknown Lua callback error" : message;
		lua_pop(L, 1);
		throw std::runtime_error(failure);
	}
}

static void deliverPvrTaResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::Sh4LuaSubscriptionQueue::PvrTaLuaObservation& observation)
{
	deliverTypedResearchObservation(token, observation, pushPvrTaResearchEvent);
}

static void deliverPvrPresentationResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation& observation)
{
	deliverTypedResearchObservation(token, observation,
			pushPvrPresentationResearchEvent);
}

static void deliverPvrDrawResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::PvrDrawObservation& observation)
{
	deliverTypedResearchObservation(token, observation, pushPvrDrawResearchEvent);
}

static void deliverGdromResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::GdromObservation& observation)
{
	deliverTypedResearchObservation(token, observation, pushGdromResearchEvent);
}

static void deliverAicaResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::AicaObservation& observation)
{
	deliverTypedResearchObservation(token, observation, pushAicaResearchEvent);
}

static void deliverCddaResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::CddaObservation& observation)
{
	deliverTypedResearchObservation(token, observation, pushCddaResearchEvent);
}

static void reportResearchCallbackFailure(
		research::Sh4LuaSubscriptionQueue::Token token,
		std::exception_ptr failure) noexcept
{
	try
	{
		if (failure != nullptr)
			std::rethrow_exception(failure);
	}
	catch (const std::exception& exception)
	{
		WARN_LOG(COMMON, "Lua research subscriber %llu failed: %s",
				static_cast<unsigned long long>(token), exception.what());
	}
	catch (...)
	{
		WARN_LOG(COMMON, "Lua research subscriber %llu failed",
				static_cast<unsigned long long>(token));
	}
}

static int researchSubscribe(lua_State *state)
{
	try
	{
		if (state != L || researchSubscriptions == nullptr
				|| !researchSubscriptionsAllowed)
			throw std::runtime_error("research subscriptions are not available");
		if (lua_gettop(state) != 2 || !lua_istable(state, 1)
				|| !lua_isfunction(state, 2))
			throw std::invalid_argument("subscribe expects a filter table and function");
		const std::optional<std::string> event = optionalStringTableField(state, 1,
				"event");
		if (!event.has_value())
			throw std::invalid_argument("event is required");
		const bool mapleEvent = *event == "maple-request" || *event == "maple-response";
		const bool pvrTaEvent = *event == "pvr-ta-list-init"
				|| *event == "pvr-ta-list-continue" || *event == "pvr-ta-block"
				|| *event == "pvr-start-render" || *event == "pvr-render-done"
				|| *event == "pvr-ta-reset";
		const bool pvrPresentationEvent = *event == "pvr-register-write"
				|| *event == "pvr-vram-write" || *event == "pvr-render-queued"
				|| *event == "pvr-render-completed" || *event == "pvr-framebuffer"
				|| *event == "pvr-presentation"
				|| *event == "pvr-presentation-reset";
		const bool pvrDrawEvent = *event == "pvr-primitive" || *event == "pvr-draw"
				|| *event == "pvr-draw-render-completed" || *event == "pvr-draw-reset";
		const bool gdromEvent = event->rfind("gdrom-", 0) == 0;
		const bool cddaEvent = event->rfind("cdda-", 0) == 0;
		const bool aicaEvent = event->rfind("aica-", 0) == 0;
		std::size_t capacity = 0;
		lua_pushvalue(state, 2);
		const int callbackRef = luaL_ref(state, LUA_REGISTRYINDEX);
		try
		{
			research::Sh4LuaSubscriptionQueue::Token token = 0;
			if (mapleEvent)
			{
				const research::MapleObservationFilter filter = mapleResearchFilterFromLua(
						state, *event, capacity);
				token = researchSubscriptions->subscribe(filter,
						deliverMapleResearchObservation, capacity,
						reportResearchCallbackFailure);
			}
			else if (pvrTaEvent)
			{
				const auto filter = pvrTaResearchFilterFromLua(state, *event, capacity);
				token = researchSubscriptions->subscribe(filter,
						deliverPvrTaResearchObservation, capacity,
						reportResearchCallbackFailure);
			}
			else if (pvrPresentationEvent)
			{
				const auto filter = pvrPresentationResearchFilterFromLua(
						state, *event, capacity);
				token = researchSubscriptions->subscribe(filter,
						deliverPvrPresentationResearchObservation, capacity,
						reportResearchCallbackFailure);
			}
			else if (pvrDrawEvent)
			{
				const auto filter = pvrDrawResearchFilterFromLua(state, *event, capacity);
				token = researchSubscriptions->subscribe(filter,
						deliverPvrDrawResearchObservation, capacity,
						reportResearchCallbackFailure);
			}
			else if (gdromEvent)
			{
				const auto filter = gdromResearchFilterFromLua(state, *event, capacity);
				token = researchSubscriptions->subscribe(filter,
						deliverGdromResearchObservation, capacity,
						reportResearchCallbackFailure);
			}
			else if (cddaEvent)
			{
				const auto filter = cddaResearchFilterFromLua(state, *event, capacity);
				token = researchSubscriptions->subscribe(filter,
						deliverCddaResearchObservation, capacity,
						reportResearchCallbackFailure);
			}
			else if (aicaEvent)
			{
				const auto filter = aicaResearchFilterFromLua(state, *event, capacity);
				token = researchSubscriptions->subscribe(filter,
						deliverAicaResearchObservation, capacity,
						reportResearchCallbackFailure);
			}
			else
			{
				const research::Sh4ObservationFilter filter = researchFilterFromLua(state,
						capacity);
				token = researchSubscriptions->subscribe(filter,
						deliverResearchObservation, capacity,
						reportResearchCallbackFailure);
			}
			try
			{
				researchCallbackRefs.emplace(token, callbackRef);
			}
			catch (...)
			{
				researchSubscriptions->unsubscribe(token);
				throw;
			}
			lua_pushnumber(state, static_cast<lua_Number>(token));
			return 1;
		}
		catch (...)
		{
			luaL_unref(state, LUA_REGISTRYINDEX, callbackRef);
			throw;
		}
	}
	catch (const std::exception& exception)
	{
		return luaL_error(state, "%s", exception.what());
	}
}

static research::Sh4LuaSubscriptionQueue::Token researchToken(lua_State *state)
{
	if (lua_gettop(state) != 1 || !lua_isnumber(state, 1))
		throw std::invalid_argument("subscription token must be a positive integer");
	const lua_Number value = lua_tonumber(state, 1);
	constexpr lua_Number MaximumExactLuaInteger = 9007199254740991.0;
	if (!std::isfinite(value) || value <= 0 || std::floor(value) != value
			|| value > MaximumExactLuaInteger)
		throw std::invalid_argument("subscription token must be a positive integer");
	return static_cast<research::Sh4LuaSubscriptionQueue::Token>(value);
}

static int researchUnsubscribe(lua_State *state)
{
	try
	{
		const auto token = researchToken(state);
		const bool removed = researchSubscriptions != nullptr
				&& researchSubscriptions->unsubscribe(token);
		if (removed)
		{
			const auto found = researchCallbackRefs.find(token);
			if (found != researchCallbackRefs.end())
			{
				luaL_unref(state, LUA_REGISTRYINDEX, found->second);
				researchCallbackRefs.erase(found);
			}
		}
		lua_pushboolean(state, removed ? 1 : 0);
		return 1;
	}
	catch (const std::exception& exception)
	{
		return luaL_error(state, "%s", exception.what());
	}
}

static int researchSubscriptionStats(lua_State *state)
{
	try
	{
		const auto token = researchToken(state);
		const auto stats = researchSubscriptions == nullptr
				? std::nullopt : researchSubscriptions->stats(token);
		if (!stats.has_value())
		{
			lua_pushnil(state);
			return 1;
		}
		lua_newtable(state);
		setBooleanField(state, "discovery", true);
		setBooleanField(state, "active", stats->active);
		setNumberField(state, "capacity", stats->capacity);
		setNumberField(state, "queued", stats->queued);
		setNumberField(state, "delivered", stats->delivered);
		setNumberField(state, "dropped", stats->dropped);
		setNumberField(state, "callback_errors", stats->callbackFailures);
		return 1;
	}
	catch (const std::exception& exception)
	{
		return luaL_error(state, "%s", exception.what());
	}
}

static int researchCurrentSh4TickDecimal(lua_State *state)
{
	if (lua_gettop(state) != 0)
		return luaL_error(state, "current_sh4_tick_decimal expects no arguments");
	if (p_sh4rcb == nullptr)
		return luaL_error(state, "SH-4 scheduler is not initialized");
	const std::string value = std::to_string(sh4_sched_now64());
	lua_pushlstring(state, value.data(), value.size());
	return 1;
}

static int researchRuntimeConfiguration(lua_State *state)
{
	if (lua_gettop(state) != 0)
		return luaL_error(state, "runtime_configuration expects no arguments");
	lua_newtable(state);
	setBooleanField(state, "dynarec_enabled", config::DynarecEnabled.get());
	setBooleanField(state, "dynarec_observation",
			config::ResearchDynarecObservation.get());
	setBooleanField(state, "threaded_rendering", config::ThreadedRendering.get());
	setStringField(state, "dreamcast_rtc_seed_decimal",
			std::to_string(config::ResearchDreamcastRtcSeed.get()));
	return 1;
}

static void luaRegister(lua_State *L)
{
	getGlobalNamespace(L)
		.beginNamespace ("flycast")
			.beginNamespace("research")
				.addCFunction("subscribe", researchSubscribe)
				.addCFunction("unsubscribe", researchUnsubscribe)
				.addCFunction("subscription_stats", researchSubscriptionStats)
				.addCFunction("current_sh4_tick_decimal", researchCurrentSh4TickDecimal)
				.addCFunction("runtime_configuration", researchRuntimeConfiguration)
			.endNamespace()
	  		.beginNamespace("emulator")
				.addFunction("startGame", gui_start_game)	// FIXME threading!
				.addFunction("stopGame", std::function<void()>([]() { gui_stop_game(""); }))
				.addFunction("pause", std::function<void()>([]() {
					if (gui_state == GuiState::Closed)
						gui_open_settings();
				}))
				.addFunction("resume", std::function<void()>([]() {
					if (gui_state == GuiState::Commands)
						gui_open_settings();
				}))
				.addFunction("saveState", std::function<void(int)>([](int index) {
					bool restart = false;
					if (gui_state == GuiState::Closed) {
						gui_open_settings();
						restart = true;
					}
					dc_savestate(index);
					if (restart)
						gui_open_settings();
				}))
				.addFunction("loadState", std::function<void(int)>([](int index) {
					bool restart = false;
					if (gui_state == GuiState::Closed) {
						gui_open_settings();
						restart = true;
					}
					dc_loadstate(index);
					if (restart)
						gui_open_settings();
				}))
				.addFunction("requestExit", mainui_stop)
				.addFunction("exit", dc_exit)
				.addFunction("displayNotification", os_notify)
			.endNamespace()

	  		.beginNamespace("config")
#define CONFIG_PROPERTY(Config, type) .addProperty<type>(#Config, get ## Config, set ## Config)
				.beginNamespace("general")
					CONFIG_PROPERTY(Cable, int)
					CONFIG_PROPERTY(Region, int)
					CONFIG_PROPERTY(Broadcast, int)
					CONFIG_PROPERTY(Language, int)
					CONFIG_PROPERTY(AutoLoadState, bool)
					CONFIG_PROPERTY(AutoSaveState, bool)
					CONFIG_PROPERTY(SavestateSlot, int)
					CONFIG_PROPERTY(HideLegacyNaomiRoms, bool)
				.endNamespace()

				.beginNamespace("video")
// FIXME			.addProperty<RenderType>("RendererType", getRendererType, setRendererType)
					CONFIG_PROPERTY(Widescreen, bool)
					CONFIG_PROPERTY(SuperWidescreen, bool)
					CONFIG_PROPERTY(UseMipmaps, bool)
					CONFIG_PROPERTY(ShowFPS, bool)
					CONFIG_PROPERTY(RenderToTextureBuffer, bool)
					CONFIG_PROPERTY(TranslucentPolygonDepthMask, bool)
					CONFIG_PROPERTY(ModifierVolumes, bool)
					CONFIG_PROPERTY(TextureUpscale, int)
					CONFIG_PROPERTY(MaxFilteredTextureSize, int)
					CONFIG_PROPERTY(ExtraDepthScale, float)
					CONFIG_PROPERTY(CustomTextures, bool)
					CONFIG_PROPERTY(DumpTextures, bool)
					CONFIG_PROPERTY(ScreenStretching, int)
					CONFIG_PROPERTY(Fog, bool)
					CONFIG_PROPERTY(FloatVMUs, bool)
					CONFIG_PROPERTY(Rotate90, bool)
					CONFIG_PROPERTY(PerStripSorting, bool)
					CONFIG_PROPERTY(DelayFrameSwapping, bool)
					CONFIG_PROPERTY(WidescreenGameHacks, bool)
					// TODO CrosshairColor;
					CONFIG_PROPERTY(SkipFrame, int)
					CONFIG_PROPERTY(MaxThreads, int)
					CONFIG_PROPERTY(AutoSkipFrame, int)
					CONFIG_PROPERTY(RenderResolution, int)
					CONFIG_PROPERTY(VSync, bool)
					CONFIG_PROPERTY(PixelBufferSize, u64)
					CONFIG_PROPERTY(AnisotropicFiltering, int)
					CONFIG_PROPERTY(TextureFiltering, int)
					CONFIG_PROPERTY(ThreadedRendering, bool)
				.endNamespace()

				.beginNamespace("audio")
					CONFIG_PROPERTY(DSPEnabled, bool)
					CONFIG_PROPERTY(AudioBufferSize, int)
					CONFIG_PROPERTY(AutoLatency, bool)
					CONFIG_PROPERTY(AudioBackend, std::string)
					CONFIG_PROPERTY(AudioVolume, int)
				.endNamespace()

				.beginNamespace("advanced")
					CONFIG_PROPERTY(DynarecEnabled, bool)
					CONFIG_PROPERTY(SerialConsole, bool)
					CONFIG_PROPERTY(SerialPTY, bool)
					CONFIG_PROPERTY(UseReios, bool)
					CONFIG_PROPERTY(FastGDRomLoad, bool)
					CONFIG_PROPERTY(OpenGlChecks, bool)
				.endNamespace()

				.beginNamespace("network")
					CONFIG_PROPERTY(NetworkEnable, bool)
					CONFIG_PROPERTY(ActAsServer, bool)
					CONFIG_PROPERTY(DNS, std::string)
					CONFIG_PROPERTY(NetworkServer, std::string)
					CONFIG_PROPERTY(EmulateBBA, bool)
					CONFIG_PROPERTY(GGPOEnable, bool)
					CONFIG_PROPERTY(GGPODelay, int)
					CONFIG_PROPERTY(NetworkStats, bool)
					CONFIG_PROPERTY(GGPOAnalogAxes, int)
				.endNamespace()

				.beginNamespace("maple")
					.addFunction("getDeviceType", getMapleType)
					.addFunction("getSubDeviceType", getMapleSubType)
					.addFunction("setDeviceType", setMapleType)
					.addFunction("setSubDeviceType", setMapleSubType)
				.endNamespace()
			.endNamespace()

	  		.beginNamespace("memory")
				.addFunction("read8", addrspace::readt<u8>)
				.addFunction("read16", addrspace::readt<u16>)
				.addFunction("read32", addrspace::readt<u32>)
				.addFunction("read64", addrspace::readt<u64>)
				.addFunction("readTable8", readMemoryTable<u8>)
				.addFunction("readTable16", readMemoryTable<u16>)
				.addFunction("readTable32", readMemoryTable<u32>)
				.addFunction("readTable64", readMemoryTable<u64>)
				.addFunction("write8", addrspace::writet<u8>)
				.addFunction("write16", addrspace::writet<u16>)
				.addFunction("write32", addrspace::writet<u32>)
				.addFunction("write64", addrspace::writet<u64>)
			.endNamespace()

			.beginNamespace("input")
				.addFunction("getButtons", getButtons)
				.addFunction("pressButtons", pressButtons)
				.addFunction("releaseButtons", releaseButtons)
				.addFunction("getAxis", getAxis)
				.addFunction("setAxis", setAxis)
				.addFunction("getAbsCoordinates", getAbsCoordinates)
				.addFunction("setAbsCoordinates", setAbsCoordinates)
				.addFunction("getRelCoordinates", getRelCoordinates)
				.addFunction("setRelCoordinates", setRelCoordinates)
			.endNamespace()

			.beginNamespace("state")
				.addProperty("system", &settings.platform.system, false)
				.addProperty("media", &settings.content.path, false)
				.addProperty("gameId", &settings.content.gameId, false)
				.beginNamespace("display")
					.addProperty("width", &settings.display.width, false)
					.addProperty("height", &settings.display.height, false)
				.endNamespace()
			.endNamespace()

			.beginNamespace("ui")
				.addFunction("beginWindow", beginWindow)
				.addFunction("endWindow", endWindow)
				.addFunction("text", uiText)
				.addFunction("rightText", uiTextRightAligned)
				.addFunction("bargraph", uiBargraph)
				.addFunction("button", uiButton)
			.endNamespace()
		.endNamespace();
}

static std::string getLuaFile()
{
	std::string initFile;
	if( !config::LuaFileName.get().empty()){
		initFile = get_readonly_config_path(config::LuaFileName.get());
	} else {
		initFile = get_readonly_config_path("flycast.lua");
	}

	return initFile;

}

static void doExec(const std::string& path)
{
	if (L == nullptr)
		return;
	DEBUG_LOG(COMMON, "Executing script: %s", path.c_str());
	int err = luaL_dofile(L, path.c_str());
	if (err != 0)
		WARN_LOG(COMMON, "Lua error: %s", lua_tostring(L, -1));
}

void exec(const std::string& path)
{
	std::string file = get_readonly_config_path(path);
	if (!file_exists(file))
		return;
	lock_guard lock(mutex);
	clearResearchSubscriptions();
	researchSubscriptionsAllowed = true;
	doExec(file);
}

void init()
{
	std::string initFile = getLuaFile();
	if (!file_exists(initFile))
		return;
	L = luaL_newstate();
	luaL_openlibs(L);
	researchSubscriptions = std::make_unique<research::Sh4LuaSubscriptionQueue>();
	researchSubscriptionsAllowed = true;
	luaRegister(L);
    EventManager::listen(Event::Start, emuEventCallback);
    EventManager::listen(Event::Resume, emuEventCallback);
    EventManager::listen(Event::Pause, emuEventCallback);
    EventManager::listen(Event::Terminate, emuEventCallback);
    EventManager::listen(Event::LoadState, emuEventCallback);
    EventManager::listen(Event::VBlank, emuEventCallback);
    EventManager::listen(Event::Network, emuEventCallback);

	doExec(initFile);
}

void term()
{
	if (L == nullptr)
		return;
	lock_guard lock(mutex);
	researchSubscriptionsAllowed = false;
	clearResearchSubscriptions();
    EventManager::unlisten(Event::Start, emuEventCallback);
    EventManager::unlisten(Event::Resume, emuEventCallback);
    EventManager::unlisten(Event::Pause, emuEventCallback);
    EventManager::unlisten(Event::Terminate, emuEventCallback);
    EventManager::unlisten(Event::LoadState, emuEventCallback);
    EventManager::unlisten(Event::VBlank, emuEventCallback);
    EventManager::unlisten(Event::Network, emuEventCallback);
	lua_close(L);
	L = nullptr;
	researchSubscriptions.reset();
}

}
#endif

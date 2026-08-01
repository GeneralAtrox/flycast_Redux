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
#include "research/maple_observation.h"
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

static std::string hexadecimalBytes(const std::vector<std::uint8_t>& bytes)
{
	static constexpr char Digits[] = "0123456789abcdef";
	std::string text(bytes.size() * 2, '0');
	for (std::size_t index = 0; index < bytes.size(); ++index)
	{
		text[index * 2] = Digits[bytes[index] >> 4];
		text[index * 2 + 1] = Digits[bytes[index] & 0x0f];
	}
	return text;
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

static void luaRegister(lua_State *L)
{
	getGlobalNamespace(L)
		.beginNamespace ("flycast")
			.beginNamespace("research")
				.addCFunction("subscribe", researchSubscribe)
				.addCFunction("unsubscribe", researchUnsubscribe)
				.addCFunction("subscription_stats", researchSubscriptionStats)
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

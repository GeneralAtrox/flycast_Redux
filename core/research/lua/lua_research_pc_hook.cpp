// flycast.research.pc_hook({pc, ..}, function(pc) .. end): the function runs
// on the emulator thread just before each hooked instruction, with the Lua
// state locked, so flycast.memory reads see guest memory at that instruction
// (research/sh4_pc_hook.h). Set it at the script's top level, before the game
// starts; one hook set per script (a later call replaces it).

#include "research/lua/lua_research_internal.h"

#include "emulator.h"
#include "log/Log.h"
#include "lua/lua.h"
#include "research/sh4_pc_hook.h"

#include <cmath>
#include <stdexcept>

namespace research::lua_api
{

static int pcHookRef = LUA_NOREF;

static void callPcHook(u32 pc)
{
	lua_State *state = researchLuaState;
	if (state == nullptr || pcHookRef == LUA_NOREF)
		return;
	lua_rawgeti(state, LUA_REGISTRYINDEX, pcHookRef);
	lua_pushnumber(state, static_cast<lua_Number>(pc));
	if (lua_pcall(state, 1, 0, 0) != 0)
	{
		const char *message = lua_tostring(state, -1);
		WARN_LOG(COMMON, "Lua pc_hook at %08x: %s", pc, message != nullptr ? message : "error");
		lua_pop(state, 1);
	}
}

int researchPcHook(lua_State *state)
{
	try
	{
		if (state != researchLuaState)
			throw std::runtime_error("research pc_hook is not available");
		if (emu.running())
			throw std::runtime_error("pc_hook must be set before the game starts");
		if (lua_gettop(state) != 2 || !lua_istable(state, 1) || !lua_isfunction(state, 2))
			throw std::invalid_argument("pc_hook expects a table of PCs and a function");
		const std::size_t count = lua_rawlen(state, 1);
		if (count == 0 || count > SH4_PC_HOOK_MAX)
			throw std::invalid_argument("pc_hook takes 1 to 8 PCs");
		u32 pcs[SH4_PC_HOOK_MAX];
		for (std::size_t i = 0; i < count; i++)
		{
			lua_rawgeti(state, 1, static_cast<int>(i + 1));
			const lua_Number value = lua_isnumber(state, -1) ? lua_tonumber(state, -1) : -1;
			lua_pop(state, 1);
			if (!(value >= 0 && value <= 4294967295.0) || std::floor(value) != value || (static_cast<u32>(value) & 1))
				throw std::invalid_argument("a PC must be an even 32-bit integer");
			pcs[i] = static_cast<u32>(value);
		}
		pcHookClear();
		lua_pushvalue(state, 2);
		pcHookRef = luaL_ref(state, LUA_REGISTRYINDEX);
		sh4PcHookSet(pcs, static_cast<unsigned>(count), [](u32 pc) {
			lua::withLock([pc] { callPcHook(pc); });
		});
		return 0;
	}
	catch (const std::exception& exception)
	{
		return luaL_error(state, "%s", exception.what());
	}
}

void pcHookClear() noexcept
{
	sh4PcHookClear();
	if (researchLuaState != nullptr && pcHookRef != LUA_NOREF)
		luaL_unref(researchLuaState, LUA_REGISTRYINDEX, pcHookRef);
	pcHookRef = LUA_NOREF;
}

} // namespace research::lua_api

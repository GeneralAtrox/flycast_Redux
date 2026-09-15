#pragma once

// The flycast.research Lua discovery API, kept out of core/lua/lua.cpp.
// lua.cpp owns the Lua mutex and calls these at the matching seam points;
// every function here expects that lock to already be held except init().

struct lua_State;

namespace research::lua_api
{

// Called from lua::init() before the namespace is registered. Creates the
// subscription queue on the calling (owner) thread and stores the state the
// delivery thunks re-enter.
void init(lua_State *state);
// Called from luaRegister(). Adds flycast.research to an existing state.
void registerNamespace(lua_State *state);
// Drops every native subscription and unrefs the Lua callbacks.
void clear() noexcept;
// Subscribing is refused while disallowed (between Terminate and Start).
void setAllowed(bool allowed) noexcept;
// Delivers queued observations. Owner thread only; called once per frame.
void drain() noexcept;
// Called from lua::term() after lua_close().
void term() noexcept;

} // namespace research::lua_api

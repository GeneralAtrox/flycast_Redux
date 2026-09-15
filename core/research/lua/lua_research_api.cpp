#include "research/lua/lua_research_api.h"

#include "research/lua/lua_research_internal.h"

#include "cfg/option.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_sched.h"
#include "log/Log.h"

#include <LuaBridge/LuaBridge.h>

#include <stdexcept>
#include <utility>

namespace research::lua_api
{

lua_State *researchLuaState = nullptr;
std::unique_ptr<Sh4LuaSubscriptionQueue> researchSubscriptions;
std::unordered_map<Sh4LuaSubscriptionQueue::Token, int> researchCallbackRefs;
bool researchSubscriptionsAllowed = false;

static int researchSubscribe(lua_State *state)
{
	try
	{
		if (state != researchLuaState || researchSubscriptions == nullptr
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

void init(lua_State *state)
{
	researchLuaState = state;
	researchSubscriptions = std::make_unique<Sh4LuaSubscriptionQueue>();
	researchSubscriptionsAllowed = true;
}

void registerNamespace(lua_State *state)
{
	luabridge::getGlobalNamespace(state)
		.beginNamespace("flycast")
			.beginNamespace("research")
				.addCFunction("subscribe", researchSubscribe)
				.addCFunction("unsubscribe", researchUnsubscribe)
				.addCFunction("subscription_stats", researchSubscriptionStats)
				.addCFunction("current_sh4_tick_decimal", researchCurrentSh4TickDecimal)
				.addCFunction("runtime_configuration", researchRuntimeConfiguration)
			.endNamespace()
		.endNamespace();
}

void clear() noexcept
{
	try
	{
		if (researchSubscriptions != nullptr)
			researchSubscriptions->clear();
		if (researchLuaState != nullptr)
		{
			for (const auto& callback : researchCallbackRefs)
				luaL_unref(researchLuaState, LUA_REGISTRYINDEX, callback.second);
		}
		researchCallbackRefs.clear();
	}
	catch (...)
	{
	}
}

void setAllowed(bool allowed) noexcept
{
	researchSubscriptionsAllowed = allowed;
}

void drain() noexcept
{
	if (researchLuaState == nullptr || researchSubscriptions == nullptr)
		return;
	try
	{
		researchSubscriptions->drain();
	}
	catch (const std::exception& exception)
	{
		WARN_LOG(COMMON, "Lua research delivery failed: %s", exception.what());
	}
}

void term() noexcept
{
	researchSubscriptions.reset();
	researchLuaState = nullptr;
}

} // namespace research::lua_api

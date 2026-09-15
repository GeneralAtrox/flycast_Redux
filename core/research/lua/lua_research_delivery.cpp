#include "research/lua/lua_research_internal.h"

#include "cfg/option.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_sched.h"
#include "log/Log.h"
#include "research/sha256.h"

#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace research::lua_api
{

void deliverResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::Sh4Observation& observation)
{
	const auto found = researchCallbackRefs.find(token);
	if (found == researchCallbackRefs.end())
		return;
	lua_rawgeti(researchLuaState, LUA_REGISTRYINDEX, found->second);
	pushResearchEvent(researchLuaState, observation);
	if (lua_pcall(researchLuaState, 1, 0, 0) != 0)
	{
		const char *message = lua_tostring(researchLuaState, -1);
		const std::string failure = message == nullptr
				? "unknown Lua callback error" : message;
		lua_pop(researchLuaState, 1);
		throw std::runtime_error(failure);
	}
}

void deliverMapleResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::MapleObservation& observation)
{
	const auto found = researchCallbackRefs.find(token);
	if (found == researchCallbackRefs.end())
		return;
	lua_rawgeti(researchLuaState, LUA_REGISTRYINDEX, found->second);
	pushMapleResearchEvent(researchLuaState, observation);
	if (lua_pcall(researchLuaState, 1, 0, 0) != 0)
	{
		const char *message = lua_tostring(researchLuaState, -1);
		const std::string failure = message == nullptr
				? "unknown Lua callback error" : message;
		lua_pop(researchLuaState, 1);
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
	lua_rawgeti(researchLuaState, LUA_REGISTRYINDEX, found->second);
	push(researchLuaState, observation);
	if (lua_pcall(researchLuaState, 1, 0, 0) != 0)
	{
		const char *message = lua_tostring(researchLuaState, -1);
		const std::string failure = message == nullptr
				? "unknown Lua callback error" : message;
		lua_pop(researchLuaState, 1);
		throw std::runtime_error(failure);
	}
}

void deliverPvrTaResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::Sh4LuaSubscriptionQueue::PvrTaLuaObservation& observation)
{
	deliverTypedResearchObservation(token, observation, pushPvrTaResearchEvent);
}

void deliverPvrPresentationResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation& observation)
{
	deliverTypedResearchObservation(token, observation,
			pushPvrPresentationResearchEvent);
}

void deliverPvrDrawResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::PvrDrawObservation& observation)
{
	deliverTypedResearchObservation(token, observation, pushPvrDrawResearchEvent);
}

void deliverGdromResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::GdromObservation& observation)
{
	deliverTypedResearchObservation(token, observation, pushGdromResearchEvent);
}

void deliverAicaResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::AicaObservation& observation)
{
	deliverTypedResearchObservation(token, observation, pushAicaResearchEvent);
}

void deliverCddaResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::CddaObservation& observation)
{
	deliverTypedResearchObservation(token, observation, pushCddaResearchEvent);
}

void reportResearchCallbackFailure(
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

} // namespace research::lua_api

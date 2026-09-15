#pragma once

// Shared declarations for the flycast.research Lua discovery API. These are
// implementation details of core/research/lua; nothing outside that directory
// should include this header.

#include "research/aica_observation.h"
#include "research/cdda_observation.h"
#include "research/gdrom_observation.h"
#include "research/maple_observation.h"
#include "research/pvr_draw_observation.h"
#include "research/pvr_presentation_observation.h"
#include "research/pvr_ta_observation.h"
#include "research/sh4_lua_subscriptions.h"
#include "research/sh4_observation.h"
#include "research/sh4_observation_runtime.h"

#include <lua.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace research::lua_api
{

// The Lua state the research API was registered on. Delivery thunks re-enter
// Lua through it and subscribe() rejects calls from any other state.
extern lua_State *researchLuaState;
extern std::unique_ptr<Sh4LuaSubscriptionQueue> researchSubscriptions;
extern std::unordered_map<Sh4LuaSubscriptionQueue::Token, int> researchCallbackRefs;
extern bool researchSubscriptionsAllowed;

const char *observationTypeName(research::Sh4ObservationType type);

const char *backendName(research::Sh4ObservationBackend backend);

void setStringField(lua_State *state, const char *name,
		const std::string& value);

void setStringField(lua_State *state, const char *name, const char *value);

void setNumberField(lua_State *state, const char *name, std::uint64_t value);

void setSignedNumberField(lua_State *state, const char *name,
		std::int64_t value);

void setFloatField(lua_State *state, const char *name, float value);

void setBooleanField(lua_State *state, const char *name, bool value);

std::string hexadecimal64(std::uint64_t value);

std::string hexadecimalBytes(const std::uint8_t *bytes, std::size_t size);

std::string hexadecimalBytes(const std::vector<std::uint8_t>& bytes);

void setBytesFields(lua_State *state, const char *bytesName,
		const char *hexName, const std::uint8_t *bytes, std::size_t size);

void pushSh4Owner(lua_State *state,
		const research::Sh4InstructionOwnerToken& owner);

void pushRegisterSnapshot(lua_State *state,
		const research::Sh4RegisterSnapshot& registers);

void pushResearchEvent(lua_State *state,
		const research::Sh4Observation& observation);

const char *mapleObservationTypeName(research::MapleObservationType type);

void pushMapleResearchEvent(lua_State *state,
		const research::MapleObservation& observation);

void pushDiscoveryBase(lua_State *state, const char *event,
		std::uint32_t schemaVersion, std::uint64_t ordinal, std::uint64_t tick);

const char *pvrTaEventName(research::PvrTaObservationType type);

void pushPvrTaResearchEvent(lua_State *state,
		const research::Sh4LuaSubscriptionQueue::PvrTaLuaObservation& queued);

const char *pvrPresentationEventName(
		research::PvrPresentationObservationType type);

void pushPvrPresentationResearchEvent(lua_State *state,
		const research::Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation& queued);

const char *pvrDrawEventName(research::PvrDrawObservationType type);

void pushPvrTaBlockProvenance(lua_State *state,
		const std::vector<research::PvrTaBlockProvenance>& blocks);

void pushPvrDrawResearchEvent(lua_State *state,
		const research::PvrDrawObservation& observation);

const char *gdromEventName(research::GdromObservationType type);

void pushGdromResearchEvent(lua_State *state,
		const research::GdromObservation& observation);

const char *cddaEventName(research::CddaObservationType type);

void pushCddaDriveState(lua_State *state,
		const research::CddaDriveState& drive);

void pushCddaResearchEvent(lua_State *state,
		const research::CddaObservation& observation);

const char *aicaEventName(research::AicaObservationType type);

void pushAicaResearchEvent(lua_State *state,
		const research::AicaObservation& observation);

std::optional<std::uint64_t> optionalUnsignedTableField(lua_State *state,
		int tableIndex, const char *name);

std::optional<bool> optionalBooleanTableField(lua_State *state,
		int tableIndex, const char *name);

std::optional<std::string> optionalStringTableField(lua_State *state,
		int tableIndex, const char *name);

std::vector<std::uint32_t> optionalU32ArrayTableField(lua_State *state,
		int tableIndex, const char *name, std::size_t maximumCount);

bool tableFieldPresent(lua_State *state, int tableIndex, const char *name);

research::Sh4ObservationFilter researchFilterFromLua(lua_State *state,
		std::size_t& capacity);

research::MapleObservationFilter mapleResearchFilterFromLua(lua_State *state,
		const std::string& event, std::size_t& capacity);

std::size_t researchQueueCapacity(lua_State *state);

research::Sh4LuaSubscriptionQueue::PvrTaFilter pvrTaResearchFilterFromLua(
		lua_State *state, const std::string& event, std::size_t& capacity);

research::Sh4LuaSubscriptionQueue::PvrPresentationFilter
pvrPresentationResearchFilterFromLua(lua_State *state, const std::string& event,
		std::size_t& capacity);

research::Sh4LuaSubscriptionQueue::PvrDrawFilter
pvrDrawResearchFilterFromLua(lua_State *state, const std::string& event,
		std::size_t& capacity);

research::Sh4LuaSubscriptionQueue::GdromFilter gdromResearchFilterFromLua(
		lua_State *state, const std::string& event, std::size_t& capacity);

research::Sh4LuaSubscriptionQueue::AicaFilter aicaResearchFilterFromLua(
		lua_State *state, const std::string& event, std::size_t& capacity);

research::Sh4LuaSubscriptionQueue::CddaFilter cddaResearchFilterFromLua(
		lua_State *state, const std::string& event, std::size_t& capacity);

void deliverResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::Sh4Observation& observation);

void deliverMapleResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::MapleObservation& observation);

void deliverPvrTaResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::Sh4LuaSubscriptionQueue::PvrTaLuaObservation& observation);

void deliverPvrPresentationResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation& observation);

void deliverPvrDrawResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::PvrDrawObservation& observation);

void deliverGdromResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::GdromObservation& observation);

void deliverAicaResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::AicaObservation& observation);

void deliverCddaResearchObservation(
		research::Sh4LuaSubscriptionQueue::Token token,
		const research::CddaObservation& observation);

void reportResearchCallbackFailure(
		research::Sh4LuaSubscriptionQueue::Token token,
		std::exception_ptr failure) noexcept;

} // namespace research::lua_api

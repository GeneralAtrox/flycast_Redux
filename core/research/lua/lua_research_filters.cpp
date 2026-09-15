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

std::optional<std::uint64_t> optionalUnsignedTableField(lua_State *state,
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

std::optional<bool> optionalBooleanTableField(lua_State *state,
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

std::optional<std::string> optionalStringTableField(lua_State *state,
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

std::vector<std::uint32_t> optionalU32ArrayTableField(lua_State *state,
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

bool tableFieldPresent(lua_State *state, int tableIndex, const char *name)
{
	tableIndex = lua_absindex(state, tableIndex);
	lua_getfield(state, tableIndex, name);
	const bool present = !lua_isnil(state, -1);
	lua_pop(state, 1);
	return present;
}

research::Sh4ObservationFilter researchFilterFromLua(lua_State *state,
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

research::MapleObservationFilter mapleResearchFilterFromLua(lua_State *state,
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

std::size_t researchQueueCapacity(lua_State *state)
{
	const auto requested = optionalUnsignedTableField(state, 1, "queue_capacity");
	return requested.has_value() ? static_cast<std::size_t>(*requested)
			: research::Sh4LuaSubscriptionQueue::DefaultCapacity;
}

} // namespace research::lua_api

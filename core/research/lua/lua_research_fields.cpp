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

const char *observationTypeName(research::Sh4ObservationType type)
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

const char *backendName(research::Sh4ObservationBackend backend)
{
	return backend == research::Sh4ObservationBackend::Interpreter
			? "interpreter" : "dynarec";
}

void setStringField(lua_State *state, const char *name,
		const std::string& value)
{
	lua_pushlstring(state, value.data(), value.size());
	lua_setfield(state, -2, name);
}

void setStringField(lua_State *state, const char *name, const char *value)
{
	lua_pushstring(state, value);
	lua_setfield(state, -2, name);
}

void setNumberField(lua_State *state, const char *name, std::uint64_t value)
{
	lua_pushnumber(state, static_cast<lua_Number>(value));
	lua_setfield(state, -2, name);
}

void setSignedNumberField(lua_State *state, const char *name,
		std::int64_t value)
{
	lua_pushnumber(state, static_cast<lua_Number>(value));
	lua_setfield(state, -2, name);
}

void setFloatField(lua_State *state, const char *name, float value)
{
	lua_pushnumber(state, static_cast<lua_Number>(value));
	lua_setfield(state, -2, name);
}

void setBooleanField(lua_State *state, const char *name, bool value)
{
	lua_pushboolean(state, value ? 1 : 0);
	lua_setfield(state, -2, name);
}

std::string hexadecimal64(std::uint64_t value)
{
	char text[19] {};
	std::snprintf(text, sizeof(text), "0x%016llx",
			static_cast<unsigned long long>(value));
	return text;
}

std::string hexadecimalBytes(const std::uint8_t *bytes, std::size_t size)
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

std::string hexadecimalBytes(const std::vector<std::uint8_t>& bytes)
{
	return hexadecimalBytes(bytes.data(), bytes.size());
}

void setBytesFields(lua_State *state, const char *bytesName,
		const char *hexName, const std::uint8_t *bytes, std::size_t size)
{
	const char *payload = size == 0 ? "" : reinterpret_cast<const char *>(bytes);
	lua_pushlstring(state, payload, size);
	lua_setfield(state, -2, bytesName);
	setStringField(state, hexName, hexadecimalBytes(bytes, size));
}

} // namespace research::lua_api

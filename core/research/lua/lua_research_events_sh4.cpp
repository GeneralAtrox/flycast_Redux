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

void pushSh4Owner(lua_State *state,
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

void pushRegisterSnapshot(lua_State *state,
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

void pushResearchEvent(lua_State *state,
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

const char *mapleObservationTypeName(research::MapleObservationType type)
{
	return type == research::MapleObservationType::Request
			? "maple-request" : "maple-response";
}

void pushMapleResearchEvent(lua_State *state,
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

void pushDiscoveryBase(lua_State *state, const char *event,
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

} // namespace research::lua_api

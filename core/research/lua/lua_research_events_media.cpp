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

const char *gdromEventName(research::GdromObservationType type)
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

void pushGdromResearchEvent(lua_State *state,
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

const char *cddaEventName(research::CddaObservationType type)
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

void pushCddaDriveState(lua_State *state,
		const research::CddaDriveState& drive)
{
	lua_newtable(state);
	setNumberField(state, "status", drive.status);
	setNumberField(state, "repeats", drive.repeats);
	setNumberField(state, "current_fad", drive.currentFad);
	setNumberField(state, "start_fad", drive.startFad);
	setNumberField(state, "end_fad", drive.endFad);
}

void pushCddaResearchEvent(lua_State *state,
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

const char *aicaEventName(research::AicaObservationType type)
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

void pushAicaResearchEvent(lua_State *state,
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

} // namespace research::lua_api

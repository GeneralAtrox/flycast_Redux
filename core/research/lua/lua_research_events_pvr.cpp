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

const char *pvrTaEventName(research::PvrTaObservationType type)
{
	switch (type)
	{
	case research::PvrTaObservationType::ListInit: return "pvr-ta-list-init";
	case research::PvrTaObservationType::ListContinue: return "pvr-ta-list-continue";
	case research::PvrTaObservationType::AcceptedBlock: return "pvr-ta-block";
	case research::PvrTaObservationType::StartRender: return "pvr-start-render";
	case research::PvrTaObservationType::RenderDone: return "pvr-render-done";
	case research::PvrTaObservationType::Reset: return "pvr-ta-reset";
	}
	return "pvr-ta-unknown";
}

void pushPvrTaResearchEvent(lua_State *state,
		const research::Sh4LuaSubscriptionQueue::PvrTaLuaObservation& queued)
{
	const research::PvrTaObservation& observation = queued.observation;
	pushDiscoveryBase(state, pvrTaEventName(observation.type),
			observation.schemaVersion, observation.emissionOrdinal, observation.tick);
	setStringField(state, "guest_snapshot_tick_decimal",
			std::to_string(queued.guestSnapshotTick));
	lua_newtable(state);
	for (std::size_t index = 0; index < queued.guestU32Snapshot.size(); ++index)
	{
		const auto& value = queued.guestU32Snapshot[index];
		lua_newtable(state);
		setNumberField(state, "address", value.address);
		setBooleanField(state, "available", value.available);
		if (value.available)
			setNumberField(state, "value", value.value);
		lua_rawseti(state, -2, static_cast<int>(index + 1));
	}
	lua_setfield(state, -2, "guest_u32_snapshot");
	pushSh4Owner(state, observation.initiator);
	lua_setfield(state, -2, "initiator");
	setNumberField(state, "context_address", observation.contextAddress);
	setNumberField(state, "context_generation", observation.contextGeneration);
	setNumberField(state, "context_block_ordinal", observation.contextBlockOrdinal);
	setNumberField(state, "render_pass", observation.renderPass);
	setNumberField(state, "list_type_before", observation.listTypeBefore);
	setNumberField(state, "list_type_after", observation.listTypeAfter);
	setNumberField(state, "parser_state_before", observation.parserStateBefore);
	setNumberField(state, "parser_state_after", observation.parserStateAfter);
	if (observation.type == research::PvrTaObservationType::AcceptedBlock)
	{
		const char *source = observation.source == research::PvrTaInputSource::StoreQueue
				? "store-queue" : observation.source
						== research::PvrTaInputSource::Channel2Dma
						? "channel2-dma" : "sort-dma";
		setStringField(state, "source", source);
		setNumberField(state, "source_address", observation.sourceAddress);
		setNumberField(state, "ta_address", observation.taAddress);
		setBytesFields(state, "block", "block_hex", observation.block.data(),
				observation.block.size());
	}
	if (observation.type == research::PvrTaObservationType::StartRender
			|| observation.type == research::PvrTaObservationType::RenderDone)
		setNumberField(state, "render_generation", observation.renderGeneration);
	if (observation.type == research::PvrTaObservationType::StartRender)
	{
		setBooleanField(state, "render_context_available",
				observation.renderContextAvailable);
		setNumberField(state, "region_base", observation.regionBase);
		setNumberField(state, "fpu_param_cfg", observation.fpuParamCfg);
		lua_newtable(state);
		for (std::size_t index = 0; index < observation.selectedContexts.size(); ++index)
		{
			const auto& context = observation.selectedContexts[index];
			lua_newtable(state);
			setNumberField(state, "address", context.address);
			setNumberField(state, "generation", context.generation);
			setBooleanField(state, "available", context.available);
			lua_rawseti(state, -2, static_cast<int>(index + 1));
		}
		lua_setfield(state, -2, "selected_contexts");
	}
}

const char *pvrPresentationEventName(
		research::PvrPresentationObservationType type)
{
	switch (type)
	{
	case research::PvrPresentationObservationType::RegisterWrite: return "pvr-register-write";
	case research::PvrPresentationObservationType::VramWrite: return "pvr-vram-write";
	case research::PvrPresentationObservationType::RenderQueued: return "pvr-render-queued";
	case research::PvrPresentationObservationType::RenderCompleted: return "pvr-render-completed";
	case research::PvrPresentationObservationType::FramebufferCaptured: return "pvr-framebuffer";
	case research::PvrPresentationObservationType::Presentation: return "pvr-presentation";
	case research::PvrPresentationObservationType::Reset: return "pvr-presentation-reset";
	case research::PvrPresentationObservationType::InitialRegisterState:
		return "pvr-initial-register-state";
	}
	return "pvr-presentation-unknown";
}

void pushPvrPresentationResearchEvent(lua_State *state,
		const research::Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation& queued)
{
	const research::PvrPresentationObservation& observation = queued.observation;
	pushDiscoveryBase(state, pvrPresentationEventName(observation.type),
			observation.schemaVersion, observation.emissionOrdinal, observation.tick);
	setStringField(state, "guest_snapshot_tick_decimal",
			std::to_string(queued.guestSnapshotTick));
	lua_newtable(state);
	for (std::size_t index = 0; index < queued.guestU32Snapshot.size(); ++index)
	{
		const auto& value = queued.guestU32Snapshot[index];
		lua_newtable(state);
		setNumberField(state, "address", value.address);
		setBooleanField(state, "available", value.available);
		if (value.available)
			setNumberField(state, "value", value.value);
		lua_rawseti(state, -2, static_cast<int>(index + 1));
	}
	lua_setfield(state, -2, "guest_u32_snapshot");
	setBooleanField(state, "guest_r15_available", queued.guestR15Available);
	if (queued.guestR15Available)
		setNumberField(state, "guest_r15", queued.guestR15);
	lua_newtable(state);
	for (std::size_t index = 0; index < queued.guestR15U32Snapshot.size(); ++index)
	{
		const auto& value = queued.guestR15U32Snapshot[index];
		lua_newtable(state);
		setNumberField(state, "address", value.address);
		setBooleanField(state, "available", value.available);
		if (value.available)
			setNumberField(state, "value", value.value);
		lua_rawseti(state, -2, static_cast<int>(index + 1));
	}
	lua_setfield(state, -2, "guest_r15_u32_snapshot");
	pushSh4Owner(state, observation.initiator);
	lua_setfield(state, -2, "initiator");
	setNumberField(state, "render_generation", observation.renderGeneration);
	setNumberField(state, "framebuffer_generation", observation.framebufferGeneration);
	setNumberField(state, "presentation_generation", observation.presentationGeneration);
	setNumberField(state, "source_generation", observation.sourceGeneration);
	setBooleanField(state, "successful", observation.successful);
	if (observation.type == research::PvrPresentationObservationType::RegisterWrite)
	{
		setNumberField(state, "physical_address", observation.registerPhysicalAddress);
		setNumberField(state, "register_address", observation.registerAddress);
		setNumberField(state, "requested_value", observation.requestedValue);
		setNumberField(state, "previous_value", observation.previousValue);
		setNumberField(state, "effective_value", observation.effectiveValue);
		setNumberField(state, "disposition", static_cast<unsigned>(observation.registerDisposition));
	}
	else if (observation.type == research::PvrPresentationObservationType::RenderQueued
			|| observation.type
					== research::PvrPresentationObservationType::RenderCompleted)
	{
		setNumberField(state, "render_kind",
				static_cast<unsigned>(observation.renderKind));
		setNumberField(state, "framebuffer_write_address",
				observation.framebufferWriteAddress);
	}
	else if (observation.type == research::PvrPresentationObservationType::VramWrite)
	{
		setNumberField(state, "source", static_cast<unsigned>(observation.vramSource));
		setNumberField(state, "logical_address", observation.logicalAddress);
		setNumberField(state, "physical_address", observation.physicalAddress);
		setBytesFields(state, "bytes", "bytes_hex", observation.bytes.data(),
				observation.bytes.size());
	}
	else if (observation.type
			== research::PvrPresentationObservationType::FramebufferCaptured)
	{
		setNumberField(state, "source_render_generation",
				observation.framebufferSourceRenderGeneration);
		setNumberField(state, "framebuffer_kind",
				static_cast<unsigned>(observation.framebufferKind));
		setNumberField(state, "width", observation.framebufferWidth);
		setNumberField(state, "height", observation.framebufferHeight);
		setNumberField(state, "row_bytes", observation.framebufferRowBytes);
		setNumberField(state, "fb_read_size",
				observation.framebufferConfig.fbReadSize);
		setNumberField(state, "fb_read_control",
				observation.framebufferConfig.fbReadControl);
		setNumberField(state, "spg_control",
				observation.framebufferConfig.spgControl);
		setNumberField(state, "spg_status",
				observation.framebufferConfig.spgStatus);
		setNumberField(state, "fb_read_sof1",
				observation.framebufferConfig.fbReadSof1);
		setNumberField(state, "fb_read_sof2",
				observation.framebufferConfig.fbReadSof2);
		setNumberField(state, "video_control",
				observation.framebufferConfig.videoControl);
		setNumberField(state, "border_color",
				observation.framebufferConfig.borderColor);
		if (observation.framebufferDigestAvailable)
			setStringField(state, "bytes_sha256",
					research::sha256ToHex(observation.framebufferDigest));
		else
			setBytesFields(state, "bytes", "bytes_hex", observation.bytes.data(),
					observation.bytes.size());
	}
	else if (observation.type == research::PvrPresentationObservationType::Presentation)
		setNumberField(state, "presentation_source",
				static_cast<unsigned>(observation.presentationSource));
}

const char *pvrDrawEventName(research::PvrDrawObservationType type)
{
	switch (type)
	{
	case research::PvrDrawObservationType::PrimitiveDecoded: return "pvr-primitive";
	case research::PvrDrawObservationType::DrawConsumed: return "pvr-draw";
	case research::PvrDrawObservationType::RenderCompleted: return "pvr-draw-render-completed";
	case research::PvrDrawObservationType::Reset: return "pvr-draw-reset";
	}
	return "pvr-draw-unknown";
}

void pushPvrTaBlockProvenance(lua_State *state,
		const std::vector<research::PvrTaBlockProvenance>& blocks)
{
	lua_newtable(state);
	for (std::size_t index = 0; index < blocks.size(); ++index)
	{
		const auto& block = blocks[index];
		lua_newtable(state);
		setBooleanField(state, "available", block.available);
		pushSh4Owner(state, block.initiator);
		lua_setfield(state, -2, "initiator");
		setNumberField(state, "context_address", block.contextAddress);
		setNumberField(state, "context_generation", block.contextGeneration);
		setNumberField(state, "context_block_ordinal", block.contextBlockOrdinal);
		setNumberField(state, "render_pass", block.renderPass);
		setNumberField(state, "source", static_cast<unsigned>(block.source));
		setNumberField(state, "source_address", block.sourceAddress);
		setNumberField(state, "ta_address", block.taAddress);
		lua_rawseti(state, -2, static_cast<int>(index + 1));
	}
}

void pushPvrDrawResearchEvent(lua_State *state,
		const research::PvrDrawObservation& observation)
{
	pushDiscoveryBase(state, pvrDrawEventName(observation.type),
			observation.schemaVersion, observation.emissionOrdinal, observation.tick);
	setNumberField(state, "render_generation", observation.renderGeneration);
	setNumberField(state, "primitive_generation", observation.primitiveGeneration);
	setNumberField(state, "raster_generation", observation.rasterGeneration);
	setNumberField(state, "context_address", observation.contextAddress);
	setNumberField(state, "context_generation", observation.contextGeneration);
	setNumberField(state, "render_pass", observation.renderPass);
	setNumberField(state, "list_type", observation.listType);
	setNumberField(state, "primitive_kind", static_cast<unsigned>(observation.primitiveKind));
	setNumberField(state, "owner_class", static_cast<unsigned>(observation.ownerClass));
	setNumberField(state, "pcw", observation.pcw);
	setNumberField(state, "isp", observation.isp);
	setNumberField(state, "tsp", observation.tsp);
	setNumberField(state, "tcw", observation.tcw);
	setNumberField(state, "tsp1", observation.tsp1);
	setNumberField(state, "tcw1", observation.tcw1);
	setNumberField(state, "tile_clip", observation.tileClip);
	setNumberField(state, "first", observation.first);
	setNumberField(state, "count", observation.count);
	setNumberField(state, "backend", static_cast<unsigned>(observation.backend));
	setNumberField(state, "draw_pass", static_cast<unsigned>(observation.drawPass));
	setBooleanField(state, "indexed", observation.indexed);
	setBooleanField(state, "successful", observation.successful);
	if (observation.bounds.available)
	{
		lua_newtable(state);
		setFloatField(state, "minimum_x", observation.bounds.minimumX);
		setFloatField(state, "minimum_y", observation.bounds.minimumY);
		setFloatField(state, "minimum_z", observation.bounds.minimumZ);
		setFloatField(state, "maximum_x", observation.bounds.maximumX);
		setFloatField(state, "maximum_y", observation.bounds.maximumY);
		setFloatField(state, "maximum_z", observation.bounds.maximumZ);
		lua_setfield(state, -2, "bounds");
	}
	if (observation.type == research::PvrDrawObservationType::PrimitiveDecoded)
	{
		pushPvrTaBlockProvenance(state, observation.parameterBlocks);
		lua_setfield(state, -2, "parameter_blocks");
		pushPvrTaBlockProvenance(state, observation.vertexBlocks);
		lua_setfield(state, -2, "vertex_blocks");
		lua_newtable(state);
		for (std::size_t index = 0; index < observation.vertices.size(); ++index)
		{
			const auto& vertex = observation.vertices[index];
			lua_newtable(state);
			setNumberField(state, "x_bits", vertex.xBits);
			setNumberField(state, "y_bits", vertex.yBits);
			setNumberField(state, "z_bits", vertex.zBits);
			setNumberField(state, "u_bits", vertex.uBits);
			setNumberField(state, "v_bits", vertex.vBits);
			setNumberField(state, "u1_bits", vertex.u1Bits);
			setNumberField(state, "v1_bits", vertex.v1Bits);
			setNumberField(state, "nx_bits", vertex.nxBits);
			setNumberField(state, "ny_bits", vertex.nyBits);
			setNumberField(state, "nz_bits", vertex.nzBits);
			setBytesFields(state, "base_color", "base_color_hex",
					vertex.baseColor.data(), vertex.baseColor.size());
			setBytesFields(state, "offset_color", "offset_color_hex",
					vertex.offsetColor.data(), vertex.offsetColor.size());
			setBytesFields(state, "base_color1", "base_color1_hex",
					vertex.baseColor1.data(), vertex.baseColor1.size());
			setBytesFields(state, "offset_color1", "offset_color1_hex",
					vertex.offsetColor1.data(), vertex.offsetColor1.size());
			lua_rawseti(state, -2, static_cast<int>(index + 1));
		}
		lua_setfield(state, -2, "vertices");
		if (observation.sampledTexture.available)
		{
			const auto& texture = observation.sampledTexture;
			lua_newtable(state);
			setNumberField(state, "source_address", texture.sourceAddress);
			setNumberField(state, "source_size", texture.sourceSize);
			setNumberField(state, "maximum_level_address",
					texture.maximumLevelAddress);
			setNumberField(state, "maximum_level_size", texture.maximumLevelSize);
			setNumberField(state, "width", texture.width);
			setNumberField(state, "height", texture.height);
			setNumberField(state, "pixel_format", texture.pixelFormat);
			setNumberField(state, "palette_first_entry", texture.paletteFirstEntry);
			setNumberField(state, "palette_entry_count", texture.paletteEntryCount);
			setNumberField(state, "cache_updates", texture.cacheUpdates);
			setBooleanField(state, "gpu_palette", texture.gpuPalette);
			setBooleanField(state, "custom_replacement", texture.customReplacement);
			setStringField(state, "source_sha256",
					research::sha256ToHex(texture.sourceDigest));
			setStringField(state, "palette_sha256",
					research::sha256ToHex(texture.paletteDigest));
			if (!texture.sourceBytes.empty())
			{
				lua_pushlstring(state,
						reinterpret_cast<const char*>(texture.sourceBytes.data()),
						texture.sourceBytes.size());
				lua_setfield(state, -2, "source_bytes");
			}
			lua_setfield(state, -2, "sampled_texture");
		}
	}
}

} // namespace research::lua_api

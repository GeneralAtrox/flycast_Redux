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

research::Sh4LuaSubscriptionQueue::PvrTaFilter pvrTaResearchFilterFromLua(
		lua_State *state, const std::string& event, std::size_t& capacity)
{
	research::Sh4LuaSubscriptionQueue::PvrTaFilter filter;
	auto& observation = filter.observation;
	if (event == "pvr-ta-list-init")
		observation.typeMask = research::pvrTaObservationTypeBit(
				research::PvrTaObservationType::ListInit);
	else if (event == "pvr-ta-list-continue")
		observation.typeMask = research::pvrTaObservationTypeBit(
				research::PvrTaObservationType::ListContinue);
	else if (event == "pvr-ta-block")
		observation.typeMask = research::pvrTaObservationTypeBit(
				research::PvrTaObservationType::AcceptedBlock);
	else if (event == "pvr-start-render")
		observation.typeMask = research::pvrTaObservationTypeBit(
				research::PvrTaObservationType::StartRender);
	else if (event == "pvr-render-done")
		observation.typeMask = research::pvrTaObservationTypeBit(
				research::PvrTaObservationType::RenderDone);
	else if (event == "pvr-ta-reset")
		observation.typeMask = research::pvrTaObservationTypeBit(
				research::PvrTaObservationType::Reset);
	else
		throw std::invalid_argument("unsupported PowerVR TA event");
	const auto source = optionalStringTableField(state, 1, "source");
	if (source.has_value())
	{
		if (event != "pvr-ta-block")
			throw std::invalid_argument("source is only valid for pvr-ta-block");
		if (*source == "store-queue")
			observation.sourceMask = research::pvrTaInputSourceBit(
					research::PvrTaInputSource::StoreQueue);
		else if (*source == "channel2-dma")
			observation.sourceMask = research::pvrTaInputSourceBit(
					research::PvrTaInputSource::Channel2Dma);
		else if (*source == "sort-dma")
			observation.sourceMask = research::pvrTaInputSourceBit(
					research::PvrTaInputSource::SortDma);
		else
			throw std::invalid_argument("unsupported PowerVR TA source");
	}
	filter.guestU32Addresses = optionalU32ArrayTableField(state, 1,
			"guest_u32_addresses",
			research::Sh4LuaSubscriptionQueue::MaximumGuestU32Snapshot);
	if (!filter.guestU32Addresses.empty() && event != "pvr-ta-block")
		throw std::invalid_argument(
				"guest_u32_addresses is only valid for pvr-ta-block");
	capacity = researchQueueCapacity(state);
	return filter;
}

research::Sh4LuaSubscriptionQueue::PvrPresentationFilter
pvrPresentationResearchFilterFromLua(lua_State *state, const std::string& event,
		std::size_t& capacity)
{
	using Type = research::PvrPresentationObservationType;
	Type type;
	if (event == "pvr-register-write") type = Type::RegisterWrite;
	else if (event == "pvr-vram-write") type = Type::VramWrite;
	else if (event == "pvr-render-queued") type = Type::RenderQueued;
	else if (event == "pvr-render-completed") type = Type::RenderCompleted;
	else if (event == "pvr-framebuffer") type = Type::FramebufferCaptured;
	else if (event == "pvr-presentation") type = Type::Presentation;
	else if (event == "pvr-presentation-reset") type = Type::Reset;
	else throw std::invalid_argument("unsupported PowerVR presentation event");
	research::Sh4LuaSubscriptionQueue::PvrPresentationFilter filter;
	filter.typeMask = std::uint32_t {1} << (static_cast<unsigned>(type) - 1u);
	const auto start = optionalUnsignedTableField(state, 1, "start_address");
	const auto end = optionalUnsignedTableField(state, 1, "end_address");
	if (start.has_value() != end.has_value())
		throw std::invalid_argument("start_address and end_address must be provided together");
	if (start.has_value())
	{
		if (type != Type::RegisterWrite && type != Type::VramWrite
				|| *start > 0xffffffffull || *end > 0xffffffffull || *start > *end)
			throw std::invalid_argument("invalid PowerVR address filter");
		filter.hasAddressRange = true;
		filter.addressStart = static_cast<std::uint32_t>(*start);
		filter.addressEndExclusive = *end + 1;
	}
	const auto payload = optionalStringTableField(state, 1, "payload");
	if (payload.has_value())
	{
		if (type != Type::FramebufferCaptured)
			throw std::invalid_argument("payload is only valid for pvr-framebuffer");
		if (*payload == "full")
			filter.framebufferDigestOnly = false;
		else if (*payload == "sha256")
			filter.framebufferDigestOnly = true;
		else
			throw std::invalid_argument("unsupported PowerVR framebuffer payload");
	}
	filter.guestU32Addresses = optionalU32ArrayTableField(state, 1,
			"guest_u32_addresses",
			research::Sh4LuaSubscriptionQueue::MaximumGuestU32Snapshot);
	if (!filter.guestU32Addresses.empty() && type != Type::RegisterWrite
			&& type != Type::VramWrite)
		throw std::invalid_argument(
				"guest_u32_addresses is only valid for synchronous PVR writes");
	filter.guestR15U32Offsets = optionalU32ArrayTableField(state, 1,
			"guest_r15_u32_offsets",
			research::Sh4LuaSubscriptionQueue::MaximumGuestR15U32Snapshot);
	if (!filter.guestR15U32Offsets.empty() && type != Type::RegisterWrite
			&& type != Type::VramWrite)
		throw std::invalid_argument(
				"guest_r15_u32_offsets is only valid for synchronous PVR writes");
	capacity = researchQueueCapacity(state);
	return filter;
}

research::Sh4LuaSubscriptionQueue::PvrDrawFilter
pvrDrawResearchFilterFromLua(lua_State *state, const std::string& event,
		std::size_t& capacity)
{
	using Type = research::PvrDrawObservationType;
	Type type;
	if (event == "pvr-primitive") type = Type::PrimitiveDecoded;
	else if (event == "pvr-draw") type = Type::DrawConsumed;
	else if (event == "pvr-draw-render-completed") type = Type::RenderCompleted;
	else if (event == "pvr-draw-reset") type = Type::Reset;
	else throw std::invalid_argument("unsupported PowerVR draw event");
	research::Sh4LuaSubscriptionQueue::PvrDrawFilter filter;
	filter.typeMask = std::uint32_t {1} << (static_cast<unsigned>(type) - 1u);
	const auto generation = optionalUnsignedTableField(state, 1, "render_generation");
	if (generation.has_value())
	{
		filter.hasRenderGeneration = true;
		filter.renderGeneration = *generation;
	}
	const auto payload = optionalStringTableField(state, 1, "payload");
	if (payload.has_value())
	{
		if (type != Type::PrimitiveDecoded)
			throw std::invalid_argument("payload is only valid for pvr-primitive");
		if (*payload == "full")
			filter.sampledTextureDigestOnly = false;
		else if (*payload == "sha256")
			filter.sampledTextureDigestOnly = true;
		else
			throw std::invalid_argument("unsupported PowerVR primitive payload");
	}
	capacity = researchQueueCapacity(state);
	return filter;
}

research::Sh4LuaSubscriptionQueue::GdromFilter gdromResearchFilterFromLua(
		lua_State *state, const std::string& event, std::size_t& capacity)
{
	using Type = research::GdromObservationType;
	Type type;
	if (event == "gdrom-command") type = Type::CommandBegin;
	else if (event == "gdrom-transfer") type = Type::TransferChunk;
	else if (event == "gdrom-complete") type = Type::Complete;
	else if (event == "gdrom-abort") type = Type::Abort;
	else if (event == "gdrom-reset") type = Type::Reset;
	else throw std::invalid_argument("unsupported GD-ROM event");
	research::Sh4LuaSubscriptionQueue::GdromFilter filter;
	filter.typeMask = std::uint32_t {1} << (static_cast<unsigned>(type) - 1u);
	const auto start = optionalUnsignedTableField(state, 1, "start_fad");
	const auto end = optionalUnsignedTableField(state, 1, "end_fad");
	if (start.has_value() != end.has_value())
		throw std::invalid_argument("start_fad and end_fad must be provided together");
	if (start.has_value())
	{
		if (type != Type::TransferChunk || *start > 0xffffffffull
				|| *end > 0xffffffffull || *start > *end)
			throw std::invalid_argument("invalid GD-ROM FAD filter");
		filter.hasFadRange = true;
		filter.fadStart = static_cast<std::uint32_t>(*start);
		filter.fadEndExclusive = *end + 1;
	}
	capacity = researchQueueCapacity(state);
	return filter;
}

research::Sh4LuaSubscriptionQueue::AicaFilter aicaResearchFilterFromLua(
		lua_State *state, const std::string& event, std::size_t& capacity)
{
	using Type = research::AicaObservationType;
	Type type;
	if (event == "aica-register-write") type = Type::RegisterWrite;
	else if (event == "aica-ram-write") type = Type::RamWrite;
	else if (event == "aica-dma-begin") type = Type::G2DmaBegin;
	else if (event == "aica-dma-transfer") type = Type::G2DmaTransfer;
	else if (event == "aica-dma-complete") type = Type::G2DmaComplete;
	else if (event == "aica-key-on") type = Type::KeyOn;
	else if (event == "aica-key-off") type = Type::KeyOff;
	else if (event == "aica-sample") type = Type::SampleFrame;
	else if (event == "aica-reset") type = Type::Reset;
	else if (event == "aica-key-batch") type = Type::KeyBatchComplete;
	else if (event == "aica-cdda-sector") type = Type::CddaSector;
	else if (event == "aica-sample-suppressed") type = Type::SampleSuppressed;
	else if (event == "aica-key-batch-begin") type = Type::KeyBatchBegin;
	else throw std::invalid_argument("unsupported AICA event");
	research::Sh4LuaSubscriptionQueue::AicaFilter filter;
	filter.typeMask = std::uint32_t {1} << (static_cast<unsigned>(type) - 1u);
	const auto writer = optionalStringTableField(state, 1, "writer");
	if (writer.has_value())
	{
		unsigned value = 0;
		if (*writer == "sh4-direct") value = 1;
		else if (*writer == "sh4-g2-dma") value = 2;
		else if (*writer == "arm7") value = 3;
		else if (*writer == "aica-dma") value = 4;
		else if (*writer == "mixer") value = 5;
		else if (*writer == "reios") value = 6;
		else throw std::invalid_argument("unsupported AICA writer");
		filter.hasWriter = true;
		filter.writerMask = std::uint32_t {1} << (value - 1u);
	}
	const auto start = optionalUnsignedTableField(state, 1, "start_address");
	const auto end = optionalUnsignedTableField(state, 1, "end_address");
	if (start.has_value() != end.has_value())
		throw std::invalid_argument("start_address and end_address must be provided together");
	if (start.has_value())
	{
		if ((type != Type::RegisterWrite && type != Type::RamWrite)
				|| *start > 0xffffffffull || *end > 0xffffffffull || *start > *end)
			throw std::invalid_argument("invalid AICA address filter");
		filter.hasAddressRange = true;
		filter.addressStart = static_cast<std::uint32_t>(*start);
		filter.addressEndExclusive = *end + 1;
	}
	const auto channel = optionalUnsignedTableField(state, 1, "channel");
	if (channel.has_value())
	{
		if (*channel >= 64 || (type != Type::KeyOn && type != Type::KeyOff
				&& type != Type::KeyBatchComplete))
			throw std::invalid_argument("invalid AICA channel filter");
		filter.hasChannel = true;
		filter.channel = static_cast<std::uint8_t>(*channel);
	}
	const auto nonzeroCdda = optionalBooleanTableField(state, 1,
			"nonzero_cdda_contribution");
	if (nonzeroCdda.has_value())
	{
		if (type != Type::SampleFrame || !*nonzeroCdda)
			throw std::invalid_argument("invalid nonzero CD-DA contribution filter");
		filter.requireNonzeroCddaContribution = true;
	}
	capacity = researchQueueCapacity(state);
	return filter;
}

research::Sh4LuaSubscriptionQueue::CddaFilter cddaResearchFilterFromLua(
		lua_State *state, const std::string& event, std::size_t& capacity)
{
	using Type = research::CddaObservationType;
	Type type;
	if (event == "cdda-control-accepted") type = Type::ControlAccepted;
	else if (event == "cdda-control-applied") type = Type::ControlApplied;
	else if (event == "cdda-sector") type = Type::Sector;
	else if (event == "cdda-reset") type = Type::Reset;
	else throw std::invalid_argument("unsupported CD-DA event");
	research::Sh4LuaSubscriptionQueue::CddaFilter filter;
	filter.typeMask = std::uint32_t {1} << (static_cast<unsigned>(type) - 1u);
	const auto command = optionalUnsignedTableField(state, 1, "command");
	if (command.has_value())
	{
		if ((type != Type::ControlAccepted && type != Type::ControlApplied)
				|| *command > 0xffffffffull
				|| !research::isCddaControlCommand(static_cast<std::uint32_t>(*command)))
			throw std::invalid_argument("invalid CD-DA command filter");
		filter.hasCommand = true;
		filter.command = static_cast<std::uint32_t>(*command);
	}
	const auto successful = optionalBooleanTableField(state, 1, "successful");
	if (successful.has_value())
	{
		if (type != Type::ControlApplied && type != Type::Sector)
			throw std::invalid_argument("successful is valid only for applied controls and sectors");
		filter.hasSuccessful = true;
		filter.successful = *successful;
	}
	const auto start = optionalUnsignedTableField(state, 1, "start_fad");
	const auto end = optionalUnsignedTableField(state, 1, "end_fad");
	if (start.has_value() != end.has_value())
		throw std::invalid_argument("start_fad and end_fad must be provided together");
	if (start.has_value())
	{
		if (type != Type::Sector || *start > 0xffffffffull
				|| *end > 0xffffffffull || *start > *end)
			throw std::invalid_argument("invalid CD-DA FAD filter");
		filter.hasFadRange = true;
		filter.fadStart = static_cast<std::uint32_t>(*start);
		filter.fadEndExclusive = *end + 1;
	}
	capacity = researchQueueCapacity(state);
	return filter;
}

} // namespace research::lua_api

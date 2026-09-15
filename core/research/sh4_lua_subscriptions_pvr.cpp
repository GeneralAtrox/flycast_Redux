// Sh4LuaSubscriptionQueue: PowerVR TA, presentation and draw subscribe overloads,
// including synchronous guest U32 / R15 stack sampling and digest-only paths.
#include "research/sh4_lua_subscriptions_internal.h"

#include "research/sha256.h"

#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_sched.h"

namespace research
{

Sh4LuaSubscriptionQueue::Token Sh4LuaSubscriptionQueue::subscribe(
		const PvrTaFilter& filter, PvrTaCallback callback,
		std::size_t capacity, ErrorCallback errorCallback)
{
	if (!callback)
		throw std::invalid_argument("Lua PowerVR TA subscription callback is empty");
	if (filter.guestU32Addresses.size() > MaximumGuestU32Snapshot
			|| (!filter.guestU32Addresses.empty()
					&& filter.observation.typeMask != pvrTaObservationTypeBit(
							PvrTaObservationType::AcceptedBlock)))
		throw std::invalid_argument("Lua PowerVR TA guest snapshot filter is invalid");
	for (const std::uint32_t address : filter.guestU32Addresses)
		if (address < 0x8c000000u || address > 0x8cfffffcu
				|| (address & 3u) != 0)
			throw std::invalid_argument(
					"Lua PowerVR TA guest U32 address is outside aligned Dreamcast system RAM");
	const auto currentState = state;
	return subscribeLuaBus(currentState, LuaSubscriptionEntry::Kind::PvrTa,
			capacity, std::move(errorCallback),
			[callback = std::move(callback)](LuaSubscriptionEntry& entry) mutable {
				entry.pvrTaCallback = std::move(callback);
			},
			[filter](const auto& shared, const auto& entry) {
				const std::weak_ptr<SharedState> weakState = shared;
				const std::weak_ptr<LuaSubscriptionEntry> weakEntry = entry;
				return subscribePvrTaObservations(filter.observation,
						[weakState, weakEntry, filter](
								const PvrTaObservation& observation) {
							PvrTaLuaObservation queued;
							queued.observation = observation;
							queued.guestSnapshotTick = sh4_sched_now64();
							queued.guestU32Snapshot.reserve(
									filter.guestU32Addresses.size());
							for (const std::uint32_t address : filter.guestU32Addresses)
							{
								GuestU32Snapshot value;
								value.address = address;
								const u8 *bytes = GetMemPtr(address, 4);
								if (bytes != nullptr)
								{
									value.available = true;
									value.value = static_cast<std::uint32_t>(bytes[0])
											| (static_cast<std::uint32_t>(bytes[1]) << 8)
											| (static_cast<std::uint32_t>(bytes[2]) << 16)
											| (static_cast<std::uint32_t>(bytes[3]) << 24);
								}
								queued.guestU32Snapshot.push_back(value);
							}
							enqueueLuaObservation(weakState, weakEntry, queued);
						});
			},
			[](std::uint64_t token) { unsubscribePvrTaObservations(token); });
}

Sh4LuaSubscriptionQueue::Token Sh4LuaSubscriptionQueue::subscribe(
		const PvrPresentationFilter& filter, PvrPresentationCallback callback,
		std::size_t capacity, ErrorCallback errorCallback)
{
	if (!callback)
		throw std::invalid_argument(
				"Lua PowerVR presentation subscription callback is empty");
	if (filter.typeMask == 0 || (filter.typeMask & ~0x7fu) != 0
			|| filter.guestU32Addresses.size() > MaximumGuestU32Snapshot
			|| filter.guestR15U32Offsets.size() > MaximumGuestR15U32Snapshot
			|| ((!filter.guestU32Addresses.empty()
						|| !filter.guestR15U32Offsets.empty())
					&& filter.typeMask != (std::uint32_t {1}
							<< (static_cast<unsigned>(
									PvrPresentationObservationType::RegisterWrite) - 1u))
					&& filter.typeMask != (std::uint32_t {1}
							<< (static_cast<unsigned>(
									PvrPresentationObservationType::VramWrite) - 1u)))
			|| (filter.hasAddressRange
					&& (filter.addressEndExclusive <= filter.addressStart
							|| filter.addressEndExclusive > 0x100000000ull))
			|| (filter.framebufferDigestOnly
					&& filter.typeMask != (std::uint32_t {1}
							<< (static_cast<unsigned>(
									PvrPresentationObservationType::FramebufferCaptured) - 1u))))
		throw std::invalid_argument("Lua PowerVR presentation filter is invalid");
	for (const std::uint32_t address : filter.guestU32Addresses)
		if (address < 0x8c000000u || address > 0x8cfffffcu
				|| (address & 3u) != 0)
			throw std::invalid_argument(
					"Lua PowerVR presentation guest U32 address is outside aligned Dreamcast system RAM");
	for (const std::uint32_t offset : filter.guestR15U32Offsets)
		if (offset > 0x1000u || (offset & 3u) != 0)
			throw std::invalid_argument(
					"Lua PowerVR presentation R15 U32 offset is outside the bounded aligned stack window");
	const auto currentState = state;
	return subscribeLuaBus(currentState,
			LuaSubscriptionEntry::Kind::PvrPresentation, capacity,
			std::move(errorCallback),
			[callback = std::move(callback)](LuaSubscriptionEntry& entry) mutable {
				entry.pvrPresentationCallback = std::move(callback);
			},
			[filter](const auto& shared, const auto& entry) {
				const std::weak_ptr<SharedState> weakState = shared;
				const std::weak_ptr<LuaSubscriptionEntry> weakEntry = entry;
				return subscribePvrPresentationObservations(
						[weakState, weakEntry, filter](
								const PvrPresentationObservation& observation) {
							const auto bit = std::uint32_t {1}
									<< (static_cast<unsigned>(observation.type) - 1u);
							if ((filter.typeMask & bit) == 0)
								return;
							if (filter.hasAddressRange)
							{
								std::uint64_t start = 0, end = 0;
								if (observation.type
										== PvrPresentationObservationType::RegisterWrite)
								{
									start = observation.registerPhysicalAddress;
									end = start + 4;
								}
								else if (observation.type
										== PvrPresentationObservationType::VramWrite)
								{
									start = observation.physicalAddress;
									end = start + observation.bytes.size();
								}
								else
									return;
								if (start >= filter.addressEndExclusive
										|| end <= filter.addressStart)
									return;
							}
							PvrPresentationLuaObservation queued;
							queued.guestSnapshotTick = sh4_sched_now64();
							queued.guestU32Snapshot.reserve(
									filter.guestU32Addresses.size());
							for (const std::uint32_t address : filter.guestU32Addresses)
							{
								GuestU32Snapshot value;
								value.address = address;
								const u8 *bytes = GetMemPtr(address, 4);
								if (bytes != nullptr)
								{
									value.available = true;
									value.value = static_cast<std::uint32_t>(bytes[0])
											| (static_cast<std::uint32_t>(bytes[1]) << 8)
											| (static_cast<std::uint32_t>(bytes[2]) << 16)
											| (static_cast<std::uint32_t>(bytes[3]) << 24);
								}
								queued.guestU32Snapshot.push_back(value);
							}
							if (!filter.guestR15U32Offsets.empty() && p_sh4rcb != nullptr)
							{
								queued.guestR15 = Sh4cntx.r[15];
								queued.guestR15Available = queued.guestR15 >= 0x8c000000u
										&& queued.guestR15 <= 0x8cfffffcu
										&& (queued.guestR15 & 3u) == 0;
								queued.guestR15U32Snapshot.reserve(
										filter.guestR15U32Offsets.size());
								for (const std::uint32_t offset : filter.guestR15U32Offsets)
								{
									GuestU32Snapshot value;
									const std::uint64_t address =
											static_cast<std::uint64_t>(queued.guestR15) + offset;
									if (queued.guestR15Available && address <= 0x8cfffffcu)
									{
										value.address = static_cast<std::uint32_t>(address);
										const u8 *bytes = GetMemPtr(value.address, 4);
										if (bytes != nullptr)
										{
											value.available = true;
											value.value = static_cast<std::uint32_t>(bytes[0])
													| (static_cast<std::uint32_t>(bytes[1]) << 8)
													| (static_cast<std::uint32_t>(bytes[2]) << 16)
													| (static_cast<std::uint32_t>(bytes[3]) << 24);
										}
									}
									queued.guestR15U32Snapshot.push_back(value);
								}
							}
							if (filter.framebufferDigestOnly
									&& observation.type
											== PvrPresentationObservationType::FramebufferCaptured)
							{
								queued.observation = observation;
								queued.observation.framebufferDigest = sha256(observation.bytes.data(),
										observation.bytes.size());
								queued.observation.framebufferDigestAvailable = true;
								queued.observation.bytes.clear();
							}
							else
								queued.observation = observation;
							enqueueLuaObservation(weakState, weakEntry, queued);
						});
			},
			[](std::uint64_t token) {
				unsubscribePvrPresentationObservations(token);
			});
}

Sh4LuaSubscriptionQueue::Token Sh4LuaSubscriptionQueue::subscribe(
		const PvrDrawFilter& filter, PvrDrawCallback callback,
		std::size_t capacity, ErrorCallback errorCallback)
{
	if (!callback)
		throw std::invalid_argument("Lua PowerVR draw subscription callback is empty");
	if (filter.typeMask == 0 || (filter.typeMask & ~0x0fu) != 0
			|| (filter.sampledTextureDigestOnly
					&& filter.typeMask != (std::uint32_t {1}
							<< (static_cast<unsigned>(
									PvrDrawObservationType::PrimitiveDecoded) - 1u))))
		throw std::invalid_argument("Lua PowerVR draw filter is invalid");
	const auto currentState = state;
	return subscribeLuaBus(currentState, LuaSubscriptionEntry::Kind::PvrDraw,
			capacity, std::move(errorCallback),
			[callback = std::move(callback)](LuaSubscriptionEntry& entry) mutable {
				entry.pvrDrawCallback = std::move(callback);
			},
			[filter](const auto& shared, const auto& entry) {
				const std::weak_ptr<SharedState> weakState = shared;
				const std::weak_ptr<LuaSubscriptionEntry> weakEntry = entry;
				return subscribePvrDrawObservations(
						[weakState, weakEntry, filter](const PvrDrawObservation& observation) {
							const auto bit = std::uint32_t {1}
									<< (static_cast<unsigned>(observation.type) - 1u);
							if ((filter.typeMask & bit) == 0
									|| (filter.hasRenderGeneration
											&& filter.renderGeneration
													!= observation.renderGeneration))
								return;
							if (filter.sampledTextureDigestOnly)
							{
								PvrDrawObservation compact = observation;
								compact.sampledTexture.sourceBytes.clear();
								enqueueLuaObservation(weakState, weakEntry, compact);
							}
							else
								enqueueLuaObservation(weakState, weakEntry, observation);
						});
			},
			[](std::uint64_t token) { unsubscribePvrDrawObservations(token); });
}

} // namespace research

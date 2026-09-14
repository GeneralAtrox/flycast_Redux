#pragma once

#include "research/aica_observation.h"
#include "research/cdda_observation.h"
#include "research/gdrom_observation.h"
#include "research/maple_observation.h"
#include "research/pvr_draw_observation.h"
#include "research/pvr_presentation_observation.h"
#include "research/pvr_ta_observation.h"
#include "research/sh4_observation.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace research
{

// Discovery-only, asynchronous delivery adapter for Lua. It subscribes to the
// canonical native observation buses and queues copies in one bounded order;
// it never participates in typed artifact recording or accepted-evidence
// publication.
class Sh4LuaSubscriptionQueue
{
public:
	struct GuestU32Snapshot
	{
		std::uint32_t address = 0;
		std::uint32_t value = 0;
		bool available = false;
	};
	// Discovery-only envelope. The canonical TA observation remains unchanged;
	// these guest reads occur synchronously when the TA accepts a block, before
	// the observation is copied into the asynchronous Lua delivery queue.
	struct PvrTaLuaObservation
	{
		PvrTaObservation observation;
		std::uint64_t guestSnapshotTick = 0;
		std::vector<GuestU32Snapshot> guestU32Snapshot;
	};
	// Discovery-only envelope for synchronous guest words sampled at a PVR
	// register or VRAM write. The canonical presentation observation is unchanged.
	struct PvrPresentationLuaObservation
	{
		PvrPresentationObservation observation;
		std::uint64_t guestSnapshotTick = 0;
		std::vector<GuestU32Snapshot> guestU32Snapshot;
		std::uint32_t guestR15 = 0;
		bool guestR15Available = false;
		std::vector<GuestU32Snapshot> guestR15U32Snapshot;
	};
	using Token = std::uint64_t;
	using Callback = std::function<void(Token, const Sh4Observation&)>;
	using MapleCallback = std::function<void(Token, const MapleObservation&)>;
	using PvrTaCallback =
			std::function<void(Token, const PvrTaLuaObservation&)>;
	using PvrPresentationCallback =
			std::function<void(Token, const PvrPresentationLuaObservation&)>;
	using PvrDrawCallback = std::function<void(Token, const PvrDrawObservation&)>;
	using GdromCallback = std::function<void(Token, const GdromObservation&)>;
	using CddaCallback = std::function<void(Token, const CddaObservation&)>;
	using AicaCallback = std::function<void(Token, const AicaObservation&)>;
	using ErrorCallback = std::function<void(Token, std::exception_ptr)>;

	struct PvrPresentationFilter
	{
		std::uint32_t typeMask = 0x7f;
		bool hasAddressRange = false;
		std::uint32_t addressStart = 0;
		std::uint64_t addressEndExclusive = 0;
		bool framebufferDigestOnly = false;
		std::vector<std::uint32_t> guestU32Addresses;
		std::vector<std::uint32_t> guestR15U32Offsets;
	};
	struct PvrTaFilter
	{
		PvrTaObservationFilter observation;
		std::vector<std::uint32_t> guestU32Addresses;
	};
	struct PvrDrawFilter
	{
		std::uint32_t typeMask = 0x0f;
		bool hasRenderGeneration = false;
		std::uint64_t renderGeneration = 0;
		bool sampledTextureDigestOnly = false;
	};
	struct GdromFilter
	{
		std::uint32_t typeMask = 0x1f;
		bool hasFadRange = false;
		std::uint32_t fadStart = 0;
		std::uint64_t fadEndExclusive = 0;
	};
	struct AicaFilter
	{
		std::uint32_t typeMask = 0x0fff;
		bool hasWriter = false;
		std::uint32_t writerMask = 0x7f;
		bool hasAddressRange = false;
		std::uint32_t addressStart = 0;
		std::uint64_t addressEndExclusive = 0;
		bool hasChannel = false;
		std::uint8_t channel = 0;
		bool requireNonzeroCddaContribution = false;
	};
	struct CddaFilter
	{
		std::uint32_t typeMask = 0x0f;
		bool hasCommand = false;
		std::uint32_t command = 0;
		bool hasSuccessful = false;
		bool successful = false;
		bool hasFadRange = false;
		std::uint32_t fadStart = 0;
		std::uint64_t fadEndExclusive = 0;
	};

	static constexpr std::size_t DefaultCapacity = 4096;
	static constexpr std::size_t MaximumCapacity = 65536;
	static constexpr std::size_t MaximumSubscriptions = 64;
	static constexpr std::size_t MaximumTotalQueued = 65536;
	static constexpr std::size_t MaximumGuestU32Snapshot = 16;
	static constexpr std::size_t MaximumGuestR15U32Snapshot = 256;
	// A full-rate AICA sample subscription produces 44,100 observations per
	// second.  The prior 1,024-event host-overlay drain could not keep pace on
	// ordinary displays and caused otherwise bounded audio probes to drop data.
	static constexpr std::size_t DefaultDrainLimit = 8192;

	struct Stats
	{
		std::size_t capacity = 0;
		std::size_t queued = 0;
		std::uint64_t delivered = 0;
		std::uint64_t dropped = 0;
		std::uint64_t callbackFailures = 0;
		bool active = false;
	};

	explicit Sh4LuaSubscriptionQueue(
			std::thread::id ownerThread = std::this_thread::get_id());
	~Sh4LuaSubscriptionQueue();

	Sh4LuaSubscriptionQueue(const Sh4LuaSubscriptionQueue&) = delete;
	Sh4LuaSubscriptionQueue& operator=(const Sh4LuaSubscriptionQueue&) = delete;

	Token subscribe(const Sh4ObservationFilter& filter, Callback callback,
			std::size_t capacity = DefaultCapacity,
			ErrorCallback errorCallback = {});
	Token subscribe(const MapleObservationFilter& filter, MapleCallback callback,
			std::size_t capacity = DefaultCapacity,
			ErrorCallback errorCallback = {});
	Token subscribe(const PvrTaFilter& filter, PvrTaCallback callback,
			std::size_t capacity = DefaultCapacity,
			ErrorCallback errorCallback = {});
	Token subscribe(const PvrPresentationFilter& filter,
			PvrPresentationCallback callback,
			std::size_t capacity = DefaultCapacity,
			ErrorCallback errorCallback = {});
	Token subscribe(const PvrDrawFilter& filter, PvrDrawCallback callback,
			std::size_t capacity = DefaultCapacity,
			ErrorCallback errorCallback = {});
	Token subscribe(const GdromFilter& filter, GdromCallback callback,
			std::size_t capacity = DefaultCapacity,
			ErrorCallback errorCallback = {});
	Token subscribe(const CddaFilter& filter, CddaCallback callback,
			std::size_t capacity = DefaultCapacity,
			ErrorCallback errorCallback = {});
	Token subscribe(const AicaFilter& filter, AicaCallback callback,
			std::size_t capacity = DefaultCapacity,
			ErrorCallback errorCallback = {});
	bool unsubscribe(Token token) noexcept;
	std::optional<Stats> stats(Token token) const;

	// Must run on the Lua-owning thread. Nested drains are ignored so a callback
	// cannot re-enter delivery; unsubscribe from inside a callback remains safe.
	std::size_t drain(std::size_t maximumDeliveries = DefaultDrainLimit);

	// May run during emulator teardown. It prevents new queue entries before it
	// synchronously detaches every native subscription.
	void clear() noexcept;
	std::size_t subscriptionCount() const noexcept;
	std::size_t pendingCount() const noexcept;

private:
	struct SharedState;
	std::shared_ptr<SharedState> state;
};

} // namespace research

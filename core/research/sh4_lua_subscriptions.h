#pragma once

#include "research/sh4_observation.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <thread>

namespace research
{

// Discovery-only, asynchronous delivery adapter for Lua. It subscribes to the
// canonical native observation bus and queues copies; it never participates in
// typed artifact recording or accepted-evidence publication.
class Sh4LuaSubscriptionQueue
{
public:
	using Token = std::uint64_t;
	using Callback = std::function<void(Token, const Sh4Observation&)>;
	using ErrorCallback = std::function<void(Token, std::exception_ptr)>;

	static constexpr std::size_t DefaultCapacity = 4096;
	static constexpr std::size_t MaximumCapacity = 65536;
	static constexpr std::size_t MaximumSubscriptions = 64;
	static constexpr std::size_t MaximumTotalQueued = 65536;
	static constexpr std::size_t DefaultDrainLimit = 1024;

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

// Sh4LuaSubscriptionQueue: SH-4 and Maple bus subscribe overloads.
#include "research/sh4_lua_subscriptions_internal.h"

namespace research
{

Sh4LuaSubscriptionQueue::Token Sh4LuaSubscriptionQueue::subscribe(
		const Sh4ObservationFilter& filter, Callback callback,
		std::size_t capacity, ErrorCallback errorCallback)
{
	if (!callback)
		throw std::invalid_argument("Lua SH-4 subscription callback is empty");
	if (capacity == 0 || capacity > MaximumCapacity)
		throw std::invalid_argument("Lua SH-4 subscription capacity is out of range");

	const std::shared_ptr<SharedState> currentState = state;
	auto entry = std::make_shared<LuaSubscriptionEntry>();
	entry->kind = LuaSubscriptionEntry::Kind::Sh4;
	entry->capacity = capacity;
	entry->callback = std::move(callback);
	entry->errorCallback = std::move(errorCallback);
	{
		const std::lock_guard<std::mutex> lock(currentState->mutex);
		if (currentState->subscriptions.size() >= MaximumSubscriptions)
			throw std::overflow_error("too many Lua SH-4 subscriptions");
		entry->token = currentState->nextToken++;
		if (entry->token == 0 || currentState->nextToken == 0)
			throw std::overflow_error("Lua SH-4 subscription token overflow");
		currentState->subscriptions.emplace(entry->token, entry);
	}

	Sh4ObservationSubscription nativeSubscription = 0;
	try
	{
		const std::weak_ptr<SharedState> weakState = currentState;
		const std::weak_ptr<LuaSubscriptionEntry> weakEntry = entry;
		nativeSubscription = subscribeSh4Observations(filter,
				[weakState, weakEntry](const Sh4Observation& observation) noexcept {
					const std::shared_ptr<SharedState> lockedState = weakState.lock();
					const std::shared_ptr<LuaSubscriptionEntry> lockedEntry = weakEntry.lock();
					if (lockedState == nullptr || lockedEntry == nullptr
							|| !lockedEntry->active.load(std::memory_order_acquire))
						return;
					try
					{
						const std::lock_guard<std::mutex> lock(lockedState->mutex);
						if (!lockedEntry->active.load(std::memory_order_relaxed)
								|| lockedEntry->queued >= lockedEntry->capacity
								|| lockedState->pending.size() >= MaximumTotalQueued)
						{
							lockedEntry->dropped.fetch_add(1, std::memory_order_relaxed);
							return;
						}
						lockedState->pending.push_back(
								QueuedLuaObservation {lockedEntry, observation});
						++lockedEntry->queued;
					}
					catch (...)
					{
						// Discovery delivery must never fail an emulator observation or
						// native recorder. Allocation failure is an observable drop.
						lockedEntry->dropped.fetch_add(1, std::memory_order_relaxed);
					}
				});
	}
	catch (...)
	{
		const std::lock_guard<std::mutex> lock(currentState->mutex);
		entry->active.store(false, std::memory_order_release);
		currentState->subscriptions.erase(entry->token);
		throw;
	}

	bool retained = false;
	{
		const std::lock_guard<std::mutex> lock(currentState->mutex);
		const auto found = currentState->subscriptions.find(entry->token);
		retained = found != currentState->subscriptions.end()
				&& found->second == entry
				&& entry->active.load(std::memory_order_acquire);
		if (retained)
			entry->nativeSubscription = nativeSubscription;
	}
	if (!retained)
	{
		unsubscribeSh4Observations(nativeSubscription);
		throw std::runtime_error("Lua SH-4 subscription was cleared during setup");
	}
	return entry->token;
}

Sh4LuaSubscriptionQueue::Token Sh4LuaSubscriptionQueue::subscribe(
		const MapleObservationFilter& filter, MapleCallback callback,
		std::size_t capacity, ErrorCallback errorCallback)
{
	if (!callback)
		throw std::invalid_argument("Lua Maple subscription callback is empty");
	if (capacity == 0 || capacity > MaximumCapacity)
		throw std::invalid_argument("Lua Maple subscription capacity is out of range");

	const std::shared_ptr<SharedState> currentState = state;
	auto entry = std::make_shared<LuaSubscriptionEntry>();
	entry->kind = LuaSubscriptionEntry::Kind::Maple;
	entry->capacity = capacity;
	entry->mapleCallback = std::move(callback);
	entry->errorCallback = std::move(errorCallback);
	{
		const std::lock_guard<std::mutex> lock(currentState->mutex);
		if (currentState->subscriptions.size() >= MaximumSubscriptions)
			throw std::overflow_error("too many Lua research subscriptions");
		entry->token = currentState->nextToken++;
		if (entry->token == 0 || currentState->nextToken == 0)
			throw std::overflow_error("Lua research subscription token overflow");
		currentState->subscriptions.emplace(entry->token, entry);
	}

	MapleObservationSubscription nativeSubscription = 0;
	try
	{
		const std::weak_ptr<SharedState> weakState = currentState;
		const std::weak_ptr<LuaSubscriptionEntry> weakEntry = entry;
		nativeSubscription = subscribeMapleObservations(filter,
				[weakState, weakEntry](const MapleObservation& observation) noexcept {
					const std::shared_ptr<SharedState> lockedState = weakState.lock();
					const std::shared_ptr<LuaSubscriptionEntry> lockedEntry = weakEntry.lock();
					if (lockedState == nullptr || lockedEntry == nullptr
							|| !lockedEntry->active.load(std::memory_order_acquire))
						return;
					try
					{
						const std::lock_guard<std::mutex> lock(lockedState->mutex);
						if (!lockedEntry->active.load(std::memory_order_relaxed)
								|| lockedEntry->queued >= lockedEntry->capacity
								|| lockedState->pending.size() >= MaximumTotalQueued)
						{
							lockedEntry->dropped.fetch_add(1, std::memory_order_relaxed);
							return;
						}
						lockedState->pending.push_back(
								QueuedLuaObservation {lockedEntry, observation});
						++lockedEntry->queued;
					}
					catch (...)
					{
						lockedEntry->dropped.fetch_add(1, std::memory_order_relaxed);
					}
				});
	}
	catch (...)
	{
		const std::lock_guard<std::mutex> lock(currentState->mutex);
		entry->active.store(false, std::memory_order_release);
		currentState->subscriptions.erase(entry->token);
		throw;
	}

	bool retained = false;
	{
		const std::lock_guard<std::mutex> lock(currentState->mutex);
		const auto found = currentState->subscriptions.find(entry->token);
		retained = found != currentState->subscriptions.end()
				&& found->second == entry
				&& entry->active.load(std::memory_order_acquire);
		if (retained)
			entry->nativeSubscription = nativeSubscription;
	}
	if (!retained)
	{
		unsubscribeMapleObservations(nativeSubscription);
		throw std::runtime_error("Lua Maple subscription was cleared during setup");
	}
	return entry->token;
}

} // namespace research

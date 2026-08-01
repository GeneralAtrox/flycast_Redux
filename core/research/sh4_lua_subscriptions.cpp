#include "research/sh4_lua_subscriptions.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>

namespace research
{
namespace
{

struct LuaSubscriptionEntry
{
	enum class Kind
	{
		Sh4,
		Maple,
	};

	Sh4LuaSubscriptionQueue::Token token = 0;
	Kind kind = Kind::Sh4;
	std::uint64_t nativeSubscription = 0;
	std::size_t capacity = 0;
	std::size_t queued = 0;
	std::atomic<bool> active {true};
	std::atomic<std::uint64_t> delivered {0};
	std::atomic<std::uint64_t> dropped {0};
	std::atomic<std::uint64_t> callbackFailures {0};
	Sh4LuaSubscriptionQueue::Callback callback;
	Sh4LuaSubscriptionQueue::MapleCallback mapleCallback;
	Sh4LuaSubscriptionQueue::ErrorCallback errorCallback;
};

struct QueuedLuaObservation
{
	std::shared_ptr<LuaSubscriptionEntry> entry;
	std::variant<Sh4Observation, MapleObservation> observation;
};

} // namespace

struct Sh4LuaSubscriptionQueue::SharedState
{
	explicit SharedState(std::thread::id ownerThread)
		: ownerThread(ownerThread)
	{
	}

	mutable std::mutex mutex;
	std::unordered_map<Token, std::shared_ptr<LuaSubscriptionEntry>> subscriptions;
	std::deque<QueuedLuaObservation> pending;
	std::thread::id ownerThread;
	Token nextToken = 1;
	bool draining = false;
};

Sh4LuaSubscriptionQueue::Sh4LuaSubscriptionQueue(std::thread::id ownerThread)
	: state(std::make_shared<SharedState>(ownerThread))
{
}

Sh4LuaSubscriptionQueue::~Sh4LuaSubscriptionQueue()
{
	clear();
}

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

bool Sh4LuaSubscriptionQueue::unsubscribe(Token token) noexcept
{
	try
	{
		if (token == 0)
			return false;
		const std::shared_ptr<SharedState> currentState = state;
		std::uint64_t nativeSubscription = 0;
		LuaSubscriptionEntry::Kind kind = LuaSubscriptionEntry::Kind::Sh4;
		{
			const std::lock_guard<std::mutex> lock(currentState->mutex);
			const auto found = currentState->subscriptions.find(token);
			if (found == currentState->subscriptions.end())
				return false;
			const std::shared_ptr<LuaSubscriptionEntry> entry = found->second;
			entry->active.store(false, std::memory_order_release);
			kind = entry->kind;
			nativeSubscription = entry->nativeSubscription;
			for (auto queued = currentState->pending.begin();
					queued != currentState->pending.end();)
			{
				if (queued->entry == entry)
					queued = currentState->pending.erase(queued);
				else
					++queued;
			}
			entry->queued = 0;
			currentState->subscriptions.erase(found);
		}
		if (nativeSubscription != 0)
		{
			if (kind == LuaSubscriptionEntry::Kind::Sh4)
				unsubscribeSh4Observations(nativeSubscription);
			else
				unsubscribeMapleObservations(nativeSubscription);
		}
		return true;
	}
	catch (...)
	{
		return false;
	}
}

std::optional<Sh4LuaSubscriptionQueue::Stats> Sh4LuaSubscriptionQueue::stats(
		Token token) const
{
	const std::shared_ptr<SharedState> currentState = state;
	const std::lock_guard<std::mutex> lock(currentState->mutex);
	const auto found = currentState->subscriptions.find(token);
	if (found == currentState->subscriptions.end())
		return std::nullopt;
	const std::shared_ptr<LuaSubscriptionEntry>& entry = found->second;
	Stats value;
	value.capacity = entry->capacity;
	value.queued = entry->queued;
	value.delivered = entry->delivered.load(std::memory_order_relaxed);
	value.dropped = entry->dropped.load(std::memory_order_relaxed);
	value.callbackFailures = entry->callbackFailures.load(std::memory_order_relaxed);
	value.active = entry->active.load(std::memory_order_acquire);
	return value;
}

std::size_t Sh4LuaSubscriptionQueue::drain(std::size_t maximumDeliveries)
{
	const std::shared_ptr<SharedState> currentState = state;
	if (std::this_thread::get_id() != currentState->ownerThread)
		throw std::logic_error("Lua SH-4 delivery attempted on a non-owning thread");
	if (maximumDeliveries == 0)
		return 0;
	{
		const std::lock_guard<std::mutex> lock(currentState->mutex);
		if (currentState->draining)
			return 0;
		currentState->draining = true;
	}
	struct DrainReset
	{
		std::shared_ptr<SharedState> state;
		~DrainReset()
		{
			const std::lock_guard<std::mutex> lock(state->mutex);
			state->draining = false;
		}
	} reset {currentState};

	std::size_t delivered = 0;
	while (delivered < maximumDeliveries)
	{
		QueuedLuaObservation queued;
		{
			const std::lock_guard<std::mutex> lock(currentState->mutex);
			if (currentState->pending.empty())
				break;
			queued = std::move(currentState->pending.front());
			currentState->pending.pop_front();
			if (queued.entry->queued != 0)
				--queued.entry->queued;
		}
		if (!queued.entry->active.load(std::memory_order_acquire))
			continue;
		try
		{
			if (queued.entry->kind == LuaSubscriptionEntry::Kind::Sh4)
				queued.entry->callback(queued.entry->token,
						std::get<Sh4Observation>(queued.observation));
			else
				queued.entry->mapleCallback(queued.entry->token,
						std::get<MapleObservation>(queued.observation));
		}
		catch (...)
		{
			const std::exception_ptr failure = std::current_exception();
			queued.entry->callbackFailures.fetch_add(1, std::memory_order_relaxed);
			if (queued.entry->errorCallback)
			{
				try
				{
					queued.entry->errorCallback(queued.entry->token, failure);
				}
				catch (...)
				{
				}
			}
		}
		queued.entry->delivered.fetch_add(1, std::memory_order_relaxed);
		++delivered;
	}
	return delivered;
}

void Sh4LuaSubscriptionQueue::clear() noexcept
{
	const std::shared_ptr<SharedState> currentState = state;
	try
	{
		bool deactivated = false;
		for (;;)
		{
			std::uint64_t nativeSubscription = 0;
			LuaSubscriptionEntry::Kind kind = LuaSubscriptionEntry::Kind::Sh4;
			{
				const std::lock_guard<std::mutex> lock(currentState->mutex);
				if (!deactivated)
				{
					for (const auto& item : currentState->subscriptions)
					{
						item.second->active.store(false, std::memory_order_release);
						item.second->queued = 0;
					}
					currentState->pending.clear();
					deactivated = true;
				}
				if (currentState->subscriptions.empty())
					break;
				const auto found = currentState->subscriptions.begin();
				nativeSubscription = found->second->nativeSubscription;
				kind = found->second->kind;
				currentState->subscriptions.erase(found);
			}
			if (nativeSubscription != 0)
			{
				if (kind == LuaSubscriptionEntry::Kind::Sh4)
					unsubscribeSh4Observations(nativeSubscription);
				else
					unsubscribeMapleObservations(nativeSubscription);
			}
		}
	}
	catch (...)
	{
		// Teardown is fail-closed for delivery. Entries are already inactive;
		// native unsubscribe itself is noexcept.
	}
}

std::size_t Sh4LuaSubscriptionQueue::subscriptionCount() const noexcept
{
	try
	{
		const std::shared_ptr<SharedState> currentState = state;
		const std::lock_guard<std::mutex> lock(currentState->mutex);
		return currentState->subscriptions.size();
	}
	catch (...)
	{
		return 0;
	}
}

std::size_t Sh4LuaSubscriptionQueue::pendingCount() const noexcept
{
	try
	{
		const std::shared_ptr<SharedState> currentState = state;
		const std::lock_guard<std::mutex> lock(currentState->mutex);
		return currentState->pending.size();
	}
	catch (...)
	{
		return 0;
	}
}

} // namespace research

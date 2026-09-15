#pragma once

// Implementation-private declarations shared by the Sh4LuaSubscriptionQueue
// translation units (sh4_lua_subscriptions*.cpp). Not for use outside them.

#include "research/sh4_lua_subscriptions.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <variant>

namespace research
{

struct LuaSubscriptionEntry
{
	enum class Kind
	{
		Sh4,
		Maple,
		PvrTa,
		PvrPresentation,
		PvrDraw,
		Gdrom,
		Cdda,
		Aica,
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
	Sh4LuaSubscriptionQueue::PvrTaCallback pvrTaCallback;
	Sh4LuaSubscriptionQueue::PvrPresentationCallback pvrPresentationCallback;
	Sh4LuaSubscriptionQueue::PvrDrawCallback pvrDrawCallback;
	Sh4LuaSubscriptionQueue::GdromCallback gdromCallback;
	Sh4LuaSubscriptionQueue::CddaCallback cddaCallback;
	Sh4LuaSubscriptionQueue::AicaCallback aicaCallback;
	Sh4LuaSubscriptionQueue::ErrorCallback errorCallback;
};

struct QueuedLuaObservation
{
	std::shared_ptr<LuaSubscriptionEntry> entry;
	std::variant<Sh4Observation, MapleObservation,
			Sh4LuaSubscriptionQueue::PvrTaLuaObservation,
			Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation,
			PvrDrawObservation, GdromObservation,
			CddaObservation,
			AicaObservation> observation;
};

inline void unsubscribeNativeObservation(LuaSubscriptionEntry::Kind kind,
		std::uint64_t subscription) noexcept
{
	switch (kind)
	{
	case LuaSubscriptionEntry::Kind::Sh4:
		unsubscribeSh4Observations(subscription);
		break;
	case LuaSubscriptionEntry::Kind::Maple:
		unsubscribeMapleObservations(subscription);
		break;
	case LuaSubscriptionEntry::Kind::PvrTa:
		unsubscribePvrTaObservations(subscription);
		break;
	case LuaSubscriptionEntry::Kind::PvrPresentation:
		unsubscribePvrPresentationObservations(subscription);
		break;
	case LuaSubscriptionEntry::Kind::PvrDraw:
		unsubscribePvrDrawObservations(subscription);
		break;
	case LuaSubscriptionEntry::Kind::Gdrom:
		unsubscribeGdromObservations(subscription);
		break;
	case LuaSubscriptionEntry::Kind::Cdda:
		unsubscribeCddaObservations(subscription);
		break;
	case LuaSubscriptionEntry::Kind::Aica:
		unsubscribeAicaObservations(subscription);
		break;
	}
}

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

template<typename State, typename Entry, typename Observation>
void enqueueLuaObservation(const std::weak_ptr<State>& weakState,
		const std::weak_ptr<Entry>& weakEntry,
		const Observation& observation) noexcept
{
	const std::shared_ptr<State> lockedState = weakState.lock();
	const std::shared_ptr<Entry> lockedEntry = weakEntry.lock();
	if (lockedState == nullptr || lockedEntry == nullptr
			|| !lockedEntry->active.load(std::memory_order_acquire))
		return;
	try
	{
		const std::lock_guard<std::mutex> lock(lockedState->mutex);
		if (!lockedEntry->active.load(std::memory_order_relaxed)
				|| lockedEntry->queued >= lockedEntry->capacity
				|| lockedState->pending.size()
						>= Sh4LuaSubscriptionQueue::MaximumTotalQueued)
		{
			lockedEntry->dropped.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		lockedState->pending.push_back(QueuedLuaObservation {lockedEntry, observation});
		++lockedEntry->queued;
	}
	catch (...)
	{
		lockedEntry->dropped.fetch_add(1, std::memory_order_relaxed);
	}
}

template<typename State, typename Configure, typename SubscribeNative,
		typename UnsubscribeNative>
Sh4LuaSubscriptionQueue::Token subscribeLuaBus(
		const std::shared_ptr<State>& currentState,
		LuaSubscriptionEntry::Kind kind, std::size_t capacity,
		Sh4LuaSubscriptionQueue::ErrorCallback errorCallback,
		Configure configure, SubscribeNative subscribeNative,
		UnsubscribeNative unsubscribeNative)
{
	if (capacity == 0 || capacity > Sh4LuaSubscriptionQueue::MaximumCapacity)
		throw std::invalid_argument("Lua research subscription capacity is out of range");
	auto entry = std::make_shared<LuaSubscriptionEntry>();
	entry->kind = kind;
	entry->capacity = capacity;
	entry->errorCallback = std::move(errorCallback);
	configure(*entry);
	{
		const std::lock_guard<std::mutex> lock(currentState->mutex);
		if (currentState->subscriptions.size()
				>= Sh4LuaSubscriptionQueue::MaximumSubscriptions)
			throw std::overflow_error("too many Lua research subscriptions");
		entry->token = currentState->nextToken++;
		if (entry->token == 0 || currentState->nextToken == 0)
			throw std::overflow_error("Lua research subscription token overflow");
		currentState->subscriptions.emplace(entry->token, entry);
	}

	std::uint64_t nativeSubscription = 0;
	try
	{
		nativeSubscription = subscribeNative(currentState, entry);
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
		unsubscribeNative(nativeSubscription);
		throw std::runtime_error("Lua research subscription was cleared during setup");
	}
	return entry->token;
}

} // namespace research

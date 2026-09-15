// Sh4LuaSubscriptionQueue: lifetime, unsubscribe, stats and owner-thread drain.
// The per-bus subscribe overloads live in sh4_lua_subscriptions_{cpu,pvr,media}.cpp.
#include "research/sh4_lua_subscriptions_internal.h"

namespace research
{

Sh4LuaSubscriptionQueue::Sh4LuaSubscriptionQueue(std::thread::id ownerThread)
	: state(std::make_shared<SharedState>(ownerThread))
{
}

Sh4LuaSubscriptionQueue::~Sh4LuaSubscriptionQueue()
{
	clear();
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
			unsubscribeNativeObservation(kind, nativeSubscription);
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
			switch (queued.entry->kind)
			{
			case LuaSubscriptionEntry::Kind::Sh4:
				queued.entry->callback(queued.entry->token,
						std::get<Sh4Observation>(queued.observation));
				break;
			case LuaSubscriptionEntry::Kind::Maple:
				queued.entry->mapleCallback(queued.entry->token,
						std::get<MapleObservation>(queued.observation));
				break;
			case LuaSubscriptionEntry::Kind::PvrTa:
				queued.entry->pvrTaCallback(queued.entry->token,
						std::get<PvrTaLuaObservation>(queued.observation));
				break;
			case LuaSubscriptionEntry::Kind::PvrPresentation:
				queued.entry->pvrPresentationCallback(queued.entry->token,
						std::get<PvrPresentationLuaObservation>(queued.observation));
				break;
			case LuaSubscriptionEntry::Kind::PvrDraw:
				queued.entry->pvrDrawCallback(queued.entry->token,
						std::get<PvrDrawObservation>(queued.observation));
				break;
			case LuaSubscriptionEntry::Kind::Gdrom:
				queued.entry->gdromCallback(queued.entry->token,
						std::get<GdromObservation>(queued.observation));
				break;
			case LuaSubscriptionEntry::Kind::Cdda:
				queued.entry->cddaCallback(queued.entry->token,
						std::get<CddaObservation>(queued.observation));
				break;
			case LuaSubscriptionEntry::Kind::Aica:
				queued.entry->aicaCallback(queued.entry->token,
						std::get<AicaObservation>(queued.observation));
				break;
			}
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
				unsubscribeNativeObservation(kind, nativeSubscription);
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

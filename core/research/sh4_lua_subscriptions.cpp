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
	std::variant<Sh4Observation, MapleObservation, PvrTaObservation,
			PvrPresentationObservation, PvrDrawObservation, GdromObservation,
			CddaObservation,
			AicaObservation> observation;
};

void unsubscribeNativeObservation(LuaSubscriptionEntry::Kind kind,
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

Sh4LuaSubscriptionQueue::Token Sh4LuaSubscriptionQueue::subscribe(
		const PvrTaObservationFilter& filter, PvrTaCallback callback,
		std::size_t capacity, ErrorCallback errorCallback)
{
	if (!callback)
		throw std::invalid_argument("Lua PowerVR TA subscription callback is empty");
	const auto currentState = state;
	return subscribeLuaBus(currentState, LuaSubscriptionEntry::Kind::PvrTa,
			capacity, std::move(errorCallback),
			[callback = std::move(callback)](LuaSubscriptionEntry& entry) mutable {
				entry.pvrTaCallback = std::move(callback);
			},
			[filter](const auto& shared, const auto& entry) {
				const std::weak_ptr<SharedState> weakState = shared;
				const std::weak_ptr<LuaSubscriptionEntry> weakEntry = entry;
				return subscribePvrTaObservations(filter,
						[weakState, weakEntry](const PvrTaObservation& observation) {
							enqueueLuaObservation(weakState, weakEntry, observation);
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
			|| (filter.hasAddressRange
					&& (filter.addressEndExclusive <= filter.addressStart
							|| filter.addressEndExclusive > 0x100000000ull)))
		throw std::invalid_argument("Lua PowerVR presentation filter is invalid");
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
							enqueueLuaObservation(weakState, weakEntry, observation);
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
	if (filter.typeMask == 0 || (filter.typeMask & ~0x0fu) != 0)
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
							enqueueLuaObservation(weakState, weakEntry, observation);
						});
			},
			[](std::uint64_t token) { unsubscribePvrDrawObservations(token); });
}

Sh4LuaSubscriptionQueue::Token Sh4LuaSubscriptionQueue::subscribe(
		const GdromFilter& filter, GdromCallback callback,
		std::size_t capacity, ErrorCallback errorCallback)
{
	if (!callback)
		throw std::invalid_argument("Lua GD-ROM subscription callback is empty");
	if (filter.typeMask == 0 || (filter.typeMask & ~0x1fu) != 0
			|| (filter.hasFadRange
					&& (filter.fadEndExclusive <= filter.fadStart
							|| filter.fadEndExclusive > 0x100000000ull)))
		throw std::invalid_argument("Lua GD-ROM filter is invalid");
	const auto currentState = state;
	return subscribeLuaBus(currentState, LuaSubscriptionEntry::Kind::Gdrom,
			capacity, std::move(errorCallback),
			[callback = std::move(callback)](LuaSubscriptionEntry& entry) mutable {
				entry.gdromCallback = std::move(callback);
			},
			[filter](const auto& shared, const auto& entry) {
				const std::weak_ptr<SharedState> weakState = shared;
				const std::weak_ptr<LuaSubscriptionEntry> weakEntry = entry;
				return subscribeGdromObservations(
						[weakState, weakEntry, filter](const GdromObservation& observation) {
							const auto bit = std::uint32_t {1}
									<< (static_cast<unsigned>(observation.type) - 1u);
							if ((filter.typeMask & bit) == 0)
								return;
							if (filter.hasFadRange)
							{
								if (observation.type != GdromObservationType::TransferChunk)
									return;
								const std::uint64_t end = std::uint64_t(observation.fad)
										+ observation.sectorCount;
								if (observation.fad >= filter.fadEndExclusive
										|| end <= filter.fadStart)
									return;
							}
							enqueueLuaObservation(weakState, weakEntry, observation);
						});
			},
			[](std::uint64_t token) { unsubscribeGdromObservations(token); });
}

Sh4LuaSubscriptionQueue::Token Sh4LuaSubscriptionQueue::subscribe(
		const AicaFilter& filter, AicaCallback callback,
		std::size_t capacity, ErrorCallback errorCallback)
{
	if (!callback)
		throw std::invalid_argument("Lua AICA subscription callback is empty");
	if (filter.typeMask == 0 || (filter.typeMask & ~0x0fffu) != 0
			|| (filter.hasWriter
					&& (filter.writerMask == 0 || (filter.writerMask & ~0x7fu) != 0))
			|| (filter.hasAddressRange
					&& (filter.addressEndExclusive <= filter.addressStart
							|| filter.addressEndExclusive > 0x100000000ull))
			|| (filter.hasChannel && filter.channel >= 64)
			|| (filter.requireNonzeroCddaContribution
					&& filter.typeMask != (std::uint32_t {1}
							<< (static_cast<unsigned>(AicaObservationType::SampleFrame) - 1u))))
		throw std::invalid_argument("Lua AICA filter is invalid");
	const auto currentState = state;
	return subscribeLuaBus(currentState, LuaSubscriptionEntry::Kind::Aica,
			capacity, std::move(errorCallback),
			[callback = std::move(callback)](LuaSubscriptionEntry& entry) mutable {
				entry.aicaCallback = std::move(callback);
			},
			[filter](const auto& shared, const auto& entry) {
				const std::weak_ptr<SharedState> weakState = shared;
				const std::weak_ptr<LuaSubscriptionEntry> weakEntry = entry;
				return subscribeAicaObservations(
						[weakState, weakEntry, filter](const AicaObservation& observation) {
							const auto typeBit = std::uint32_t {1}
									<< (static_cast<unsigned>(observation.type) - 1u);
							const auto writer = static_cast<unsigned>(observation.owner.writer);
							const auto writerBit = writer == 0 ? 0u
									: std::uint32_t {1} << (writer - 1u);
							if ((filter.typeMask & typeBit) == 0
									|| (filter.hasWriter
											&& (writerBit == 0
													|| (filter.writerMask & writerBit) == 0)))
								return;
							if (filter.hasAddressRange)
							{
								if (observation.type != AicaObservationType::RegisterWrite
										&& observation.type != AicaObservationType::RamWrite)
									return;
								const std::uint64_t size = observation.type
										== AicaObservationType::RegisterWrite
										? observation.width : observation.bytes.size();
								const std::uint64_t end = std::uint64_t(observation.address) + size;
								if (observation.address >= filter.addressEndExclusive
										|| end <= filter.addressStart)
									return;
							}
							if (filter.hasChannel)
							{
								if ((observation.type == AicaObservationType::KeyOn
										|| observation.type == AicaObservationType::KeyOff)
										&& observation.channel != filter.channel)
									return;
								if (observation.type == AicaObservationType::KeyBatchComplete)
								{
									const std::uint64_t bit = std::uint64_t {1} << filter.channel;
									if (((observation.keyOnMask | observation.keyOffMask) & bit) == 0)
										return;
								}
								else if (observation.type != AicaObservationType::KeyOn
										&& observation.type != AicaObservationType::KeyOff)
									return;
							}
							if (filter.requireNonzeroCddaContribution
									&& observation.cddaContributionLeft == 0
									&& observation.cddaContributionRight == 0)
								return;
							enqueueLuaObservation(weakState, weakEntry, observation);
						});
			},
			[](std::uint64_t token) { unsubscribeAicaObservations(token); });
}

Sh4LuaSubscriptionQueue::Token Sh4LuaSubscriptionQueue::subscribe(
		const CddaFilter& filter, CddaCallback callback,
		std::size_t capacity, ErrorCallback errorCallback)
{
	if (!callback)
		throw std::invalid_argument("Lua CD-DA subscription callback is empty");
	if (filter.typeMask == 0 || (filter.typeMask & ~0x0fu) != 0
			|| (filter.hasCommand && !isCddaControlCommand(filter.command))
			|| (filter.hasSuccessful
					&& filter.typeMask != (std::uint32_t {1}
								<< (static_cast<unsigned>(CddaObservationType::ControlApplied) - 1u))
					&& filter.typeMask != (std::uint32_t {1}
								<< (static_cast<unsigned>(CddaObservationType::Sector) - 1u)))
			|| (filter.hasFadRange
					&& (filter.fadEndExclusive <= filter.fadStart
							|| filter.fadEndExclusive > 0x100000000ull)))
		throw std::invalid_argument("Lua CD-DA filter is invalid");
	const auto currentState = state;
	return subscribeLuaBus(currentState, LuaSubscriptionEntry::Kind::Cdda,
			capacity, std::move(errorCallback),
			[callback = std::move(callback)](LuaSubscriptionEntry& entry) mutable {
				entry.cddaCallback = std::move(callback);
			},
			[filter](const auto& shared, const auto& entry) {
				const std::weak_ptr<SharedState> weakState = shared;
				const std::weak_ptr<LuaSubscriptionEntry> weakEntry = entry;
				return subscribeCddaObservations(
						[weakState, weakEntry, filter](const CddaObservation& observation) {
							const auto bit = std::uint32_t {1}
									<< (static_cast<unsigned>(observation.type) - 1u);
							if ((filter.typeMask & bit) == 0)
								return;
							if (filter.hasCommand
									&& ((observation.type != CddaObservationType::ControlAccepted
												&& observation.type != CddaObservationType::ControlApplied)
											|| observation.command != filter.command))
								return;
							if (filter.hasSuccessful)
							{
								const bool successful = observation.type
										== CddaObservationType::ControlApplied
										? observation.appliedSuccessfully
										: observation.readSuccessful;
								if (successful != filter.successful)
									return;
							}
							if (filter.hasFadRange
									&& (observation.type != CddaObservationType::Sector
											|| observation.fad < filter.fadStart
											|| observation.fad >= filter.fadEndExclusive))
								return;
							enqueueLuaObservation(weakState, weakEntry, observation);
						});
			},
			[](std::uint64_t token) { unsubscribeCddaObservations(token); });
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
						std::get<PvrTaObservation>(queued.observation));
				break;
			case LuaSubscriptionEntry::Kind::PvrPresentation:
				queued.entry->pvrPresentationCallback(queued.entry->token,
						std::get<PvrPresentationObservation>(queued.observation));
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

#include "research/maple_observation.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace research
{
namespace
{

struct SubscriptionEntry
{
	MapleObservationSubscription id = 0;
	MapleObservationFilter filter;
	MapleObservationCallback callback;
	std::atomic<bool> active {true};
};

std::mutex subscriptionsMutex;
std::recursive_mutex dispatchMutex;
std::vector<std::shared_ptr<SubscriptionEntry>> subscriptions;
std::atomic<std::size_t> activeSubscriptionCount {0};
std::atomic<std::uint64_t> nextSubscription {1};
std::atomic<std::uint64_t> nextEmissionOrdinal {0};
std::atomic<std::uint64_t> nextDmaOrdinal {0};
std::atomic<std::uint64_t> nextTransactionOrdinal {0};
thread_local bool publishingObservation = false;

void validateFilter(const MapleObservationFilter& filter)
{
	if (filter.typeMask == 0
			|| (filter.typeMask & ~AllMapleObservationTypes) != 0)
		throw std::invalid_argument("Maple observation filter has an invalid type mask");
	if (filter.busMask == 0 || (filter.busMask & ~std::uint8_t {0x0f}) != 0)
		throw std::invalid_argument("Maple observation filter has an invalid bus mask");
	if (filter.portMask == 0 || (filter.portMask & ~std::uint8_t {0x3f}) != 0)
		throw std::invalid_argument("Maple observation filter has an invalid port mask");
}

void validateObservation(const MapleObservation& observation)
{
	if (observation.schemaVersion != MapleObservationSchemaVersion)
		throw std::invalid_argument("unsupported Maple observation schema version");
	if (observation.type != MapleObservationType::Request
			&& observation.type != MapleObservationType::Response)
		throw std::invalid_argument("Maple observation has an invalid type");
	if (observation.bus > 3 || observation.port > 5)
		throw std::invalid_argument("Maple observation has invalid topology");
	if ((observation.flags & ~MapleTransactionDevicePresent) != 0)
		throw std::invalid_argument("Maple observation has invalid flags");
	const bool devicePresent = (observation.flags
			& MapleTransactionDevicePresent) != 0;
	if (devicePresent != (observation.deviceType != UINT32_MAX))
		throw std::invalid_argument("Maple observation device presence and type disagree");
	if (observation.payload.size() > 1024 || observation.payload.size() % 4 != 0
			|| (observation.type == MapleObservationType::Request
					&& observation.payload.empty()))
		throw std::invalid_argument("Maple observation has an invalid payload size");
	if (observation.type == MapleObservationType::Request
			&& observation.payload.front() != observation.command)
		throw std::invalid_argument("Maple request command does not match its payload");
	if (observation.type == MapleObservationType::Request
			&& (static_cast<std::size_t>(observation.payload[3]) + 1u) * 4u
					!= observation.payload.size())
		throw std::invalid_argument("Maple request frame length is inconsistent");
	if (observation.type == MapleObservationType::Response && devicePresent
			&& !observation.payload.empty()
			&& (static_cast<std::size_t>(observation.payload[3]) + 1u) * 4u
					!= observation.payload.size())
		throw std::invalid_argument("Maple response frame length is inconsistent");
}

bool matches(const MapleObservationFilter& filter,
		const MapleObservation& observation)
{
	return (filter.typeMask & mapleObservationTypeBit(observation.type)) != 0
			&& (filter.busMask & (std::uint8_t {1} << observation.bus)) != 0
			&& (filter.portMask & (std::uint8_t {1} << observation.port)) != 0
			&& (!filter.hasCommand || filter.command == observation.command);
}

bool publishMapleObservation(MapleObservation observation) noexcept
{
	try
	{
		if (!mapleObservationBusActive() || publishingObservation)
			return false;
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		if (!mapleObservationBusActive() || publishingObservation)
			return false;
		publishingObservation = true;
		struct PublishingReset
		{
			~PublishingReset() { publishingObservation = false; }
		} publishingReset;
		observation.emissionOrdinal = nextEmissionOrdinal.fetch_add(1,
				std::memory_order_relaxed);
		std::vector<std::shared_ptr<SubscriptionEntry>> snapshot;
		{
			const std::lock_guard<std::mutex> lock(subscriptionsMutex);
			snapshot = subscriptions;
		}
		bool delivered = false;
		for (const std::shared_ptr<SubscriptionEntry>& entry : snapshot)
		{
			if (!entry->active.load(std::memory_order_acquire)
					|| !matches(entry->filter, observation))
				continue;
			try
			{
				entry->callback(observation);
			}
			catch (...)
			{
				// Discovery subscribers cannot alter Maple execution or prevent
				// later subscribers and the paired response from being observed.
			}
			delivered = true;
		}
		return delivered;
	}
	catch (...)
	{
		// Allocation or synchronization failure drops discovery output only.
		return false;
	}
}

} // namespace

MapleObservationSubscription subscribeMapleObservations(
		const MapleObservationFilter& filter, MapleObservationCallback callback)
{
	validateFilter(filter);
	if (!callback)
		throw std::invalid_argument("Maple observation callback is empty");
	const MapleObservationSubscription id = nextSubscription.fetch_add(1,
			std::memory_order_relaxed);
	if (id == 0)
		throw std::overflow_error("Maple observation subscription id overflow");
	auto entry = std::make_shared<SubscriptionEntry>();
	entry->id = id;
	entry->filter = filter;
	entry->callback = std::move(callback);
	{
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		const std::lock_guard<std::mutex> lock(subscriptionsMutex);
		subscriptions.push_back(std::move(entry));
		activeSubscriptionCount.fetch_add(1, std::memory_order_release);
	}
	return id;
}

bool unsubscribeMapleObservations(MapleObservationSubscription subscription) noexcept
{
	if (subscription == 0)
		return false;
	const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
	const std::lock_guard<std::mutex> lock(subscriptionsMutex);
	const auto found = std::find_if(subscriptions.begin(), subscriptions.end(),
			[subscription](const std::shared_ptr<SubscriptionEntry>& entry) {
				return entry->id == subscription;
			});
	if (found == subscriptions.end())
		return false;
	(*found)->active.store(false, std::memory_order_release);
	subscriptions.erase(found);
	activeSubscriptionCount.fetch_sub(1, std::memory_order_release);
	return true;
}

bool mapleObservationBusActive() noexcept
{
	return activeSubscriptionCount.load(std::memory_order_acquire) != 0;
}

std::size_t mapleObservationSubscriberCount() noexcept
{
	return activeSubscriptionCount.load(std::memory_order_acquire);
}

std::uint64_t beginMapleObservationDma() noexcept
{
	return mapleObservationBusActive()
			? nextDmaOrdinal.fetch_add(1, std::memory_order_relaxed) : UINT64_MAX;
}

bool publishMapleTransactionObservations(std::uint64_t observationDmaOrdinal,
		const MapleTransactionEvent& transaction) noexcept

{
	return publishMapleTransactionObservations(observationDmaOrdinal, transaction,
			transaction.response);
}

bool publishMapleTransactionObservations(std::uint64_t observationDmaOrdinal,
		const MapleTransactionEvent& transaction,
		const std::vector<std::uint8_t>& selectedResponse) noexcept
{
	try
	{
		if (observationDmaOrdinal == UINT64_MAX || !mapleObservationBusActive())
			return false;
		const std::uint64_t transactionOrdinal = nextTransactionOrdinal.fetch_add(1,
				std::memory_order_relaxed);
		MapleObservation request;
		request.dmaOrdinal = observationDmaOrdinal;
		request.transactionOrdinal = transactionOrdinal;
		request.tick = transaction.tick;
		request.descriptorAddress = transaction.descriptorAddress;
		request.destinationAddress = transaction.destinationAddress;
		request.descriptorHeader1 = transaction.descriptorHeader1;
		request.descriptorHeader2 = transaction.descriptorHeader2;
		request.deviceType = transaction.deviceType;
		request.bus = transaction.bus;
		request.port = transaction.port;
		request.command = transaction.command;
		request.flags = transaction.flags;
		request.type = MapleObservationType::Request;
		request.payload = transaction.request;
		MapleObservation response = request;
		response.type = MapleObservationType::Response;
		response.payload = selectedResponse;
		// Validate and allocate the complete pair before publishing either half.
		validateObservation(request);
		validateObservation(response);
		const bool requestDelivered = publishMapleObservation(std::move(request));
		return publishMapleObservation(std::move(response)) || requestDelivered;
	}
	catch (...)
	{
		// Guest-controlled malformed frames and host allocation failures may
		// suppress discovery, but can never escape into Maple execution.
		return false;
	}
}

} // namespace research

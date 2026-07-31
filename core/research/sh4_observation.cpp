#include "research/sh4_observation.h"

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
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
	Sh4ObservationSubscription id = 0;
	Sh4ObservationFilter filter;
	Sh4ObservationCallback callback;
	std::atomic<bool> active {true};
};

std::mutex subscriptionsMutex;
std::recursive_mutex dispatchMutex;
std::vector<std::shared_ptr<SubscriptionEntry>> subscriptions;
std::atomic<std::size_t> activeSubscriptionCount {0};
std::array<std::atomic<std::size_t>, 2> activeBackendSubscriptionCounts {};
std::array<std::atomic<std::uint64_t>, 2> backendSubscriptionGenerations {};
std::atomic<std::uint64_t> nextSubscription {1};
std::atomic<std::uint64_t> nextEmissionOrdinal {0};
thread_local bool publishingObservation = false;

bool isMemoryObservation(Sh4ObservationType type)
{
	return type == Sh4ObservationType::MemoryRead
			|| type == Sh4ObservationType::MemoryWrite;
}

std::size_t backendIndex(Sh4ObservationBackend backend)
{
	return static_cast<std::size_t>(backend) - 1;
}

void updateBackendCounts(std::uint32_t backendMask, bool add) noexcept
{
	for (const Sh4ObservationBackend backend : {Sh4ObservationBackend::Interpreter,
			Sh4ObservationBackend::Dynarec})
	{
		if ((backendMask & sh4ObservationBackendBit(backend)) == 0)
			continue;
		backendSubscriptionGenerations[backendIndex(backend)].fetch_add(1,
				std::memory_order_acq_rel);
		if (add)
			activeBackendSubscriptionCounts[backendIndex(backend)].fetch_add(1,
					std::memory_order_release);
		else
			activeBackendSubscriptionCounts[backendIndex(backend)].fetch_sub(1,
					std::memory_order_release);
	}
}

bool matches(const Sh4ObservationFilter& filter, const Sh4Observation& observation)
{
	if ((filter.backendMask & sh4ObservationBackendBit(observation.backend)) == 0)
		return false;
	if ((filter.typeMask & sh4ObservationTypeBit(observation.type)) == 0)
		return false;
	if (!filter.hasMemoryRange || !isMemoryObservation(observation.type))
		return true;
	const std::uint64_t width = observation.memoryWidth;
	const std::uint64_t end = static_cast<std::uint64_t>(observation.memoryAddress) + width;
	return width != 0 && observation.memoryAddress < filter.memoryEndExclusive
			&& filter.memoryStart < end;
}

void validateFilter(const Sh4ObservationFilter& filter)
{
	if (filter.backendMask == 0
			|| (filter.backendMask & ~AllSh4ObservationBackends) != 0)
		throw std::invalid_argument("SH-4 observation filter has an invalid backend mask");
	if (filter.typeMask == 0 || (filter.typeMask & ~AllSh4ObservationTypes) != 0)
		throw std::invalid_argument("SH-4 observation filter has an invalid type mask");
	if (filter.hasMemoryRange
			&& (filter.memoryEndExclusive > (std::uint64_t {1} << 32)
					|| filter.memoryStart >= filter.memoryEndExclusive))
		throw std::invalid_argument("SH-4 observation filter has an invalid memory range");
}

void validateObservation(const Sh4Observation& observation)
{
	if ((observation.availableFields
			& ~(Sh4Observation::HasNextPc | Sh4Observation::HasRegisters)) != 0)
		throw std::invalid_argument("SH-4 observation has unknown available fields");
	const unsigned backend = static_cast<unsigned>(observation.backend);
	if (backend < static_cast<unsigned>(Sh4ObservationBackend::Interpreter)
			|| backend > static_cast<unsigned>(Sh4ObservationBackend::Dynarec))
		throw std::invalid_argument("SH-4 observation has an invalid backend");
	const unsigned type = static_cast<unsigned>(observation.type);
	if (type < static_cast<unsigned>(Sh4ObservationType::InstructionBegin)
			|| type > static_cast<unsigned>(Sh4ObservationType::Return))
		throw std::invalid_argument("SH-4 observation has an invalid type");
	if (isMemoryObservation(observation.type) && observation.memoryWidth != 1
			&& observation.memoryWidth != 2 && observation.memoryWidth != 4
			&& observation.memoryWidth != 8)
		throw std::invalid_argument("SH-4 memory observation has an invalid width");
}

} // namespace

Sh4ObservationSubscription subscribeSh4Observations(
		const Sh4ObservationFilter& filter, Sh4ObservationCallback callback)
{
	validateFilter(filter);
	if (!callback)
		throw std::invalid_argument("SH-4 observation callback is empty");
	const Sh4ObservationSubscription id = nextSubscription.fetch_add(1,
			std::memory_order_relaxed);
	if (id == 0)
		throw std::overflow_error("SH-4 observation subscription id overflow");
	auto entry = std::make_shared<SubscriptionEntry>();
	entry->id = id;
	entry->filter = filter;
	entry->callback = std::move(callback);
	{
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		const std::lock_guard<std::mutex> lock(subscriptionsMutex);
		subscriptions.push_back(std::move(entry));
		activeSubscriptionCount.fetch_add(1, std::memory_order_release);
		updateBackendCounts(filter.backendMask, true);
	}
	return id;
}

bool unsubscribeSh4Observations(Sh4ObservationSubscription subscription) noexcept
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
	updateBackendCounts((*found)->filter.backendMask, false);
	subscriptions.erase(found);
	activeSubscriptionCount.fetch_sub(1, std::memory_order_release);
	return true;
}

bool sh4ObservationBusActive() noexcept
{
	return activeSubscriptionCount.load(std::memory_order_acquire) != 0;
}

bool sh4ObservationBusActive(Sh4ObservationBackend backend) noexcept
{
	if (backend != Sh4ObservationBackend::Interpreter
			&& backend != Sh4ObservationBackend::Dynarec)
		return false;
	return activeBackendSubscriptionCounts[backendIndex(backend)].load(
			std::memory_order_acquire) != 0;
}

std::size_t sh4ObservationSubscriberCount() noexcept
{
	return activeSubscriptionCount.load(std::memory_order_acquire);
}

std::size_t sh4ObservationSubscriberCount(Sh4ObservationBackend backend) noexcept
{
	if (backend != Sh4ObservationBackend::Interpreter
			&& backend != Sh4ObservationBackend::Dynarec)
		return 0;
	return activeBackendSubscriptionCounts[backendIndex(backend)].load(
			std::memory_order_acquire);
}

std::uint64_t sh4ObservationSubscriptionGeneration(
		Sh4ObservationBackend backend) noexcept
{
	if (backend != Sh4ObservationBackend::Interpreter
			&& backend != Sh4ObservationBackend::Dynarec)
		return 0;
	return backendSubscriptionGenerations[backendIndex(backend)].load(
			std::memory_order_acquire);
}

bool publishSh4Observation(Sh4Observation observation)
{
	if (observation.schemaVersion != Sh4ObservationSchemaVersion)
		throw std::invalid_argument("unsupported SH-4 observation schema version");
	validateObservation(observation);
	if (!sh4ObservationBusActive(observation.backend))
		return false;
	if (publishingObservation)
		throw std::logic_error("recursive SH-4 observation publication is not supported");
	const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
	if (!sh4ObservationBusActive(observation.backend))
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
	std::exception_ptr firstFailure;
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
			if (firstFailure == nullptr)
				firstFailure = std::current_exception();
		}
		delivered = true;
	}
	if (firstFailure != nullptr)
		std::rethrow_exception(firstFailure);
	return delivered;
}

bool decodeSh4Call(const Sh4InstructionState& state, Sh4CallKind& kind,
		std::uint32_t& targetPc)
{
	const std::uint16_t opcode = state.opcode;
	if ((opcode & 0xf000u) == 0xb000u)
	{
		kind = Sh4CallKind::Bsr;
		const std::int32_t displacement = static_cast<std::int16_t>(
				static_cast<std::uint16_t>((opcode & 0x0fffu) << 4)) >> 4;
		targetPc = state.pc + 4u + static_cast<std::uint32_t>(displacement * 2);
		return true;
	}
	if ((opcode & 0xf0ffu) == 0x0003u)
	{
		kind = Sh4CallKind::Bsrf;
		const std::uint32_t registerIndex = (opcode >> 8) & 0x0fu;
		targetPc = state.pc + 4u + state.registers.r[registerIndex];
		return true;
	}
	if ((opcode & 0xf0ffu) == 0x400bu)
	{
		kind = Sh4CallKind::Jsr;
		const std::uint32_t registerIndex = (opcode >> 8) & 0x0fu;
		targetPc = state.registers.r[registerIndex];
		return true;
	}
	return false;
}

} // namespace research

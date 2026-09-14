#include "research/pvr_draw_observation.h"

#include <algorithm>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace research
{
namespace
{

struct SubscriptionEntry
{
	PvrDrawObservationSubscription id = 0;
	PvrDrawObservationCallback callback;
	std::atomic<bool> active {true};
	bool evidence = false;
};

std::mutex subscriptionsMutex;
std::recursive_mutex dispatchMutex;
std::vector<std::shared_ptr<SubscriptionEntry>> subscriptions;
std::atomic<std::size_t> activeSubscriptionCount {0};
std::atomic<bool> activeEvidenceSubscription {false};
std::atomic<std::uint64_t> nextSubscription {1};
std::atomic<std::uint64_t> nextEmissionOrdinal {0};
std::atomic<std::uint64_t> nextPrimitiveGeneration {1};
std::atomic<std::uint64_t> nextRasterGeneration {1};
std::atomic<std::uint64_t> droppedObservationCount {0};
thread_local bool publishingObservation = false;

void noteDroppedObservation() noexcept
{
	droppedObservationCount.fetch_add(1, std::memory_order_relaxed);
}

bool sameOwner(const Sh4InstructionOwnerToken& lhs,
		const Sh4InstructionOwnerToken& rhs) noexcept
{
	return lhs.valid && rhs.valid && lhs.backend == rhs.backend
			&& lhs.generation == rhs.generation && lhs.tick == rhs.tick
			&& lhs.pc == rhs.pc && lhs.pr == rhs.pr
			&& lhs.opcode == rhs.opcode
			&& lhs.delaySlotDepth == rhs.delaySlotDepth;
}

bool publish(PvrDrawObservation observation) noexcept
{
	try
	{
		if (!pvrDrawObservationBusActive())
			return false;
		if (publishingObservation)
		{
			noteDroppedObservation();
			return false;
		}
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		if (!pvrDrawObservationBusActive())
		{
			noteDroppedObservation();
			return false;
		}
		publishingObservation = true;
		struct PublishingReset
		{
			~PublishingReset() { publishingObservation = false; }
		} reset;
		observation.emissionOrdinal = nextEmissionOrdinal.fetch_add(1,
				std::memory_order_relaxed);
		std::vector<std::shared_ptr<SubscriptionEntry>> snapshot;
		{
			const std::lock_guard<std::mutex> lock(subscriptionsMutex);
			snapshot = subscriptions;
		}
		bool delivered = false;
		for (const auto& entry : snapshot)
		{
			if (!entry->active.load(std::memory_order_acquire))
				continue;
			try
			{
				entry->callback(observation);
			}
			catch (...)
			{
				noteDroppedObservation();
			}
			delivered = true;
		}
		return delivered;
	}
	catch (...)
	{
		noteDroppedObservation();
		return false;
	}
}

PvrDrawObservationSubscription subscribe(PvrDrawObservationCallback callback,
		bool evidence)
{
	if (!callback)
		throw std::invalid_argument("PowerVR draw observation callback is empty");
	const auto id = nextSubscription.fetch_add(1, std::memory_order_relaxed);
	if (id == 0)
		throw std::overflow_error("PowerVR draw subscription id overflow");
	auto entry = std::make_shared<SubscriptionEntry>();
	entry->id = id;
	entry->callback = std::move(callback);
	entry->evidence = evidence;
	{
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		const std::lock_guard<std::mutex> lock(subscriptionsMutex);
		if (evidence && !subscriptions.empty())
			throw std::logic_error(
					"PowerVR draw evidence observation requires exclusive ownership");
		if (!evidence && activeEvidenceSubscription.load(std::memory_order_acquire))
			throw std::logic_error(
					"PowerVR draw evidence observation owns the bus exclusively");
		if (subscriptions.empty())
			beginPvrTaProvenanceSession();
		subscriptions.push_back(std::move(entry));
		retainSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
		retainSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
		if (evidence)
			activeEvidenceSubscription.store(true, std::memory_order_release);
		activeSubscriptionCount.fetch_add(1, std::memory_order_release);
	}
	return id;
}

} // namespace

PvrDrawObservationSubscription subscribePvrDrawObservations(
		PvrDrawObservationCallback callback)
{
	return subscribe(std::move(callback), false);
}

PvrDrawObservationSubscription subscribePvrDrawEvidenceObservations(
		PvrDrawObservationCallback callback)
{
	return subscribe(std::move(callback), true);
}

bool unsubscribePvrDrawObservations(
		PvrDrawObservationSubscription subscription) noexcept
{
	if (subscription == 0)
		return false;
	const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
	const std::lock_guard<std::mutex> lock(subscriptionsMutex);
	const auto found = std::find_if(subscriptions.begin(), subscriptions.end(),
			[subscription](const auto& entry) { return entry->id == subscription; });
	if (found == subscriptions.end())
		return false;
	(*found)->active.store(false, std::memory_order_release);
	if ((*found)->evidence)
		activeEvidenceSubscription.store(false, std::memory_order_release);
	subscriptions.erase(found);
	activeSubscriptionCount.fetch_sub(1, std::memory_order_release);
	releaseSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
	releaseSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
	return true;
}

bool pvrDrawObservationBusActive() noexcept
{
	return activeSubscriptionCount.load(std::memory_order_acquire) != 0;
}

bool pvrDrawEvidenceSubscriptionActive() noexcept
{
	return activeEvidenceSubscription.load(std::memory_order_acquire);
}

std::uint64_t pvrDrawObservationDroppedCount() noexcept
{
	return droppedObservationCount.load(std::memory_order_acquire);
}

std::uint64_t allocatePvrPrimitiveGeneration() noexcept
{
	if (!pvrDrawObservationBusActive())
		return 0;
	const auto generation = nextPrimitiveGeneration.fetch_add(1,
			std::memory_order_relaxed);
	if (generation == 0)
	{
		noteDroppedObservation();
		return 0;
	}
	return generation;
}

PvrPrimitiveOwnerClass classifyPvrPrimitiveOwnership(
		const std::vector<PvrTaBlockProvenance>& parameterBlocks,
		const std::vector<PvrTaBlockProvenance>& vertexBlocks) noexcept
{
	const Sh4InstructionOwnerToken* owner = nullptr;
	bool sawBlock = false;
	for (const auto* blocks : {&parameterBlocks, &vertexBlocks})
	{
		for (const PvrTaBlockProvenance& block : *blocks)
		{
			sawBlock = true;
			if (!block.available || !block.initiator.valid)
				return PvrPrimitiveOwnerClass::Mixed;
			if (owner == nullptr)
				owner = &block.initiator;
			else if (!sameOwner(*owner, block.initiator))
				return PvrPrimitiveOwnerClass::Mixed;
		}
	}
	return !sawBlock ? PvrPrimitiveOwnerClass::Unowned
			: PvrPrimitiveOwnerClass::Exact;
}

void observePvrPrimitiveDecoded(PvrDrawObservation observation) noexcept
{
	if (!pvrDrawObservationBusActive())
		return;
	if (!pvrTaRenderGenerationObserved(observation.renderGeneration))
		return;
	if (observation.renderGeneration == 0 || observation.primitiveGeneration == 0
			|| observation.count == 0)
	{
		noteDroppedObservation();
		return;
	}
	observation.type = PvrDrawObservationType::PrimitiveDecoded;
	observation.ownerClass = classifyPvrPrimitiveOwnership(
			observation.parameterBlocks, observation.vertexBlocks);
	publish(std::move(observation));
}

std::uint64_t observePvrDrawConsumed(std::uint64_t renderGeneration,
		const std::vector<std::uint64_t>& primitiveGenerations,
		PvrDrawBackend backend,
		PvrDrawPass drawPass, std::uint32_t first, std::uint32_t count,
		bool indexed, std::uint64_t tick) noexcept
{
	if (!pvrDrawObservationBusActive())
		return 0;
	if (!pvrTaRenderGenerationObserved(renderGeneration))
		return 0;
	if (renderGeneration == 0 || count == 0)
	{
		noteDroppedObservation();
		return 0;
	}
	PvrDrawObservation observation;
	observation.type = PvrDrawObservationType::DrawConsumed;
	observation.tick = tick;
	observation.renderGeneration = renderGeneration;
	observation.primitiveGenerations.reserve(primitiveGenerations.size());
	std::copy_if(primitiveGenerations.begin(), primitiveGenerations.end(),
			std::back_inserter(observation.primitiveGenerations),
			[](std::uint64_t generation) { return generation != 0; });
	if (observation.primitiveGenerations.empty()
			&& drawPass != PvrDrawPass::ModifierResolve)
	{
		// Renderer work sourced only from pre-capture state has no causal
		// primitive identity. Do not manufacture an ordinary owned draw.
		return 0;
	}
	const auto rasterGeneration = nextRasterGeneration.fetch_add(1,
			std::memory_order_relaxed);
	if (rasterGeneration == 0)
	{
		noteDroppedObservation();
		return 0;
	}
	observation.rasterGeneration = rasterGeneration;
	observation.backend = backend;
	observation.drawPass = drawPass;
	observation.first = first;
	observation.count = count;
	observation.indexed = indexed;
	publish(std::move(observation));
	return rasterGeneration;
}

void observePvrDrawRenderCompleted(std::uint64_t renderGeneration,
		bool successful, std::uint64_t tick) noexcept
{
	if (!pvrDrawObservationBusActive())
		return;
	if (!pvrTaRenderGenerationObserved(renderGeneration))
		return;
	if (renderGeneration == 0)
	{
		noteDroppedObservation();
		return;
	}
	PvrDrawObservation observation;
	observation.type = PvrDrawObservationType::RenderCompleted;
	observation.tick = tick;
	observation.renderGeneration = renderGeneration;
	observation.successful = successful;
	publish(std::move(observation));
}

void resetPvrDrawObservation(std::uint64_t tick) noexcept
{
	if (!pvrDrawObservationBusActive())
		return;
	PvrDrawObservation observation;
	observation.type = PvrDrawObservationType::Reset;
	observation.tick = tick;
	publish(std::move(observation));
}

} // namespace research

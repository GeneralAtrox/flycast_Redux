#include "research/pvr_ta_observation.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace research
{
namespace
{

struct SubscriptionEntry
{
	PvrTaObservationSubscription id = 0;
	PvrTaObservationFilter filter;
	PvrTaObservationCallback callback;
	std::atomic<bool> active {true};
	bool evidence = false;
};

struct ContextState
{
	std::uint64_t generation = 0;
	std::uint64_t nextBlockOrdinal = 0;
};

std::mutex subscriptionsMutex;
std::recursive_mutex dispatchMutex;
std::mutex stateMutex;
std::vector<std::shared_ptr<SubscriptionEntry>> subscriptions;
std::unordered_map<std::uint32_t, ContextState> contexts;
std::atomic<std::size_t> activeSubscriptionCount {0};
std::atomic<bool> activeEvidenceSubscription {false};
std::atomic<std::uint64_t> nextSubscription {1};
std::atomic<std::uint64_t> nextEmissionOrdinal {0};
std::atomic<std::uint64_t> droppedObservationCount {0};
std::atomic<std::uint64_t> nextContextGeneration {1};
std::atomic<std::uint64_t> nextRenderGeneration {1};
std::uint64_t pendingRenderGeneration = 0;
thread_local bool publishingObservation = false;

void noteDroppedObservation() noexcept
{
	droppedObservationCount.fetch_add(1, std::memory_order_relaxed);
}

void validateFilter(const PvrTaObservationFilter& filter)
{
	if (filter.typeMask == 0
			|| (filter.typeMask & ~AllPvrTaObservationTypes) != 0)
		throw std::invalid_argument("PowerVR TA observation filter has an invalid type mask");
	if (filter.sourceMask == 0
			|| (filter.sourceMask & ~AllPvrTaInputSources) != 0)
		throw std::invalid_argument("PowerVR TA observation filter has an invalid source mask");
}

bool matches(const PvrTaObservationFilter& filter,
		const PvrTaObservation& observation)
{
	if ((filter.typeMask & pvrTaObservationTypeBit(observation.type)) == 0)
		return false;
	return observation.type != PvrTaObservationType::AcceptedBlock
			|| (filter.sourceMask & pvrTaInputSourceBit(observation.source)) != 0;
}

PvrTaObservation baseObservation(PvrTaObservationType type,
		std::uint64_t tick)
{
	PvrTaObservation observation;
	observation.type = type;
	observation.initiator = sh4ObservationCurrentInstructionOwner();
	// sh4_sched_now64() is the scheduler's slice base while an interpreter
	// instruction is open. The ownership token carries the precise cycle for
	// that instruction, so a synchronous hardware boundary cannot precede it.
	observation.tick = observation.initiator.valid
			? std::max(tick, observation.initiator.tick) : tick;
	return observation;
}

bool publish(PvrTaObservation observation) noexcept
{
	try
	{
		if (!pvrTaObservationBusActive())
			return false;
		if (publishingObservation)
		{
			noteDroppedObservation();
			return false;
		}
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		if (!pvrTaObservationBusActive())
		{
			noteDroppedObservation();
			return false;
		}
		if (publishingObservation)
		{
			noteDroppedObservation();
			return false;
		}
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
				// Callbacks cannot change TA parsing or render timing, but evidence
				// recorders must be able to fail closed when delivery was lost.
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

PvrTaObservationSubscription subscribe(
		const PvrTaObservationFilter& filter, PvrTaObservationCallback callback,
		bool evidence)
{
	validateFilter(filter);
	if (!callback)
		throw std::invalid_argument("PowerVR TA observation callback is empty");
	const PvrTaObservationSubscription id = nextSubscription.fetch_add(1,
			std::memory_order_relaxed);
	if (id == 0)
		throw std::overflow_error("PowerVR TA observation subscription id overflow");
	auto entry = std::make_shared<SubscriptionEntry>();
	entry->id = id;
	entry->filter = filter;
	entry->callback = std::move(callback);
	entry->evidence = evidence;
	{
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		const std::lock_guard<std::mutex> subscriptionLock(subscriptionsMutex);
		if (evidence && !subscriptions.empty())
			throw std::logic_error(
					"PowerVR TA evidence observation requires exclusive ownership");
		if (!evidence && activeEvidenceSubscription.load(std::memory_order_acquire))
			throw std::logic_error(
					"PowerVR TA evidence observation owns the bus exclusively");
		if (subscriptions.empty())
		{
			const std::lock_guard<std::mutex> stateLock(stateMutex);
			contexts.clear();
			pendingRenderGeneration = 0;
		}
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

PvrTaObservationSubscription subscribePvrTaObservations(
		const PvrTaObservationFilter& filter, PvrTaObservationCallback callback)
{
	return subscribe(filter, std::move(callback), false);
}

PvrTaObservationSubscription subscribePvrTaEvidenceObservations(
		PvrTaObservationCallback callback)
{
	return subscribe(PvrTaObservationFilter {}, std::move(callback), true);
}

bool unsubscribePvrTaObservations(
		PvrTaObservationSubscription subscription) noexcept
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
	if ((*found)->evidence)
		activeEvidenceSubscription.store(false, std::memory_order_release);
	subscriptions.erase(found);
	activeSubscriptionCount.fetch_sub(1, std::memory_order_release);
	releaseSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
	releaseSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
	return true;
}

bool pvrTaObservationBusActive() noexcept
{
	return activeSubscriptionCount.load(std::memory_order_acquire) != 0;
}

bool pvrTaEvidenceSubscriptionActive() noexcept
{
	return activeEvidenceSubscription.load(std::memory_order_acquire);
}

std::size_t pvrTaObservationSubscriberCount() noexcept
{
	return activeSubscriptionCount.load(std::memory_order_acquire);
}

std::uint64_t pvrTaObservationDroppedCount() noexcept
{
	return droppedObservationCount.load(std::memory_order_acquire);
}

void observePvrTaListBoundary(bool continuation, std::uint32_t contextAddress,
		std::uint32_t renderPass, std::uint64_t tick) noexcept
{
	if (!pvrTaObservationBusActive())
		return;
	try
	{
		PvrTaObservation observation = baseObservation(continuation
				? PvrTaObservationType::ListContinue
				: PvrTaObservationType::ListInit, tick);
		observation.contextAddress = contextAddress;
		observation.renderPass = renderPass;
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			if (!continuation)
			{
				ContextState& state = contexts[contextAddress];
				state.generation = nextContextGeneration.fetch_add(1,
						std::memory_order_relaxed);
				state.nextBlockOrdinal = 0;
				observation.contextGeneration = state.generation;
			}
			else
			{
				const auto found = contexts.find(contextAddress);
				if (found != contexts.end())
					observation.contextGeneration = found->second.generation;
			}
		}
		publish(std::move(observation));
	}
	catch (...)
	{
		noteDroppedObservation();
	}
}

void observePvrTaAcceptedBlock(PvrTaInputSource source,
		std::uint32_t sourceAddress, std::uint32_t taAddress,
		const std::uint8_t* block, std::uint32_t contextAddress,
		std::uint32_t renderPass, std::uint32_t listTypeBefore,
		std::uint32_t listTypeAfter, std::uint32_t parserStateBefore,
		std::uint32_t parserStateAfter, std::uint64_t tick) noexcept
{
	if (!pvrTaObservationBusActive())
		return;
	if (block == nullptr)
	{
		noteDroppedObservation();
		return;
	}
	try
	{
		PvrTaObservation observation = baseObservation(
				PvrTaObservationType::AcceptedBlock, tick);
		observation.source = source;
		observation.sourceAddress = sourceAddress;
		observation.taAddress = taAddress;
		observation.contextAddress = contextAddress;
		observation.renderPass = renderPass;
		observation.listTypeBefore = listTypeBefore;
		observation.listTypeAfter = listTypeAfter;
		observation.parserStateBefore = parserStateBefore;
		observation.parserStateAfter = parserStateAfter;
		std::memcpy(observation.block.data(), block, observation.block.size());
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			const auto found = contexts.find(contextAddress);
			if (found != contexts.end())
			{
				observation.contextGeneration = found->second.generation;
				observation.contextBlockOrdinal = found->second.nextBlockOrdinal++;
			}
		}
		publish(std::move(observation));
	}
	catch (...)
	{
		noteDroppedObservation();
	}
}

std::uint64_t observePvrTaStartRender(const std::uint32_t* contextAddresses,
		const bool* contextAvailability, std::size_t contextCount,
		const PvrTaRenderSelectionTranscript* transcript,
		std::uint64_t tick) noexcept
{
	const std::uint64_t renderGeneration = nextRenderGeneration.fetch_add(1,
			std::memory_order_relaxed);
	if (!pvrTaObservationBusActive())
		return renderGeneration;
	if ((contextCount != 0
			&& (contextAddresses == nullptr || contextAvailability == nullptr))
			|| transcript == nullptr || !transcript->initialized
			|| transcript->overflow)
	{
		noteDroppedObservation();
		return renderGeneration;
	}
	try
	{
		PvrTaObservation observation = baseObservation(
				PvrTaObservationType::StartRender, tick);
		observation.renderGeneration = renderGeneration;
		observation.renderContextAvailable = contextCount != 0
				&& contextAvailability[0];
		observation.regionBase = transcript->regionBase;
		observation.fpuParamCfg = transcript->fpuParamCfg;
		observation.renderSelectionReadCount = transcript->readCount;
		std::copy_n(transcript->reads.begin(), transcript->readCount,
				observation.renderSelectionReads.begin());
		observation.selectedContexts.reserve(contextCount);
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			for (std::size_t index = 0; index < contextCount; ++index)
			{
				PvrTaContextRef ref;
				ref.address = contextAddresses[index];
				ref.available = contextAvailability[index];
				const auto found = contexts.find(ref.address);
				if (ref.available && found != contexts.end())
				{
					ref.generation = found->second.generation;
					contexts.erase(found);
				}
				observation.selectedContexts.push_back(ref);
			}
			pendingRenderGeneration = observation.renderGeneration;
		}
		publish(std::move(observation));
	}
	catch (...)
	{
		noteDroppedObservation();
	}
	return renderGeneration;
}

void observePvrTaRenderDone(std::uint64_t tick) noexcept
{
	if (!pvrTaObservationBusActive())
		return;
	try
	{
		PvrTaObservation observation = baseObservation(
				PvrTaObservationType::RenderDone, tick);
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			observation.renderGeneration = pendingRenderGeneration;
			pendingRenderGeneration = 0;
		}
		publish(std::move(observation));
	}
	catch (...)
	{
		noteDroppedObservation();
	}
}

void resetPvrTaObservation(std::uint64_t tick) noexcept
{
	if (!pvrTaObservationBusActive())
		return;
	try
	{
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			contexts.clear();
			pendingRenderGeneration = 0;
		}
		publish(baseObservation(PvrTaObservationType::Reset, tick));
	}
	catch (...)
	{
		noteDroppedObservation();
	}
}

} // namespace research

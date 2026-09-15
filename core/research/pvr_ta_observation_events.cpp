#include "research/pvr_ta_observation.h"
#include "research/pvr_ta_observation_internal.h"
#include "research/pvr_draw_observation.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

namespace research
{

using namespace detail_pvr_ta;

namespace
{

bool provenanceActive() noexcept
{
	return pvrTaObservationBusActive() || pvrDrawObservationBusActive();
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

} // namespace

void observePvrTaListBoundary(bool continuation, std::uint32_t contextAddress,
		std::uint32_t renderPass, std::uint64_t tick) noexcept
{
	if (!provenanceActive())
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
				// A continuation for a context created before observation began
				// has no causal generation. Do not write an invalid partial
				// context into an evidence artifact; wait for the next observed
				// ListInit boundary to establish provenance.
				if (found == contexts.end())
					return;
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

PvrTaBlockProvenance observePvrTaAcceptedBlock(PvrTaInputSource source,
		std::uint32_t sourceAddress, std::uint32_t taAddress,
		const std::uint8_t* block, std::uint32_t contextAddress,
		std::uint32_t renderPass, std::uint32_t listTypeBefore,
		std::uint32_t listTypeAfter, std::uint32_t parserStateBefore,
		std::uint32_t parserStateAfter, std::uint64_t tick) noexcept
{
	if (!provenanceActive())
		return {};
	if (block == nullptr)
	{
		noteDroppedObservation();
		return {};
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
			// The TA may already be inside a context when delayed observation
			// begins. Its bytes remain valid renderer input, but without the
			// initiating ListInit they cannot be claimed as causal evidence.
			// Return unavailable provenance to downstream semantic observation
			// and suppress the structurally invalid partial artifact event.
			if (found == contexts.end())
			{
				PvrTaBlockProvenance provenance;
				provenance.initiator = observation.initiator;
				provenance.contextAddress = observation.contextAddress;
				provenance.renderPass = observation.renderPass;
				provenance.source = observation.source;
				provenance.sourceAddress = observation.sourceAddress;
				provenance.taAddress = observation.taAddress;
				return provenance;
			}
			observation.contextGeneration = found->second.generation;
			observation.contextBlockOrdinal = found->second.nextBlockOrdinal++;
		}
		PvrTaBlockProvenance provenance;
		provenance.initiator = observation.initiator;
		provenance.contextAddress = observation.contextAddress;
		provenance.contextGeneration = observation.contextGeneration;
		provenance.contextBlockOrdinal = observation.contextBlockOrdinal;
		provenance.renderPass = observation.renderPass;
		provenance.source = observation.source;
		provenance.sourceAddress = observation.sourceAddress;
		provenance.taAddress = observation.taAddress;
		provenance.available = observation.contextGeneration != 0;
		publish(std::move(observation));
		return provenance;
	}
	catch (...)
	{
		noteDroppedObservation();
		return {};
	}
}

std::uint64_t observePvrTaStartRender(const std::uint32_t* contextAddresses,
		const bool* contextAvailability, std::size_t contextCount,
		const PvrTaRenderSelectionTranscript* transcript,
		std::uint64_t tick) noexcept
{
	const std::uint64_t renderGeneration = nextRenderGeneration.fetch_add(1,
			std::memory_order_relaxed);
	if (!provenanceActive())
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
				const auto found = contexts.find(ref.address);
				// A TA context restored from a save state is available to the
				// renderer, but its pre-capture input bytes have no observation
				// generation. Preserve the selected address while marking that
				// context unavailable as causal evidence. A later context built
				// from observed TA blocks receives the normal generation.
				ref.available = contextAvailability[index]
						&& found != contexts.end();
				if (ref.available)
				{
					ref.generation = found->second.generation;
					contexts.erase(found);
				}
				observation.selectedContexts.push_back(ref);
			}
			observation.renderContextAvailable =
					!observation.selectedContexts.empty()
					&& observation.selectedContexts.front().available;
			pendingRenderGeneration = observation.renderGeneration;
			if (!observedRenderGenerationWindowFrozen)
				observedRenderGenerations.insert(observation.renderGeneration);
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
		if (observation.renderGeneration == 0)
			return;
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
			if (!observedRenderGenerationWindowFrozen)
				observedRenderGenerations.clear();
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

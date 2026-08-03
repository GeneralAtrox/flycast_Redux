#include "research/sh4_profile.h"

#include <limits>
#include <stdexcept>
#include <tuple>

namespace research
{

namespace
{
bool branchOpcode(std::uint16_t opcode,bool& conditional,bool& delayed)
{
	const auto high=opcode&0xff00u;
	conditional=high==0x8900u||high==0x8b00u||high==0x8d00u||high==0x8f00u;
	delayed=high==0x8d00u||high==0x8f00u;
	return conditional||(opcode&0xf000u)==0xa000u||(opcode&0xf000u)==0xb000u
			||(opcode&0xf0ffu)==0x0023u||(opcode&0xf0ffu)==0x0003u
			||(opcode&0xf0ffu)==0x402bu||(opcode&0xf0ffu)==0x400bu
			||opcode==0x000bu||opcode==0x002bu;
}
}

Sh4ProfileAccumulator::Sh4ProfileAccumulator(Sh4ObservationBackend backend,
		std::size_t maximumBlocks,std::size_t maximumBranches,
		std::uint64_t maximumObservations)
	:backend(backend),maximumBlocks(maximumBlocks),maximumBranches(maximumBranches),maximumObservations(maximumObservations)
{
	if((backend!=Sh4ObservationBackend::Interpreter&&backend!=Sh4ObservationBackend::Dynarec)
			||maximumBlocks==0||maximumBranches==0||maximumObservations==0)
		throw std::invalid_argument("SH-4 profile bounds are invalid");
}

void Sh4ProfileAccumulator::observe(const Sh4Observation& event)
{
	if(event.schemaVersion!=Sh4ObservationSchemaVersion||event.backend!=backend)
		throw std::invalid_argument("SH-4 profile observation binding mismatch");
	if(observations==maximumObservations)throw std::overflow_error("SH-4 profile observation limit exceeded");++observations;
	if(event.type==Sh4ObservationType::InstructionBegin){frames.push_back({event.instructionPc,event.opcode,event.tick});return;}
	if(event.type==Sh4ObservationType::InstructionAbort){if(!frames.empty())frames.pop_back();++incompleteExecutions;return;}
	if(event.type!=Sh4ObservationType::InstructionEnd)return;
	InstructionFrame frame;bool paired=false;
	if(!frames.empty()&&frames.back().pc==event.instructionPc&&frames.back().opcode==event.opcode){frame=frames.back();frames.pop_back();paired=true;}else{frame={event.instructionPc,event.opcode,event.tick};++incompleteExecutions;}
	const BlockKey blockKey{event.instructionPc,event.opcode};auto block=blocks.find(blockKey);
	if(block==blocks.end()){
		if(blocks.size()==maximumBlocks)throw std::overflow_error("SH-4 profile block limit exceeded");Sh4ExecutedBlockProfile value;value.guestAddress=event.instructionPc;value.guestBytes={static_cast<std::uint8_t>(event.opcode),static_cast<std::uint8_t>(event.opcode>>8)};value.firstTick=event.tick;block=blocks.emplace(blockKey,std::move(value)).first;
	}
	auto& value=block->second;if(value.executionCount==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("SH-4 profile execution count overflow");++value.executionCount;value.lastTick=event.tick;
	if(paired&&event.tick>=frame.tick){const auto cycles=event.tick-frame.tick;if(value.totalCycles>std::numeric_limits<std::uint64_t>::max()-cycles)throw std::overflow_error("SH-4 profile cycle count overflow");value.totalCycles+=cycles;}else value.cycleCountComplete=false;
	bool conditional=false,delayed=false;if(!branchOpcode(event.opcode,conditional,delayed))return;
	if((event.availableFields&Sh4Observation::HasNextPc)==0){++incompleteExecutions;return;}
	const std::uint32_t fallthrough=event.instructionPc+(delayed?4u:2u);const bool taken=!conditional||event.nextPc!=fallthrough;
	const BranchKey branchKey{event.instructionPc,event.nextPc,event.opcode,taken};auto branch=branches.find(branchKey);
	if(branch==branches.end()){
		if(branches.size()==maximumBranches)throw std::overflow_error("SH-4 profile branch limit exceeded");Sh4DynamicBranchProfile next;next.source=event.instructionPc;next.destination=event.nextPc;next.opcode=event.opcode;next.taken=taken;next.firstBoundaryTick=event.tick;branch=branches.emplace(branchKey,next).first;
	}
	if(branch->second.count==std::numeric_limits<std::uint64_t>::max())throw std::overflow_error("SH-4 profile branch count overflow");++branch->second.count;branch->second.lastBoundaryTick=event.tick;
}

Sh4ExecutionProfile Sh4ProfileAccumulator::snapshot() const
{
	Sh4ExecutionProfile result;result.backend=backend;result.observations=observations;result.incompleteExecutions=incompleteExecutions+frames.size();
	for(const auto& [key,value]:blocks)result.blocks.push_back(value);
	for(const auto& [key,value]:branches)result.branches.push_back(value);
	return result;
}

namespace
{
std::mutex activeCollectorMutex;
std::shared_ptr<Sh4DynarecProfileCollector> activeCollector;
}

Sh4DynarecProfileCollector::Sh4DynarecProfileCollector(
		std::size_t maximumBlocks, std::size_t maximumBranches,
		std::uint64_t maximumExecutions)
	: maximumBlocks(maximumBlocks), maximumBranches(maximumBranches),
	  maximumExecutions(maximumExecutions)
{
	if (maximumBlocks == 0 || maximumBranches == 0 || maximumExecutions == 0)
		throw std::invalid_argument("SH-4 dynarec profile bounds are invalid");
}

void Sh4DynarecProfileCollector::fail(const char *message) noexcept
{
	if (failure.empty())
		failure = message;
}

void Sh4DynarecProfileCollector::abortActive() noexcept
{
	if (activeGeneration == 0)
		return;
	auto found = blocks.find(activeGeneration);
	if (found == blocks.end()
			|| found->second.abortedCount == std::numeric_limits<std::uint64_t>::max()
			|| abortedExecutions == std::numeric_limits<std::uint64_t>::max())
		fail("SH-4 dynarec profile abort count overflow");
	else
	{
		++found->second.abortedCount;
		++abortedExecutions;
	}
	activeGeneration = 0;
}

std::uint64_t Sh4DynarecProfileCollector::registerBlock(
		Sh4DynarecBlockDefinition definition) noexcept
{
	try
	{
		const std::lock_guard<std::mutex> lock(mutex);
		if (!failure.empty())
			return 0;
		if (blocks.size() == maximumBlocks || nextGeneration == 0)
		{
			fail("SH-4 dynarec profile block-generation limit exceeded");
			return 0;
		}
		const std::uint64_t generation = nextGeneration++;
		Sh4DynarecBlockExecution execution;
		execution.generation = generation;
		execution.definition = std::move(definition);
		blocks.emplace(generation, std::move(execution));
		return generation;
	}
	catch (...)
	{
		const std::lock_guard<std::mutex> lock(mutex);
		fail("SH-4 dynarec profile block registration failed");
		return 0;
	}
}

void Sh4DynarecProfileCollector::enter(std::uint64_t generation,
		std::uint64_t tick) noexcept
{
	const std::lock_guard<std::mutex> lock(mutex);
	if (!failure.empty())
		return;
	abortActive();
	auto found = blocks.find(generation);
	if (found == blocks.end())
	{
		fail("SH-4 dynarec profile entered an undeclared generation");
		return;
	}
	if (enteredExecutions == maximumExecutions
			|| enteredExecutions == std::numeric_limits<std::uint64_t>::max()
			|| found->second.enteredCount == std::numeric_limits<std::uint64_t>::max())
	{
		fail("SH-4 dynarec profile execution limit exceeded");
		return;
	}
	if (found->second.enteredCount == 0)
		found->second.firstEntryTick = tick;
	++found->second.enteredCount;
	++enteredExecutions;
	activeGeneration = generation;
}

void Sh4DynarecProfileCollector::exit(std::uint64_t generation,
		std::uint32_t destination, std::uint64_t tick) noexcept
{
	const std::lock_guard<std::mutex> lock(mutex);
	if (!failure.empty())
		return;
	if (activeGeneration != generation)
	{
		fail("SH-4 dynarec profile exit does not match active generation");
		activeGeneration = 0;
		return;
	}
	auto found = blocks.find(generation);
	if (found == blocks.end())
	{
		fail("SH-4 dynarec profile exited an undeclared generation");
		activeGeneration = 0;
		return;
	}
	Sh4DynarecBlockExecution& block = found->second;
	if (block.completedCount == std::numeric_limits<std::uint64_t>::max()
			|| completedExecutions == std::numeric_limits<std::uint64_t>::max()
			|| block.totalCycles > std::numeric_limits<std::uint64_t>::max()
					- block.definition.guestCycles)
	{
		fail("SH-4 dynarec profile completion count overflow");
		activeGeneration = 0;
		return;
	}
	++block.completedCount;
	++completedExecutions;
	block.totalCycles += block.definition.guestCycles;
	block.lastExitTick = tick;
	activeGeneration = 0;

	if (block.definition.branchKind == Sh4DynarecBranchKind::None)
		return;
	const bool taken = block.definition.branchKind != Sh4DynarecBranchKind::Conditional
			|| destination == block.definition.branchTarget;
	const BranchKey key {generation, destination, taken};
	auto branch = branches.find(key);
	if (branch == branches.end())
	{
		if (branches.size() == maximumBranches)
		{
			fail("SH-4 dynarec profile branch limit exceeded");
			return;
		}
		Sh4DynarecBranchExecution value;
		value.sourceGeneration = generation;
		value.source = block.definition.branchSource;
		value.destination = destination;
		value.opcode = block.definition.branchOpcode;
		value.kind = block.definition.branchKind;
		value.taken = taken;
		value.firstBoundaryTick = tick;
		branch = branches.emplace(key, value).first;
	}
	if (branch->second.count == std::numeric_limits<std::uint64_t>::max())
	{
		fail("SH-4 dynarec profile branch count overflow");
		return;
	}
	++branch->second.count;
	branch->second.lastBoundaryTick = tick;
}

Sh4DynarecProfileSnapshot Sh4DynarecProfileCollector::snapshot() noexcept
{
	const std::lock_guard<std::mutex> lock(mutex);
	abortActive();
	Sh4DynarecProfileSnapshot result;
	result.enteredExecutions = enteredExecutions;
	result.completedExecutions = completedExecutions;
	result.abortedExecutions = abortedExecutions;
	result.complete = failure.empty();
	result.failure = failure;
	for (const auto& [generation, block] : blocks)
		if (block.enteredCount != 0)
			result.blocks.push_back(block);
	for (const auto& [key, branch] : branches)
		result.branches.push_back(branch);
	return result;
}

void activateSh4DynarecProfileCollector(
		std::shared_ptr<Sh4DynarecProfileCollector> collector) noexcept
{
	const std::lock_guard<std::mutex> lock(activeCollectorMutex);
	activeCollector = std::move(collector);
}

void deactivateSh4DynarecProfileCollector() noexcept
{
	const std::lock_guard<std::mutex> lock(activeCollectorMutex);
	activeCollector.reset();
}

bool sh4DynarecProfileCollectorActive() noexcept
{
	const std::lock_guard<std::mutex> lock(activeCollectorMutex);
	return activeCollector != nullptr;
}

std::uint64_t registerSh4DynarecProfileBlock(
		Sh4DynarecBlockDefinition definition) noexcept
{
	std::shared_ptr<Sh4DynarecProfileCollector> collector;
	{
		const std::lock_guard<std::mutex> lock(activeCollectorMutex);
		collector = activeCollector;
	}
	return collector == nullptr ? 0 : collector->registerBlock(std::move(definition));
}

void sh4DynarecProfileBlockEnterAt(std::uint64_t generation,
		std::uint64_t tick) noexcept
{
	std::shared_ptr<Sh4DynarecProfileCollector> collector;
	{
		const std::lock_guard<std::mutex> lock(activeCollectorMutex);
		collector = activeCollector;
	}
	if (collector != nullptr)
		collector->enter(generation, tick);
}

void sh4DynarecProfileBlockExitAt(std::uint64_t generation,
		std::uint32_t destination, std::uint64_t tick) noexcept
{
	std::shared_ptr<Sh4DynarecProfileCollector> collector;
	{
		const std::lock_guard<std::mutex> lock(activeCollectorMutex);
		collector = activeCollector;
	}
	if (collector != nullptr)
		collector->exit(generation, destination, tick);
}

} // namespace research

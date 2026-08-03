#pragma once

#include "research/sh4_observation.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace research
{

enum class Sh4ProfileByteStatus : std::uint8_t
{
	Complete = 1,
	Incomplete = 2,
};

struct Sh4ExecutedBlockProfile
{
	std::uint32_t guestAddress = 0;
	std::vector<std::uint8_t> guestBytes;
	Sh4ProfileByteStatus byteStatus = Sh4ProfileByteStatus::Complete;
	std::uint64_t executionCount = 0;
	std::uint64_t totalCycles = 0;
	std::uint64_t firstTick = 0;
	std::uint64_t lastTick = 0;
	bool cycleCountComplete = true;
};

struct Sh4DynamicBranchProfile
{
	std::uint32_t source = 0;
	std::uint32_t destination = 0;
	std::uint16_t opcode = 0;
	bool taken = false;
	std::uint64_t count = 0;
	std::uint64_t firstBoundaryTick = 0;
	std::uint64_t lastBoundaryTick = 0;
};

struct Sh4ExecutionProfile
{
	Sh4ObservationBackend backend = Sh4ObservationBackend::Interpreter;
	std::vector<Sh4ExecutedBlockProfile> blocks;
	std::vector<Sh4DynamicBranchProfile> branches;
	std::uint64_t observations = 0;
	std::uint64_t incompleteExecutions = 0;
};

// A bounded accumulator over the canonical SH-4 instruction bus. Research
// dynarec observation mode deliberately emits one top-level guest instruction
// per compiled block (plus its inseparable delay slot), so these records are
// also the exact executed dynarec block inventory in that mode.
class Sh4ProfileAccumulator
{
public:
	Sh4ProfileAccumulator(Sh4ObservationBackend backend,
			std::size_t maximumBlocks, std::size_t maximumBranches,
			std::uint64_t maximumObservations);
	void observe(const Sh4Observation& observation);
	Sh4ExecutionProfile snapshot() const;

private:
	struct InstructionFrame { std::uint32_t pc=0;std::uint16_t opcode=0;std::uint64_t tick=0; };
	struct BlockKey { std::uint32_t pc=0;std::uint16_t opcode=0;bool operator<(const BlockKey& other)const{return std::tie(pc,opcode)<std::tie(other.pc,other.opcode);} };
	struct BranchKey { std::uint32_t source=0,destination=0;std::uint16_t opcode=0;bool taken=false;bool operator<(const BranchKey& other)const{return std::tie(source,destination,opcode,taken)<std::tie(other.source,other.destination,other.opcode,other.taken);} };
	Sh4ObservationBackend backend;
	std::size_t maximumBlocks;
	std::size_t maximumBranches;
	std::uint64_t maximumObservations;
	std::uint64_t observations = 0;
	std::uint64_t incompleteExecutions = 0;
	std::vector<InstructionFrame> frames;
	std::map<BlockKey, Sh4ExecutedBlockProfile> blocks;
	std::map<BranchKey, Sh4DynamicBranchProfile> branches;
};

enum class Sh4DynarecProfileByteStatus : std::uint8_t
{
	Complete = 1,
	Incomplete = 2,
};

enum class Sh4DynarecBranchKind : std::uint8_t
{
	None = 0,
	Conditional = 1,
	Call = 2,
	Jump = 3,
	Return = 4,
};

struct Sh4DynarecBlockDefinition
{
	std::uint32_t virtualAddress = 0;
	std::uint32_t physicalAddress = 0;
	std::uint32_t fpuConfiguration = 0;
	std::uint32_t guestCodeSize = 0;
	std::uint32_t guestCycles = 0;
	std::uint32_t guestOpcodes = 0;
	std::vector<std::uint8_t> guestBytes;
	Sh4DynarecProfileByteStatus byteStatus =
			Sh4DynarecProfileByteStatus::Incomplete;
	Sh4DynarecBranchKind branchKind = Sh4DynarecBranchKind::None;
	std::uint32_t branchSource = 0;
	std::uint16_t branchOpcode = 0;
	std::uint32_t branchTarget = UINT32_MAX;
	std::uint32_t fallthroughTarget = UINT32_MAX;
};

struct Sh4DynarecBlockExecution
{
	std::uint64_t generation = 0;
	Sh4DynarecBlockDefinition definition;
	std::uint64_t enteredCount = 0;
	std::uint64_t completedCount = 0;
	std::uint64_t abortedCount = 0;
	std::uint64_t totalCycles = 0;
	std::uint64_t firstEntryTick = 0;
	std::uint64_t lastExitTick = 0;
};

struct Sh4DynarecBranchExecution
{
	std::uint64_t sourceGeneration = 0;
	std::uint32_t source = 0;
	std::uint32_t destination = 0;
	std::uint16_t opcode = 0;
	Sh4DynarecBranchKind kind = Sh4DynarecBranchKind::None;
	bool taken = false;
	std::uint64_t count = 0;
	std::uint64_t firstBoundaryTick = 0;
	std::uint64_t lastBoundaryTick = 0;
};

struct Sh4DynarecProfileSnapshot
{
	std::vector<Sh4DynarecBlockExecution> blocks;
	std::vector<Sh4DynarecBranchExecution> branches;
	std::uint64_t enteredExecutions = 0;
	std::uint64_t completedExecutions = 0;
	std::uint64_t abortedExecutions = 0;
	bool complete = true;
	std::string failure;
};

// Bounded production-dynarec accumulator. Unlike Sh4ProfileAccumulator this
// consumes actual compiled block entry/exit boundaries and therefore preserves
// normal block geometry.
class Sh4DynarecProfileCollector
{
public:
	Sh4DynarecProfileCollector(std::size_t maximumBlocks,
			std::size_t maximumBranches, std::uint64_t maximumExecutions);
	std::uint64_t registerBlock(Sh4DynarecBlockDefinition definition) noexcept;
	void enter(std::uint64_t generation, std::uint64_t tick) noexcept;
	void exit(std::uint64_t generation, std::uint32_t destination,
			std::uint64_t tick) noexcept;
	Sh4DynarecProfileSnapshot snapshot() noexcept;

private:
	struct BranchKey
	{
		std::uint64_t generation = 0;
		std::uint32_t destination = 0;
		bool taken = false;
		bool operator<(const BranchKey& other) const
		{
			return std::tie(generation, destination, taken)
					< std::tie(other.generation, other.destination, other.taken);
		}
	};
	void fail(const char *message) noexcept;
	void abortActive() noexcept;

	const std::size_t maximumBlocks;
	const std::size_t maximumBranches;
	const std::uint64_t maximumExecutions;
	std::mutex mutex;
	std::uint64_t nextGeneration = 1;
	std::uint64_t enteredExecutions = 0;
	std::uint64_t completedExecutions = 0;
	std::uint64_t abortedExecutions = 0;
	std::uint64_t activeGeneration = 0;
	std::map<std::uint64_t, Sh4DynarecBlockExecution> blocks;
	std::map<BranchKey, Sh4DynarecBranchExecution> branches;
	std::string failure;
};

void activateSh4DynarecProfileCollector(
		std::shared_ptr<Sh4DynarecProfileCollector> collector) noexcept;
void deactivateSh4DynarecProfileCollector() noexcept;
bool sh4DynarecProfileCollectorActive() noexcept;
std::uint64_t registerSh4DynarecProfileBlock(
		Sh4DynarecBlockDefinition definition) noexcept;
void sh4DynarecProfileBlockEnter(std::uint64_t generation) noexcept;
void sh4DynarecProfileBlockExit(std::uint64_t generation,
		std::uint32_t destination) noexcept;
void sh4DynarecProfileBlockEnterAt(std::uint64_t generation,
		std::uint64_t tick) noexcept;
void sh4DynarecProfileBlockExitAt(std::uint64_t generation,
		std::uint32_t destination, std::uint64_t tick) noexcept;

} // namespace research

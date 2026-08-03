#include "research/sh4_profile_artifact.h"

#include "research/identity_manifest.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <tuple>
#include <stdexcept>
#include <string>
#include <vector>

namespace research
{
namespace
{
constexpr std::array<std::uint8_t, 8> Magic {'F','C','S','H','4','P','R','1'};
constexpr std::uint32_t BlockRecordType = 1;
constexpr std::uint32_t BranchRecordType = 2;

[[noreturn]] void invalid(const std::string& message)
{
	throw std::runtime_error("invalid SH-4 dynarec profile artifact: " + message);
}

void require(bool condition, const std::string& message)
{
	if (!condition)
		invalid(message);
}

class Reader
{
public:
	Reader(const std::uint8_t* data, std::size_t size) : data(data), size(size) {}
	std::uint8_t byte() { need(1); return data[position++]; }
	std::uint16_t u16()
	{
		need(2);
		std::uint16_t value = 0;
		for (unsigned index = 0; index < 2; ++index)
			value |= std::uint16_t(data[position++]) << (index * 8);
		return value;
	}
	std::uint32_t u32()
	{
		need(4);
		std::uint32_t value = 0;
		for (unsigned index = 0; index < 4; ++index)
			value |= std::uint32_t(data[position++]) << (index * 8);
		return value;
	}
	std::uint64_t u64()
	{
		need(8);
		std::uint64_t value = 0;
		for (unsigned index = 0; index < 8; ++index)
			value |= std::uint64_t(data[position++]) << (index * 8);
		return value;
	}
	Sha256Digest digest()
	{
		Sha256Digest value {};
		for (auto& byte : value)
			byte = this->byte();
		return value;
	}
	void skip(std::size_t count) { need(count); position += count; }
	const std::uint8_t *current() const { return data + position; }
	std::size_t tell() const { return position; }
	std::size_t remaining() const { return size - position; }
private:
	void need(std::size_t count)
	{
		if (count > size - position)
			invalid("binary input is truncated");
	}
	const std::uint8_t* data;
	std::size_t size;
	std::size_t position = 0;
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
	std::uint32_t crc = 0xffffffffu;
	for (std::size_t index = 0; index < size; ++index)
	{
		crc ^= data[index];
		for (unsigned bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

struct ValidatedBlock
{
	std::uint32_t source = 0;
	std::uint32_t branchTarget = UINT32_MAX;
	std::uint32_t fallthroughTarget = UINT32_MAX;
	std::uint16_t opcode = 0;
	Sh4DynarecBranchKind kind = Sh4DynarecBranchKind::None;
	std::uint64_t completed = 0;
	std::uint64_t observedBranches = 0;
};
}

Sh4DynarecProfileArtifactSummary validateSh4DynarecProfileArtifact(
		const std::filesystem::path& path,
		const Sh4DynarecProfileArtifactBinding& expectedBinding,
		std::uint64_t maximumBytes, std::uint64_t maximumBlocks,
		std::uint64_t maximumBranches)
{
	const std::vector<std::uint8_t> file = readFileExact(path, maximumBytes);
	require(file.size() >= Sh4DynarecProfileArtifactHeaderSize,
			"file is smaller than its header");
	require(std::equal(Magic.begin(), Magic.end(), file.begin()), "magic is invalid");
	Reader header(file.data() + Magic.size(),
			Sh4DynarecProfileArtifactHeaderSize - Magic.size());
	require(header.u32() == Sh4DynarecProfileArtifactSchemaVersion,
			"schema version is unsupported");
	require(header.u32() == Sh4DynarecProfileArtifactHeaderSize,
			"header size is invalid");
	require(header.u32() == 0x01020304, "endian sentinel is invalid");
	require(header.u32() == 1, "artifact was not finalized");
	Sh4DynarecProfileArtifactSummary summary;
	summary.blockCount = header.u64();
	summary.branchCount = header.u64();
	summary.enteredExecutions = header.u64();
	summary.completedExecutions = header.u64();
	summary.abortedExecutions = header.u64();
	summary.incompleteByteBlocks = header.u64();
	summary.payloadBytes = header.u64();
	summary.binding.identityDigest = header.digest();
	summary.binding.replayDigest = header.digest();
	summary.binding.configurationDigest = header.digest();
	summary.payloadDigest = header.digest();
	require(summary.blockCount > 0 && summary.blockCount <= maximumBlocks,
			"block count is outside its bound");
	require(summary.branchCount <= maximumBranches,
			"branch count is outside its bound");
	require(summary.payloadBytes == file.size() - Sh4DynarecProfileArtifactHeaderSize,
			"payload size does not match the file");
	require(sha256Equal(summary.binding.identityDigest, expectedBinding.identityDigest)
			&& sha256Equal(summary.binding.replayDigest, expectedBinding.replayDigest)
			&& sha256Equal(summary.binding.configurationDigest,
					expectedBinding.configurationDigest),
			"artifact binding does not match the expected inputs");
	require(sha256Equal(summary.payloadDigest,
			sha256(file.data() + Sh4DynarecProfileArtifactHeaderSize,
					summary.payloadBytes)), "payload digest is invalid");
	std::uint32_t storedCrc = 0;
	for (unsigned index = 0; index < 4; ++index)
		storedCrc |= std::uint32_t(file[Sh4DynarecProfileArtifactHeaderSize - 4 + index])
				<< (index * 8);
	require(storedCrc == crc32(file.data(), Sh4DynarecProfileArtifactHeaderSize - 4),
			"header CRC is invalid");
	require(summary.enteredExecutions
			== summary.completedExecutions + summary.abortedExecutions,
			"execution totals are inconsistent");

	Reader payload(file.data() + Sh4DynarecProfileArtifactHeaderSize,
			summary.payloadBytes);
	std::map<std::uint64_t, ValidatedBlock> blocks;
	std::uint64_t totalEntered = 0;
	std::uint64_t totalCompleted = 0;
	std::uint64_t totalAborted = 0;
	std::uint64_t incompleteBytes = 0;
	for (std::uint64_t index = 0; index < summary.blockCount; ++index)
	{
		const std::size_t start = payload.tell();
		require(payload.u32() == BlockRecordType, "block record type is invalid");
		const std::uint32_t recordSize = payload.u32();
		require(recordSize >= 108 && recordSize <= payload.remaining() + 8,
				"block record size is invalid");
		const std::uint64_t generation = payload.u64();
		require(generation != 0 && (blocks.empty() || generation > blocks.rbegin()->first),
				"block generations are not uniquely increasing");
		const std::uint32_t virtualAddress = payload.u32();
		const std::uint32_t physicalAddress = payload.u32();
		const std::uint32_t fpuConfiguration = payload.u32();
		const std::uint32_t guestCodeSize = payload.u32();
		const std::uint32_t guestCycles = payload.u32();
		const std::uint32_t guestOpcodes = payload.u32();
		const auto byteStatus = static_cast<Sh4DynarecProfileByteStatus>(payload.byte());
		const auto branchKind = static_cast<Sh4DynarecBranchKind>(payload.byte());
		const std::uint16_t opcode = payload.u16();
		const std::uint32_t source = payload.u32();
		const std::uint32_t branchTarget = payload.u32();
		const std::uint32_t fallthroughTarget = payload.u32();
		const std::uint32_t byteCount = payload.u32();
		const std::uint64_t entered = payload.u64();
		const std::uint64_t completed = payload.u64();
		const std::uint64_t aborted = payload.u64();
		const std::uint64_t totalCycles = payload.u64();
		const std::uint64_t firstTick = payload.u64();
		const std::uint64_t lastTick = payload.u64();
		require((virtualAddress & 1u) == 0 && guestCodeSize != 0
				&& (guestCodeSize & 1u) == 0 && guestCodeSize <= 1022,
				"block guest range is invalid");
		require(byteCount == guestCodeSize && recordSize == 108u + byteCount,
				"block byte count differs from its guest code size");
		require(guestCycles != 0 && guestOpcodes != 0,
				"block cycle/opcode count is empty");
		require(byteStatus == Sh4DynarecProfileByteStatus::Complete
				|| byteStatus == Sh4DynarecProfileByteStatus::Incomplete,
				"block byte status is invalid");
		if (byteStatus == Sh4DynarecProfileByteStatus::Incomplete)
			++incompleteBytes;
		require(branchKind >= Sh4DynarecBranchKind::None
				&& branchKind <= Sh4DynarecBranchKind::Return,
				"block branch kind is invalid");
		if (branchKind == Sh4DynarecBranchKind::None)
			require(opcode == 0, "non-branch block has a branch opcode");
		else
			require((source & 1u) == 0 && source >= virtualAddress
					&& std::uint64_t(source) < std::uint64_t(virtualAddress) + guestCodeSize,
					"branch source is outside its block");
		require(entered != 0 && entered == completed + aborted,
				"block execution counts are inconsistent");
		require(completed == 0 || guestCycles <= std::numeric_limits<std::uint64_t>::max()
				/ completed, "block cycle total overflowed");
		require(totalCycles == completed * guestCycles,
				"block cycle total is inconsistent");
		require(firstTick <= lastTick || completed == 0,
				"block tick interval is inconsistent");
		if (branchKind != Sh4DynarecBranchKind::None)
		{
			const std::size_t opcodeOffset = source - virtualAddress;
			require(opcodeOffset + 2 <= byteCount,
					"branch opcode offset is outside its exact guest bytes");
			const std::uint8_t *bytes = payload.current();
			const std::uint16_t exactOpcode = static_cast<std::uint16_t>(
					bytes[opcodeOffset] | (std::uint16_t(bytes[opcodeOffset + 1]) << 8));
			require(exactOpcode == opcode,
					"branch opcode differs from the exact guest bytes");
		}
		Sh4DynarecBlockExecution decoded;
		decoded.generation = generation;
		decoded.definition.virtualAddress = virtualAddress;
		decoded.definition.physicalAddress = physicalAddress;
		decoded.definition.fpuConfiguration = fpuConfiguration;
		decoded.definition.guestCodeSize = guestCodeSize;
		decoded.definition.guestCycles = guestCycles;
		decoded.definition.guestOpcodes = guestOpcodes;
		decoded.definition.byteStatus = byteStatus;
		decoded.definition.branchKind = branchKind;
		decoded.definition.branchOpcode = opcode;
		decoded.definition.branchSource = source;
		decoded.definition.branchTarget = branchTarget;
		decoded.definition.fallthroughTarget = fallthroughTarget;
		decoded.definition.guestBytes.assign(payload.current(),
				payload.current() + byteCount);
		decoded.enteredCount = entered;
		decoded.completedCount = completed;
		decoded.abortedCount = aborted;
		decoded.totalCycles = totalCycles;
		decoded.firstEntryTick = firstTick;
		decoded.lastExitTick = lastTick;
		payload.skip(byteCount);
		require(payload.tell() == start + recordSize,
				"block record length does not match its payload");
		blocks.emplace(generation, ValidatedBlock {source, branchTarget,
				fallthroughTarget, opcode, branchKind, completed, 0});
		summary.blocks.push_back(std::move(decoded));
		require(totalEntered <= std::numeric_limits<std::uint64_t>::max() - entered
				&& totalCompleted <= std::numeric_limits<std::uint64_t>::max() - completed
				&& totalAborted <= std::numeric_limits<std::uint64_t>::max() - aborted,
				"aggregate execution totals overflowed");
		totalEntered += entered;
		totalCompleted += completed;
		totalAborted += aborted;
	}
	require(totalEntered == summary.enteredExecutions
			&& totalCompleted == summary.completedExecutions
			&& totalAborted == summary.abortedExecutions,
			"header execution totals differ from block records");
	require(incompleteBytes == summary.incompleteByteBlocks,
			"header incomplete-byte count differs from block records");

	std::tuple<std::uint64_t, std::uint32_t, bool> previousBranch {};
	bool havePreviousBranch = false;
	for (std::uint64_t index = 0; index < summary.branchCount; ++index)
	{
		const std::size_t start = payload.tell();
		require(payload.u32() == BranchRecordType && payload.u32() == 64,
				"branch record header is invalid");
		const std::uint64_t generation = payload.u64();
		const std::uint32_t source = payload.u32();
		const std::uint32_t destination = payload.u32();
		const std::uint16_t opcode = payload.u16();
		const auto kind = static_cast<Sh4DynarecBranchKind>(payload.byte());
		const std::uint8_t takenValue = payload.byte();
		payload.u32();
		const std::uint64_t count = payload.u64();
		const std::uint64_t firstTick = payload.u64();
		const std::uint64_t lastTick = payload.u64();
		payload.u64();
		auto block = blocks.find(generation);
		require(block != blocks.end(), "branch references an undeclared generation");
		require(kind != Sh4DynarecBranchKind::None && kind == block->second.kind
				&& source == block->second.source && opcode == block->second.opcode,
				"branch ownership differs from its block definition");
		require(takenValue <= 1 && count != 0 && firstTick <= lastTick,
				"branch count, taken state or ticks are invalid");
		if (kind == Sh4DynarecBranchKind::Conditional)
		{
			require(destination == block->second.branchTarget
					|| destination == block->second.fallthroughTarget,
					"conditional destination is neither branch nor fallthrough");
			require((takenValue != 0) == (destination == block->second.branchTarget),
					"conditional taken state differs from destination");
		}
		else
			require(takenValue == 1, "unconditional branch is marked not taken");
		const auto branchKey = std::make_tuple(generation, destination,
				takenValue != 0);
		require(!havePreviousBranch || previousBranch < branchKey,
				"branch records are not uniquely ordered");
		havePreviousBranch = true;
		previousBranch = branchKey;
		require(block->second.observedBranches
				<= std::numeric_limits<std::uint64_t>::max() - count,
				"branch execution count overflowed");
		block->second.observedBranches += count;
		Sh4DynarecBranchExecution decoded;
		decoded.sourceGeneration = generation;
		decoded.source = source;
		decoded.destination = destination;
		decoded.opcode = opcode;
		decoded.kind = kind;
		decoded.taken = takenValue != 0;
		decoded.count = count;
		decoded.firstBoundaryTick = firstTick;
		decoded.lastBoundaryTick = lastTick;
		summary.branches.push_back(std::move(decoded));
		require(payload.tell() == start + 64,
				"branch record length does not match its payload");
	}
	for (const auto& [generation, block] : blocks)
	{
		(void)generation;
		if (block.kind == Sh4DynarecBranchKind::None)
			require(block.observedBranches == 0,
					"non-branch block owns dynamic branch records");
		else
			require(block.observedBranches == block.completed,
					"dynamic branch counts do not cover every completed block execution");
	}
	require(payload.remaining() == 0, "payload contains trailing bytes");
	return summary;
}

} // namespace research

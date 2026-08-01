#include "research/pvr_ta_artifact.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace research
{
namespace
{

constexpr std::array<std::uint8_t, 8> ArtifactMagic {
	'F', 'C', 'P', 'V', 'R', 'T', 'A', '1',
};
constexpr std::uint32_t HeaderComplete = 1u << 0;
constexpr std::uint32_t KnownHeaderFlags = HeaderComplete;
constexpr std::uint32_t EventHeaderSize = 32;
constexpr std::uint32_t OwnerSize = 32;
constexpr std::size_t MaximumSelectedContexts = 10;
constexpr std::uint32_t ListBoundaryEventSize = EventHeaderSize + OwnerSize + 16;
constexpr std::uint32_t AcceptedBlockEventSize = EventHeaderSize + OwnerSize + 88;
constexpr std::uint32_t RenderDoneEventSize = EventHeaderSize + OwnerSize + 8;
constexpr std::uint32_t ResetEventSize = EventHeaderSize + OwnerSize;

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid PowerVR TA artifact: " + reason);
}

void require(bool condition, const std::string& reason)
{
	if (!condition)
		invalid(reason);
}

class ByteReader
{
public:
	ByteReader(const std::uint8_t *data, std::size_t size) : data(data), size(size) {}

	std::uint8_t u8()
	{
		need(1);
		return data[position++];
	}

	std::uint16_t u16()
	{
		need(2);
		const std::uint16_t result = static_cast<std::uint16_t>(data[position])
				| static_cast<std::uint16_t>(data[position + 1] << 8);
		position += 2;
		return result;
	}

	std::uint32_t u32()
	{
		need(4);
		std::uint32_t result = 0;
		for (unsigned index = 0; index < 4; ++index)
			result |= static_cast<std::uint32_t>(data[position + index]) << (index * 8);
		position += 4;
		return result;
	}

	std::uint64_t u64()
	{
		need(8);
		std::uint64_t result = 0;
		for (unsigned index = 0; index < 8; ++index)
			result |= static_cast<std::uint64_t>(data[position + index]) << (index * 8);
		position += 8;
		return result;
	}

	void bytes(void *destination, std::size_t count)
	{
		need(count);
		std::memcpy(destination, data + position, count);
		position += count;
	}

	std::size_t remaining() const { return size - position; }

private:
	void need(std::size_t count) const
	{
		if (position > size || count > size - position)
			invalid("truncated scalar or event payload");
	}

	const std::uint8_t *data;
	std::size_t size;
	std::size_t position = 0;
};

std::uint32_t crc32(const std::uint8_t *data, std::size_t size)
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

bool validBackend(Sh4ObservationBackend backend)
{
	return backend == Sh4ObservationBackend::Interpreter
			|| backend == Sh4ObservationBackend::Dynarec;
}

bool validType(PvrTaObservationType type)
{
	return type >= PvrTaObservationType::ListInit
			&& type <= PvrTaObservationType::Reset;
}

std::size_t typeIndex(PvrTaObservationType type)
{
	return static_cast<std::size_t>(type) - 1;
}

std::uint64_t stableFileSize(const std::filesystem::path& path)
{
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error || size > std::numeric_limits<std::uint64_t>::max())
		invalid("file size is unavailable");
	return static_cast<std::uint64_t>(size);
}

void readExact(std::ifstream& input, void *destination, std::size_t count,
		const std::string& reason)
{
	if (count == 0)
		return;
	input.read(static_cast<char *>(destination), static_cast<std::streamsize>(count));
	require(input.gcount() == static_cast<std::streamsize>(count) && input.good(),
			reason);
}

Sha256Digest readDigest(ByteReader& reader)
{
	Sha256Digest digest {};
	reader.bytes(digest.data(), digest.size());
	return digest;
}

void requireDigest(const Sha256Digest& actual, const Sha256Digest& expected,
		const char *field)
{
	require(sha256Equal(actual, expected), std::string(field) + " digest mismatch");
}

PvrTaArtifactSummary parseHeader(const std::array<std::uint8_t,
		PvrTaArtifactHeaderSize>& bytes, const PvrTaArtifactBinding& expected)
{
	ByteReader reader(bytes.data(), bytes.size());
	std::array<std::uint8_t, 8> magic {};
	reader.bytes(magic.data(), magic.size());
	require(magic == ArtifactMagic, "magic mismatch");
	require(reader.u32() == PvrTaArtifactSchemaVersion,
			"unsupported schema version");
	require(reader.u32() == PvrTaArtifactHeaderSize, "header size mismatch");
	require(reader.u32() == PvrTaArtifactEndianSentinel,
			"endian sentinel mismatch");
	const std::uint32_t flags = reader.u32();
	require((flags & ~KnownHeaderFlags) == 0, "unknown header flags");
	require((flags & HeaderComplete) != 0, "artifact is incomplete");

	PvrTaArtifactSummary summary;
	summary.binding.backend = static_cast<Sh4ObservationBackend>(reader.u32());
	require(validBackend(summary.binding.backend), "backend is invalid");
	require(reader.u32() == 0, "header reserved field is nonzero");
	summary.eventCount = reader.u64();
	for (std::uint64_t& count : summary.typeCounts)
		count = reader.u64();
	summary.payloadBytes = reader.u64();
	summary.droppedEvents = reader.u64();
	summary.startTick = reader.u64();
	summary.endTick = reader.u64();
	summary.firstEmissionOrdinal = reader.u64();
	summary.lastEmissionOrdinal = reader.u64();
	summary.binding.identityDigest = readDigest(reader);
	summary.binding.replayDigest = readDigest(reader);
	summary.binding.manifestDigest = readDigest(reader);
	summary.payloadDigest = readDigest(reader);
	require(reader.u32() == 0, "header terminal reserved field is nonzero");
	const std::uint32_t storedCrc = reader.u32();
	require(reader.remaining() == 0, "internal header decoder mismatch");
	require(storedCrc == crc32(bytes.data(), bytes.size() - 4),
			"header CRC mismatch");
	require(summary.binding.backend == expected.backend,
			"backend binding mismatch");
	requireDigest(summary.binding.identityDigest, expected.identityDigest, "identity");
	requireDigest(summary.binding.replayDigest, expected.replayDigest, "replay");
	requireDigest(summary.binding.manifestDigest, expected.manifestDigest, "manifest");
	require(summary.droppedEvents == 0, "artifact records dropped events");
	return summary;
}

struct Owner
{
	bool valid = false;
	Sh4ObservationBackend backend = Sh4ObservationBackend::Interpreter;
	std::uint16_t delaySlotDepth = 0;
	std::uint16_t opcode = 0;
	std::uint64_t generation = 0;
	std::uint64_t tick = 0;
	std::uint32_t pc = 0;
	std::uint32_t pr = 0;
};

Owner readOwner(ByteReader& reader, Sh4ObservationBackend expectedBackend,
		std::uint64_t eventTick)
{
	Owner owner;
	const std::uint8_t valid = reader.u8();
	const std::uint8_t backend = reader.u8();
	owner.delaySlotDepth = reader.u16();
	owner.opcode = reader.u16();
	require(reader.u16() == 0, "owner reserved field is nonzero");
	owner.generation = reader.u64();
	owner.tick = reader.u64();
	owner.pc = reader.u32();
	owner.pr = reader.u32();
	require(valid <= 1, "owner valid flag is noncanonical");
	owner.valid = valid != 0;
	if (!owner.valid)
	{
		require(backend == 0 && owner.delaySlotDepth == 0 && owner.opcode == 0
				&& owner.generation == 0 && owner.tick == 0 && owner.pc == 0
				&& owner.pr == 0, "unowned event has nonzero owner fields");
		return owner;
	}
	owner.backend = static_cast<Sh4ObservationBackend>(backend);
	require(owner.backend == expectedBackend, "owner backend differs from binding");
	require(owner.generation != 0, "owner generation is zero");
	require((owner.pc & 1u) == 0, "owner PC is not aligned");
	require(owner.tick <= eventTick, "owner tick follows its event");
	return owner;
}

void validateSource(PvrTaInputSource source, std::uint32_t sourceAddress,
		std::uint32_t taAddress)
{
	require(source >= PvrTaInputSource::StoreQueue
			&& source <= PvrTaInputSource::SortDma,
			"accepted block source is invalid");
	require((sourceAddress & 31u) == 0, "source address is not 32-byte aligned");
	if (source == PvrTaInputSource::StoreQueue)
		require((sourceAddress & 0xfc000000u) == 0xe0000000u,
				"store-queue source address is outside P4 store queues");
	else
		require((sourceAddress & 0xff000000u) == 0x0c000000u,
				"DMA source address is not canonical Dreamcast RAM");
	if (source == PvrTaInputSource::SortDma)
		require(taAddress == UINT32_MAX,
				"sort-DMA TA address does not use its sentinel");
	else
		require(taAddress != UINT32_MAX && (taAddress & 31u) == 0,
				"TA destination address is unavailable or unaligned");
}

struct TranscriptReader
{
	const std::vector<PvrTaVramRead>& reads;
	std::size_t position = 0;

	std::uint32_t read(std::uint32_t expectedAddress)
	{
		require(position < reads.size(), "render-selection transcript is truncated");
		const PvrTaVramRead& item = reads[position++];
		require(item.address == expectedAddress,
				"render-selection read address differs from the independent decoder");
		return item.value;
	}
};

std::vector<std::uint32_t> independentlySelectContexts(std::uint32_t regionBase,
		std::uint32_t fpuParamCfg, const std::vector<PvrTaVramRead>& reads)
{
	TranscriptReader transcript {reads};
	std::uint32_t address = regionBase;
	const bool type1 = ((fpuParamCfg >> 21) & 1u) == 0;
	std::uint32_t tileSize = (type1 ? 5u : 6u) * 4u;
	bool emptyFirst = true;
	for (int index = type1 ? 4 : 5; index > 0; --index)
	{
		if ((transcript.read(address + static_cast<std::uint32_t>(index) * 4u)
				& 0x80000000u) == 0)
		{
			emptyFirst = false;
			break;
		}
	}
	if (emptyFirst)
		address += tileSize;
	const std::uint32_t initialTile = transcript.read(address);
	if ((initialTile & (1u << 29)) != 0)
		tileSize = 24;

	const std::uint32_t coordinateTile = transcript.read(address);
	const std::uint32_t x = (coordinateTile >> 2) & 0x3fu;
	const std::uint32_t y = (coordinateTile >> 8) & 0x3fu;
	std::vector<std::uint32_t> selected;
	do
	{
		const std::uint32_t tile = transcript.read(address);
		if (((tile >> 2) & 0x3fu) != x || ((tile >> 8) & 0x3fu) != y)
			break;
		std::uint32_t opbAddress = transcript.read(address + 4);
		if ((opbAddress & 0x80000000u) != 0)
		{
			opbAddress = transcript.read(address + 12);
			if ((opbAddress & 0x80000000u) != 0)
			{
				if (tileSize >= 24)
					opbAddress = transcript.read(address + 20);
				if ((opbAddress & 0x80000000u) != 0)
					break;
			}
		}
		selected.push_back(transcript.read(opbAddress));
		address += tileSize;
		if ((tile & 0x80000000u) != 0 || selected.size() == MaximumSelectedContexts)
			break;
	} while (true);
	require(transcript.position == reads.size(),
			"render-selection transcript has unused reads");
	return selected;
}

struct ContextState
{
	std::uint64_t generation = 0;
	std::uint64_t nextBlockOrdinal = 0;
};

} // namespace

PvrTaArtifactSummary validatePvrTaArtifactFile(
		const std::filesystem::path& path,
		const PvrTaArtifactBinding& expectedBinding,
		std::uint64_t maximumBytes, std::uint64_t maximumEvents)
{
	require(validBackend(expectedBinding.backend), "expected backend is invalid");
	const std::uint64_t sizeBefore = stableFileSize(path);
	require(sizeBefore >= PvrTaArtifactHeaderSize && sizeBefore <= maximumBytes,
			"file size is outside the admission limit");
	std::ifstream input(path, std::ios::binary);
	require(input.good(), "cannot open artifact");
	std::array<std::uint8_t, PvrTaArtifactHeaderSize> headerBytes {};
	readExact(input, headerBytes.data(), headerBytes.size(), "truncated header");
	PvrTaArtifactSummary summary = parseHeader(headerBytes, expectedBinding);
	require(summary.eventCount != 0 && summary.eventCount <= maximumEvents,
			"event count is outside the admission limit");
	require(summary.payloadBytes == sizeBefore - PvrTaArtifactHeaderSize,
			"payload byte count differs from file size");

	Sha256 payloadHasher;
	std::array<std::uint64_t, 6> typeCounts {};
	std::unordered_map<std::uint32_t, ContextState> contexts;
	std::unordered_set<std::uint64_t> contextGenerations;
	std::unordered_set<std::uint64_t> renderGenerations;
	std::uint64_t pendingRenderGeneration = 0;
	bool pendingRenderHasAcceptedBlock = false;
	bool completedCausalRender = false;
	std::uint64_t previousTick = 0;
	std::uint64_t previousEmission = 0;
	std::uint64_t remaining = summary.payloadBytes;

	for (std::uint64_t ordinal = 0; ordinal < summary.eventCount; ++ordinal)
	{
		require(remaining >= EventHeaderSize, "payload ends before event header");
		std::array<std::uint8_t, EventHeaderSize> eventHeader {};
		readExact(input, eventHeader.data(), eventHeader.size(),
				"truncated event header");
		payloadHasher.update(eventHeader.data(), eventHeader.size());
		ByteReader eventHeaderReader(eventHeader.data(), eventHeader.size());
		const auto type = static_cast<PvrTaObservationType>(eventHeaderReader.u32());
		const std::uint32_t eventSize = eventHeaderReader.u32();
		const std::uint64_t artifactOrdinal = eventHeaderReader.u64();
		const std::uint64_t emissionOrdinal = eventHeaderReader.u64();
		const std::uint64_t tick = eventHeaderReader.u64();
		require(validType(type), "event type is invalid");
		require(artifactOrdinal == ordinal, "artifact ordinal is not contiguous");
		require(eventSize >= EventHeaderSize && eventSize <= remaining,
				"event size exceeds the remaining payload");
		if (ordinal == 0)
		{
			require(tick == summary.startTick, "first tick differs from header");
			require(emissionOrdinal == summary.firstEmissionOrdinal,
					"first emission ordinal differs from header");
		}
		else
		{
			require(tick >= previousTick, "scheduler tick moved backwards");
			require(emissionOrdinal == previousEmission + 1,
					"source emission ordinal is not contiguous");
		}
		previousTick = tick;
		previousEmission = emissionOrdinal;

		std::vector<std::uint8_t> payload(eventSize - EventHeaderSize);
		readExact(input, payload.data(), payload.size(), "truncated event payload");
		payloadHasher.update(payload.data(), payload.size());
		ByteReader reader(payload.data(), payload.size());
		const Owner owner = readOwner(reader, expectedBinding.backend, tick);
		const bool synchronous = type != PvrTaObservationType::RenderDone
				&& type != PvrTaObservationType::Reset;
		require(owner.valid == synchronous,
				"instruction ownership does not match the event boundary");

		switch (type)
		{
		case PvrTaObservationType::ListInit:
		case PvrTaObservationType::ListContinue:
		{
			require(eventSize == ListBoundaryEventSize,
					"list-boundary event size mismatch");
			const std::uint32_t address = reader.u32();
			const std::uint64_t generation = reader.u64();
			const std::uint32_t renderPass = reader.u32();
			require(address != UINT32_MAX && generation != 0,
					"list boundary has no context generation");
			if (type == PvrTaObservationType::ListInit)
			{
				require(renderPass == 0, "list-init render pass is not zero");
				require(contextGenerations.insert(generation).second,
						"context generation is reused");
				contexts[address] = {generation, 0};
			}
			else
			{
				const auto found = contexts.find(address);
				require(found != contexts.end()
						&& found->second.generation == generation,
						"list continuation has no active matching context");
				require(renderPass != 0,
						"list continuation render pass is zero");
			}
			break;
		}
		case PvrTaObservationType::AcceptedBlock:
		{
			require(eventSize == AcceptedBlockEventSize,
					"accepted-block event size mismatch");
			const std::uint32_t address = reader.u32();
			const std::uint64_t generation = reader.u64();
			const std::uint64_t blockOrdinal = reader.u64();
			reader.u32(); // render pass; exact value remains part of the typed bytes
			const std::uint32_t listBefore = reader.u32();
			const std::uint32_t listAfter = reader.u32();
			const std::uint32_t parserBefore = reader.u32();
			const std::uint32_t parserAfter = reader.u32();
			const auto source = static_cast<PvrTaInputSource>(reader.u32());
			const std::uint32_t sourceAddress = reader.u32();
			const std::uint32_t taAddress = reader.u32();
			std::array<std::uint8_t, 32> block {};
			reader.bytes(block.data(), block.size());
			require(reader.u32() == 0, "accepted-block reserved field is nonzero");
			const auto found = contexts.find(address);
			require(found != contexts.end() && found->second.generation == generation,
					"accepted block has no active matching context");
			require(blockOrdinal == found->second.nextBlockOrdinal++,
					"context block ordinal is not contiguous");
			require(listBefore <= 7 && listAfter <= 7,
					"accepted block list type is invalid");
			require(parserBefore <= 7 && parserAfter <= 7,
					"accepted block parser state is invalid");
			validateSource(source, sourceAddress, taAddress);
			break;
		}
		case PvrTaObservationType::StartRender:
		{
			const std::uint64_t renderGeneration = reader.u64();
			const std::uint32_t firstAvailable = reader.u32();
			const std::uint32_t regionBase = reader.u32();
			const std::uint32_t fpuParamCfg = reader.u32();
			const std::uint32_t selectedCount = reader.u32();
			const std::uint32_t readCount = reader.u32();
			require(reader.u32() == 0, "STARTRENDER reserved field is nonzero");
			require(renderGeneration != 0
					&& renderGenerations.insert(renderGeneration).second,
					"render generation is zero or reused");
			require(pendingRenderGeneration == 0,
					"STARTRENDER overlaps an unmatched render generation");
			require(firstAvailable <= 1, "first-context availability is noncanonical");
			require(regionBase != UINT32_MAX && fpuParamCfg != UINT32_MAX,
					"render-selection registers are unavailable");
			require(selectedCount <= MaximumSelectedContexts,
					"STARTRENDER selects too many contexts");
			require(readCount != 0 && readCount <= MaxPvrTaRenderSelectionReads,
					"render-selection read count is invalid");
			const std::uint64_t expectedSize = EventHeaderSize + OwnerSize + 32
					+ std::uint64_t {selectedCount} * 16
					+ std::uint64_t {readCount} * 8;
			require(eventSize == expectedSize, "STARTRENDER event size mismatch");
			std::vector<PvrTaContextRef> selected;
			for (std::uint32_t index = 0; index < selectedCount; ++index)
			{
				PvrTaContextRef context;
				context.address = reader.u32();
				context.generation = reader.u64();
				const std::uint32_t available = reader.u32();
				require(context.address != UINT32_MAX && available <= 1,
						"selected context fields are noncanonical");
				context.available = available != 0;
				require((context.available && context.generation != 0)
						|| (!context.available && context.generation == 0),
						"selected context availability/generation mismatch");
				selected.push_back(context);
			}
			require((firstAvailable != 0) == (!selected.empty()
					&& selected.front().available),
					"first-context availability disagrees with selected context");
			std::vector<PvrTaVramRead> reads(readCount);
			for (PvrTaVramRead& read : reads)
			{
				read.address = reader.u32();
				read.value = reader.u32();
			}
			const std::vector<std::uint32_t> independentlySelected =
					independentlySelectContexts(regionBase, fpuParamCfg, reads);
			require(independentlySelected.size() == selected.size(),
					"selected context count differs from independent decoding");
			bool hasAcceptedBlock = false;
			for (std::size_t index = 0; index < selected.size(); ++index)
			{
				require(independentlySelected[index] == selected[index].address,
						"selected context address differs from independent decoding");
				if (selected[index].available)
				{
					const auto found = contexts.find(selected[index].address);
					require(found != contexts.end()
							&& found->second.generation == selected[index].generation,
							"available render context has no active generation");
					hasAcceptedBlock = hasAcceptedBlock
							|| found->second.nextBlockOrdinal != 0;
					contexts.erase(found);
				}
			}
			pendingRenderGeneration = renderGeneration;
			pendingRenderHasAcceptedBlock = hasAcceptedBlock;
			break;
		}
		case PvrTaObservationType::RenderDone:
		{
			require(eventSize == RenderDoneEventSize,
					"render-done event size mismatch");
			const std::uint64_t generation = reader.u64();
			require(generation != 0 && generation == pendingRenderGeneration,
					"render-done does not match the pending render generation");
			completedCausalRender = completedCausalRender
					|| pendingRenderHasAcceptedBlock;
			pendingRenderGeneration = 0;
			pendingRenderHasAcceptedBlock = false;
			break;
		}
		case PvrTaObservationType::Reset:
			require(eventSize == ResetEventSize, "reset event size mismatch");
			contexts.clear();
			pendingRenderGeneration = 0;
			pendingRenderHasAcceptedBlock = false;
			break;
		}
		require(reader.remaining() == 0, "event has trailing fields");
		++typeCounts[typeIndex(type)];
		remaining -= eventSize;
	}

	require(remaining == 0, "payload contains trailing bytes");
	char extra = 0;
	input.read(&extra, 1);
	require(input.gcount() == 0, "file grew while validating");
	require(stableFileSize(path) == sizeBefore, "file size changed while validating");
	require(sha256Equal(payloadHasher.finalize(), summary.payloadDigest),
			"payload digest mismatch");
	require(typeCounts == summary.typeCounts, "event type counts differ from header");
	require(previousTick == summary.endTick, "last tick differs from header");
	require(previousEmission == summary.lastEmissionOrdinal,
			"last emission ordinal differs from header");
	require(typeCounts[typeIndex(PvrTaObservationType::ListInit)] != 0,
			"artifact has no list initialization");
	require(typeCounts[typeIndex(PvrTaObservationType::AcceptedBlock)] != 0,
			"artifact has no accepted TA block");
	require(typeCounts[typeIndex(PvrTaObservationType::StartRender)] != 0,
			"artifact has no STARTRENDER event");
	require(typeCounts[typeIndex(PvrTaObservationType::RenderDone)] != 0,
			"artifact has no render-done event");
	require(completedCausalRender,
			"artifact has no accepted TA block in a completed render context");
	require(pendingRenderGeneration == 0,
			"artifact ends with an unmatched render generation");
	return summary;
}

} // namespace research

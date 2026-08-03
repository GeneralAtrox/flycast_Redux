#include "research/pvr_draw_artifact.h"
#include "research/pvr_ta_semantic_decoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace research
{
namespace
{

constexpr std::array<std::uint8_t, 8> ArtifactMagic {
	'F', 'C', 'P', 'V', 'R', 'D', 'R', '1',
};
constexpr std::uint32_t HeaderComplete = 1u;
constexpr std::uint32_t EventHeaderSize = 32;
constexpr std::uint32_t PrimitiveBodySize = 120;
constexpr std::uint32_t DrawBodySize = 44;
constexpr std::uint32_t RenderCompletedBodySize = 16;
constexpr std::uint32_t BlockProvenanceSize = 72;

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid PowerVR draw artifact: " + reason);
}

void require(bool condition, const std::string& reason)
{
	if (!condition)
		invalid(reason);
}

class ByteReader
{
public:
	ByteReader(const std::uint8_t* data, std::size_t size)
			: data(data), size(size) {}

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
			result |= static_cast<std::uint32_t>(data[position + index])
					<< (index * 8);
		position += 4;
		return result;
	}

	std::uint64_t u64()
	{
		need(8);
		std::uint64_t result = 0;
		for (unsigned index = 0; index < 8; ++index)
			result |= static_cast<std::uint64_t>(data[position + index])
					<< (index * 8);
		position += 8;
		return result;
	}

	float f32()
	{
		const std::uint32_t bits = u32();
		float result = 0;
		std::memcpy(&result, &bits, sizeof(result));
		return result;
	}

	void bytes(void* destination, std::size_t count)
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
			invalid("truncated scalar or event");
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

Sha256Digest digest(ByteReader& reader)
{
	Sha256Digest result {};
	reader.bytes(result.data(), result.size());
	return result;
}

void requireDigest(const Sha256Digest& actual, const Sha256Digest& expected,
		const char* field)
{
	require(sha256Equal(actual, expected), std::string(field) + " digest mismatch");
}

bool validBackend(Sh4ObservationBackend backend)
{
	return backend == Sh4ObservationBackend::Interpreter
			|| backend == Sh4ObservationBackend::Dynarec;
}

PvrDrawArtifactSummary parseHeader(const std::uint8_t* data,
		const PvrDrawArtifactBinding& expected)
{
	ByteReader reader(data, PvrDrawArtifactHeaderSize);
	std::array<std::uint8_t, 8> magic {};
	reader.bytes(magic.data(), magic.size());
	require(magic == ArtifactMagic, "magic mismatch");
	require(reader.u32() == PvrDrawArtifactSchemaVersion,
			"unsupported schema version");
	require(reader.u32() == PvrDrawArtifactHeaderSize, "header size mismatch");
	require(reader.u32() == PvrDrawArtifactEndianSentinel,
			"endian sentinel mismatch");
	require(reader.u32() == HeaderComplete, "artifact is incomplete");
	PvrDrawArtifactSummary summary;
	summary.binding.backend = static_cast<Sh4ObservationBackend>(reader.u32());
	require(validBackend(summary.binding.backend), "CPU backend is invalid");
	require(reader.u32() == 0, "header reserved field is nonzero");
	summary.eventCount = reader.u64();
	for (auto& count : summary.typeCounts)
		count = reader.u64();
	summary.payloadBytes = reader.u64();
	summary.droppedEvents = reader.u64();
	summary.startTick = reader.u64();
	summary.endTick = reader.u64();
	summary.firstEmissionOrdinal = reader.u64();
	summary.lastEmissionOrdinal = reader.u64();
	summary.binding.identityDigest = digest(reader);
	summary.binding.replayDigest = digest(reader);
	summary.binding.taArtifactDigest = digest(reader);
	summary.binding.presentationArtifactDigest = digest(reader);
	summary.binding.rendererConfigurationDigest = digest(reader);
	summary.payloadDigest = digest(reader);
	require(reader.u32() == 0, "header terminal reserved field is nonzero");
	const std::uint32_t storedCrc = reader.u32();
	require(reader.remaining() == 0, "header decoder did not consume all bytes");
	require(storedCrc == crc32(data, PvrDrawArtifactHeaderSize - 4),
			"header CRC mismatch");
	require(summary.binding.backend == expected.backend,
			"CPU backend binding mismatch");
	requireDigest(summary.binding.identityDigest, expected.identityDigest, "identity");
	requireDigest(summary.binding.replayDigest, expected.replayDigest, "replay");
	requireDigest(summary.binding.taArtifactDigest, expected.taArtifactDigest,
			"TA artifact");
	requireDigest(summary.binding.presentationArtifactDigest,
			expected.presentationArtifactDigest, "presentation artifact");
	requireDigest(summary.binding.rendererConfigurationDigest,
			expected.rendererConfigurationDigest, "renderer configuration");
	require(summary.droppedEvents == 0, "artifact records dropped observations");
	return summary;
}

PvrTaBlockProvenance readBlock(ByteReader& reader,
		Sh4ObservationBackend expectedBackend, std::uint64_t eventTick)
{
	PvrTaBlockProvenance block;
	block.available = reader.u8() == 1;
	block.initiator.valid = block.available;
	block.initiator.backend = static_cast<Sh4ObservationBackend>(reader.u8());
	block.initiator.delaySlotDepth = reader.u16();
	block.initiator.opcode = reader.u16();
	require(reader.u16() == 0, "block owner reserved field is nonzero");
	block.initiator.generation = reader.u64();
	block.initiator.tick = reader.u64();
	block.initiator.pc = reader.u32();
	block.initiator.pr = reader.u32();
	block.contextAddress = reader.u32();
	block.contextGeneration = reader.u64();
	block.contextBlockOrdinal = reader.u64();
	block.renderPass = reader.u32();
	block.source = static_cast<PvrTaInputSource>(reader.u32());
	block.sourceAddress = reader.u32();
	block.taAddress = reader.u32();
	require(reader.u32() == 0, "block provenance reserved field is nonzero");
	require(block.available && block.initiator.valid,
			"block provenance is unavailable");
	require(block.initiator.backend == expectedBackend,
			"block owner backend differs from artifact binding");
	require(block.initiator.generation != 0 && (block.initiator.pc & 1u) == 0,
			"block owner token is invalid");
	require(block.initiator.tick <= eventTick,
			"block owner tick follows primitive decoding");
	require(block.contextAddress != UINT32_MAX && block.contextGeneration != 0,
			"block context identity is invalid");
	require(block.source >= PvrTaInputSource::StoreQueue
			&& block.source <= PvrTaInputSource::SortDma,
			"block source is invalid");
	require((block.sourceAddress & 31u) == 0,
			"block source address is not 32-byte aligned");
	if (block.source == PvrTaInputSource::StoreQueue)
		require((block.sourceAddress & 0xfc000000u) == 0xe0000000u,
				"store-queue block source is outside P4");
	else
		require((block.sourceAddress & 0xff000000u) == 0x0c000000u,
				"DMA block source is outside canonical RAM");
	if (block.source == PvrTaInputSource::SortDma)
		require(block.taAddress == UINT32_MAX,
				"sort-DMA block has a TA destination address");
	else
		require(block.taAddress != UINT32_MAX && (block.taAddress & 31u) == 0,
				"TA block destination is unavailable or unaligned");
	return block;
}

struct Primitive
{
	std::uint64_t renderGeneration = 0;
	PvrPrimitiveKind kind = PvrPrimitiveKind::PolygonStrip;
};

struct RenderState
{
	bool completed = false;
	bool successful = false;
	bool hasNonBackgroundPrimitive = false;
	bool consumedNonBackgroundPrimitive = false;
};

bool sameOwner(const Sh4InstructionOwnerToken& lhs,
		const Sh4InstructionOwnerToken& rhs)
{
	return lhs.valid == rhs.valid && lhs.backend == rhs.backend
			&& lhs.generation == rhs.generation && lhs.tick == rhs.tick
			&& lhs.pc == rhs.pc && lhs.pr == rhs.pr && lhs.opcode == rhs.opcode
			&& lhs.delaySlotDepth == rhs.delaySlotDepth;
}

bool sameBlock(const PvrTaBlockProvenance& lhs,
		const PvrTaBlockProvenance& rhs)
{
	return lhs.available == rhs.available && sameOwner(lhs.initiator, rhs.initiator)
			&& lhs.contextAddress == rhs.contextAddress
			&& lhs.contextGeneration == rhs.contextGeneration
			&& lhs.contextBlockOrdinal == rhs.contextBlockOrdinal
			&& lhs.renderPass == rhs.renderPass && lhs.source == rhs.source
			&& lhs.sourceAddress == rhs.sourceAddress && lhs.taAddress == rhs.taAddress;
}

PvrPrimitiveOwnerClass independentlyClassifyOwnership(
		const std::vector<PvrTaBlockProvenance>& parameterBlocks,
		const std::vector<PvrTaBlockProvenance>& vertexBlocks)
{
	const Sh4InstructionOwnerToken* first = nullptr;
	for (const auto* blocks : {&parameterBlocks, &vertexBlocks})
		for (const PvrTaBlockProvenance& block : *blocks)
		{
			if (!block.available || !block.initiator.valid)
				return PvrPrimitiveOwnerClass::Mixed;
			if (first == nullptr)
				first = &block.initiator;
			else if (!sameOwner(*first, block.initiator))
				return PvrPrimitiveOwnerClass::Mixed;
		}
	return first == nullptr ? PvrPrimitiveOwnerClass::Unowned
			: PvrPrimitiveOwnerClass::Exact;
}

std::uint32_t floatBits(float value)
{
	std::uint32_t result = 0;
	std::memcpy(&result, &value, sizeof(result));
	return result;
}

void comparePrimitive(const PvrTaSemanticPrimitive& expected,
		std::uint64_t renderGeneration, std::uint32_t contextAddress,
		std::uint64_t contextGeneration, std::uint32_t renderPass,
		std::uint32_t listType, PvrPrimitiveKind kind,
		const std::array<std::uint32_t, 9>& fields,
		const PvrPrimitiveBounds& bounds,
		const std::vector<PvrTaBlockProvenance>& parameterBlocks,
		const std::vector<PvrTaBlockProvenance>& vertexBlocks)
{
	require(renderGeneration == expected.renderGeneration
			&& contextAddress == expected.contextAddress
			&& contextGeneration == expected.contextGeneration
			&& renderPass == expected.renderPass && listType == expected.listType
			&& kind == expected.kind,
			"primitive identity differs from independent raw-TA reconstruction");
	const std::array<std::uint32_t, 9> expectedFields {
		expected.pcw, expected.isp, expected.tsp, expected.tcw,
		expected.tsp1, expected.tcw1, expected.tileClip, expected.first,
		expected.count,
	};
	require(fields == expectedFields,
			"primitive material or range differs from independent raw-TA reconstruction");
	const std::array<std::uint32_t, 6> actualBounds {
		floatBits(bounds.minimumX), floatBits(bounds.minimumY),
		floatBits(bounds.minimumZ), floatBits(bounds.maximumX),
		floatBits(bounds.maximumY), floatBits(bounds.maximumZ),
	};
	const std::array<std::uint32_t, 6> expectedBounds {
		floatBits(expected.bounds.minimumX), floatBits(expected.bounds.minimumY),
		floatBits(expected.bounds.minimumZ), floatBits(expected.bounds.maximumX),
		floatBits(expected.bounds.maximumY), floatBits(expected.bounds.maximumZ),
	};
	require(bounds.available == expected.bounds.available
			&& actualBounds == expectedBounds,
			"primitive bounds differ from independent raw-TA reconstruction");
	require(parameterBlocks.size() == expected.parameterBlocks.size()
			&& vertexBlocks.size() == expected.vertexBlocks.size(),
			"primitive contributor count differs from independent raw-TA reconstruction");
	for (std::size_t i = 0; i < parameterBlocks.size(); ++i)
		require(sameBlock(parameterBlocks[i], expected.parameterBlocks[i]),
				"primitive parameter ownership differs from raw TA");
	for (std::size_t i = 0; i < vertexBlocks.size(); ++i)
		require(sameBlock(vertexBlocks[i], expected.vertexBlocks[i]),
				"primitive vertex ownership differs from raw TA");
}

PvrDrawArtifactSummary validateFile(const std::filesystem::path& path,
		const PvrDrawArtifactBinding& expectedBinding,
		std::uint64_t maximumBytes, std::uint64_t maximumEvents,
		const std::vector<PvrTaSemanticPrimitive>* expectedPrimitives)
{

	std::error_code error;
	const std::uintmax_t fileSize = std::filesystem::file_size(path, error);
	require(!error && fileSize >= PvrDrawArtifactHeaderSize
			&& fileSize <= maximumBytes, "file size is invalid");
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileSize));
	std::ifstream input(path, std::ios::binary);
	require(input.good(), "file cannot be opened");
	input.read(reinterpret_cast<char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
	require(input.gcount() == static_cast<std::streamsize>(bytes.size()),
			"file is truncated");
	require(std::filesystem::file_size(path, error) == fileSize && !error,
			"file changed during validation");

	PvrDrawArtifactSummary summary = parseHeader(bytes.data(), expectedBinding);
	require(summary.eventCount != 0 && summary.eventCount <= maximumEvents,
			"event count is outside the configured limit");
	require(summary.payloadBytes == fileSize - PvrDrawArtifactHeaderSize,
			"payload size differs from the header");
	const std::uint8_t* payload = bytes.data() + PvrDrawArtifactHeaderSize;
	requireDigest(sha256(payload, static_cast<std::size_t>(summary.payloadBytes)),
			summary.payloadDigest, "payload");

	ByteReader reader(payload, static_cast<std::size_t>(summary.payloadBytes));
	std::array<std::uint64_t, 4> typeCounts {};
	std::unordered_map<std::uint64_t, Primitive> primitives;
	std::unordered_map<std::uint64_t, RenderState> renders;
	std::uint64_t previousTick = 0;
	std::uint64_t previousEmission = 0;
	std::uint64_t previousRaster = 0;
	bool complete = false;
	std::size_t semanticIndex = 0;

	for (std::uint64_t ordinal = 0; ordinal < summary.eventCount; ++ordinal)
	{
		require(reader.remaining() >= EventHeaderSize,
				"payload ends before an event header");
		const auto type = static_cast<PvrDrawObservationType>(reader.u32());
		const std::uint32_t eventSize = reader.u32();
		const std::uint64_t artifactOrdinal = reader.u64();
		const std::uint64_t emissionOrdinal = reader.u64();
		const std::uint64_t tick = reader.u64();
		require(type >= PvrDrawObservationType::PrimitiveDecoded
				&& type <= PvrDrawObservationType::Reset,
				"event type is invalid");
		require(artifactOrdinal == ordinal, "artifact ordinal is not contiguous");
		require(eventSize >= EventHeaderSize
				&& eventSize - EventHeaderSize <= reader.remaining(),
				"event size exceeds the payload");
		if (ordinal == 0)
		{
			require(tick == summary.startTick, "first tick differs from header");
			require(emissionOrdinal == summary.firstEmissionOrdinal,
					"first emission differs from header");
		}
		else
		{
			require(tick >= previousTick, "scheduler tick moved backwards");
			require(emissionOrdinal == previousEmission + 1,
					"emission ordinal is not contiguous");
		}
		previousTick = tick;
		previousEmission = emissionOrdinal;
		++typeCounts[static_cast<std::size_t>(type) - 1];
		std::vector<std::uint8_t> body(eventSize - EventHeaderSize);
		reader.bytes(body.data(), body.size());
		ByteReader event(body.data(), body.size());

		switch (type)
		{
		case PvrDrawObservationType::PrimitiveDecoded:
		{
			require(body.size() >= PrimitiveBodySize,
					"primitive body is truncated");
			const std::uint64_t renderGeneration = event.u64();
			const std::uint64_t primitiveGeneration = event.u64();
			const std::uint32_t contextAddress = event.u32();
			const std::uint64_t contextGeneration = event.u64();
			const std::uint32_t renderPass = event.u32();
			const std::uint32_t listType = event.u32();
			const auto kind = static_cast<PvrPrimitiveKind>(event.u32());
			const auto ownerClass = static_cast<PvrPrimitiveOwnerClass>(event.u32());
			std::array<std::uint32_t, 9> fields {};
			for (std::uint32_t& field : fields)
				field = event.u32();
			const float minX = event.f32();
			const float minY = event.f32();
			const float minZ = event.f32();
			const float maxX = event.f32();
			const float maxY = event.f32();
			const float maxZ = event.f32();
			const std::uint32_t boundsAvailable = event.u32();
			const std::uint32_t parameterCount = event.u32();
			const std::uint32_t vertexCount = event.u32();
			require(event.u32() == 0, "primitive reserved field is nonzero");
			require(renderGeneration != 0 && primitiveGeneration != 0,
					"primitive generation is zero");
			require(kind >= PvrPrimitiveKind::Background
					&& kind <= PvrPrimitiveKind::ModifierVolume,
					"primitive kind is invalid");
			require(ownerClass >= PvrPrimitiveOwnerClass::Unowned
					&& ownerClass <= PvrPrimitiveOwnerClass::Mixed,
					"primitive owner class is invalid");
			require(listType <= 4, "primitive list type is invalid");
			require(boundsAvailable == 1 && std::isfinite(minX)
					&& std::isfinite(minY) && std::isfinite(minZ)
					&& std::isfinite(maxX) && std::isfinite(maxY)
					&& std::isfinite(maxZ) && minX <= maxX && minY <= maxY
					&& minZ <= maxZ, "primitive bounds are invalid");
			require(parameterCount <= MaximumPvrDrawBlocksPerPrimitive
					&& vertexCount <= MaximumPvrDrawBlocksPerPrimitive,
					"primitive block count exceeds the limit");
			const std::uint64_t expectedBody = PrimitiveBodySize
					+ static_cast<std::uint64_t>(parameterCount + vertexCount)
							* BlockProvenanceSize;
			require(body.size() == expectedBody, "primitive body size mismatch");
			std::vector<PvrTaBlockProvenance> parameterBlocks;
			std::vector<PvrTaBlockProvenance> vertexBlocks;
			parameterBlocks.reserve(parameterCount);
			vertexBlocks.reserve(vertexCount);
			for (std::uint32_t index = 0; index < parameterCount; ++index)
				parameterBlocks.push_back(readBlock(event, expectedBinding.backend,
						tick));
			for (std::uint32_t index = 0; index < vertexCount; ++index)
				vertexBlocks.push_back(readBlock(event, expectedBinding.backend,
						tick));
			require(event.remaining() == 0, "primitive has trailing bytes");
			if (kind == PvrPrimitiveKind::Background)
				require(parameterBlocks.empty() && vertexBlocks.empty()
						&& ownerClass == PvrPrimitiveOwnerClass::Unowned,
						"background primitive fabricates TA ownership");
			else
				require(!parameterBlocks.empty() && !vertexBlocks.empty()
						&& contextAddress != UINT32_MAX && contextGeneration != 0,
						"guest primitive lacks parameter or vertex provenance");
			for (const auto* blocks : {&parameterBlocks, &vertexBlocks})
			{
				std::uint64_t priorOrdinal = 0;
				bool first = true;
				for (const auto& block : *blocks)
				{
					require(block.contextAddress == contextAddress
							&& block.contextGeneration == contextGeneration
							&& block.renderPass == renderPass,
							"primitive block context or pass differs");
					require(first || block.contextBlockOrdinal > priorOrdinal,
							"primitive block ordinals are not strictly increasing");
					first = false;
					priorOrdinal = block.contextBlockOrdinal;
				}
			}
			require(ownerClass == independentlyClassifyOwnership(
					parameterBlocks, vertexBlocks),
					"primitive owner class disagrees with its exact contributors");
			if (expectedPrimitives != nullptr && kind != PvrPrimitiveKind::Background)
			{
				require(semanticIndex < expectedPrimitives->size(),
						"draw artifact invents a primitive absent from raw TA");
				PvrPrimitiveBounds decodedBounds;
				decodedBounds.minimumX = minX;
				decodedBounds.minimumY = minY;
				decodedBounds.minimumZ = minZ;
				decodedBounds.maximumX = maxX;
				decodedBounds.maximumY = maxY;
				decodedBounds.maximumZ = maxZ;
				decodedBounds.available = boundsAvailable == 1;
				comparePrimitive((*expectedPrimitives)[semanticIndex++],
						renderGeneration, contextAddress, contextGeneration,
						renderPass, listType, kind, fields, decodedBounds,
						parameterBlocks, vertexBlocks);
			}
			require(primitives.emplace(primitiveGeneration,
					Primitive {renderGeneration, kind}).second,
					"primitive generation is reused");
			RenderState& render = renders[renderGeneration];
			require(!render.completed, "primitive follows render completion");
			render.hasNonBackgroundPrimitive = render.hasNonBackgroundPrimitive
					|| kind != PvrPrimitiveKind::Background;
			break;
		}
		case PvrDrawObservationType::DrawConsumed:
		{
			require(body.size() >= DrawBodySize, "draw body is truncated");
			const std::uint64_t renderGeneration = event.u64();
			const std::uint64_t rasterGeneration = event.u64();
			const auto backend = static_cast<PvrDrawBackend>(event.u32());
			const auto drawPass = static_cast<PvrDrawPass>(event.u32());
			event.u32(); // first
			const std::uint32_t count = event.u32();
			const std::uint32_t indexed = event.u32();
			const std::uint32_t refCount = event.u32();
			require(event.u32() == 0, "draw reserved field is nonzero");
			require(renderGeneration != 0 && rasterGeneration != 0 && count != 0,
					"draw generation or count is zero");
			require(backend >= PvrDrawBackend::DirectX9
					&& backend <= PvrDrawBackend::Vulkan,
					"draw backend is invalid");
			require(drawPass >= PvrDrawPass::Background
					&& drawPass <= PvrDrawPass::ModifierResolve,
					"draw pass is invalid");
			require(indexed <= 1, "draw indexed flag is invalid");
			require(refCount <= MaximumPvrDrawPrimitiveRefs
					&& body.size() == DrawBodySize
							+ static_cast<std::uint64_t>(refCount) * 8,
					"draw primitive references exceed its body or limit");
			require(previousRaster == 0 || rasterGeneration == previousRaster + 1,
					"raster generations are not contiguous");
			previousRaster = rasterGeneration;
			RenderState& render = renders[renderGeneration];
			require(!render.completed, "draw follows render completion");
			std::unordered_set<std::uint64_t> refs;
			for (std::uint32_t index = 0; index < refCount; ++index)
			{
				const std::uint64_t ref = event.u64();
				require(ref != 0 && refs.insert(ref).second,
						"draw primitive reference is zero or duplicated");
				const auto found = primitives.find(ref);
				require(found != primitives.end()
						&& found->second.renderGeneration == renderGeneration,
						"draw references a missing or foreign-render primitive");
				if (found->second.kind != PvrPrimitiveKind::Background)
					render.consumedNonBackgroundPrimitive = true;
			}
			require(refCount != 0 || drawPass == PvrDrawPass::ModifierResolve,
					"ordinary draw is unowned");
			require(event.remaining() == 0, "draw has trailing bytes");
			break;
		}
		case PvrDrawObservationType::RenderCompleted:
		{
			require(body.size() == RenderCompletedBodySize,
					"render-completed body size mismatch");
			const std::uint64_t renderGeneration = event.u64();
			const std::uint32_t successful = event.u32();
			require(event.u32() == 0,
					"render-completed reserved field is nonzero");
			require(renderGeneration != 0 && successful <= 1,
					"render-completed fields are invalid");
			RenderState& render = renders[renderGeneration];
			require(!render.completed, "render generation completes twice");
			render.completed = true;
			render.successful = successful == 1;
			complete = complete || (render.successful
					&& render.hasNonBackgroundPrimitive
					&& render.consumedNonBackgroundPrimitive);
			break;
		}
		case PvrDrawObservationType::Reset:
			for (const auto& [generation, render] : renders)
				require(render.completed, "reset interrupts an open draw render");
			break;
		}
	}
	require(reader.remaining() == 0, "payload has trailing bytes");
	require(previousTick == summary.endTick, "last tick differs from header");
	require(previousEmission == summary.lastEmissionOrdinal,
			"last emission differs from header");
	require(typeCounts == summary.typeCounts, "event type counts differ from header");
	require(typeCounts[0] != 0 && typeCounts[1] != 0 && typeCounts[2] != 0,
			"artifact lacks a primitive, draw, or render-completed event");
	require(complete, "artifact lacks a consumed non-background primitive in a successful render");
	if (expectedPrimitives != nullptr)
		require(semanticIndex == expectedPrimitives->size(),
				"draw artifact omits a primitive reconstructed from raw TA");
	summary.completeVerticalSlice = true;
	return summary;
}

} // namespace

PvrDrawArtifactSummary validatePvrDrawArtifactFile(
		const std::filesystem::path& path,
		const PvrDrawArtifactBinding& expectedBinding,
		std::uint64_t maximumBytes, std::uint64_t maximumEvents)
{
	return validateFile(path, expectedBinding, maximumBytes, maximumEvents, nullptr);
}

PvrDrawArtifactSummary validatePvrDrawArtifactAgainstTaFile(
		const std::filesystem::path& drawPath,
		const PvrDrawArtifactBinding& expectedDrawBinding,
		const std::filesystem::path& taPath,
		const PvrTaArtifactBinding& expectedTaBinding,
		std::uint64_t maximumDrawBytes, std::uint64_t maximumDrawEvents,
		std::uint64_t maximumTaBytes, std::uint64_t maximumTaEvents)
{
	const PvrTaSemanticSummary semantics = reconstructPvrTaSemantics(taPath,
			expectedTaBinding, maximumTaBytes, maximumTaEvents);
	require(!semantics.primitives.empty(),
			"raw TA artifact contains no non-background primitive");
	return validateFile(drawPath, expectedDrawBinding, maximumDrawBytes,
			maximumDrawEvents, &semantics.primitives);
}

} // namespace research

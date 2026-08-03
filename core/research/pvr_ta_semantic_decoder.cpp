#include "research/pvr_ta_semantic_decoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace research
{
namespace
{

constexpr std::uint32_t EventHeaderSize = 32;
constexpr std::uint32_t OwnerSize = 32;
constexpr std::uint32_t NoList = UINT32_MAX;

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("cannot reconstruct PowerVR TA semantics: " + reason);
}

void require(bool condition, const std::string& reason)
{
	if (!condition)
		invalid(reason);
}

class Reader
{
public:
	Reader(const std::uint8_t* data, std::size_t size) : data(data), size(size) {}

	std::uint8_t u8()
	{
		need(1);
		return data[position++];
	}
	std::uint16_t u16()
	{
		need(2);
		const auto result = static_cast<std::uint16_t>(data[position])
				| static_cast<std::uint16_t>(data[position + 1] << 8);
		position += 2;
		return static_cast<std::uint16_t>(result);
	}
	std::uint32_t u32()
	{
		need(4);
		std::uint32_t result = 0;
		for (unsigned i = 0; i < 4; ++i)
			result |= static_cast<std::uint32_t>(data[position + i]) << (i * 8);
		position += 4;
		return result;
	}
	std::uint64_t u64()
	{
		need(8);
		std::uint64_t result = 0;
		for (unsigned i = 0; i < 8; ++i)
			result |= static_cast<std::uint64_t>(data[position + i]) << (i * 8);
		position += 8;
		return result;
	}
	void bytes(void* destination, std::size_t count)
	{
		need(count);
		std::memcpy(destination, data + position, count);
		position += count;
	}
	void skip(std::size_t count)
	{
		need(count);
		position += count;
	}
	std::size_t remaining() const { return size - position; }

private:
	void need(std::size_t count) const
	{
		if (position > size || count > size - position)
			invalid("truncated artifact event");
	}
	const std::uint8_t* data;
	std::size_t size;
	std::size_t position = 0;
};

std::uint32_t word(const std::array<std::uint8_t, 32>& block,
		std::size_t offset)
{
	require(offset <= 28, "TA word offset is invalid");
	return static_cast<std::uint32_t>(block[offset])
			| static_cast<std::uint32_t>(block[offset + 1]) << 8
			| static_cast<std::uint32_t>(block[offset + 2]) << 16
			| static_cast<std::uint32_t>(block[offset + 3]) << 24;
}

float real(const std::array<std::uint8_t, 32>& block, std::size_t offset)
{
	const std::uint32_t bits = word(block, offset);
	float result = 0;
	std::memcpy(&result, &bits, sizeof(result));
	require(std::isfinite(result), "TA vertex coordinate is non-finite");
	return result;
}

struct RawBlock
{
	PvrTaBlockProvenance provenance;
	std::array<std::uint8_t, 32> bytes {};
};

struct Context
{
	std::uint64_t generation = 0;
	std::vector<RawBlock> current;
	std::vector<RawBlock> previous;
};

Sh4InstructionOwnerToken owner(Reader& reader)
{
	Sh4InstructionOwnerToken result;
	result.valid = reader.u8() == 1;
	result.backend = static_cast<Sh4ObservationBackend>(reader.u8());
	result.delaySlotDepth = reader.u16();
	result.opcode = reader.u16();
	require(reader.u16() == 0, "owner reserved field is nonzero");
	result.generation = reader.u64();
	result.tick = reader.u64();
	result.pc = reader.u32();
	result.pr = reader.u32();
	return result;
}

PvrPrimitiveBounds bounds(const std::vector<std::array<float, 3>>& vertices)
{
	require(!vertices.empty(), "primitive has no reconstructed vertices");
	PvrPrimitiveBounds result;
	result.minimumX = result.maximumX = vertices[0][0];
	result.minimumY = result.maximumY = vertices[0][1];
	result.minimumZ = result.maximumZ = vertices[0][2];
	for (const auto& vertex : vertices)
	{
		result.minimumX = std::min(result.minimumX, vertex[0]);
		result.minimumY = std::min(result.minimumY, vertex[1]);
		result.minimumZ = std::min(result.minimumZ, vertex[2]);
		result.maximumX = std::max(result.maximumX, vertex[0]);
		result.maximumY = std::max(result.maximumY, vertex[1]);
		result.maximumZ = std::max(result.maximumZ, vertex[2]);
	}
	result.available = true;
	return result;
}

unsigned polygonVertexType(std::uint32_t pcw)
{
	const bool uv16 = (pcw & 1u) != 0;
	const bool texture = (pcw & 8u) != 0;
	const unsigned color = (pcw >> 4) & 3u;
	const bool volume = (pcw & 0x40u) != 0;
	if (!texture)
	{
		if (!volume)
			return color == 0 ? 0 : color == 1 ? 1 : 2;
		if (color == 1)
			invalid("invalid two-volume floating-color polygon format");
		return color == 0 ? 9 : 10;
	}
	if (!volume)
	{
		if (color == 0)
			return uv16 ? 4 : 3;
		if (color == 1)
			return uv16 ? 6 : 5;
		return uv16 ? 8 : 7;
	}
	if (color == 1)
		invalid("invalid two-volume floating-color polygon format");
	if (color == 0)
		return uv16 ? 12 : 11;
	return uv16 ? 14 : 13;
}

unsigned polygonHeaderBlocks(std::uint32_t pcw)
{
	const bool texture = (pcw & 8u) != 0;
	const bool offset = (pcw & 4u) != 0;
	const unsigned color = (pcw >> 4) & 3u;
	const bool volume = (pcw & 0x40u) != 0;
	if (volume)
	{
		if (color == 1)
			invalid("invalid two-volume polygon header");
		return color == 2 ? 2 : 1;
	}
	return color == 2 && texture && offset ? 2 : 1;
}

bool twoBlockVertex(unsigned type)
{
	return type == 5 || type == 6 || type == 11 || type == 12
			|| type == 13 || type == 14;
}

struct PrimitiveBuilder
{
	PvrTaSemanticPrimitive value;
	std::vector<std::array<float, 3>> vertices;
	unsigned vertexType = 0;
};

class StreamDecoder
{
public:
	StreamDecoder(std::uint64_t renderGeneration, std::uint32_t rootContext)
		: renderGeneration(renderGeneration), rootContext(rootContext) {}

	void parsePass(const std::vector<RawBlock>& blocks, std::uint32_t pass)
	{
		const std::array<std::size_t, 5> before {
			lists[0].size(), lists[1].size(), lists[2].size(),
			lists[3].size(), lists[4].size(),
		};
		for (const RawBlock& block : blocks)
			parse(block);
		const bool emptyPolygonPass = lists[0].size() == before[0]
				&& lists[2].size() == before[2] && lists[4].size() == before[4];
		if (pass != 0 && emptyPolygonPass)
			return;
		for (std::uint32_t list : {0u, 4u, 2u, 1u, 3u})
		{
			for (std::size_t index = before[list]; index < lists[list].size(); ++index)
			{
				PvrTaSemanticPrimitive primitive = lists[list][index];
				primitive.renderGeneration = renderGeneration;
				primitive.contextAddress = rootContext;
				const auto& identityBlocks = !primitive.parameterBlocks.empty()
						? primitive.parameterBlocks : primitive.vertexBlocks;
				require(!identityBlocks.empty(), "guest primitive lacks provenance");
				primitive.contextGeneration = identityBlocks.front().contextGeneration;
				primitive.renderPass = pass;
				primitive.listType = list;
				if (pass == 0 && list == 0)
				{
					primitive.tsp &= ~((7u << 26) | (7u << 29));
					primitive.tsp |= 1u << 29;
				}
				output.push_back(std::move(primitive));
			}
		}
	}

	std::vector<PvrTaSemanticPrimitive> take() { return std::move(output); }

private:
	void startPolygon(const RawBlock& block, bool sprite)
	{
		const std::uint32_t pcw = word(block.bytes, 0);
		const std::uint32_t list = (pcw >> 24) & 7u;
		require(list <= 4 && list != NoList, "polygon list type is invalid");
		if (currentList == NoList)
			currentList = list;
		require(currentList == list && currentList != 1 && currentList != 3,
				"polygon header conflicts with the active list");
		current = PrimitiveBuilder {};
		current->value.kind = sprite ? PvrPrimitiveKind::Sprite
				: PvrPrimitiveKind::PolygonStrip;
		current->value.pcw = pcw;
		current->value.isp = word(block.bytes, 4);
		if (sprite)
			current->value.isp ^= 1u << 27;
		current->value.tsp = word(block.bytes, 8);
		current->value.tcw = word(block.bytes, 12);
		current->value.tileClip = tileClip;
		current->value.first = vertexCount;
		current->value.parameterBlocks.push_back(block.provenance);
		current->vertexType = sprite ? 15 : polygonVertexType(pcw);
		if (!sprite && (pcw & 0x40u) != 0)
		{
			current->value.tsp1 = word(block.bytes, 16);
			current->value.tcw1 = word(block.bytes, 20);
		}
		const unsigned headerBlocks = sprite ? 1 : polygonHeaderBlocks(pcw);
		pending = headerBlocks == 2 ? Pending::HeaderSecond : Pending::None;
	}

	void finishPolygonVertex()
	{
		require(current.has_value(), "polygon vertex has no active parameter");
		++vertexCount;
		if (!pendingEndOfStrip)
			return;
		current->value.count = vertexCount - current->value.first;
		current->value.bounds = bounds(current->vertices);
		lists[currentList].push_back(current->value);
		current->value.first = vertexCount;
		current->value.count = 0;
		current->value.vertexBlocks.clear();
		current->vertices.clear();
		pendingEndOfStrip = false;
	}

	void finishSprite(const RawBlock& second)
	{
		require(current.has_value(), "sprite vertex has no active parameter");
		const auto& first = pendingFirst.bytes;
		std::array<float, 3> a {real(first, 4), real(first, 8), real(first, 12)};
		std::array<float, 3> b {real(first, 16), real(first, 20), real(first, 24)};
		std::array<float, 3> c {real(first, 28), real(second.bytes, 0),
				real(second.bytes, 4)};
		std::array<float, 3> p {real(second.bytes, 8), real(second.bytes, 12), 0};
		const float acx = c[0] - a[0];
		const float acy = c[1] - a[1];
		const float acz = c[2] - a[2];
		const float abx = b[0] - a[0];
		const float aby = b[1] - a[1];
		const float abz = b[2] - a[2];
		const float denominator = acx * aby - acy * abx;
		require(denominator != 0, "degenerate sprite plane");
		const float k2 = ((p[0] - a[0]) * aby - (p[1] - a[1]) * abx)
				/ denominator;
		float k1 = 0;
		if (abx == 0)
		{
			require(aby != 0, "degenerate sprite edge");
			k1 = (p[1] - a[1] - k2 * acy) / aby;
		}
		else
			k1 = (p[0] - a[0] - k2 * acx) / abx;
		p[2] = a[2] + k1 * abz + k2 * acz;
		require(std::isfinite(p[2]), "sprite plane produced non-finite depth");
		current->vertices = {p, c, a, b};
		current->value.vertexBlocks.push_back(pendingFirst.provenance);
		current->value.vertexBlocks.push_back(second.provenance);
		current->value.count = 4;
		current->value.bounds = bounds(current->vertices);
		lists[currentList].push_back(current->value);
		vertexCount += 4;
		current->value.first = vertexCount;
		current->value.count = 0;
		current->value.vertexBlocks.clear();
		current->vertices.clear();
	}

	void startModifier(const RawBlock& block)
	{
		finishModifier();
		const std::uint32_t pcw = word(block.bytes, 0);
		const std::uint32_t list = (pcw >> 24) & 7u;
		require(list == 1 || list == 3, "modifier header has a non-modifier list");
		if (currentList == NoList)
			currentList = list;
		require(currentList == list, "modifier header conflicts with active list");
		modifier = PvrTaSemanticPrimitive {};
		modifier->kind = PvrPrimitiveKind::ModifierVolume;
		modifier->isp = (word(block.bytes, 4) & ~(1u << 26))
				| ((pcw & 0x40u) != 0 ? 1u << 26 : 0u);
		modifier->tileClip = tileClip;
		modifier->first = modifierCount;
		modifier->parameterBlocks.push_back(block.provenance);
		modifierVertices.clear();
	}

	void finishModifier()
	{
		if (!modifier.has_value())
			return;
		if (!modifierVertices.empty())
		{
			modifier->count = static_cast<std::uint32_t>(modifierVertices.size() / 3);
			modifier->bounds = bounds(modifierVertices);
			lists[currentList].push_back(*modifier);
			modifierCount += modifier->count;
		}
		modifier.reset();
		modifierVertices.clear();
	}

	void parse(const RawBlock& block)
	{
		if (pending == Pending::HeaderSecond)
		{
			require(current.has_value(), "second polygon header block is orphaned");
			current->value.parameterBlocks.push_back(block.provenance);
			pending = Pending::None;
			return;
		}
		if (pending == Pending::PolygonSecond)
		{
			require(current.has_value(), "second polygon vertex block is orphaned");
			current->value.vertexBlocks.push_back(block.provenance);
			pending = Pending::None;
			finishPolygonVertex();
			return;
		}
		if (pending == Pending::SpriteSecond)
		{
			finishSprite(block);
			pending = Pending::None;
			return;
		}
		if (pending == Pending::ModifierSecond)
		{
			require(modifier.has_value(), "second modifier vertex block is orphaned");
			modifier->vertexBlocks.push_back(block.provenance);
			modifierVertices.push_back({real(pendingFirst.bytes, 4),
					real(pendingFirst.bytes, 8), real(pendingFirst.bytes, 12)});
			modifierVertices.push_back({real(pendingFirst.bytes, 16),
					real(pendingFirst.bytes, 20), real(pendingFirst.bytes, 24)});
			modifierVertices.push_back({real(pendingFirst.bytes, 28),
					real(block.bytes, 0), real(block.bytes, 4)});
			pending = Pending::None;
			return;
		}

		const std::uint32_t pcw = word(block.bytes, 0);
		const unsigned parameterType = pcw >> 29;
		switch (parameterType)
		{
		case 0: // end of list
			if (current.has_value() && current->value.count == 0)
				current.reset();
			finishModifier();
			currentList = NoList;
			break;
		case 1: // user tile clip
			tileClip = (tileClip & 0xf0000000u) | (word(block.bytes, 12) & 63u)
					| ((word(block.bytes, 20) & 63u) << 6)
					| ((word(block.bytes, 16) & 31u) << 12)
					| ((word(block.bytes, 24) & 31u) << 17);
			break;
		case 2: // object-list set: deliberately ignored by Flycast's parser
			break;
		case 4:
			tileClip = (tileClip & 0x0fffffffu) | (((pcw >> 16) & 3u) << 28);
			if (currentList == 1 || currentList == 3 || ((pcw >> 24) & 7u) == 1
					|| ((pcw >> 24) & 7u) == 3)
				startModifier(block);
			else
				startPolygon(block, false);
			break;
		case 5:
			tileClip = (tileClip & 0x0fffffffu) | (((pcw >> 16) & 3u) << 28);
			startPolygon(block, true);
			break;
		case 7:
			if (currentList == 1 || currentList == 3)
			{
				require(modifier.has_value(), "modifier vertex has no active parameter");
				modifier->vertexBlocks.push_back(block.provenance);
				pendingFirst = block;
				pending = Pending::ModifierSecond;
			}
			else
			{
				require(current.has_value(), "vertex has no active polygon parameter");
				if (current->value.kind == PvrPrimitiveKind::Sprite)
				{
					pendingFirst = block;
					pending = Pending::SpriteSecond;
				}
				else
				{
					current->value.vertexBlocks.push_back(block.provenance);
					current->vertices.push_back({real(block.bytes, 4),
							real(block.bytes, 8), real(block.bytes, 12)});
					pendingEndOfStrip = ((pcw >> 28) & 1u) != 0;
					if (twoBlockVertex(current->vertexType))
						pending = Pending::PolygonSecond;
					else
						finishPolygonVertex();
				}
			}
			break;
		default:
			invalid("unsupported or reserved TA parameter type");
		}
	}

	enum class Pending { None, HeaderSecond, PolygonSecond, SpriteSecond, ModifierSecond };
	std::uint64_t renderGeneration;
	std::uint32_t rootContext;
	std::uint32_t currentList = NoList;
	std::uint32_t tileClip = (39u << 6) | (14u << 17);
	std::uint32_t vertexCount = 4; // four renderer-created background vertices
	std::uint32_t modifierCount = 0;
	Pending pending = Pending::None;
	bool pendingEndOfStrip = false;
	RawBlock pendingFirst;
	std::optional<PrimitiveBuilder> current;
	std::optional<PvrTaSemanticPrimitive> modifier;
	std::vector<std::array<float, 3>> modifierVertices;
	std::array<std::vector<PvrTaSemanticPrimitive>, 5> lists;
	std::vector<PvrTaSemanticPrimitive> output;
};

} // namespace

PvrTaSemanticSummary reconstructPvrTaSemantics(
		const std::filesystem::path& path,
		const PvrTaArtifactBinding& expectedBinding,
		std::uint64_t maximumBytes, std::uint64_t maximumEvents)
{
	const PvrTaArtifactSummary validated = validatePvrTaArtifactFile(path,
			expectedBinding, maximumBytes, maximumEvents);
	std::ifstream input(path, std::ios::binary);
	require(input.good(), "cannot open validated artifact");
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(
			PvrTaArtifactHeaderSize + validated.payloadBytes));
	input.read(reinterpret_cast<char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
	require(input.gcount() == static_cast<std::streamsize>(bytes.size()),
			"validated artifact changed while reopening");
	Reader stream(bytes.data() + PvrTaArtifactHeaderSize,
			static_cast<std::size_t>(validated.payloadBytes));
	std::unordered_map<std::uint32_t, Context> contexts;
	PvrTaSemanticSummary result;

	for (std::uint64_t ordinal = 0; ordinal < validated.eventCount; ++ordinal)
	{
		const auto type = static_cast<PvrTaObservationType>(stream.u32());
		const std::uint32_t eventSize = stream.u32();
		stream.u64(); // artifact ordinal
		stream.u64(); // emission ordinal
		stream.u64(); // tick
		require(eventSize >= EventHeaderSize
				&& eventSize - EventHeaderSize <= stream.remaining(),
				"event size exceeds the validated payload");
		std::vector<std::uint8_t> body(eventSize - EventHeaderSize);
		stream.bytes(body.data(), body.size());
		Reader event(body.data(), body.size());
		const Sh4InstructionOwnerToken initiator = owner(event);
		(void)initiator;
		if (type == PvrTaObservationType::ListInit
				|| type == PvrTaObservationType::ListContinue)
		{
			const std::uint32_t address = event.u32();
			const std::uint64_t generation = event.u64();
			event.u32();
			Context& context = contexts[address];
			if (type == PvrTaObservationType::ListInit)
			{
				context = Context {};
				context.generation = generation;
			}
			else
			{
				require(context.generation == generation,
						"list continuation context generation differs");
				context.previous = std::move(context.current);
				context.current.clear();
			}
		}
		else if (type == PvrTaObservationType::AcceptedBlock)
		{
			RawBlock block;
			block.provenance.initiator = initiator;
			block.provenance.contextAddress = event.u32();
			block.provenance.contextGeneration = event.u64();
			block.provenance.contextBlockOrdinal = event.u64();
			block.provenance.renderPass = event.u32();
			event.skip(16); // list-before/after and parser-before/after
			block.provenance.source = static_cast<PvrTaInputSource>(event.u32());
			block.provenance.sourceAddress = event.u32();
			block.provenance.taAddress = event.u32();
			event.bytes(block.bytes.data(), block.bytes.size());
			require(event.u32() == 0 && event.remaining() == 0,
					"accepted block body differs from schema v1");
			block.provenance.available = true;
			Context& context = contexts[block.provenance.contextAddress];
			require(context.generation == block.provenance.contextGeneration,
					"accepted block context generation differs");
			context.current.push_back(std::move(block));
		}
		else if (type == PvrTaObservationType::StartRender)
		{
			const std::uint64_t renderGeneration = event.u64();
			event.u32(); // first-context availability
			event.skip(8); // selection registers
			const std::uint32_t contextCount = event.u32();
			const std::uint32_t readCount = event.u32();
			require(event.u32() == 0, "STARTRENDER reserved field is nonzero");
			struct Ref { std::uint32_t address; std::uint64_t generation; bool available; };
			std::vector<Ref> refs;
			refs.reserve(contextCount);
			for (std::uint32_t i = 0; i < contextCount; ++i)
			{
				Ref ref {event.u32(), event.u64(), event.u32() == 1};
				refs.push_back(ref);
			}
			event.skip(static_cast<std::size_t>(readCount) * 8);
			require(event.remaining() == 0, "STARTRENDER body has trailing bytes");
			const auto root = std::find_if(refs.begin(), refs.end(),
					[](const Ref& ref) { return ref.available; });
			if (root == refs.end())
				continue;
			StreamDecoder decoder(renderGeneration, root->address);
			std::uint32_t pass = 0;
			for (const Ref& ref : refs)
			{
				if (!ref.available)
					continue;
				const auto found = contexts.find(ref.address);
				require(found != contexts.end() && found->second.generation == ref.generation,
						"selected context is unavailable to semantic decoder");
				const auto& blocks = found->second.current.empty()
						? found->second.previous : found->second.current;
				decoder.parsePass(blocks, pass++);
			}
			auto primitives = decoder.take();
			result.primitives.insert(result.primitives.end(),
					std::make_move_iterator(primitives.begin()),
					std::make_move_iterator(primitives.end()));
			++result.renderCount;
		}
	}
	require(stream.remaining() == 0, "validated artifact payload was not consumed");
	return result;
}

} // namespace research

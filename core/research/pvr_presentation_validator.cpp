#include "research/pvr_presentation_artifact.h"
#include "research/pvr_framebuffer.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace research
{
namespace
{

constexpr std::array<std::uint8_t, 8> ArtifactMagic {
	'F', 'C', 'P', 'V', 'P', 'R', 'S', '1',
};
constexpr std::uint32_t HeaderComplete = 1u;
constexpr std::uint32_t EventHeaderSize = 32;
constexpr std::uint32_t OwnerSize = 32;
constexpr std::uint32_t VramSize = 8 * 1024 * 1024;

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid PowerVR presentation artifact: " + reason);
}

void require(bool condition, const std::string& reason)
{
	if (!condition)
		invalid(reason);
}

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

class Reader
{
public:
	Reader(const std::uint8_t* data, std::size_t size) : data(data), size(size) {}

	std::uint8_t u8()
	{
		require(remaining() >= 1, "truncated u8");
		return data[offset++];
	}

	std::uint16_t u16()
	{
		std::uint16_t value = 0;
		for (unsigned index = 0; index < 2; ++index)
			value |= static_cast<std::uint16_t>(u8()) << (index * 8);
		return value;
	}

	std::uint32_t u32()
	{
		std::uint32_t value = 0;
		for (unsigned index = 0; index < 4; ++index)
			value |= static_cast<std::uint32_t>(u8()) << (index * 8);
		return value;
	}

	std::uint64_t u64()
	{
		std::uint64_t value = 0;
		for (unsigned index = 0; index < 8; ++index)
			value |= static_cast<std::uint64_t>(u8()) << (index * 8);
		return value;
	}

	const std::uint8_t* take(std::size_t count)
	{
		require(count <= remaining(), "truncated byte range");
		const auto* result = data + offset;
		offset += count;
		return result;
	}

	void skip(std::size_t count) { take(count); }
	std::size_t remaining() const { return size - offset; }
	std::size_t position() const { return offset; }

private:
	const std::uint8_t* data;
	std::size_t size;
	std::size_t offset = 0;
};

bool digestEqual(const Sha256Digest& lhs, const Sha256Digest& rhs)
{
	return std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

Sha256Digest digest(Reader& reader)
{
	Sha256Digest result;
	const auto* bytes = reader.take(result.size());
	std::copy_n(bytes, result.size(), result.begin());
	return result;
}

bool validBackend(std::uint32_t backend)
{
	return backend == static_cast<std::uint32_t>(Sh4ObservationBackend::Interpreter)
			|| backend == static_cast<std::uint32_t>(Sh4ObservationBackend::Dynarec);
}

bool validType(std::uint32_t type)
{
	return type >= static_cast<std::uint32_t>(
			PvrPresentationObservationType::RegisterWrite)
			&& type <= static_cast<std::uint32_t>(
					PvrPresentationObservationType::Reset);
}

bool sourceRequiresOwner(PvrVramWriteSource source)
{
	return source != PvrVramWriteSource::RendererRtt
			&& source != PvrVramWriteSource::RendererFramebuffer
			&& source != PvrVramWriteSource::Naomi2Elan;
}

bool readOwner(Reader& reader, Sh4ObservationBackend backend,
		std::uint64_t eventTick)
{
	const std::size_t start = reader.position();
	const bool valid = reader.u8() != 0;
	const std::uint8_t ownerBackend = reader.u8();
	const std::uint16_t depth = reader.u16();
	const std::uint16_t opcode = reader.u16();
	require(reader.u16() == 0, "owner reserved field is nonzero");
	const std::uint64_t generation = reader.u64();
	const std::uint64_t tick = reader.u64();
	const std::uint32_t pc = reader.u32();
	reader.u32(); // PR
	require(reader.position() - start == OwnerSize, "owner size mismatch");
	if (!valid)
	{
		require(ownerBackend == 0 && depth == 0 && opcode == 0
				&& generation == 0 && tick == 0 && pc == 0,
				"invalid owner sentinel is not zero");
		return false;
	}
	require(ownerBackend == static_cast<std::uint8_t>(backend),
			"owner backend differs from artifact");
	require(generation != 0 && (pc & 1u) == 0 && tick <= eventTick,
			"owner identity is invalid");
	return true;
}

std::vector<std::uint8_t> readFile(const std::filesystem::path& path,
		std::uint64_t maximumBytes)
{
	std::error_code error;
	const auto length = std::filesystem::file_size(path, error);
	if (error)
		throw std::system_error(error, "cannot stat PowerVR presentation artifact");
	if (length < PvrPresentationArtifactHeaderSize || length > maximumBytes)
		invalid("file size is outside limits");
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
	std::ifstream input(path, std::ios::binary);
	require(input.good(), "cannot open file");
	input.read(reinterpret_cast<char*>(bytes.data()),
			static_cast<std::streamsize>(bytes.size()));
	require(input.good() || input.eof(), "cannot read file");
	require(static_cast<std::size_t>(input.gcount()) == bytes.size(),
			"short file read");
	return bytes;
}

} // namespace

PvrPresentationArtifactSummary validatePvrPresentationArtifactFile(
		const std::filesystem::path& path,
		const PvrPresentationArtifactBinding& expectedBinding,
		std::uint64_t maximumBytes, std::uint64_t maximumEvents,
		std::vector<PvrValidatedFramebuffer>* framebuffers)
{
	const auto bytes = readFile(path, maximumBytes);
	Reader header(bytes.data(), PvrPresentationArtifactHeaderSize);
	require(std::equal(ArtifactMagic.begin(), ArtifactMagic.end(), header.take(8)),
			"magic mismatch");
	require(header.u32() == PvrPresentationArtifactSchemaVersion,
			"schema version mismatch");
	require(header.u32() == PvrPresentationArtifactHeaderSize,
			"header size mismatch");
	require(header.u32() == PvrPresentationArtifactEndianSentinel,
			"endian sentinel mismatch");
	require(header.u32() == HeaderComplete, "artifact is incomplete");
	const std::uint32_t backendValue = header.u32();
	require(validBackend(backendValue), "backend is invalid");
	require(header.u32() == 0, "header reserved field is nonzero");
	const auto backend = static_cast<Sh4ObservationBackend>(backendValue);
	require(backend == expectedBinding.backend, "backend binding mismatch");

	PvrPresentationArtifactSummary summary;
	summary.binding.backend = backend;
	summary.eventCount = header.u64();
	require(summary.eventCount != 0 && summary.eventCount <= maximumEvents,
			"event count is outside limits");
	for (auto& count : summary.typeCounts)
		count = header.u64();
	summary.payloadBytes = header.u64();
	summary.droppedEvents = header.u64();
	summary.startTick = header.u64();
	summary.endTick = header.u64();
	summary.firstEmissionOrdinal = header.u64();
	summary.lastEmissionOrdinal = header.u64();
	summary.binding.identityDigest = digest(header);
	summary.binding.replayDigest = digest(header);
	summary.payloadDigest = digest(header);
	require(digestEqual(summary.binding.identityDigest,
			expectedBinding.identityDigest), "identity digest mismatch");
	require(digestEqual(summary.binding.replayDigest,
			expectedBinding.replayDigest), "replay digest mismatch");
	require(summary.droppedEvents == 0, "artifact reports dropped events");
	header.skip(header.remaining() - 4);
	const std::uint32_t storedCrc = header.u32();
	require(storedCrc == crc32(bytes.data(),
			PvrPresentationArtifactHeaderSize - 4), "header CRC mismatch");
	require(summary.payloadBytes == bytes.size()
			- PvrPresentationArtifactHeaderSize, "payload size mismatch");
	require(digestEqual(summary.payloadDigest,
			sha256(bytes.data() + PvrPresentationArtifactHeaderSize,
					summary.payloadBytes)), "payload digest mismatch");

	Reader payload(bytes.data() + PvrPresentationArtifactHeaderSize,
			static_cast<std::size_t>(summary.payloadBytes));
	std::array<std::uint64_t, 7> observedCounts {};
	std::unordered_set<std::uint64_t> queuedRenders;
	std::unordered_set<std::uint64_t> completedScreenRenders;
	std::unordered_set<std::uint64_t> framebufferGenerations;
	std::unordered_set<std::uint64_t> queuedFramebuffers;
	std::unordered_set<std::uint64_t> completedFramebuffers;
	std::unordered_set<std::uint64_t> capturedPresentedFramebuffers;
	std::unordered_set<std::uint64_t> presentations;
	std::unordered_set<std::uint64_t> referencedRendererWrites;
	std::unordered_set<std::uint64_t> registerRenderReferences;
	std::uint64_t previousEmission = 0;
	bool hasPreviousEmission = false;

	for (std::uint64_t ordinal = 0; ordinal < summary.eventCount; ++ordinal)
	{
		require(payload.remaining() >= EventHeaderSize, "truncated event header");
		const std::uint32_t typeValue = payload.u32();
		require(validType(typeValue), "event type is invalid");
		const auto type = static_cast<PvrPresentationObservationType>(typeValue);
		const std::uint32_t eventSize = payload.u32();
		require(eventSize >= EventHeaderSize + OwnerSize
				&& eventSize - EventHeaderSize <= payload.remaining(),
				"event size is invalid");
		require(payload.u64() == ordinal, "artifact ordinal is not contiguous");
		const std::uint64_t emission = payload.u64();
		const std::uint64_t tick = payload.u64();
		if (hasPreviousEmission)
			require(emission > previousEmission, "emission ordinal is not increasing");
		previousEmission = emission;
		hasPreviousEmission = true;
		Reader event(payload.take(eventSize - EventHeaderSize),
				eventSize - EventHeaderSize);
		const bool owner = readOwner(event, backend, tick);
		++observedCounts[static_cast<std::size_t>(type) - 1];

		switch (type)
		{
		case PvrPresentationObservationType::RegisterWrite:
			event.u32();
			require(event.u32() != UINT32_MAX, "register address is unavailable");
			event.u32();
			event.u32();
			event.u32();
			{
				const std::uint8_t disposition = event.u8();
				require(disposition >= 1 && disposition <= 5,
						"register disposition is invalid");
			}
			event.skip(7);
			{
				const std::uint64_t generation = event.u64();
				if (generation != 0)
					registerRenderReferences.insert(generation);
			}
			break;
		case PvrPresentationObservationType::VramWrite:
			{
				const auto source = static_cast<PvrVramWriteSource>(event.u8());
				require(source >= PvrVramWriteSource::Sh4Area1Direct
						&& source <= PvrVramWriteSource::Naomi2Elan,
						"VRAM source is invalid");
				require(sourceRequiresOwner(source) == owner,
						"VRAM source ownership is invalid");
				event.skip(3);
				event.u32();
				const std::uint32_t physicalAddress = event.u32();
				const std::uint32_t length = event.u32();
				const std::uint64_t generation = event.u64();
				require(length != 0 && physicalAddress < VramSize
						&& length <= VramSize - physicalAddress,
						"VRAM range is invalid");
				event.skip(length);
				if (source == PvrVramWriteSource::RendererRtt
						|| source == PvrVramWriteSource::RendererFramebuffer)
				{
					require(generation != 0,
							"renderer VRAM write has no generation");
					referencedRendererWrites.insert(generation);
				}
				else
					require(generation == 0,
							"non-renderer VRAM write has a render generation");
			}
			break;
		case PvrPresentationObservationType::RenderQueued:
		case PvrPresentationObservationType::RenderCompleted:
			{
				require(!owner, "renderer lifecycle has an SH-4 owner");
				const std::uint64_t generation = event.u64();
				const auto kind = static_cast<PvrRenderKind>(event.u8());
				const bool successful = event.u8() != 0;
				require(kind >= PvrRenderKind::Screen
						&& kind <= PvrRenderKind::DirectFramebuffer,
						"render kind is invalid");
				require(event.u16() == 0, "render reserved field is nonzero");
				event.u32();
				if (type == PvrPresentationObservationType::RenderQueued)
				{
					require(successful, "queued render is not successful");
					if (kind == PvrRenderKind::DirectFramebuffer)
					{
						require(framebufferGenerations.count(generation) == 1,
								"direct framebuffer queued before capture");
						require(queuedFramebuffers.insert(generation).second,
								"duplicate framebuffer queue");
					}
					else
						require(queuedRenders.insert(generation).second,
								"duplicate render queue");
				}
				else if (kind == PvrRenderKind::DirectFramebuffer)
				{
					require(queuedFramebuffers.count(generation) == 1,
							"framebuffer completed without queue");
					if (successful)
						completedFramebuffers.insert(generation);
				}
				else
				{
					require(queuedRenders.count(generation) == 1,
							"render completed without queue");
					if (successful && kind == PvrRenderKind::Screen)
						completedScreenRenders.insert(generation);
				}
			}
			break;
		case PvrPresentationObservationType::FramebufferCaptured:
			{
				require(!owner, "framebuffer capture has an SH-4 owner");
				const std::uint64_t generation = event.u64();
				const std::uint64_t sourceRenderGeneration = event.u64();
				const auto framebufferKind =
						static_cast<PvrFramebufferKind>(event.u8());
				event.skip(7);
				const std::uint32_t width = event.u32();
				const std::uint32_t height = event.u32();
				const std::uint32_t rowBytes = event.u32();
				const std::uint32_t length = event.u32();
				PvrFramebufferConfig config;
				config.fbReadSize = event.u32();
				config.fbReadControl = event.u32();
				config.spgControl = event.u32();
				config.spgStatus = event.u32();
				config.fbReadSof1 = event.u32();
				config.fbReadSof2 = event.u32();
				config.videoControl = event.u32();
				config.borderColor = event.u32();
				const auto* raw = event.take(length);
				auto decoded = decodePvrFramebuffer(config, width, height,
						rowBytes, raw, length, framebufferKind);
				if (framebufferKind == PvrFramebufferKind::PresentedRgb24)
				{
					require(completedScreenRenders.count(sourceRenderGeneration) == 1,
							"presented framebuffer references incomplete screen render");
					capturedPresentedFramebuffers.insert(generation);
				}
				else
					require(framebufferKind == PvrFramebufferKind::DreamcastVram
							&& sourceRenderGeneration == 0,
							"Dreamcast framebuffer has an invalid source render");
				if (framebuffers != nullptr)
				{
					PvrValidatedFramebuffer framebuffer;
					framebuffer.generation = generation;
					framebuffer.sourceRenderGeneration = sourceRenderGeneration;
					framebuffer.kind = framebufferKind;
					framebuffer.config = config;
					framebuffer.rowBytes = rowBytes;
					framebuffer.rawBytes.assign(raw, raw + length);
					framebuffer.decoded = std::move(decoded);
					framebuffers->push_back(std::move(framebuffer));
				}
				++summary.decodedFramebufferCount;
				require(generation != 0
						&& framebufferGenerations.insert(generation).second,
						"duplicate framebuffer generation");
			}
			break;
		case PvrPresentationObservationType::Presentation:
			{
				require(!owner, "presentation has an SH-4 owner");
				const std::uint64_t generation = event.u64();
				const auto source = static_cast<PvrPresentationSource>(event.u8());
				const bool successful = event.u8() != 0;
				require(event.u16() == 0 && event.u32() == 0,
						"presentation reserved field is nonzero");
				const std::uint64_t sourceGeneration = event.u64();
				require(generation != 0 && presentations.insert(generation).second,
						"duplicate presentation generation");
				if (source == PvrPresentationSource::Render)
					require(completedScreenRenders.count(sourceGeneration) == 1,
							"presentation references incomplete screen render");
				else if (source == PvrPresentationSource::Framebuffer)
					require(completedFramebuffers.count(sourceGeneration) == 1
							|| capturedPresentedFramebuffers.count(sourceGeneration) == 1,
							"presentation references incomplete framebuffer");
				else
					invalid("presentation source is invalid");
				require(successful, "presentation was unsuccessful");
			}
			break;
		case PvrPresentationObservationType::Reset:
			require(!owner && event.remaining() == 0, "reset payload is invalid");
			break;
		}
		require(event.remaining() == 0, "event has trailing bytes");
	}

	require(payload.remaining() == 0, "payload has trailing bytes");
	require(observedCounts == summary.typeCounts, "type counts differ from header");
	require(summary.firstEmissionOrdinal <= summary.lastEmissionOrdinal,
			"emission ordinal range is invalid");
	require(summary.startTick <= summary.endTick, "tick range is invalid");
	for (const auto generation : referencedRendererWrites)
		require(queuedRenders.count(generation) == 1,
				"renderer VRAM write references unknown render");
	for (const auto generation : registerRenderReferences)
		require(queuedRenders.count(generation) == 1,
				"register write references unknown render");
	summary.completeVerticalSlice = summary.typeCounts[0] != 0
			&& summary.typeCounts[1] != 0 && summary.typeCounts[2] != 0
			&& summary.typeCounts[3] != 0 && summary.typeCounts[4] != 0
			&& summary.typeCounts[5] != 0 && summary.decodedFramebufferCount != 0;
	return summary;
}

} // namespace research

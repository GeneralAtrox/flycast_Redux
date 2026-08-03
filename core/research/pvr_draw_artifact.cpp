#include "research/pvr_draw_artifact.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace research
{
namespace
{

constexpr std::array<std::uint8_t, 8> ArtifactMagic {
	'F', 'C', 'P', 'V', 'R', 'D', 'R', '1',
};
constexpr std::uint32_t HeaderComplete = 1u;
constexpr std::uint32_t EventHeaderSize = 32;
constexpr std::uint32_t BlockProvenanceSize = 72;

void require(bool condition, const char* reason)
{
	if (!condition)
		throw std::runtime_error(std::string("PowerVR draw artifact: ") + reason);
}

void appendU8(std::vector<std::uint8_t>& bytes, std::uint8_t value)
{
	bytes.push_back(value);
}

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
	appendU8(bytes, static_cast<std::uint8_t>(value));
	appendU8(bytes, static_cast<std::uint8_t>(value >> 8));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
	for (unsigned shift = 0; shift < 32; shift += 8)
		appendU8(bytes, static_cast<std::uint8_t>(value >> shift));
}

void appendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
	for (unsigned shift = 0; shift < 64; shift += 8)
		appendU8(bytes, static_cast<std::uint8_t>(value >> shift));
}

void appendFloat(std::vector<std::uint8_t>& bytes, float value)
{
	std::uint32_t bits = 0;
	static_assert(sizeof(bits) == sizeof(value));
	std::memcpy(&bits, &value, sizeof(bits));
	appendU32(bytes, bits);
}

void writeU32(std::vector<std::uint8_t>& bytes, std::size_t offset,
		std::uint32_t value)
{
	for (unsigned shift = 0; shift < 32; shift += 8)
		bytes[offset++] = static_cast<std::uint8_t>(value >> shift);
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

void appendDigest(std::vector<std::uint8_t>& bytes,
		const Sha256Digest& digest)
{
	bytes.insert(bytes.end(), digest.begin(), digest.end());
}

void appendOwner(std::vector<std::uint8_t>& bytes,
		const Sh4InstructionOwnerToken& owner)
{
	require(owner.valid, "block provenance has no instruction owner");
	appendU8(bytes, 1);
	appendU8(bytes, static_cast<std::uint8_t>(owner.backend));
	appendU16(bytes, owner.delaySlotDepth);
	appendU16(bytes, owner.opcode);
	appendU16(bytes, 0);
	appendU64(bytes, owner.generation);
	appendU64(bytes, owner.tick);
	appendU32(bytes, owner.pc);
	appendU32(bytes, owner.pr);
}

void appendBlock(std::vector<std::uint8_t>& bytes,
		const PvrTaBlockProvenance& block)
{
	const std::size_t start = bytes.size();
	require(block.available, "primitive contains unavailable block provenance");
	appendOwner(bytes, block.initiator);
	appendU32(bytes, block.contextAddress);
	appendU64(bytes, block.contextGeneration);
	appendU64(bytes, block.contextBlockOrdinal);
	appendU32(bytes, block.renderPass);
	appendU32(bytes, static_cast<std::uint32_t>(block.source));
	appendU32(bytes, block.sourceAddress);
	appendU32(bytes, block.taAddress);
	appendU32(bytes, 0);
	require(bytes.size() - start == BlockProvenanceSize,
			"internal block provenance size mismatch");
}

void appendEventHeader(std::vector<std::uint8_t>& bytes,
		const PvrDrawObservation& observation, std::uint64_t artifactOrdinal,
		std::uint32_t eventSize)
{
	appendU32(bytes, static_cast<std::uint32_t>(observation.type));
	appendU32(bytes, eventSize);
	appendU64(bytes, artifactOrdinal);
	appendU64(bytes, observation.emissionOrdinal);
	appendU64(bytes, observation.tick);
}

std::vector<std::uint8_t> serializeObservation(
		const PvrDrawObservation& observation, std::uint64_t artifactOrdinal)
{
	require(observation.schemaVersion == PvrDrawObservationSchemaVersion,
			"observation schema mismatch");
	std::vector<std::uint8_t> body;
	switch (observation.type)
	{
	case PvrDrawObservationType::PrimitiveDecoded:
		require(observation.parameterBlocks.size()
				<= MaximumPvrDrawBlocksPerPrimitive
				&& observation.vertexBlocks.size()
						<= MaximumPvrDrawBlocksPerPrimitive,
				"primitive block count exceeds the schema limit");
		appendU64(body, observation.renderGeneration);
		appendU64(body, observation.primitiveGeneration);
		appendU32(body, observation.contextAddress);
		appendU64(body, observation.contextGeneration);
		appendU32(body, observation.renderPass);
		appendU32(body, observation.listType);
		appendU32(body, static_cast<std::uint32_t>(observation.primitiveKind));
		appendU32(body, static_cast<std::uint32_t>(observation.ownerClass));
		appendU32(body, observation.pcw);
		appendU32(body, observation.isp);
		appendU32(body, observation.tsp);
		appendU32(body, observation.tcw);
		appendU32(body, observation.tsp1);
		appendU32(body, observation.tcw1);
		appendU32(body, observation.tileClip);
		appendU32(body, observation.first);
		appendU32(body, observation.count);
		appendFloat(body, observation.bounds.minimumX);
		appendFloat(body, observation.bounds.minimumY);
		appendFloat(body, observation.bounds.minimumZ);
		appendFloat(body, observation.bounds.maximumX);
		appendFloat(body, observation.bounds.maximumY);
		appendFloat(body, observation.bounds.maximumZ);
		appendU32(body, observation.bounds.available ? 1u : 0u);
		appendU32(body, static_cast<std::uint32_t>(
				observation.parameterBlocks.size()));
		appendU32(body, static_cast<std::uint32_t>(
				observation.vertexBlocks.size()));
		appendU32(body, 0);
		for (const auto& block : observation.parameterBlocks)
			appendBlock(body, block);
		for (const auto& block : observation.vertexBlocks)
			appendBlock(body, block);
		break;
	case PvrDrawObservationType::DrawConsumed:
		require(observation.primitiveGenerations.size()
				<= MaximumPvrDrawPrimitiveRefs,
				"draw primitive-reference count exceeds the schema limit");
		appendU64(body, observation.renderGeneration);
		appendU64(body, observation.rasterGeneration);
		appendU32(body, static_cast<std::uint32_t>(observation.backend));
		appendU32(body, static_cast<std::uint32_t>(observation.drawPass));
		appendU32(body, observation.first);
		appendU32(body, observation.count);
		appendU32(body, observation.indexed ? 1u : 0u);
		appendU32(body, static_cast<std::uint32_t>(
				observation.primitiveGenerations.size()));
		appendU32(body, 0);
		for (const std::uint64_t generation : observation.primitiveGenerations)
			appendU64(body, generation);
		break;
	case PvrDrawObservationType::RenderCompleted:
		appendU64(body, observation.renderGeneration);
		appendU32(body, observation.successful ? 1u : 0u);
		appendU32(body, 0);
		break;
	case PvrDrawObservationType::Reset:
		break;
	default:
		require(false, "observation type is invalid");
	}
	const std::uint64_t size = EventHeaderSize + body.size();
	require(size <= std::numeric_limits<std::uint32_t>::max(),
			"event size overflows the format");
	std::vector<std::uint8_t> result;
	result.reserve(static_cast<std::size_t>(size));
	appendEventHeader(result, observation, artifactOrdinal,
			static_cast<std::uint32_t>(size));
	result.insert(result.end(), body.begin(), body.end());
	return result;
}

std::vector<std::uint8_t> serializeHeader(const PvrDrawArtifactSummary& summary,
		bool complete)
{
	std::vector<std::uint8_t> bytes;
	bytes.insert(bytes.end(), ArtifactMagic.begin(), ArtifactMagic.end());
	appendU32(bytes, PvrDrawArtifactSchemaVersion);
	appendU32(bytes, PvrDrawArtifactHeaderSize);
	appendU32(bytes, PvrDrawArtifactEndianSentinel);
	appendU32(bytes, complete ? HeaderComplete : 0);
	appendU32(bytes, static_cast<std::uint32_t>(summary.binding.backend));
	appendU32(bytes, 0);
	appendU64(bytes, summary.eventCount);
	for (const auto count : summary.typeCounts)
		appendU64(bytes, count);
	appendU64(bytes, summary.payloadBytes);
	appendU64(bytes, summary.droppedEvents);
	appendU64(bytes, summary.startTick);
	appendU64(bytes, summary.endTick);
	appendU64(bytes, summary.firstEmissionOrdinal);
	appendU64(bytes, summary.lastEmissionOrdinal);
	appendDigest(bytes, summary.binding.identityDigest);
	appendDigest(bytes, summary.binding.replayDigest);
	appendDigest(bytes, summary.binding.taArtifactDigest);
	appendDigest(bytes, summary.binding.presentationArtifactDigest);
	appendDigest(bytes, summary.binding.rendererConfigurationDigest);
	appendDigest(bytes, summary.payloadDigest);
	appendU32(bytes, 0);
	appendU32(bytes, 0);
	require(bytes.size() == PvrDrawArtifactHeaderSize,
			"internal header size mismatch");
	writeU32(bytes, bytes.size() - 4, crc32(bytes.data(), bytes.size() - 4));
	return bytes;
}

} // namespace

class PvrDrawArtifactWriter::OutputFile
{
public:
	explicit OutputFile(const std::filesystem::path& path)
	{
#ifdef _WIN32
		handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			throw std::system_error(static_cast<int>(GetLastError()),
					std::system_category(), "cannot create PowerVR draw artifact");
#else
		fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot create PowerVR draw artifact");
#endif
	}

	~OutputFile() { close(); }

	void write(const void* data, std::size_t size)
	{
		const auto* bytes = static_cast<const std::uint8_t*>(data);
		while (size != 0)
		{
#ifdef _WIN32
			const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size,
					std::numeric_limits<DWORD>::max()));
			DWORD written = 0;
			if (!WriteFile(handle, bytes, chunk, &written, nullptr) || written != chunk)
				throw std::system_error(static_cast<int>(GetLastError()),
						std::system_category(), "cannot write PowerVR draw artifact");
#else
			const ssize_t written = ::write(fd, bytes, size);
			if (written <= 0)
				throw std::system_error(errno, std::generic_category(),
						"cannot write PowerVR draw artifact");
#endif
			bytes += written;
			size -= written;
		}
	}

	void seekStart()
	{
#ifdef _WIN32
		LARGE_INTEGER zero {};
		if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN))
			throw std::system_error(static_cast<int>(GetLastError()),
					std::system_category(), "cannot seek PowerVR draw artifact");
#else
		if (::lseek(fd, 0, SEEK_SET) != 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot seek PowerVR draw artifact");
#endif
	}

	void flush()
	{
#ifdef _WIN32
		if (!FlushFileBuffers(handle))
			throw std::system_error(static_cast<int>(GetLastError()),
					std::system_category(), "cannot flush PowerVR draw artifact");
#else
		if (::fsync(fd) != 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot flush PowerVR draw artifact");
#endif
	}

	void close() noexcept
	{
#ifdef _WIN32
		if (handle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(handle);
			handle = INVALID_HANDLE_VALUE;
		}
#else
		if (fd >= 0)
		{
			::close(fd);
			fd = -1;
		}
#endif
	}

private:
#ifdef _WIN32
	HANDLE handle = INVALID_HANDLE_VALUE;
#else
	int fd = -1;
#endif
};

PvrDrawArtifactWriter::PvrDrawArtifactWriter(
		const std::filesystem::path& path,
		const PvrDrawArtifactBinding& binding, std::uint64_t maximumBytes,
		std::uint64_t maximumEvents)
		: path(path), maximumBytes(maximumBytes), maximumEvents(maximumEvents)
{
	if (maximumBytes < PvrDrawArtifactHeaderSize || maximumEvents == 0)
		throw std::invalid_argument("invalid PowerVR draw artifact limits");
	summary.binding = binding;
	output = new OutputFile(path);
	const auto header = serializeHeader(summary, false);
	output->write(header.data(), header.size());
	output->flush();
}

PvrDrawArtifactWriter::~PvrDrawArtifactWriter()
{
	if (!finalized)
		abandon();
	delete output;
}

void PvrDrawArtifactWriter::write(const PvrDrawObservation& observation)
{
	if (finalized || abandoned)
		throw std::logic_error("PowerVR draw artifact is not writable");
	if (summary.eventCount == maximumEvents)
		throw std::length_error("PowerVR draw artifact event limit exceeded");
	const auto event = serializeObservation(observation, summary.eventCount);
	if (event.size() > maximumBytes - PvrDrawArtifactHeaderSize
			|| payload.size() > maximumBytes - PvrDrawArtifactHeaderSize
					- event.size())
		throw std::length_error("PowerVR draw artifact byte limit exceeded");
	if (summary.eventCount == 0)
	{
		summary.startTick = observation.tick;
		summary.firstEmissionOrdinal = observation.emissionOrdinal;
	}
	summary.endTick = observation.tick;
	summary.lastEmissionOrdinal = observation.emissionOrdinal;
	++summary.typeCounts[static_cast<std::size_t>(observation.type) - 1];
	++summary.eventCount;
	payload.insert(payload.end(), event.begin(), event.end());
}

void PvrDrawArtifactWriter::bindLinkedArtifacts(
		const Sha256Digest& taArtifactDigest,
		const Sha256Digest& presentationArtifactDigest,
		const Sha256Digest& rendererConfigurationDigest)
{
	if (finalized || abandoned)
		throw std::logic_error("PowerVR draw artifact cannot be rebound");
	summary.binding.taArtifactDigest = taArtifactDigest;
	summary.binding.presentationArtifactDigest = presentationArtifactDigest;
	summary.binding.rendererConfigurationDigest = rendererConfigurationDigest;
}

PvrDrawArtifactSummary PvrDrawArtifactWriter::finalize(
		std::uint64_t droppedEvents)
{
	if (finalized || abandoned)
		throw std::logic_error("PowerVR draw artifact cannot be finalized");
	const Sha256Digest zeroDigest {};
	if (sha256Equal(summary.binding.taArtifactDigest, zeroDigest)
			|| sha256Equal(summary.binding.presentationArtifactDigest, zeroDigest)
			|| sha256Equal(summary.binding.rendererConfigurationDigest, zeroDigest))
		throw std::logic_error(
				"PowerVR draw artifact linked digests are not bound");
	summary.droppedEvents = droppedEvents;
	if (droppedEvents != 0)
		throw std::runtime_error("PowerVR draw observation delivery was dropped");
	summary.payloadBytes = payload.size();
	summary.payloadDigest = sha256(payload.data(), payload.size());
	output->write(payload.data(), payload.size());
	output->flush();
	const auto header = serializeHeader(summary, true);
	output->seekStart();
	output->write(header.data(), header.size());
	output->flush();
	output->close();
	finalized = true;
	return summary;
}

void PvrDrawArtifactWriter::abandon() noexcept
{
	if (abandoned || finalized)
		return;
	abandoned = true;
	if (output != nullptr)
		output->close();
}

} // namespace research

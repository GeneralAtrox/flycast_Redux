#include "research/pvr_presentation_artifact.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

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

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error(
			"invalid PowerVR presentation artifact observation: " + reason);
}

void require(bool condition, const std::string& reason)
{
	if (!condition)
		invalid(reason);
}

void appendU8(std::vector<std::uint8_t>& bytes, std::uint8_t value)
{
	bytes.push_back(value);
}

void appendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
	bytes.push_back(static_cast<std::uint8_t>(value));
	bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}

void appendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
	for (unsigned index = 0; index < 4; ++index)
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

void appendU64(std::vector<std::uint8_t>& bytes, std::uint64_t value)
{
	for (unsigned index = 0; index < 8; ++index)
		bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8)));
}

void writeU32(std::vector<std::uint8_t>& bytes, std::size_t offset,
		std::uint32_t value)
{
	require(offset <= bytes.size() && bytes.size() - offset >= 4,
			"internal header offset is out of range");
	for (unsigned index = 0; index < 4; ++index)
		bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
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

bool validBackend(Sh4ObservationBackend backend)
{
	return backend == Sh4ObservationBackend::Interpreter
			|| backend == Sh4ObservationBackend::Dynarec;
}

bool validType(PvrPresentationObservationType type)
{
	return type >= PvrPresentationObservationType::RegisterWrite
			&& type <= PvrPresentationObservationType::InitialRegisterState;
}

std::size_t typeIndex(PvrPresentationObservationType type)
{
	return static_cast<std::size_t>(type) - 1;
}

bool sourceRequiresOwner(PvrVramWriteSource source)
{
	return source != PvrVramWriteSource::RendererRtt
			&& source != PvrVramWriteSource::RendererFramebuffer
			&& source != PvrVramWriteSource::Naomi2Elan;
}

void appendOwner(std::vector<std::uint8_t>& bytes,
		const Sh4InstructionOwnerToken& owner,
		Sh4ObservationBackend expectedBackend, std::uint64_t eventTick)
{
	const std::size_t start = bytes.size();
	if (!owner.valid)
	{
		bytes.insert(bytes.end(), OwnerSize, 0);
		return;
	}
	require(owner.backend == expectedBackend, "owner backend differs from binding");
	require(owner.generation != 0, "owner generation is zero");
	require((owner.pc & 1u) == 0, "owner PC is not aligned");
	require(owner.tick <= eventTick, "owner tick follows event");
	appendU8(bytes, 1);
	appendU8(bytes, static_cast<std::uint8_t>(owner.backend));
	appendU16(bytes, owner.delaySlotDepth);
	appendU16(bytes, owner.opcode);
	appendU16(bytes, 0);
	appendU64(bytes, owner.generation);
	appendU64(bytes, owner.tick);
	appendU32(bytes, owner.pc);
	appendU32(bytes, owner.pr);
	require(bytes.size() - start == OwnerSize, "internal owner size mismatch");
}

std::vector<std::uint8_t> serializeObservation(
		const PvrPresentationObservation& observation,
		const PvrPresentationArtifactBinding& binding,
		std::uint64_t artifactOrdinal)
{
	require(observation.schemaVersion == PvrPresentationObservationSchemaVersion,
			"observation schema mismatch");
	require(validType(observation.type), "observation type is invalid");

	std::vector<std::uint8_t> body;
	appendOwner(body, observation.initiator, binding.backend, observation.tick);
	switch (observation.type)
	{
	case PvrPresentationObservationType::RegisterWrite:
		require(observation.registerAddress != UINT32_MAX,
				"register address is unavailable");
		appendU32(body, observation.registerPhysicalAddress);
		appendU32(body, observation.registerAddress);
		appendU32(body, observation.requestedValue);
		appendU32(body, observation.previousValue);
		appendU32(body, observation.effectiveValue);
		appendU8(body, static_cast<std::uint8_t>(observation.registerDisposition));
		body.insert(body.end(), 7, 0);
		appendU64(body, observation.renderGeneration);
		break;
	case PvrPresentationObservationType::VramWrite:
		require(!observation.bytes.empty(), "VRAM write is empty");
		require(sourceRequiresOwner(observation.vramSource)
				== observation.initiator.valid,
				"VRAM source ownership is invalid");
		appendU8(body, static_cast<std::uint8_t>(observation.vramSource));
		body.insert(body.end(), 3, 0);
		appendU32(body, observation.logicalAddress);
		appendU32(body, observation.physicalAddress);
		appendU32(body, static_cast<std::uint32_t>(observation.bytes.size()));
		appendU64(body, observation.renderGeneration);
		body.insert(body.end(), observation.bytes.begin(), observation.bytes.end());
		break;
	case PvrPresentationObservationType::RenderQueued:
	case PvrPresentationObservationType::RenderCompleted:
		require(!observation.initiator.valid, "renderer lifecycle has an SH-4 owner");
		require(observation.renderGeneration != 0, "renderer generation is zero");
		appendU64(body, observation.renderGeneration);
		appendU8(body, static_cast<std::uint8_t>(observation.renderKind));
		appendU8(body, observation.successful ? 1 : 0);
		appendU16(body, 0);
		appendU32(body, observation.framebufferWriteAddress);
		break;
	case PvrPresentationObservationType::FramebufferCaptured:
		require(!observation.initiator.valid, "framebuffer capture has an SH-4 owner");
		require(observation.framebufferGeneration != 0,
				"framebuffer generation is zero");
		require(!observation.bytes.empty(), "framebuffer capture is empty");
		appendU64(body, observation.framebufferGeneration);
		appendU64(body, observation.framebufferSourceRenderGeneration);
		appendU8(body, static_cast<std::uint8_t>(observation.framebufferKind));
		body.insert(body.end(), 7, 0);
		appendU32(body, observation.framebufferWidth);
		appendU32(body, observation.framebufferHeight);
		appendU32(body, observation.framebufferRowBytes);
		appendU32(body, static_cast<std::uint32_t>(observation.bytes.size()));
		appendU32(body, observation.framebufferConfig.fbReadSize);
		appendU32(body, observation.framebufferConfig.fbReadControl);
		appendU32(body, observation.framebufferConfig.spgControl);
		appendU32(body, observation.framebufferConfig.spgStatus);
		appendU32(body, observation.framebufferConfig.fbReadSof1);
		appendU32(body, observation.framebufferConfig.fbReadSof2);
		appendU32(body, observation.framebufferConfig.videoControl);
		appendU32(body, observation.framebufferConfig.borderColor);
		body.insert(body.end(), observation.bytes.begin(), observation.bytes.end());
		break;
	case PvrPresentationObservationType::Presentation:
		require(!observation.initiator.valid, "presentation has an SH-4 owner");
		require(observation.presentationGeneration != 0
				&& observation.sourceGeneration != 0,
				"presentation generation is zero");
		appendU64(body, observation.presentationGeneration);
		appendU8(body, static_cast<std::uint8_t>(observation.presentationSource));
		appendU8(body, observation.successful ? 1 : 0);
		appendU16(body, 0);
		appendU32(body, 0);
		appendU64(body, observation.sourceGeneration);
		break;
	case PvrPresentationObservationType::Reset:
		require(!observation.initiator.valid, "reset has an SH-4 owner");
		break;
	case PvrPresentationObservationType::InitialRegisterState:
		require(!observation.initiator.valid,
				"initial register state has an SH-4 owner");
		require(observation.bytes.size() == 0x8000,
				"initial register state size differs");
		appendU32(body, static_cast<std::uint32_t>(observation.bytes.size()));
		appendU32(body, 0);
		appendU64(body, observation.renderGeneration);
		body.insert(body.end(), observation.bytes.begin(), observation.bytes.end());
		break;
	}

	const std::uint64_t totalSize = static_cast<std::uint64_t>(EventHeaderSize)
			+ body.size();
	require(totalSize <= std::numeric_limits<std::uint32_t>::max(),
			"event is too large");
	std::vector<std::uint8_t> result;
	result.reserve(static_cast<std::size_t>(totalSize));
	appendU32(result, static_cast<std::uint32_t>(observation.type));
	appendU32(result, static_cast<std::uint32_t>(totalSize));
	appendU64(result, artifactOrdinal);
	appendU64(result, observation.emissionOrdinal);
	appendU64(result, observation.tick);
	result.insert(result.end(), body.begin(), body.end());
	return result;
}

std::vector<std::uint8_t> serializeHeader(
		const PvrPresentationArtifactSummary& summary)
{
	std::vector<std::uint8_t> bytes;
	bytes.reserve(PvrPresentationArtifactHeaderSize);
	bytes.insert(bytes.end(), ArtifactMagic.begin(), ArtifactMagic.end());
	appendU32(bytes, PvrPresentationArtifactSchemaVersion);
	appendU32(bytes, PvrPresentationArtifactHeaderSize);
	appendU32(bytes, PvrPresentationArtifactEndianSentinel);
	appendU32(bytes, HeaderComplete);
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
	bytes.insert(bytes.end(), summary.binding.identityDigest.begin(),
			summary.binding.identityDigest.end());
	bytes.insert(bytes.end(), summary.binding.replayDigest.begin(),
			summary.binding.replayDigest.end());
	bytes.insert(bytes.end(), summary.payloadDigest.begin(),
			summary.payloadDigest.end());
	while (bytes.size() < PvrPresentationArtifactHeaderSize - 4)
		bytes.push_back(0);
	appendU32(bytes, 0);
	require(bytes.size() == PvrPresentationArtifactHeaderSize,
			"internal header size mismatch");
	writeU32(bytes, bytes.size() - 4, crc32(bytes.data(), bytes.size() - 4));
	return bytes;
}

void writeExclusive(const std::filesystem::path& path,
		const std::vector<std::uint8_t>& bytes)
{
	if (!path.has_parent_path() || !std::filesystem::is_directory(path.parent_path()))
		throw std::runtime_error("PowerVR presentation output parent is not a directory");
#ifdef _WIN32
	HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
	if (file == INVALID_HANDLE_VALUE)
		throw std::system_error(static_cast<int>(GetLastError()),
				std::system_category(), "cannot exclusively create presentation artifact");
	try
	{
		std::size_t offset = 0;
		while (offset < bytes.size())
		{
			const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
					bytes.size() - offset, std::numeric_limits<DWORD>::max()));
			DWORD written = 0;
			if (!WriteFile(file, bytes.data() + offset, chunk, &written, nullptr)
					|| written != chunk)
				throw std::system_error(static_cast<int>(GetLastError()),
						std::system_category(), "cannot write presentation artifact");
			offset += written;
		}
		if (!FlushFileBuffers(file))
			throw std::system_error(static_cast<int>(GetLastError()),
					std::system_category(), "cannot flush presentation artifact");
		CloseHandle(file);
	}
	catch (...)
	{
		CloseHandle(file);
		std::error_code ignored;
		std::filesystem::remove(path, ignored);
		throw;
	}
#else
	const int file = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (file < 0)
		throw std::system_error(errno, std::generic_category(),
				"cannot exclusively create presentation artifact");
	std::size_t offset = 0;
	while (offset < bytes.size())
	{
		const ssize_t written = ::write(file, bytes.data() + offset,
				bytes.size() - offset);
		if (written <= 0)
		{
			const int error = errno;
			::close(file);
			::unlink(path.c_str());
			throw std::system_error(error, std::generic_category(),
					"cannot write presentation artifact");
		}
		offset += static_cast<std::size_t>(written);
	}
	if (::fsync(file) != 0 || ::close(file) != 0)
	{
		const int error = errno;
		::unlink(path.c_str());
		throw std::system_error(error, std::generic_category(),
				"cannot flush presentation artifact");
	}
#endif
}

} // namespace

PvrPresentationArtifactWriter::PvrPresentationArtifactWriter(
		const std::filesystem::path& path,
		const PvrPresentationArtifactBinding& binding,
		std::uint64_t maximumBytes, std::uint64_t maximumEvents)
	: path(path), maximumBytes(maximumBytes), maximumEvents(maximumEvents)
{
	if (!validBackend(binding.backend))
		throw std::invalid_argument("PowerVR presentation backend is invalid");
	if (maximumBytes < PvrPresentationArtifactHeaderSize || maximumEvents == 0)
		throw std::invalid_argument("PowerVR presentation limits are invalid");
	if (std::filesystem::exists(path))
		throw std::runtime_error("PowerVR presentation output already exists");
	summary.binding = binding;
}

PvrPresentationArtifactWriter::~PvrPresentationArtifactWriter()
{
	if (!finalized)
		abandon();
}

void PvrPresentationArtifactWriter::write(
		const PvrPresentationObservation& observation)
{
	if (finalized || abandoned)
		throw std::logic_error("PowerVR presentation writer is closed");
	if (summary.eventCount >= maximumEvents)
		throw std::runtime_error("PowerVR presentation event limit exceeded");
	const auto event = serializeObservation(observation, summary.binding,
			summary.eventCount);
	if (event.size() > maximumBytes - PvrPresentationArtifactHeaderSize
			|| payload.size() > maximumBytes - PvrPresentationArtifactHeaderSize
					- event.size())
		throw std::runtime_error("PowerVR presentation byte limit exceeded");
	if (summary.eventCount == 0)
	{
		summary.startTick = observation.tick;
		summary.firstEmissionOrdinal = observation.emissionOrdinal;
	}
	summary.endTick = observation.tick;
	summary.lastEmissionOrdinal = observation.emissionOrdinal;
	++summary.typeCounts[typeIndex(observation.type)];
	++summary.eventCount;
	payload.insert(payload.end(), event.begin(), event.end());
}

PvrPresentationArtifactSummary PvrPresentationArtifactWriter::finalize(
		std::uint64_t droppedEvents)
{
	if (finalized || abandoned)
		throw std::logic_error("PowerVR presentation writer is closed");
	if (summary.eventCount == 0)
		throw std::logic_error("cannot finalize an empty presentation artifact");
	if (droppedEvents != 0)
		throw std::runtime_error("cannot finalize presentation artifact with drops");
	summary.droppedEvents = droppedEvents;
	summary.payloadBytes = payload.size();
	summary.payloadDigest = sha256(payload.data(), payload.size());
	const auto header = serializeHeader(summary);
	std::vector<std::uint8_t> file;
	file.reserve(header.size() + payload.size());
	file.insert(file.end(), header.begin(), header.end());
	file.insert(file.end(), payload.begin(), payload.end());
	writeExclusive(path, file);
	finalized = true;
	return summary;
}

void PvrPresentationArtifactWriter::abandon() noexcept
{
	abandoned = true;
	payload.clear();
}

} // namespace research

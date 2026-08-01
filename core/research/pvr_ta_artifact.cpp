#include "research/pvr_ta_artifact.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#endif

namespace research
{
namespace
{

constexpr std::array<std::uint8_t, 8> ArtifactMagic {
	'F', 'C', 'P', 'V', 'R', 'T', 'A', '1',
};
constexpr std::uint32_t HeaderComplete = 1u << 0;
constexpr std::uint32_t EventHeaderSize = 32;
constexpr std::uint32_t OwnerSize = 32;
constexpr std::size_t MaximumSelectedContexts = 10;

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid PowerVR TA artifact observation: " + reason);
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

bool synchronousType(PvrTaObservationType type)
{
	return type != PvrTaObservationType::RenderDone
			&& type != PvrTaObservationType::Reset;
}

std::size_t typeIndex(PvrTaObservationType type)
{
	return static_cast<std::size_t>(type) - 1;
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
	require(owner.tick <= eventTick, "owner tick follows its PowerVR event");
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

std::vector<std::uint8_t> serializeHeader(const PvrTaArtifactSummary& summary,
		bool complete)
{
	std::vector<std::uint8_t> bytes;
	bytes.reserve(PvrTaArtifactHeaderSize);
	bytes.insert(bytes.end(), ArtifactMagic.begin(), ArtifactMagic.end());
	appendU32(bytes, PvrTaArtifactSchemaVersion);
	appendU32(bytes, PvrTaArtifactHeaderSize);
	appendU32(bytes, PvrTaArtifactEndianSentinel);
	appendU32(bytes, complete ? HeaderComplete : 0);
	appendU32(bytes, static_cast<std::uint32_t>(summary.binding.backend));
	appendU32(bytes, 0);
	appendU64(bytes, summary.eventCount);
	for (const std::uint64_t count : summary.typeCounts)
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
	bytes.insert(bytes.end(), summary.binding.manifestDigest.begin(),
			summary.binding.manifestDigest.end());
	bytes.insert(bytes.end(), summary.payloadDigest.begin(),
			summary.payloadDigest.end());
	appendU32(bytes, 0);
	appendU32(bytes, 0);
	require(bytes.size() == PvrTaArtifactHeaderSize,
			"internal header size mismatch");
	writeU32(bytes, PvrTaArtifactHeaderSize - 4,
			crc32(bytes.data(), PvrTaArtifactHeaderSize - 4));
	return bytes;
}

void appendEventHeader(std::vector<std::uint8_t>& bytes,
		const PvrTaObservation& observation, std::uint64_t artifactOrdinal,
		std::uint32_t eventSize)
{
	appendU32(bytes, static_cast<std::uint32_t>(observation.type));
	appendU32(bytes, eventSize);
	appendU64(bytes, artifactOrdinal);
	appendU64(bytes, observation.emissionOrdinal);
	appendU64(bytes, observation.tick);
}

void validateSource(const PvrTaObservation& observation)
{
	require(observation.source >= PvrTaInputSource::StoreQueue
			&& observation.source <= PvrTaInputSource::SortDma,
			"accepted block source is invalid");
	require((observation.sourceAddress & 31u) == 0,
			"accepted block source address is not 32-byte aligned");
	if (observation.source == PvrTaInputSource::StoreQueue)
		require((observation.sourceAddress & 0xfc000000u) == 0xe0000000u,
				"store-queue source address is outside P4 store queues");
	else
		require((observation.sourceAddress & 0xff000000u) == 0x0c000000u,
				"DMA source address is not canonical Dreamcast RAM");
	if (observation.source == PvrTaInputSource::SortDma)
		require(observation.taAddress == UINT32_MAX,
				"sort-DMA TA address must use the unavailable sentinel");
	else
		require(observation.taAddress != UINT32_MAX
				&& (observation.taAddress & 31u) == 0,
				"TA destination address is unavailable or unaligned");
}

std::vector<std::uint8_t> serializeObservation(const PvrTaObservation& observation,
		const PvrTaArtifactBinding& binding, std::uint64_t artifactOrdinal)
{
	require(observation.schemaVersion == PvrTaObservationSchemaVersion,
			"observation schema version mismatch");
	require(validType(observation.type), "observation type is invalid");
	require(synchronousType(observation.type) == observation.initiator.valid,
			"instruction ownership does not match the event boundary");

	std::vector<std::uint8_t> payload;
	appendOwner(payload, observation.initiator, binding.backend, observation.tick);
	switch (observation.type)
	{
	case PvrTaObservationType::ListInit:
	case PvrTaObservationType::ListContinue:
		require(observation.contextAddress != UINT32_MAX
				&& observation.contextGeneration != 0,
				"list boundary has no context generation");
		appendU32(payload, observation.contextAddress);
		appendU64(payload, observation.contextGeneration);
		appendU32(payload, observation.renderPass);
		break;
	case PvrTaObservationType::AcceptedBlock:
		require(observation.contextAddress != UINT32_MAX
				&& observation.contextGeneration != 0,
				"accepted block has no context generation");
		require(observation.listTypeBefore <= 7 && observation.listTypeAfter <= 7,
				"accepted block list type is invalid");
		require(observation.parserStateBefore <= 7
				&& observation.parserStateAfter <= 7,
				"accepted block parser state is invalid");
		validateSource(observation);
		appendU32(payload, observation.contextAddress);
		appendU64(payload, observation.contextGeneration);
		appendU64(payload, observation.contextBlockOrdinal);
		appendU32(payload, observation.renderPass);
		appendU32(payload, observation.listTypeBefore);
		appendU32(payload, observation.listTypeAfter);
		appendU32(payload, observation.parserStateBefore);
		appendU32(payload, observation.parserStateAfter);
		appendU32(payload, static_cast<std::uint32_t>(observation.source));
		appendU32(payload, observation.sourceAddress);
		appendU32(payload, observation.taAddress);
		payload.insert(payload.end(), observation.block.begin(), observation.block.end());
		appendU32(payload, 0);
		break;
	case PvrTaObservationType::StartRender:
		require(observation.renderGeneration != 0,
				"STARTRENDER generation is zero");
		require(observation.selectedContexts.size() <= MaximumSelectedContexts,
				"STARTRENDER selects too many contexts");
		require(observation.renderSelectionReadCount != 0
				&& observation.renderSelectionReadCount
						<= MaxPvrTaRenderSelectionReads,
				"STARTRENDER selection transcript count is invalid");
		require(observation.regionBase != UINT32_MAX
				&& observation.fpuParamCfg != UINT32_MAX,
				"STARTRENDER selection registers are unavailable");
		require(observation.renderContextAvailable
				== (!observation.selectedContexts.empty()
						&& observation.selectedContexts.front().available),
				"STARTRENDER first-context availability is inconsistent");
		appendU64(payload, observation.renderGeneration);
		appendU32(payload, observation.renderContextAvailable ? 1u : 0u);
		appendU32(payload, observation.regionBase);
		appendU32(payload, observation.fpuParamCfg);
		appendU32(payload, static_cast<std::uint32_t>(
				observation.selectedContexts.size()));
		appendU32(payload, static_cast<std::uint32_t>(
				observation.renderSelectionReadCount));
		appendU32(payload, 0);
		for (const PvrTaContextRef& context : observation.selectedContexts)
		{
			require(context.address != UINT32_MAX,
					"STARTRENDER selected context address is unavailable");
			require((context.available && context.generation != 0)
					|| (!context.available && context.generation == 0),
					"STARTRENDER context availability/generation mismatch");
			appendU32(payload, context.address);
			appendU64(payload, context.generation);
			appendU32(payload, context.available ? 1u : 0u);
		}
		for (std::size_t index = 0;
				index < observation.renderSelectionReadCount; ++index)
		{
			appendU32(payload, observation.renderSelectionReads[index].address);
			appendU32(payload, observation.renderSelectionReads[index].value);
		}
		break;
	case PvrTaObservationType::RenderDone:
		require(observation.renderGeneration != 0,
				"render-done generation is zero");
		appendU64(payload, observation.renderGeneration);
		break;
	case PvrTaObservationType::Reset:
		break;
	}

	const std::uint64_t eventSize64 = EventHeaderSize + payload.size();
	require(eventSize64 <= std::numeric_limits<std::uint32_t>::max(),
			"event size overflows the format");
	std::vector<std::uint8_t> event;
	event.reserve(static_cast<std::size_t>(eventSize64));
	appendEventHeader(event, observation, artifactOrdinal,
			static_cast<std::uint32_t>(eventSize64));
	event.insert(event.end(), payload.begin(), payload.end());
	require(event.size() == eventSize64, "internal event size mismatch");
	return event;
}

} // namespace

class PvrTaArtifactWriter::OutputFile
{
public:
	explicit OutputFile(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::filesystem::path parent = path.parent_path();
		if (parent.empty() || !std::filesystem::is_directory(parent, error) || error)
			throw std::runtime_error("PowerVR TA output parent is not a directory");
#ifdef _WIN32
		handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
				FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			throw std::system_error(static_cast<int>(GetLastError()),
					std::system_category(), "cannot exclusively create PowerVR TA output");
#else
		fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot exclusively create PowerVR TA output");
#endif
	}

	~OutputFile()
	{
#ifdef _WIN32
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
#else
		if (fd >= 0)
			::close(fd);
#endif
	}

	void write(const void *source, std::size_t size)
	{
		const auto *data = static_cast<const std::uint8_t *>(source);
		while (size != 0)
		{
#ifdef _WIN32
			const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size,
					(std::numeric_limits<DWORD>::max)()));
			DWORD written = 0;
			if (!WriteFile(handle, data, chunk, &written, nullptr) || written == 0)
				throw std::system_error(static_cast<int>(GetLastError()),
						std::system_category(), "cannot write PowerVR TA output");
#else
			const ssize_t written = ::write(fd, data, size);
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0)
				throw std::system_error(errno, std::generic_category(),
						"cannot write PowerVR TA output");
#endif
			data += written;
			size -= static_cast<std::size_t>(written);
		}
	}

	void seek(std::uint64_t offset)
	{
#ifdef _WIN32
		LARGE_INTEGER position;
		position.QuadPart = static_cast<LONGLONG>(offset);
		if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN))
			throw std::system_error(static_cast<int>(GetLastError()),
					std::system_category(), "cannot seek PowerVR TA output");
#else
		if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot seek PowerVR TA output");
#endif
	}

	void flush()
	{
#ifdef _WIN32
		if (!FlushFileBuffers(handle))
			throw std::system_error(static_cast<int>(GetLastError()),
					std::system_category(), "cannot flush PowerVR TA output");
#else
		if (::fsync(fd) != 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot flush PowerVR TA output");
#endif
	}

private:
#ifdef _WIN32
	HANDLE handle = INVALID_HANDLE_VALUE;
#else
	int fd = -1;
#endif
};

PvrTaArtifactWriter::PvrTaArtifactWriter(const std::filesystem::path& path,
		const PvrTaArtifactBinding& binding, std::uint64_t maximumBytes,
		std::uint64_t maximumEvents)
	: path(path), maximumBytes(maximumBytes), maximumEvents(maximumEvents)
{
	if (!validBackend(binding.backend))
		throw std::invalid_argument("PowerVR TA artifact backend is invalid");
	if (maximumBytes < PvrTaArtifactHeaderSize || maximumEvents == 0)
		throw std::invalid_argument("PowerVR TA artifact limits are invalid");
	summary.binding = binding;
	output = std::make_unique<OutputFile>(path);
	const std::vector<std::uint8_t> header = serializeHeader(summary, false);
	output->write(header.data(), header.size());
	output->flush();
}

PvrTaArtifactWriter::~PvrTaArtifactWriter()
{
	if (!finalized)
		abandon();
}

void PvrTaArtifactWriter::ensureWritable() const
{
	if (finalized)
		throw std::logic_error("PowerVR TA artifact is already finalized");
	if (abandoned || output == nullptr)
		throw std::logic_error("PowerVR TA artifact has been abandoned");
}

void PvrTaArtifactWriter::write(const PvrTaObservation& observation)
{
	ensureWritable();
	if (summary.eventCount >= maximumEvents)
		throw std::runtime_error("PowerVR TA artifact event limit exceeded");
	if (hasEvents)
	{
		require(observation.emissionOrdinal == summary.lastEmissionOrdinal + 1,
				"source emission ordinal is not contiguous");
		require(observation.tick >= summary.endTick,
				"scheduler tick moved backwards");
	}
	const std::vector<std::uint8_t> event = serializeObservation(observation,
			summary.binding, summary.eventCount);
	if (event.size() > maximumBytes - PvrTaArtifactHeaderSize
			|| summary.payloadBytes
					> maximumBytes - PvrTaArtifactHeaderSize - event.size())
		throw std::runtime_error("PowerVR TA artifact byte limit exceeded");
	output->write(event.data(), event.size());
	payloadHasher.update(event.data(), event.size());
	if (!hasEvents)
	{
		summary.startTick = observation.tick;
		summary.firstEmissionOrdinal = observation.emissionOrdinal;
	}
	summary.endTick = observation.tick;
	summary.lastEmissionOrdinal = observation.emissionOrdinal;
	summary.payloadBytes += event.size();
	++summary.typeCounts[typeIndex(observation.type)];
	++summary.eventCount;
	hasEvents = true;
}

PvrTaArtifactSummary PvrTaArtifactWriter::finalize()
{
	ensureWritable();
	if (!hasEvents)
		throw std::logic_error("cannot finalize an empty PowerVR TA artifact");
	summary.payloadDigest = payloadHasher.finalize();
	const std::vector<std::uint8_t> header = serializeHeader(summary, true);
	output->flush();
	output->seek(0);
	output->write(header.data(), header.size());
	output->flush();
	finalized = true;
	return summary;
}

void PvrTaArtifactWriter::abandon() noexcept
{
	if (finalized || abandoned)
		return;
	abandoned = true;
	output.reset();
}

} // namespace research

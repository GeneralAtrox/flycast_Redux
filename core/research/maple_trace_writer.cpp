// Maple trace writing: exclusive-create output file and the incremental
// MapleTraceWriter that appends events and patches the header on finalize.
#include "research/maple_trace_internal.h"

#include <algorithm>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>

#ifdef _WIN32
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

using namespace maple_trace_detail;

class MapleTraceWriter::OutputFile
{
public:
	explicit OutputFile(const std::filesystem::path& path)
	{
		std::error_code error;
		const std::filesystem::path parent = path.parent_path();
		if (parent.empty() || !std::filesystem::is_directory(parent, error) || error)
			throw std::runtime_error("Maple trace output parent is not a directory: " + parent.string());
#ifdef _WIN32
		handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
				nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
					"cannot exclusively create Maple trace output");
#else
		fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot exclusively create Maple trace output");
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

	void write(const std::uint8_t *data, std::size_t size)
	{
		while (size != 0)
		{
#ifdef _WIN32
			const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size,
					(std::numeric_limits<DWORD>::max)()));
			DWORD written = 0;
			if (!WriteFile(handle, data, chunk, &written, nullptr) || written == 0)
				throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
						"cannot write Maple trace output");
#else
			const std::size_t chunk = std::min<std::size_t>(size,
					static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
			const ssize_t written = ::write(fd, data, chunk);
			if (written < 0 && errno == EINTR)
				continue;
			if (written <= 0)
				throw std::system_error(errno, std::generic_category(),
						"cannot write Maple trace output");
#endif
			data += written;
			size -= written;
		}
	}

	void seek(std::uint64_t offset)
	{
#ifdef _WIN32
		LARGE_INTEGER position;
		position.QuadPart = static_cast<LONGLONG>(offset);
		if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN))
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
					"cannot seek Maple trace output");
#else
		if (::lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot seek Maple trace output");
#endif
	}

	void flush()
	{
#ifdef _WIN32
		if (!FlushFileBuffers(handle))
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
					"cannot flush Maple trace output");
#else
		if (::fsync(fd) != 0)
			throw std::system_error(errno, std::generic_category(),
					"cannot flush Maple trace output");
#endif
	}

private:
#ifdef _WIN32
	HANDLE handle = INVALID_HANDLE_VALUE;
#else
	int fd = -1;
#endif
};

MapleTraceWriter::MapleTraceWriter(const std::filesystem::path& path,
		const Sha256Digest& identityDigest, std::uint64_t maximumBytes,
		std::uint32_t schemaVersion)
	: path(path), identityDigest(identityDigest), maximumBytes(maximumBytes)
{
	if (schemaVersion != MapleTraceSchemaVersionV1
			&& schemaVersion != MapleTraceSchemaVersionV2)
		throw std::invalid_argument("unsupported Maple trace writer schema version");
	if (maximumBytes < MapleTraceHeaderSize)
		throw std::invalid_argument("Maple trace size limit is smaller than the header");
	output = std::make_unique<OutputFile>(path);
	summary.schemaVersion = schemaVersion;
	summary.identityDigest = identityDigest;
	const std::vector<std::uint8_t> header = serializeHeader(summary, false);
	output->write(header.data(), header.size());
	output->flush();
}

MapleTraceWriter::~MapleTraceWriter()
{
	if (!finalized)
		abandon();
}

void MapleTraceWriter::ensureWritable() const
{
	if (finalized)
		throw std::logic_error("Maple trace is already finalized");
	if (abandoned || output == nullptr)
		throw std::logic_error("Maple trace has been abandoned");
}

void MapleTraceWriter::appendEvent(MapleTraceEventType type, std::uint64_t tick,
		const std::vector<std::uint8_t>& payload)
{
	ensureWritable();
	if (payload.size() > MaximumEventSize - EventHeaderSize)
		throw std::runtime_error("Maple trace event exceeds the schema size limit");
	if (summary.payloadBytes > UINT64_MAX - EventHeaderSize - payload.size())
		throw std::overflow_error("Maple trace payload byte count overflow");
	const std::uint64_t eventBytes = EventHeaderSize + payload.size();
	if (summary.payloadBytes > maximumBytes - MapleTraceHeaderSize
			|| eventBytes > maximumBytes - MapleTraceHeaderSize - summary.payloadBytes)
		throw std::runtime_error("Maple trace exceeds the configured size limit");

	std::vector<std::uint8_t> bytes;
	bytes.reserve(EventHeaderSize + payload.size());
	appendU32(bytes, static_cast<std::uint32_t>(type));
	appendU32(bytes, static_cast<std::uint32_t>(EventHeaderSize + payload.size()));
	appendU64(bytes, nextEventOrdinal++);
	bytes.insert(bytes.end(), payload.begin(), payload.end());
	output->write(bytes.data(), bytes.size());
	output->flush();
	payloadHasher.update(bytes.data(), bytes.size());
	if (summary.eventCount == 0)
		summary.startTick = tick;
	summary.endTick = tick;
	++summary.eventCount;
	summary.payloadBytes += bytes.size();
}

std::uint64_t MapleTraceWriter::beginDma(MapleDmaBeginEvent event)
{
	ensureWritable();
	if (openDma || !pendingDmas.empty())
		throw std::logic_error("Maple DMA began before the prior DMA committed");
	if (event.trigger != MapleDmaTrigger::Software && event.trigger != MapleDmaTrigger::VBlank)
		throw std::invalid_argument("invalid Maple DMA trigger");
	if (event.mden != 1 || event.mdst != 1 || event.mmsel > 1
			|| event.swapMsb != (event.mmsel == 0) || (event.descriptorAddress & 31u) != 0)
		throw std::invalid_argument("invalid Maple DMA begin state");

	event.dmaOrdinal = nextDmaOrdinal++;
	appendEvent(MapleTraceEventType::DmaBegin, event.tick, serializePayload(event));
	openDma = true;
	currentDmaOrdinal = event.dmaOrdinal;
	currentDmaResponses = 0;
	nextDescriptorAddress = event.descriptorAddress;
	descriptorTerminalSeen = false;
	++summary.dmaCount;
	return event.dmaOrdinal;
}

std::uint64_t MapleTraceWriter::writeTransaction(MapleTransactionEvent event)
{
	ensureWritable();
	if (!openDma || event.dmaOrdinal != currentDmaOrdinal)
		throw std::logic_error("Maple transaction is outside the active DMA");
	event.transactionOrdinal = nextTransactionOrdinal++;
	validateTransactionShape(event);
	if (summary.schemaVersion == MapleTraceSchemaVersionV2)
	{
		if (descriptorTerminalSeen)
			throw std::logic_error("Maple transaction follows the terminal descriptor");
		if (event.descriptorAddress != nextDescriptorAddress)
			throw std::invalid_argument("Maple transaction breaks descriptor-table continuity");
		nextDescriptorAddress += static_cast<std::uint32_t>(8u + event.request.size());
		descriptorTerminalSeen = (event.descriptorHeader1 >> 31) != 0;
	}

	appendEvent(MapleTraceEventType::Transaction, event.tick, serializePayload(event));
	++summary.transactionCount;
	++currentDmaResponses;
	return event.transactionOrdinal;
}

std::uint64_t MapleTraceWriter::writeControlDescriptor(
		MapleControlDescriptorEvent event)
{
	ensureWritable();
	if (summary.schemaVersion != MapleTraceSchemaVersionV2)
		throw std::logic_error("Maple control descriptors require trace schema v2");
	if (!openDma || event.dmaOrdinal != currentDmaOrdinal)
		throw std::logic_error("Maple control descriptor is outside the active DMA");
	if (descriptorTerminalSeen)
		throw std::logic_error("Maple control descriptor follows the terminal descriptor");
	if (event.descriptorAddress != nextDescriptorAddress)
		throw std::invalid_argument("Maple control descriptor breaks descriptor-table continuity");
	if (event.operation != MapleControlOperation::Nop
			|| ((event.descriptorHeader >> 8) & 7u)
					!= static_cast<std::uint32_t>(event.operation))
		throw std::invalid_argument("unsupported or inconsistent Maple control descriptor");
	if (event.last != ((event.descriptorHeader >> 31) != 0)
			|| (event.descriptorAddress & 3u) != 0)
		throw std::invalid_argument("invalid Maple control descriptor shape");

	event.controlOrdinal = nextControlOrdinal++;
	appendEvent(MapleTraceEventType::ControlDescriptor, event.tick, serializePayload(event));
	nextDescriptorAddress += 4;
	descriptorTerminalSeen = event.last;
	++summary.controlDescriptorCount;
	return event.controlOrdinal;
}

void MapleTraceWriter::scheduleDma(MapleDmaScheduleEvent event)
{
	ensureWritable();
	if (!openDma || event.dmaOrdinal != currentDmaOrdinal)
		throw std::logic_error("Maple DMA schedule does not match the active DMA");
	if (event.responseCount != currentDmaResponses)
		throw std::invalid_argument("Maple DMA schedule response count mismatch");
	if (summary.schemaVersion == MapleTraceSchemaVersionV2 && !descriptorTerminalSeen)
		throw std::invalid_argument("Maple DMA schedule precedes the terminal descriptor");
	if ((event.flags & ~MapleScheduleDeferredUntilVBlank) != 0
			|| ((event.flags & MapleScheduleDeferredUntilVBlank) != 0 && event.scheduledCycles != 0))
		throw std::invalid_argument("invalid Maple DMA schedule flags");

	appendEvent(MapleTraceEventType::DmaSchedule, event.tick, serializePayload(event));
	pendingDmas.emplace_back(event.dmaOrdinal, event.responseCount);
	openDma = false;
	currentDmaOrdinal = UINT64_MAX;
	currentDmaResponses = 0;
}

void MapleTraceWriter::commitDma(MapleDmaCommitEvent event)
{
	ensureWritable();
	if (openDma || pendingDmas.empty() || event.dmaOrdinal != pendingDmas.front().first)
		throw std::logic_error("Maple DMA commit does not match the pending DMA");
	if (event.responseCount != pendingDmas.front().second
			|| event.flags != MapleCommitInterruptRaised)
		throw std::invalid_argument("invalid Maple DMA commit state");

	appendEvent(MapleTraceEventType::DmaCommit, event.tick, serializePayload(event));
	pendingDmas.pop_front();
}

void MapleTraceWriter::abortDma(MapleDmaAbortEvent event)
{
	ensureWritable();
	if (openDma)
	{
		if (event.dmaOrdinal != currentDmaOrdinal)
			throw std::logic_error("Maple DMA abort does not match the active DMA");
		openDma = false;
		currentDmaOrdinal = UINT64_MAX;
		currentDmaResponses = 0;
		descriptorTerminalSeen = false;
	}
	else
	{
		if (pendingDmas.empty() || event.dmaOrdinal != pendingDmas.front().first)
			throw std::logic_error("Maple DMA abort does not match the pending DMA");
		pendingDmas.pop_front();
	}

	appendEvent(MapleTraceEventType::DmaAbort, event.tick, serializePayload(event));
}

MapleTraceSummary MapleTraceWriter::finalize()
{
	ensureWritable();
	if (openDma || !pendingDmas.empty())
		throw std::logic_error("cannot finalize Maple trace with active DMA work");
	if (summary.dmaCount == 0 || summary.transactionCount == 0 || summary.eventCount == 0)
		throw std::logic_error("cannot finalize an empty Maple trace");
	summary.payloadDigest = payloadHasher.finalize();
	const std::vector<std::uint8_t> header = serializeHeader(summary, true);
	output->seek(0);
	output->write(header.data(), header.size());
	output->flush();
	finalized = true;
	return summary;
}

void MapleTraceWriter::abandon() noexcept
{
	if (finalized || abandoned)
		return;
	abandoned = true;
	output.reset();
}

} // namespace research

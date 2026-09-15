// Maple trace reading: whole-file loading into memory and streaming
// validation that never materializes the event list.
#include "research/maple_trace_internal.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace research
{

using namespace maple_trace_detail;

namespace
{

std::uint64_t traceFileSize(const std::filesystem::path& path)
{
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error)
		throw std::runtime_error("cannot stat Maple trace '" + path.string()
				+ "': " + error.message());
	if (size > std::numeric_limits<std::uint64_t>::max())
		throw std::runtime_error("Maple trace is too large: " + path.string());
	return static_cast<std::uint64_t>(size);
}

void readTraceExact(std::ifstream& input, std::uint8_t *destination, std::size_t count,
		const std::string& truncatedReason)
{
	if (count == 0)
		return;
	input.read(reinterpret_cast<char *>(destination), static_cast<std::streamsize>(count));
	require(input.gcount() == static_cast<std::streamsize>(count) && input.good(),
			truncatedReason);
}

std::vector<std::uint8_t> readFileExact(const std::filesystem::path& path,
		std::uint64_t maximumBytes)
{
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error)
		throw std::runtime_error("cannot read Maple trace: " + path.string());
	if (size > maximumBytes)
		throw std::runtime_error("Maple trace exceeds the size limit: " + path.string());
	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open Maple trace for reading: " + path.string());
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
	if (!bytes.empty())
	{
		input.read(reinterpret_cast<char *>(bytes.data()),
				static_cast<std::streamsize>(bytes.size()));
		if (!input || static_cast<std::size_t>(input.gcount()) != bytes.size())
			throw std::runtime_error("Maple trace changed while reading: " + path.string());
	}
	return bytes;
}

MapleTrace loadTrace(const std::filesystem::path& path,
		const Sha256Digest *expectedIdentity, std::uint64_t maximumBytes)
{
	const std::vector<std::uint8_t> bytes = readFileExact(path, maximumBytes);
	require(bytes.size() >= MapleTraceHeaderSize, "file is smaller than the header");
	MapleTrace trace;
	trace.summary = parseTraceHeader(bytes.data(), expectedIdentity);
	trace.summary.fileBytes = bytes.size();
	trace.summary.fileDigest = sha256(bytes.data(), bytes.size());
	require(trace.summary.payloadBytes == bytes.size() - MapleTraceHeaderSize,
			"payload byte count mismatch");
	const Sha256Digest computedPayload = sha256(bytes.data() + MapleTraceHeaderSize,
			bytes.size() - MapleTraceHeaderSize);
	require(sha256Equal(trace.summary.payloadDigest, computedPayload), "payload SHA-256 mismatch");

	std::size_t offset = MapleTraceHeaderSize;
	trace.events.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(
			trace.summary.eventCount, trace.summary.payloadBytes / EventHeaderSize)));
	while (offset < bytes.size())
	{
		require(bytes.size() - offset >= EventHeaderSize, "truncated event header");
		ByteReader eventHeader(bytes.data() + offset, EventHeaderSize);
		const auto type = static_cast<MapleTraceEventType>(eventHeader.u32());
		const std::uint32_t eventSize = eventHeader.u32();
		const std::uint64_t ordinal = eventHeader.u64();
		require(eventSize >= EventHeaderSize && eventSize <= MaximumEventSize,
				"event size is out of range");
		require(eventSize <= bytes.size() - offset, "event extends beyond payload");
		trace.events.push_back(parseEvent(trace.summary.schemaVersion, type, ordinal,
				bytes.data() + offset + EventHeaderSize, eventSize - EventHeaderSize));
		offset += eventSize;
	}
	require(offset == bytes.size(), "payload contains trailing bytes");
	validateProductionTrace(trace);
	return trace;
}

MapleTraceSummary validateTrace(const std::filesystem::path& path,
		const Sha256Digest *expectedIdentity, std::uint64_t maximumBytes)
{
	const std::uint64_t sizeBefore = traceFileSize(path);
	require(sizeBefore <= maximumBytes, "file exceeds size limit");
	require(sizeBefore >= MapleTraceHeaderSize, "file is smaller than the header");

	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open Maple trace for reading: " + path.string());
	std::array<std::uint8_t, MapleTraceHeaderSize> headerBytes {};
	readTraceExact(input, headerBytes.data(), headerBytes.size(), "truncated trace header");
	MapleTraceSummary summary = parseTraceHeader(headerBytes.data(), expectedIdentity);
	summary.fileBytes = sizeBefore;
	Sha256 fileHasher;
	fileHasher.update(headerBytes.data(), headerBytes.size());
	require(summary.payloadBytes == sizeBefore - MapleTraceHeaderSize,
			"payload byte count mismatch");

	Sha256 payloadHasher;
	ProductionTraceValidator validator(summary.schemaVersion);
	std::array<std::uint8_t, EventHeaderSize> eventHeaderBytes {};
	std::array<std::uint8_t, MaximumEventSize - EventHeaderSize> eventPayload {};
	std::uint64_t remaining = summary.payloadBytes;
	while (remaining != 0)
	{
		require(remaining >= EventHeaderSize, "truncated event header");
		readTraceExact(input, eventHeaderBytes.data(), eventHeaderBytes.size(),
				"truncated event header");
		fileHasher.update(eventHeaderBytes.data(), eventHeaderBytes.size());
		payloadHasher.update(eventHeaderBytes.data(), eventHeaderBytes.size());
		ByteReader eventHeader(eventHeaderBytes.data(), eventHeaderBytes.size());
		const auto type = static_cast<MapleTraceEventType>(eventHeader.u32());
		const std::uint32_t eventSize = eventHeader.u32();
		const std::uint64_t ordinal = eventHeader.u64();
		require(eventSize >= EventHeaderSize && eventSize <= MaximumEventSize,
				"event size is out of range");
		require(eventSize <= remaining, "event extends beyond payload");
		const std::size_t payloadSize = eventSize - EventHeaderSize;
		readTraceExact(input, eventPayload.data(), payloadSize, "truncated event payload");
		fileHasher.update(eventPayload.data(), payloadSize);
		payloadHasher.update(eventPayload.data(), payloadSize);
		validator.consume(parseEvent(summary.schemaVersion, type, ordinal,
				eventPayload.data(), payloadSize));
		remaining -= eventSize;
	}

	char extra = 0;
	input.read(&extra, 1);
	require(input.gcount() == 0, "file grew while reading");
	const std::uint64_t sizeAfter = traceFileSize(path);
	require(sizeBefore == sizeAfter, "file size changed while reading");
	require(sha256Equal(summary.payloadDigest, payloadHasher.finalize()),
			"payload SHA-256 mismatch");
	summary.fileDigest = fileHasher.finalize();
	validator.finish(summary);
	return summary;
}

} // namespace

MapleTrace loadProductionMapleTrace(const std::filesystem::path& path,
		std::uint64_t maximumBytes)
{
	return loadTrace(path, nullptr, maximumBytes);
}

MapleTrace loadProductionMapleTrace(const std::filesystem::path& path,
		const Sha256Digest& expectedIdentity, std::uint64_t maximumBytes)
{
	return loadTrace(path, &expectedIdentity, maximumBytes);
}

MapleTraceSummary validateProductionMapleTraceFile(const std::filesystem::path& path,
		std::uint64_t maximumBytes)
{
	return validateTrace(path, nullptr, maximumBytes);
}

MapleTraceSummary validateProductionMapleTraceFile(const std::filesystem::path& path,
		const Sha256Digest& expectedIdentity, std::uint64_t maximumBytes)
{
	return validateTrace(path, &expectedIdentity, maximumBytes);
}

} // namespace research

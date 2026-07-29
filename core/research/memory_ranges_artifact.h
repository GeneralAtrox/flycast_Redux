#pragma once

#include "research/memory_ranges_manifest.h"
#include "research/sha256.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace research
{

constexpr std::uint32_t MemoryRangesArtifactSchemaVersion = 1;
constexpr std::uint32_t MemoryRangesArtifactHeaderSize = 192;
constexpr std::uint32_t MemoryRangesArtifactEndianSentinel = 0x01020304;
constexpr std::uint64_t DefaultMaximumMemoryRangesArtifactBytes =
		1025ull * 1024 * 1024;

struct MemoryRangeView
{
	const std::uint8_t *data = nullptr;
	std::size_t size = 0;
};

struct MemoryRangesArtifactSummary
{
	Sha256Digest identityDigest {};
	Sha256Digest manifestDigest {};
	Sha256Digest payloadDigest {};
	std::uint64_t eventCount = 0;
	std::uint64_t triggerCount = 0;
	std::uint64_t rangeEventCount = 0;
	std::uint64_t totalMemoryBytes = 0;
	std::uint64_t startTick = 0;
	std::uint64_t endTick = 0;
	std::uint64_t payloadBytes = 0;
	std::uint64_t droppedEvents = 0;
	std::uint32_t triggerPc = 0;
};

MemoryRangesArtifactSummary validateProductionMemoryRangesArtifactFile(
		const std::filesystem::path& path, const IdentityManifest& identity,
		const MemoryRangesManifest& manifest,
		std::uint64_t maximumBytes = DefaultMaximumMemoryRangesArtifactBytes);

class MemoryRangesArtifactWriter
{
public:
	MemoryRangesArtifactWriter(const std::filesystem::path& path,
			const Sha256Digest& identityDigest, const MemoryRangesManifest& manifest,
			std::uint64_t maximumBytes = DefaultMaximumMemoryRangesArtifactBytes);
	~MemoryRangesArtifactWriter();

	MemoryRangesArtifactWriter(const MemoryRangesArtifactWriter&) = delete;
	MemoryRangesArtifactWriter& operator=(const MemoryRangesArtifactWriter&) = delete;

	void capture(std::uint32_t triggerPc, std::uint64_t tick,
			const std::vector<MemoryRangeView>& ranges);
	MemoryRangesArtifactSummary finalize();
	void abandon() noexcept;

	bool isCaptured() const { return captured; }
	bool isFinalized() const { return finalized; }

private:
	class OutputFile;

	void ensureWritable() const;
	void writePayload(const void *data, std::size_t size);
	void writeEventHeader(std::uint32_t type, std::uint32_t size);

	std::filesystem::path path;
	Sha256Digest identityDigest {};
	const MemoryRangesManifest manifest;
	std::uint64_t maximumBytes = DefaultMaximumMemoryRangesArtifactBytes;
	std::unique_ptr<OutputFile> output;
	Sha256 payloadHasher;
	MemoryRangesArtifactSummary summary;
	std::uint64_t nextEventOrdinal = 0;
	bool captured = false;
	bool finalized = false;
	bool abandoned = false;
};

} // namespace research

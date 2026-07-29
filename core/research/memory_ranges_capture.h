#pragma once

#include "research/memory_ranges_artifact.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

namespace research
{

using GuestMemoryReader =
		std::function<const std::uint8_t *(std::uint32_t address, std::uint32_t length)>;

class MemoryRangesCapture
{
public:
	MemoryRangesCapture(const std::filesystem::path& outputPath,
			const IdentityManifest& identity, const MemoryRangesManifest& manifest,
			std::uint64_t maximumBytes = DefaultMaximumMemoryRangesArtifactBytes);
	~MemoryRangesCapture();

	MemoryRangesCapture(const MemoryRangesCapture&) = delete;
	MemoryRangesCapture& operator=(const MemoryRangesCapture&) = delete;

	bool observeInstruction(std::uint32_t executedPc, std::uint64_t tick,
			const GuestMemoryReader& reader);
	MemoryRangesArtifactSummary finish();
	void abandon() noexcept;

	bool hasCaptured() const { return captured; }

private:
	MemoryRangesManifest manifest;
	std::unique_ptr<MemoryRangesArtifactWriter> writer;
	bool captured = false;
	bool finished = false;
};

} // namespace research

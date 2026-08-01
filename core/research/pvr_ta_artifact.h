#pragma once

#include "research/pvr_ta_observation.h"
#include "research/sha256.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace research
{

constexpr std::uint32_t PvrTaArtifactSchemaVersion = 1;
constexpr std::uint32_t PvrTaArtifactHeaderSize = 272;
constexpr std::uint32_t PvrTaArtifactEndianSentinel = 0x01020304;
constexpr std::uint64_t DefaultMaximumPvrTaArtifactBytes = 512ull * 1024 * 1024;
constexpr std::uint64_t DefaultMaximumPvrTaArtifactEvents = 10'000'000;

struct PvrTaArtifactBinding
{
	Sh4ObservationBackend backend = Sh4ObservationBackend::Interpreter;
	Sha256Digest identityDigest {};
	Sha256Digest replayDigest {};
	Sha256Digest manifestDigest {};
};

struct PvrTaArtifactSummary
{
	PvrTaArtifactBinding binding;
	Sha256Digest payloadDigest {};
	std::uint64_t eventCount = 0;
	std::array<std::uint64_t, 6> typeCounts {};
	std::uint64_t payloadBytes = 0;
	std::uint64_t droppedEvents = 0;
	std::uint64_t startTick = 0;
	std::uint64_t endTick = 0;
	std::uint64_t firstEmissionOrdinal = 0;
	std::uint64_t lastEmissionOrdinal = 0;
};

class PvrTaArtifactWriter
{
public:
	PvrTaArtifactWriter(const std::filesystem::path& path,
			const PvrTaArtifactBinding& binding,
			std::uint64_t maximumBytes = DefaultMaximumPvrTaArtifactBytes,
			std::uint64_t maximumEvents = DefaultMaximumPvrTaArtifactEvents);
	~PvrTaArtifactWriter();

	PvrTaArtifactWriter(const PvrTaArtifactWriter&) = delete;
	PvrTaArtifactWriter& operator=(const PvrTaArtifactWriter&) = delete;

	void write(const PvrTaObservation& observation);
	PvrTaArtifactSummary finalize();
	void abandon() noexcept;

	const PvrTaArtifactSummary& getSummary() const { return summary; }

private:
	class OutputFile;
	void ensureWritable() const;

	std::filesystem::path path;
	std::uint64_t maximumBytes = DefaultMaximumPvrTaArtifactBytes;
	std::uint64_t maximumEvents = DefaultMaximumPvrTaArtifactEvents;
	std::unique_ptr<OutputFile> output;
	Sha256 payloadHasher;
	PvrTaArtifactSummary summary;
	bool hasEvents = false;
	bool finalized = false;
	bool abandoned = false;
};

// This implementation lives in pvr_ta_validator.cpp and deliberately has its
// own binary decoder and render-selection state machine.
PvrTaArtifactSummary validatePvrTaArtifactFile(
		const std::filesystem::path& path,
		const PvrTaArtifactBinding& expectedBinding,
		std::uint64_t maximumBytes = DefaultMaximumPvrTaArtifactBytes,
		std::uint64_t maximumEvents = DefaultMaximumPvrTaArtifactEvents);

} // namespace research

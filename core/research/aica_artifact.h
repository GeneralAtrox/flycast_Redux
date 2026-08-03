#pragma once

#include "research/aica_observation.h"
#include "research/identity_manifest.h"
#include "research/sha256.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace research
{

constexpr std::uint32_t AicaArtifactSchemaVersion = 1;
constexpr std::uint32_t AicaArtifactHeaderSize = 512;
constexpr std::uint64_t DefaultMaximumAicaArtifactBytes = 512ull * 1024 * 1024;
constexpr std::uint64_t DefaultMaximumAicaArtifactEvents = 1'000'000;
constexpr std::uint64_t MaximumAicaSampleFrames = 441'000;

struct AicaArtifactBinding
{
	Sh4ObservationBackend backend = Sh4ObservationBackend::Interpreter;
	Sha256Digest identityDigest {};
	Sha256Digest replayDigest {};
	Sha256Digest configurationDigest {};
	bool dspEnabled = false;
	bool vmuSound = false;
};

struct AicaArtifactSummary
{
	AicaArtifactBinding binding;
	Sha256Digest payloadDigest {};
	Sha256Digest pcmDigest {};
	Sha256Digest keyedSourceDigest {};
	std::uint64_t eventCount = 0;
	std::array<std::uint64_t, 12> typeCounts {};
	std::uint64_t payloadBytes = 0;
	std::uint64_t droppedEvents = 0;
	std::uint64_t startTick = 0;
	std::uint64_t endTick = 0;
	std::uint64_t targetSampleFrames = 0;
	std::uint64_t sampleFrames = 0;
	std::uint64_t keyOnCount = 0;
	std::uint64_t keyedSourceCount = 0;
	std::uint64_t nonzeroSampleFrames = 0;
	std::uint64_t checkpointRamBytes = 0;
};

class AicaArtifactWriter
{
public:
	AicaArtifactWriter(const std::filesystem::path& path,
			const AicaArtifactBinding& binding, std::uint64_t targetSampleFrames,
			std::uint64_t maximumBytes = DefaultMaximumAicaArtifactBytes,
			std::uint64_t maximumEvents = DefaultMaximumAicaArtifactEvents);
	~AicaArtifactWriter();
	AicaArtifactWriter(const AicaArtifactWriter&) = delete;
	AicaArtifactWriter& operator=(const AicaArtifactWriter&) = delete;

	void writeCheckpoint(const AicaCheckpoint& checkpoint);
	void write(const AicaObservation& observation);
	bool targetReached() const noexcept;
	AicaArtifactSummary finalize(std::uint64_t droppedEvents = 0,
			bool dmaActive = false);
	void abandon() noexcept;
	const AicaArtifactSummary& getSummary() const noexcept { return summary; }

private:
	class OutputFile;
	std::filesystem::path path;
	std::unique_ptr<OutputFile> output;
	std::uint64_t maximumBytes;
	std::uint64_t maximumEvents;
	Sha256 payloadHasher;
	Sha256 pcmHasher;
	Sha256 sourceHasher;
	AicaArtifactSummary summary;
	std::vector<std::uint8_t> ramMirror;
	bool checkpointWritten = false;
	bool keyBatchSeen = false;
	bool finalized = false;
	bool abandoned = false;
};

// Implemented in aica_validator.cpp. The validator parses the binary format
// independently and does not call the AICA mixer, DSP, or GD-ROM image reader.
AicaArtifactSummary validateAicaArtifactFile(
		const std::filesystem::path& artifact,
		const AicaArtifactBinding& expectedBinding,
		std::uint64_t maximumBytes = DefaultMaximumAicaArtifactBytes,
		std::uint64_t maximumEvents = DefaultMaximumAicaArtifactEvents);

// Independently authenticates the replay and its terminal DMA boundary before
// accepting the AICA artifact bound to it.
AicaArtifactSummary validateAicaArtifactWithReplay(
		const std::filesystem::path& artifact,
		const IdentityManifest& identity,
		const std::filesystem::path& replay,
		std::uint64_t maximumArtifactBytes = DefaultMaximumAicaArtifactBytes,
		std::uint64_t maximumEvents = DefaultMaximumAicaArtifactEvents,
		std::uint64_t maximumReplayBytes = 512ull * 1024 * 1024);

} // namespace research

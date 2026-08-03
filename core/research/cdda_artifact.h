#pragma once

#include "research/aica_observation.h"
#include "research/cdda_observation.h"
#include "research/identity_manifest.h"
#include "research/sha256.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>

namespace research
{

constexpr std::uint32_t CddaArtifactSchemaVersion = 2;
constexpr std::uint32_t CddaArtifactHeaderSize = 384;
constexpr std::uint64_t DefaultMaximumCddaArtifactBytes = 64ull * 1024 * 1024;
constexpr std::uint64_t DefaultMaximumCddaArtifactEvents = 250'000;
constexpr std::uint64_t MaximumCddaSampleFrames = 441'000;

struct CddaArtifactBinding
{
	Sh4ObservationBackend backend = Sh4ObservationBackend::Interpreter;
	Sha256Digest identityDigest {};
	Sha256Digest replayDigest {};
	Sha256Digest configurationDigest {};
	bool dspEnabled = false;
};

struct CddaArtifactSummary
{
	CddaArtifactBinding binding;
	Sha256Digest payloadDigest {};
	Sha256Digest pcmDigest {};
	std::uint64_t eventCount = 0;
	std::array<std::uint64_t, 4> typeCounts {};
	std::uint64_t payloadBytes = 0;
	std::uint64_t droppedCddaEvents = 0;
	std::uint64_t droppedAicaEvents = 0;
	std::uint64_t startTick = 0;
	std::uint64_t endTick = 0;
	std::uint64_t targetSampleFrames = 0;
	std::uint64_t sampleFrames = 0;
	std::uint64_t successfulSectors = 0;
	std::uint64_t contributingSampleFrames = 0;
	std::uint64_t appliedPlayControls = 0;
};

class CddaArtifactWriter
{
public:
	CddaArtifactWriter(const std::filesystem::path& path,
			const CddaArtifactBinding& binding, std::uint64_t targetSampleFrames,
			std::uint64_t maximumBytes = DefaultMaximumCddaArtifactBytes,
			std::uint64_t maximumEvents = DefaultMaximumCddaArtifactEvents);
	~CddaArtifactWriter();
	CddaArtifactWriter(const CddaArtifactWriter&) = delete;
	CddaArtifactWriter& operator=(const CddaArtifactWriter&) = delete;

	void writeCheckpoint(const AicaCheckpoint& checkpoint);
	void write(const CddaObservation& observation);
	void write(const AicaObservation& observation);
	bool knowsSuccessfulAicaGeneration(std::uint64_t generation) const noexcept;
	bool targetReached() const noexcept;
	CddaArtifactSummary finalize(std::uint64_t droppedCddaEvents = 0,
			std::uint64_t droppedAicaEvents = 0);
	void abandon() noexcept;
	const CddaArtifactSummary& getSummary() const noexcept { return summary; }

private:
	class OutputFile;
	void writeRecord(std::uint32_t type, std::uint64_t sourceEmission,
			std::uint64_t tick, const std::vector<std::uint8_t>& payload);
	std::filesystem::path path;
	std::unique_ptr<OutputFile> output;
	std::uint64_t maximumBytes = 0;
	std::uint64_t maximumEvents = 0;
	Sha256 payloadHasher;
	Sha256 pcmHasher;
	CddaArtifactSummary summary;
	std::set<std::uint64_t> successfulAicaGenerations;
	bool checkpointWritten = false;
	bool finalized = false;
	bool abandoned = false;
};

// Implemented independently in cdda_validator.cpp. This parser authenticates
// raw GDI audio sectors and does not call Flycast's image reader or mixer.
CddaArtifactSummary validateCddaArtifactFile(
		const std::filesystem::path& artifact,
		const IdentityManifest& identity,
		const Sha256Digest& replayDigest,
		const Sha256Digest& configurationDigest,
		std::uint64_t maximumBytes = DefaultMaximumCddaArtifactBytes,
		std::uint64_t maximumEvents = DefaultMaximumCddaArtifactEvents);

} // namespace research

#pragma once

#include "research/gdrom_observation.h"
#include "research/identity_manifest.h"
#include "research/sha256.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace research
{

constexpr std::uint32_t GdromArtifactSchemaVersion = 1;
constexpr std::uint32_t GdromArtifactHeaderSize = 256;
constexpr std::uint64_t DefaultMaximumGdromArtifactBytes = 512ull * 1024 * 1024;
constexpr std::uint64_t DefaultMaximumGdromArtifactEvents = 1'000'000;

struct GdromArtifactBinding
{
	Sh4ObservationBackend backend = Sh4ObservationBackend::Interpreter;
	Sha256Digest identityDigest {};
	Sha256Digest replayDigest {};
};

struct GdromArtifactSummary
{
	GdromArtifactBinding binding;
	Sha256Digest payloadDigest {};
	std::uint64_t eventCount = 0;
	std::array<std::uint64_t, 5> typeCounts {};
	std::uint64_t payloadBytes = 0;
	std::uint64_t droppedEvents = 0;
	std::uint64_t startTick = 0;
	std::uint64_t endTick = 0;
};

class GdromArtifactWriter
{
public:
	GdromArtifactWriter(const std::filesystem::path& path,
			const GdromArtifactBinding& binding,
			std::uint64_t maximumBytes = DefaultMaximumGdromArtifactBytes,
			std::uint64_t maximumEvents = DefaultMaximumGdromArtifactEvents);
	~GdromArtifactWriter();
	GdromArtifactWriter(const GdromArtifactWriter&) = delete;
	GdromArtifactWriter& operator=(const GdromArtifactWriter&) = delete;

	void write(const GdromObservation& observation);
	GdromArtifactSummary finalize(std::uint64_t droppedEvents = 0);
	void abandon() noexcept;
	const GdromArtifactSummary& getSummary() const { return summary; }

private:
	class OutputFile;
	std::filesystem::path path;
	std::unique_ptr<OutputFile> output;
	std::uint64_t maximumBytes;
	std::uint64_t maximumEvents;
	Sha256 payloadHasher;
	GdromArtifactSummary summary;
	bool hasEvents = false;
	bool commandOpen = false;
	std::uint64_t openGeneration = 0;
	bool finalized = false;
	bool abandoned = false;
};

// Deliberately implemented in gdrom_validator.cpp. It does not call libGDR or
// share Flycast's image reader.
GdromArtifactSummary validateGdromArtifactFile(
		const std::filesystem::path& artifact,
		const IdentityManifest& identity,
		const Sha256Digest& replayDigest,
		std::uint64_t maximumBytes = DefaultMaximumGdromArtifactBytes,
		std::uint64_t maximumEvents = DefaultMaximumGdromArtifactEvents);

} // namespace research

#pragma once

#include "research/gdrom_hardware_observation.h"
#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/sha256.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace research
{

constexpr std::uint32_t GdromHardwareArtifactSchemaVersion = 1;
constexpr std::uint32_t GdromHardwareArtifactHeaderSize = 384;
constexpr std::uint64_t DefaultMaximumGdromHardwareArtifactBytes =
		512ull * 1024 * 1024;
constexpr std::uint64_t DefaultMaximumGdromHardwareArtifactEvents = 2'000'000;

struct GdromHardwareArtifactBinding
{
	Sh4ObservationBackend backend = Sh4ObservationBackend::Interpreter;
	Sha256Digest identityDigest {};
	Sha256Digest replayDigest {};
	Sha256Digest biosDigest {};
	Sha256Digest flashDigest {};
};

struct GdromHardwareArtifactSummary
{
	GdromHardwareArtifactBinding binding;
	Sha256Digest payloadDigest {};
	std::uint64_t eventCount = 0;
	std::array<std::uint64_t, 13> typeCounts {};
	std::uint64_t payloadBytes = 0;
	std::uint64_t droppedEvents = 0;
	std::uint64_t startTick = 0;
	std::uint64_t endTick = 0;
	std::uint64_t completedCommands = 0;
};

class GdromHardwareArtifactWriter
{
public:
	GdromHardwareArtifactWriter(const std::filesystem::path& path,
			const GdromHardwareArtifactBinding& binding,
			std::uint64_t maximumBytes = DefaultMaximumGdromHardwareArtifactBytes,
			std::uint64_t maximumEvents = DefaultMaximumGdromHardwareArtifactEvents);
	~GdromHardwareArtifactWriter();
	GdromHardwareArtifactWriter(const GdromHardwareArtifactWriter&) = delete;
	GdromHardwareArtifactWriter& operator=(const GdromHardwareArtifactWriter&) = delete;

	void write(const GdromHardwareObservation& observation);
	GdromHardwareArtifactSummary finalize(std::uint64_t droppedEvents = 0);
	void abandon() noexcept;
	const GdromHardwareArtifactSummary& getSummary() const { return summary; }

private:
	class OutputFile;
	std::filesystem::path path;
	std::unique_ptr<OutputFile> output;
	std::uint64_t maximumBytes;
	std::uint64_t maximumEvents;
	Sha256 payloadHasher;
	GdromHardwareArtifactSummary summary;
	bool hasEvents = false;
	bool commandOpen = false;
	bool invalidCandidate = false;
	bool awaitingStatusAck = false;
	std::uint64_t completedGeneration = 0;
	std::uint64_t openGeneration = 0;
	std::uint64_t expectedBytes = 0;
	bool finalized = false;
	bool abandoned = false;
};

// Deliberately implemented independently of Flycast's GD-ROM and image-reader
// code in gdrom_hardware_validator.cpp.
GdromHardwareArtifactSummary validateGdromHardwareArtifactFile(
		const std::filesystem::path& artifact,
		const IdentityManifest& identity,
		const Sha256Digest& replayDigest,
		std::uint64_t maximumBytes = DefaultMaximumGdromHardwareArtifactBytes,
		std::uint64_t maximumEvents = DefaultMaximumGdromHardwareArtifactEvents);

// Independently authenticates the replay as a complete Maple artifact made by
// a real-firmware record identity, then joins every boot authority that can
// affect the GD-ROM run to the capture identity.
MapleTraceSummary validateGdromHardwareReplayFile(
		const std::filesystem::path& replay,
		const IdentityManifest& captureIdentity,
		const IdentityManifest& recordIdentity,
		std::uint64_t maximumBytes = DefaultMaximumMapleTraceBytes);

void requireGdromHardwareReplayTimeline(
		const GdromHardwareArtifactSummary& artifact,
		const MapleTraceSummary& replay);

} // namespace research

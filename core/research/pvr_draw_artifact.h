#pragma once

#include "research/pvr_draw_observation.h"
#include "research/pvr_ta_artifact.h"
#include "research/sha256.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace research
{

constexpr std::uint32_t PvrDrawArtifactSchemaVersion = 3;
constexpr std::uint32_t PvrDrawArtifactHeaderSize = 320;
constexpr std::uint32_t PvrDrawArtifactEndianSentinel = 0x01020304;
constexpr std::uint64_t DefaultMaximumPvrDrawArtifactBytes =
		512ull * 1024 * 1024;
constexpr std::uint64_t DefaultMaximumPvrDrawArtifactEvents = 10'000'000;
constexpr std::size_t MaximumPvrDrawBlocksPerPrimitive = 65'536;
constexpr std::size_t MaximumPvrDrawPrimitiveRefs = 65'536;
constexpr std::size_t MaximumPvrDrawVerticesPerPrimitive = 65'536;
constexpr std::size_t MaximumPvrDrawTextureBytes = 8u * 1024u * 1024u;

struct PvrDrawArtifactBinding
{
	Sh4ObservationBackend backend = Sh4ObservationBackend::Interpreter;
	Sha256Digest identityDigest {};
	Sha256Digest replayDigest {};
	Sha256Digest taArtifactDigest {};
	Sha256Digest presentationArtifactDigest {};
	Sha256Digest rendererConfigurationDigest {};
};

struct PvrDrawArtifactSummary
{
	std::uint32_t schemaVersion = PvrDrawArtifactSchemaVersion;
	PvrDrawArtifactBinding binding;
	Sha256Digest payloadDigest {};
	std::uint64_t eventCount = 0;
	std::array<std::uint64_t, 4> typeCounts {};
	std::uint64_t payloadBytes = 0;
	std::uint64_t droppedEvents = 0;
	std::uint64_t startTick = 0;
	std::uint64_t endTick = 0;
	std::uint64_t firstEmissionOrdinal = 0;
	std::uint64_t lastEmissionOrdinal = 0;
	bool completeVerticalSlice = false;
};

class PvrDrawArtifactWriter
{
public:
	PvrDrawArtifactWriter(const std::filesystem::path& path,
			const PvrDrawArtifactBinding& binding,
			std::uint64_t maximumBytes = DefaultMaximumPvrDrawArtifactBytes,
			std::uint64_t maximumEvents = DefaultMaximumPvrDrawArtifactEvents);
	~PvrDrawArtifactWriter();

	PvrDrawArtifactWriter(const PvrDrawArtifactWriter&) = delete;
	PvrDrawArtifactWriter& operator=(const PvrDrawArtifactWriter&) = delete;

	void write(const PvrDrawObservation& observation);
	void bindLinkedArtifacts(const Sha256Digest& taArtifactDigest,
			const Sha256Digest& presentationArtifactDigest,
			const Sha256Digest& rendererConfigurationDigest);
	PvrDrawArtifactSummary finalize(std::uint64_t droppedEvents = 0);
	void abandon() noexcept;
	const PvrDrawArtifactSummary& getSummary() const { return summary; }

private:
	class OutputFile;
	std::filesystem::path path;
	std::uint64_t maximumBytes;
	std::uint64_t maximumEvents;
	std::vector<std::uint8_t> payload;
	PvrDrawArtifactSummary summary;
	OutputFile* output = nullptr;
	bool finalized = false;
	bool abandoned = false;
};

PvrDrawArtifactSummary validatePvrDrawArtifactFile(
		const std::filesystem::path& path,
		const PvrDrawArtifactBinding& expectedBinding,
		std::uint64_t maximumBytes = DefaultMaximumPvrDrawArtifactBytes,
		std::uint64_t maximumEvents = DefaultMaximumPvrDrawArtifactEvents);

PvrDrawArtifactSummary validatePvrDrawArtifactAgainstTaFile(
		const std::filesystem::path& drawPath,
		const PvrDrawArtifactBinding& expectedDrawBinding,
		const std::filesystem::path& taPath,
		const PvrTaArtifactBinding& expectedTaBinding,
		std::uint64_t maximumDrawBytes = DefaultMaximumPvrDrawArtifactBytes,
		std::uint64_t maximumDrawEvents = DefaultMaximumPvrDrawArtifactEvents,
		std::uint64_t maximumTaBytes = DefaultMaximumPvrTaArtifactBytes,
		std::uint64_t maximumTaEvents = DefaultMaximumPvrTaArtifactEvents);

} // namespace research

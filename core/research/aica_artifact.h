#pragma once

#include "research/aica_observation.h"
#include "research/identity_manifest.h"
#include "research/sha256.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace research
{

constexpr std::uint32_t AicaArtifactSchemaVersion = 2;
constexpr std::uint32_t AicaArtifactHeaderSize = 512;
constexpr std::uint64_t DefaultMaximumAicaArtifactBytes = 3ull * 1024 * 1024 * 1024;
constexpr std::uint64_t DefaultMaximumAicaArtifactEvents = 20'000'000;
constexpr std::uint64_t MaximumAicaSampleFrames = 5'500'000;
constexpr std::uint32_t AicaSampleRateHz = 44'100;
constexpr std::uint32_t LodossLogicRateHz = 25;
constexpr std::uint32_t AicaSamplesPerLodossLogicTick =
		AicaSampleRateHz / LodossLogicRateHz;
static_assert(AicaSampleRateHz % LodossLogicRateHz == 0
		&& AicaSamplesPerLodossLogicTick == 1'764);

enum AicaFieldCoverageGroup : std::uint64_t
{
	AicaCoverageHeaderBindingCounts = 1ull << 0,
	AicaCoverageCheckpointBoundary = 1ull << 1,
	AicaCoverageCheckpointRegisters = 1ull << 2,
	AicaCoverageCheckpointRam = 1ull << 3,
	AicaCoverageCheckpointChannels = 1ull << 4,
	AicaCoverageCheckpointDspPersistent = 1ull << 5,
	AicaCoverageCheckpointMixs = 1ull << 6,
	AicaCoverageCheckpointCddaAudio = 1ull << 7,
	AicaCoverageCddaMetadata = 1ull << 8,
	AicaCoverageChannelLoopStatus = 1ull << 9,
	AicaCoverageEventOrdering = 1ull << 10,
	AicaCoverageEventOwners = 1ull << 11,
	AicaCoverageRegisterMutations = 1ull << 12,
	AicaCoverageRamMutations = 1ull << 13,
	AicaCoverageDmaMutations = 1ull << 14,
	AicaCoverageKeyTransitions = 1ull << 15,
	AicaCoverageKeyBatchMasks = 1ull << 16,
	AicaCoverageCddaSectors = 1ull << 17,
	AicaCoverageChannelReplay = 1ull << 18,
	AicaCoverageDspReplay = 1ull << 19,
	AicaCoverageOutputComposition = 1ull << 20,
	AicaCoverageTerminalCycle = 1ull << 21,
};

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
	Sha256Digest artifactDigest {};
	Sha256Digest payloadDigest {};
	Sha256Digest pcmDigest {};
	Sha256Digest keyedSourceDigest {};
	std::uint64_t eventCount = 0;
	std::uint64_t artifactBytes = 0;
	std::uint64_t replayBytes = 0;
	std::uint32_t replaySchemaVersion = 0;
	bool artifactDigestAvailable = false;
	std::array<std::uint64_t, 13> typeCounts {};
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
	std::uint64_t checkpointNextSampleOrdinal = 0;
	AicaCheckpointPhase checkpointPhase = AicaCheckpointPhase::Unknown;
	bool dspTailProofVerified = false;
	bool dspTailStateHashesAvailable = false;
	std::uint64_t dspTailSampleFrames = 0;
	std::uint64_t dspTailMdecSteps = 0;
	std::uint32_t dspTailAnchorMdec = 0;
	std::uint32_t dspTailTerminalMdec = 0;
	Sha256Digest dspTailAnchorStateDigest {};
	Sha256Digest dspTailTerminalStateDigest {};
	std::uint64_t lastNonzeroDrySampleOrdinal = 0;
	std::uint64_t lastNonzeroDspSampleOrdinal = 0;
	std::uint64_t lastNonzeroFinalSampleOrdinal = 0;
	std::uint64_t fieldCoveragePresentMask = 0;
	std::uint64_t fieldCoverageConsumedMask = 0;
	std::uint64_t fieldCoverageProvenIrrelevantMask = 0;
	std::uint64_t channelOwnerPresentMask = 0;
	std::uint64_t channelOwnerCheckpointActiveMask = 0;
	std::uint64_t channelOwnerKeyTargetMask = 0;
	std::uint64_t channelOwnerObservedActiveMask = 0;
	std::uint64_t channelOwnerConsumedMask = 0;
	std::uint64_t channelOwnerInactiveUnkeyedMask = 0;
	std::uint32_t ownerWriterMask = 0;
	bool fieldCoverageComplete = false;
};

struct AicaDspTailFailure
{
	std::uint64_t anchorSampleOrdinal = 0;
	std::uint64_t terminalSampleOrdinal = 0;
	std::uint64_t firstNonzeroSampleOrdinal = 0;
	std::uint64_t activeChannelMask = 0;
	std::array<std::int32_t, 2> dry {};
	std::array<std::int32_t, 2> cddaInput {};
	std::array<std::int32_t, 2> cdda {};
	std::array<std::int32_t, 2> dsp {};
	std::array<std::int16_t, 2> final {};
	std::uint64_t mdecSteps = 0;
	std::uint32_t anchorMdec = 0;
	std::uint32_t terminalMdec = 0;
	std::int32_t anchorTemp0 = 0;
	std::int32_t terminalTemp0 = 0;
	Sha256Digest anchorStateDigest {};
	Sha256Digest terminalStateDigest {};
	bool outputZero = false;
	bool outputSampleAvailable = false;
	bool geometryStable = false;
	bool statesEqual = false;
	bool stateHashesEqual = false;
	std::string stateDifference;
};

enum class AicaInputFailureStage : std::uint8_t
{
	Identity = 1,
	Replay = 2,
};

class AicaInputValidationError final : public std::runtime_error
{
public:
	AicaInputValidationError(const std::string& message, AicaInputFailureStage stage,
			const Sha256Digest& identityDigest, bool replayDigestAvailable = false,
			const Sha256Digest& replayDigest = {}, std::uint64_t replayBytes = 0,
			std::uint32_t replaySchemaVersion = 0)
		: std::runtime_error(message), stage(stage), identityDigest(identityDigest),
		  replayDigestAvailable(replayDigestAvailable), replayDigest(replayDigest),
		  replayBytes(replayBytes), replaySchemaVersion(replaySchemaVersion) {}
	AicaInputFailureStage stage;
	Sha256Digest identityDigest {};
	bool replayDigestAvailable = false;
	Sha256Digest replayDigest {};
	std::uint64_t replayBytes = 0;
	std::uint32_t replaySchemaVersion = 0;
};

class AicaArtifactValidationError : public std::runtime_error
{
public:
	AicaArtifactValidationError(const std::string& message,
			const AicaArtifactSummary& summary)
		: std::runtime_error(message), summary(summary) {}
	AicaArtifactSummary summary;
};

class AicaDspTailValidationError final : public AicaArtifactValidationError
{
public:
	AicaDspTailValidationError(const std::string& message,
			const AicaArtifactSummary& summary, const AicaDspTailFailure& failure)
		: AicaArtifactValidationError(message, summary), failure(failure) {}
	AicaDspTailFailure failure;
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
	std::uint64_t lastEmissionOrdinal = 0;
	bool haveEmissionOrdinal = false;
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
		std::uint64_t maximumEvents = DefaultMaximumAicaArtifactEvents,
		bool requireDspTailProof = false);

// Independently authenticates the replay and its terminal DMA boundary before
// accepting the AICA artifact bound to it.
AicaArtifactSummary validateAicaArtifactWithReplay(
		const std::filesystem::path& artifact,
		const IdentityManifest& identity,
		const std::filesystem::path& replay,
		std::uint64_t maximumArtifactBytes = DefaultMaximumAicaArtifactBytes,
		std::uint64_t maximumEvents = DefaultMaximumAicaArtifactEvents,
		std::uint64_t maximumReplayBytes = 512ull * 1024 * 1024,
		bool requireDspTailProof = true);

} // namespace research

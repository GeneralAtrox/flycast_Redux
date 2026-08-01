#pragma once

#include "research/pvr_presentation_observation.h"
#include "research/pvr_framebuffer.h"
#include "research/sha256.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace research
{

constexpr std::uint32_t PvrPresentationArtifactSchemaVersion = 1;
constexpr std::uint32_t PvrPresentationArtifactHeaderSize = 256;
constexpr std::uint32_t PvrPresentationArtifactEndianSentinel = 0x01020304;
constexpr std::uint64_t DefaultMaximumPvrPresentationArtifactBytes =
		512ull * 1024 * 1024;
constexpr std::uint64_t DefaultMaximumPvrPresentationArtifactEvents = 10'000'000;

struct PvrPresentationArtifactBinding
{
	Sh4ObservationBackend backend = Sh4ObservationBackend::Interpreter;
	Sha256Digest identityDigest {};
	Sha256Digest replayDigest {};
};

struct PvrPresentationArtifactSummary
{
	PvrPresentationArtifactBinding binding;
	Sha256Digest payloadDigest {};
	std::uint64_t eventCount = 0;
	std::array<std::uint64_t, 7> typeCounts {};
	std::uint64_t payloadBytes = 0;
	std::uint64_t droppedEvents = 0;
	std::uint64_t startTick = 0;
	std::uint64_t endTick = 0;
	std::uint64_t firstEmissionOrdinal = 0;
	std::uint64_t lastEmissionOrdinal = 0;
	std::uint64_t decodedFramebufferCount = 0;
	bool completeVerticalSlice = false;
};

struct PvrValidatedFramebuffer
{
	std::uint64_t generation = 0;
	std::uint64_t sourceRenderGeneration = 0;
	PvrFramebufferKind kind = PvrFramebufferKind::DreamcastVram;
	PvrFramebufferConfig config;
	std::uint32_t rowBytes = 0;
	std::vector<std::uint8_t> rawBytes;
	DecodedPvrFramebuffer decoded;
};

class PvrPresentationArtifactWriter
{
public:
	PvrPresentationArtifactWriter(const std::filesystem::path& path,
			const PvrPresentationArtifactBinding& binding,
			std::uint64_t maximumBytes =
					DefaultMaximumPvrPresentationArtifactBytes,
			std::uint64_t maximumEvents =
					DefaultMaximumPvrPresentationArtifactEvents);
	~PvrPresentationArtifactWriter();

	PvrPresentationArtifactWriter(const PvrPresentationArtifactWriter&) = delete;
	PvrPresentationArtifactWriter& operator=(
			const PvrPresentationArtifactWriter&) = delete;

	void write(const PvrPresentationObservation& observation);
	PvrPresentationArtifactSummary finalize(std::uint64_t droppedEvents = 0);
	void abandon() noexcept;

private:
	std::filesystem::path path;
	std::uint64_t maximumBytes;
	std::uint64_t maximumEvents;
	std::vector<std::uint8_t> payload;
	PvrPresentationArtifactSummary summary;
	bool finalized = false;
	bool abandoned = false;
};

PvrPresentationArtifactSummary validatePvrPresentationArtifactFile(
		const std::filesystem::path& path,
		const PvrPresentationArtifactBinding& expectedBinding,
		std::uint64_t maximumBytes = DefaultMaximumPvrPresentationArtifactBytes,
		std::uint64_t maximumEvents = DefaultMaximumPvrPresentationArtifactEvents,
		std::vector<PvrValidatedFramebuffer>* framebuffers = nullptr);

} // namespace research

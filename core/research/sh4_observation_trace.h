#pragma once

#include "research/sh4_observation.h"
#include "research/sha256.h"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace research
{

constexpr std::uint32_t Sh4ObservationTraceSchemaVersion = 1;
constexpr std::uint32_t Sh4ObservationTraceHeaderSize = 208;
constexpr std::uint32_t Sh4ObservationTraceEventSize = 184;
constexpr std::uint32_t Sh4ObservationTraceEndianSentinel = 0x01020304;
constexpr std::uint64_t DefaultMaximumSh4ObservationTraceBytes =
		512ull * 1024 * 1024;
constexpr std::uint64_t DefaultMaximumSh4ObservationTraceEvents = 10'000'000;

struct Sh4ObservationTraceBinding
{
	Sh4ObservationBackend backend = Sh4ObservationBackend::Interpreter;
	Sha256Digest identityDigest {};
	Sha256Digest replayDigest {};
	Sha256Digest manifestSetDigest {};
};

struct Sh4ObservationTraceSummary
{
	Sh4ObservationTraceBinding binding;
	Sha256Digest payloadDigest {};
	std::uint64_t eventCount = 0;
	std::uint64_t payloadBytes = 0;
	std::uint64_t droppedEvents = 0;
	std::uint64_t startTick = 0;
	std::uint64_t endTick = 0;
};

// Writes one backend's canonical semantic stream. The output is created
// exclusively with an incomplete header. finalize() publishes the complete
// header only after the payload digest is known and durable.
class Sh4ObservationTraceWriter
{
public:
	Sh4ObservationTraceWriter(const std::filesystem::path& path,
			const Sh4ObservationTraceBinding& binding,
			std::uint64_t maximumBytes = DefaultMaximumSh4ObservationTraceBytes,
			std::uint64_t maximumEvents = DefaultMaximumSh4ObservationTraceEvents);
	~Sh4ObservationTraceWriter();

	Sh4ObservationTraceWriter(const Sh4ObservationTraceWriter&) = delete;
	Sh4ObservationTraceWriter& operator=(const Sh4ObservationTraceWriter&) = delete;

	void write(const Sh4Observation& observation);
	Sh4ObservationTraceSummary finalize();
	void abandon() noexcept;

	const Sh4ObservationTraceSummary& getSummary() const { return summary; }

private:
	class OutputFile;

	void ensureWritable() const;

	std::filesystem::path path;
	std::uint64_t maximumBytes = DefaultMaximumSh4ObservationTraceBytes;
	std::uint64_t maximumEvents = DefaultMaximumSh4ObservationTraceEvents;
	std::unique_ptr<OutputFile> output;
	Sha256 payloadHasher;
	Sh4ObservationTraceSummary summary;
	bool hasEvents = false;
	bool finalized = false;
	bool abandoned = false;
};

// Production validator for a single trace. The backend-equivalence comparator
// has its own parser and does not trust this validation path.
Sh4ObservationTraceSummary validateSh4ObservationTraceFile(
		const std::filesystem::path& path,
		const Sh4ObservationTraceBinding& expectedBinding,
		std::uint64_t maximumBytes = DefaultMaximumSh4ObservationTraceBytes,
		std::uint64_t maximumEvents = DefaultMaximumSh4ObservationTraceEvents);

} // namespace research

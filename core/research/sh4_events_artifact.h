#pragma once

#include "research/sh4_events_manifest.h"
#include "research/sha256.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

namespace research
{

constexpr std::uint32_t Sh4EventsArtifactSchemaVersion = 1;
constexpr std::uint32_t Sh4EventsArtifactHeaderSize = 232;
constexpr std::uint32_t Sh4EventsArtifactEndianSentinel = 0x01020304;
constexpr std::uint64_t DefaultMaximumSh4EventsArtifactBytes = 256ull * 1024 * 1024;

enum class Sh4CallKind : std::uint16_t
{
	Bsr = 1,
	Bsrf = 2,
	Jsr = 3,
};

enum class Sh4MemoryAccessKind : std::uint8_t
{
	Read = 1,
	Write = 2,
};

struct Sh4RegisterSnapshot
{
	std::array<std::uint32_t, 16> r {};
	std::uint32_t pr = 0;
	std::uint32_t gbr = 0;
	std::uint32_t vbr = 0;
	std::uint32_t mach = 0;
	std::uint32_t macl = 0;
	std::uint32_t sr = 0;
	std::uint32_t fpul = 0;
	std::uint32_t fpscr = 0;
};

struct Sh4SnapshotView
{
	std::uint32_t definitionIndex = 0;
	std::uint32_t address = 0;
	std::uint32_t declaredLength = 0;
	const std::uint8_t *data = nullptr;
	std::size_t size = 0;
	bool required = true;
};

struct Sh4CallEvent
{
	std::uint64_t tick = 0;
	std::uint64_t invocationId = 0;
	std::uint32_t hookIndex = 0;
	Sh4CallKind kind = Sh4CallKind::Bsr;
	std::uint16_t opcode = 0;
	std::uint32_t callPc = 0;
	std::uint32_t targetPc = 0;
	std::uint32_t returnPc = 0;
	std::uint32_t delaySlotPc = 0;
	std::uint32_t delaySlotDepth = 0;
	Sh4RegisterSnapshot registers;
	std::vector<Sh4SnapshotView> snapshots;
};

struct Sh4ReturnEvent
{
	std::uint64_t instructionTick = 0;
	std::uint64_t completionTick = 0;
	std::uint64_t invocationId = 0;
	std::uint32_t hookIndex = 0;
	std::uint16_t opcode = 0;
	std::uint32_t instructionPc = 0;
	std::uint32_t resumedPc = 0;
	std::uint32_t expectedReturnPc = 0;
	std::uint32_t delaySlotDepth = 0;
	Sh4RegisterSnapshot registers;
	std::vector<Sh4SnapshotView> snapshots;
};

struct Sh4WatchEvent
{
	std::uint64_t tick = 0;
	std::uint32_t watchIndex = 0;
	std::uint32_t instructionPc = 0;
	std::uint32_t address = 0;
	std::uint8_t width = 0;
	Sh4MemoryAccessKind kind = Sh4MemoryAccessKind::Read;
	std::uint16_t delaySlotDepth = 0;
	std::uint64_t value = 0;
};

struct Sh4ExceptionEvent
{
	std::uint64_t tick = 0;
	std::uint32_t instructionPc = 0;
	std::uint32_t exceptionPc = 0;
	std::uint32_t vectorPc = 0;
	std::uint32_t exceptionCode = 0;
	std::uint16_t delaySlotDepth = 0;
	Sh4RegisterSnapshot registers;
};

struct Sh4EventsArtifactSummary
{
	Sha256Digest identityDigest {};
	Sha256Digest manifestDigest {};
	Sha256Digest payloadDigest {};
	std::uint64_t eventCount = 0;
	std::uint64_t callCount = 0;
	std::uint64_t returnCount = 0;
	std::uint64_t watchReadCount = 0;
	std::uint64_t watchWriteCount = 0;
	std::uint64_t snapshotCount = 0;
	std::uint64_t snapshotBytes = 0;
	std::uint64_t exceptionCount = 0;
	std::uint64_t startTick = 0;
	std::uint64_t endTick = 0;
	std::uint64_t payloadBytes = 0;
	std::uint64_t droppedEvents = 0;
	std::uint64_t maximumOpenInvocations = 0;
};

Sh4EventsArtifactSummary validateProductionSh4EventsArtifactFile(
		const std::filesystem::path& path, const IdentityManifest& identity,
		const Sh4EventsManifest& manifest,
		std::uint64_t maximumBytes = DefaultMaximumSh4EventsArtifactBytes);

class Sh4EventsArtifactWriter
{
public:
	Sh4EventsArtifactWriter(const std::filesystem::path& path,
			const Sha256Digest& identityDigest, const Sh4EventsManifest& manifest,
			std::uint64_t maximumBytes = DefaultMaximumSh4EventsArtifactBytes);
	~Sh4EventsArtifactWriter();

	Sh4EventsArtifactWriter(const Sh4EventsArtifactWriter&) = delete;
	Sh4EventsArtifactWriter& operator=(const Sh4EventsArtifactWriter&) = delete;

	void writeCall(const Sh4CallEvent& event);
	void writeReturn(const Sh4ReturnEvent& event);
	void writeWatch(const Sh4WatchEvent& event);
	void writeException(const Sh4ExceptionEvent& event);
	void observeOpenInvocations(std::size_t count);
	Sh4EventsArtifactSummary finalize();
	void abandon() noexcept;

	const Sh4EventsArtifactSummary& getSummary() const { return summary; }

private:
	class OutputFile;

	void ensureWritable() const;
	void beginEvent(std::uint32_t type, std::uint32_t size, std::uint64_t tick);
	void writePayload(const void *data, std::size_t size);
	void writeSnapshots(const std::vector<Sh4SnapshotView>& snapshots);

	std::filesystem::path path;
	Sha256Digest identityDigest {};
	const Sh4EventsManifest manifest;
	std::uint64_t maximumBytes = DefaultMaximumSh4EventsArtifactBytes;
	std::unique_ptr<OutputFile> output;
	Sha256 payloadHasher;
	Sh4EventsArtifactSummary summary;
	std::uint64_t nextEventOrdinal = 0;
	bool hasEvents = false;
	bool finalized = false;
	bool abandoned = false;
};

} // namespace research

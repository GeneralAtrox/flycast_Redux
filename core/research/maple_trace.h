#pragma once

#include "research/sha256.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <variant>
#include <vector>

namespace research
{

constexpr std::uint32_t MapleTraceSchemaVersion = 1;
constexpr std::uint32_t MapleTraceHeaderSize = 160;
constexpr std::uint32_t MapleTraceEndianSentinel = 0x01020304;
constexpr std::uint64_t DefaultMaximumMapleTraceBytes = 512ull * 1024 * 1024;

enum class MapleTraceEventType : std::uint32_t
{
	DmaBegin = 1,
	Transaction = 2,
	DmaSchedule = 3,
	DmaCommit = 4,
	DmaAbort = 5,
};

enum class MapleDmaTrigger : std::uint8_t
{
	Software = 1,
	VBlank = 2,
};

enum class MapleDmaAbortReason : std::uint32_t
{
	InvalidDescriptor = 1,
	InvalidSource = 2,
	InvalidDestination = 3,
	Overrun = 4,
	Reset = 5,
	Shutdown = 6,
	IoFailure = 7,
	UnsupportedDescriptor = 8,
};

enum MapleTransactionFlags : std::uint8_t
{
	MapleTransactionDevicePresent = 1 << 0,
};

enum MapleScheduleFlags : std::uint32_t
{
	MapleScheduleDeferredUntilVBlank = 1 << 0,
};

enum MapleCommitFlags : std::uint32_t
{
	MapleCommitInterruptRaised = 1 << 0,
};

struct MapleDmaBeginEvent
{
	std::uint64_t dmaOrdinal = 0;
	std::uint64_t tick = 0;
	std::uint32_t descriptorAddress = 0;
	std::uint32_t mden = 0;
	std::uint32_t mdst = 0;
	std::uint32_t mmsel = 0;
	MapleDmaTrigger trigger = MapleDmaTrigger::Software;
	bool swapMsb = false;
};

struct MapleTransactionEvent
{
	std::uint64_t dmaOrdinal = 0;
	std::uint64_t transactionOrdinal = 0;
	std::uint64_t tick = 0;
	std::uint32_t descriptorAddress = 0;
	std::uint32_t destinationAddress = 0;
	std::uint32_t descriptorHeader1 = 0;
	std::uint32_t descriptorHeader2 = 0;
	std::uint32_t deviceType = UINT32_MAX;
	std::uint8_t bus = 0;
	std::uint8_t port = 0;
	std::uint8_t command = 0;
	std::uint8_t flags = 0;
	std::vector<std::uint8_t> request;
	std::vector<std::uint8_t> response;
};

struct MapleDmaScheduleEvent
{
	std::uint64_t dmaOrdinal = 0;
	std::uint64_t tick = 0;
	std::uint32_t inputWireBytes = 0;
	std::uint32_t outputWireBytes = 0;
	std::uint32_t scheduledCycles = 0;
	std::uint32_t responseCount = 0;
	std::uint32_t flags = 0;
};

struct MapleDmaCommitEvent
{
	std::uint64_t dmaOrdinal = 0;
	std::uint64_t tick = 0;
	std::int32_t callbackCycles = 0;
	std::int32_t jitter = 0;
	std::uint32_t responseCount = 0;
	std::uint32_t flags = 0;
};

struct MapleDmaAbortEvent
{
	std::uint64_t dmaOrdinal = 0;
	std::uint64_t tick = 0;
	MapleDmaAbortReason reason = MapleDmaAbortReason::IoFailure;
	std::uint32_t stage = 0;
};

using MapleTraceEventData = std::variant<MapleDmaBeginEvent, MapleTransactionEvent,
		MapleDmaScheduleEvent, MapleDmaCommitEvent, MapleDmaAbortEvent>;

struct MapleTraceEvent
{
	MapleTraceEventType type = MapleTraceEventType::DmaBegin;
	std::uint64_t ordinal = 0;
	MapleTraceEventData data;
};

struct MapleTraceSummary
{
	Sha256Digest identityDigest {};
	Sha256Digest payloadDigest {};
	std::uint64_t eventCount = 0;
	std::uint64_t transactionCount = 0;
	std::uint64_t dmaCount = 0;
	std::uint64_t startTick = 0;
	std::uint64_t endTick = 0;
	std::uint64_t payloadBytes = 0;
	std::uint64_t droppedEvents = 0;
};

struct MapleTrace
{
	MapleTraceSummary summary;
	std::vector<MapleTraceEvent> events;
};

MapleTrace loadProductionMapleTrace(const std::filesystem::path& path,
		const Sha256Digest& expectedIdentity,
		std::uint64_t maximumBytes = DefaultMaximumMapleTraceBytes);
MapleTraceSummary validateProductionMapleTraceFile(const std::filesystem::path& path,
		const Sha256Digest& expectedIdentity,
		std::uint64_t maximumBytes = DefaultMaximumMapleTraceBytes);

class MapleTraceWriter
{
public:
	MapleTraceWriter(const std::filesystem::path& path, const Sha256Digest& identityDigest,
			std::uint64_t maximumBytes = DefaultMaximumMapleTraceBytes);
	~MapleTraceWriter();

	MapleTraceWriter(const MapleTraceWriter&) = delete;
	MapleTraceWriter& operator=(const MapleTraceWriter&) = delete;

	std::uint64_t beginDma(MapleDmaBeginEvent event);
	std::uint64_t writeTransaction(MapleTransactionEvent event);
	void scheduleDma(MapleDmaScheduleEvent event);
	void commitDma(MapleDmaCommitEvent event);
	void abortDma(MapleDmaAbortEvent event);
	MapleTraceSummary finalize();
	void abandon() noexcept;

	bool isFinalized() const { return finalized; }
	bool hasOpenDma() const { return openDma; }
	bool hasPendingCommit() const { return !pendingDmas.empty(); }

private:
	class OutputFile;

	void appendEvent(MapleTraceEventType type, std::uint64_t tick,
			const std::vector<std::uint8_t>& payload);
	void ensureWritable() const;

	std::filesystem::path path;
	Sha256Digest identityDigest {};
	std::uint64_t maximumBytes = DefaultMaximumMapleTraceBytes;
	std::unique_ptr<OutputFile> output;
	Sha256 payloadHasher;
	MapleTraceSummary summary;
	std::uint64_t nextEventOrdinal = 0;
	std::uint64_t nextDmaOrdinal = 0;
	std::uint64_t nextTransactionOrdinal = 0;
	std::uint64_t currentDmaOrdinal = UINT64_MAX;
	std::uint32_t currentDmaResponses = 0;
	std::deque<std::pair<std::uint64_t, std::uint32_t>> pendingDmas;
	bool openDma = false;
	bool finalized = false;
	bool abandoned = false;
};

} // namespace research

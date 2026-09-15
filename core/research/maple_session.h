// Internal to the Maple research runtime: one record or replay session. The
// public entry points are declared in research/maple_runtime.h.
#pragma once

#include "research/maple_runtime.h"
#include "research/maple_trace.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace research
{

enum class MapleSessionMode
{
	None,
	Record,
	Replay,
};

class MapleSession
{
public:
	MapleSession(MapleSessionMode mode, std::unique_ptr<MapleTraceWriter> writer,
			MapleTrace replay, std::uint64_t dmaCheckpoint);

	MapleSession(const MapleSession&) = delete;
	MapleSession& operator=(const MapleSession&) = delete;

	// In Record mode each call appends to the trace; in Replay mode it checks
	// the observed event against the next recorded one and throws
	// FlycastException on any divergence.
	std::uint64_t beginDma(MapleDmaBeginEvent event);
	std::vector<std::uint8_t> transaction(MapleTransactionEvent event);
	void controlDescriptor(MapleControlDescriptorEvent event);
	void scheduleDma(MapleDmaScheduleEvent event);
	void commitDma(MapleDmaCommitEvent event);
	void abortDma(MapleDmaAbortEvent event);
	void finish();
	void abandon() noexcept;

	MapleSessionMode getMode() const { return mode; }
	bool replayConsumed() const
	{
		return mode == MapleSessionMode::Replay && cursor == replay.events.size();
	}

	// Handlers are looked up when the corresponding event is recorded or
	// replayed, so they may be installed before or after a session starts.
	static void setCheckpointHandler(MapleCheckpointHandler handler);
	static void setDmaBeginHandler(MapleDmaBeginHandler handler);

private:
	void notifyDmaBegin(std::uint64_t zeroBasedOrdinal);
	void checkpointAfterCommit();

	template<typename T>
	const T& next(MapleTraceEventType type);

	MapleSessionMode mode;
	std::unique_ptr<MapleTraceWriter> writer;
	MapleTrace replay;
	std::size_t cursor = 0;
	std::uint64_t dmaCheckpoint = 0;
	std::uint64_t committedDmaCount = 0;
	bool checkpointReached = false;
};

} // namespace research

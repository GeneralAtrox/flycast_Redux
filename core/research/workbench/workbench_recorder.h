#pragma once

// The workbench recorder: subscribes to the native observation buses named
// in a RecorderConfig, copies events into a bounded queue, and writes them
// to one SQLite database from a dedicated thread. Discovery subscriptions
// are used so Lua watchers can run alongside a recording.

#include "research/workbench/workbench_config.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace research::workbench
{

struct RunInfo
{
	std::string gameId;
	std::string mediaPath;
	std::string flycastVersion;
	std::string cpuBackend;   // "interpreter" or "dynarec"
	bool dynarecObservation = false;
	bool threadedRendering = false;
	std::int64_t rtcSeed = -1;
};

struct RecorderStatus
{
	bool active = false;
	std::string path;
	std::int64_t runId = 0;
	std::uint64_t written = 0;
	std::uint64_t queued = 0;
	std::uint64_t dropped = 0;
	std::string error;
};

class EventQueue;
class Database;
struct RowWriters;

class WorkbenchRecorder
{
public:
	WorkbenchRecorder();
	~WorkbenchRecorder();
	WorkbenchRecorder(const WorkbenchRecorder&) = delete;
	WorkbenchRecorder& operator=(const WorkbenchRecorder&) = delete;

	// Opens the database, creates the schema, inserts the run row, subscribes
	// to the configured buses, and starts the writer thread. Throws on any
	// failure and leaves nothing subscribed.
	void start(const std::filesystem::path& database, const RecorderConfig& config,
			const RunInfo& run);
	// Unsubscribes, drains the queue, builds indexes, closes the run row and
	// the database. Safe to call when not active.
	void stop();
	bool active() const noexcept;
	RecorderStatus status() const;

private:
	struct Subscriptions;

	void subscribeAll(const RecorderConfig& config);
	void unsubscribeAll() noexcept;
	void writerLoop();
	void finalize();

	mutable std::mutex mutex;
	std::unique_ptr<Database> db;
	std::unique_ptr<EventQueue> queue;
	std::unique_ptr<RowWriters> rows;
	std::unique_ptr<Subscriptions> subscriptions;
	std::thread writer;
	std::filesystem::path databasePath;
	std::int64_t runId = 0;
	std::atomic<bool> running {false};
	std::atomic<std::uint64_t> written {0};
	std::atomic<std::uint64_t> callbackDrops {0};
	std::string failure;
	bool ownershipRetained = false;
};

} // namespace research::workbench

#include "research/workbench/workbench_recorder.h"

#include "research/sh4_observation_runtime.h"
#include "research/workbench/workbench_db.h"
#include "research/workbench/workbench_queue.h"
#include "research/workbench/workbench_rows.h"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <utility>

namespace research::workbench
{

struct RowWriters
{
	std::unique_ptr<Sh4Rows> sh4;
	std::unique_ptr<MapleRows> maple;
	std::unique_ptr<PvrTaRows> pvrTa;
	std::unique_ptr<PvrDrawRows> pvrDraw;
	std::unique_ptr<PvrPresentationRows> pvrPresentation;
	std::unique_ptr<GdromRows> gdrom;
	std::unique_ptr<AudioRows> audio;

	void write(const WorkbenchEvent& event)
	{
		std::visit([this](const auto& observation) { dispatch(observation); }, event);
	}

private:
	void dispatch(const Sh4Observation& o) { if (sh4) sh4->write(o); }
	void dispatch(const MapleObservation& o) { if (maple) maple->write(o); }
	void dispatch(const PvrTaObservation& o) { if (pvrTa) pvrTa->write(o); }
	void dispatch(const PvrDrawObservation& o) { if (pvrDraw) pvrDraw->write(o); }
	void dispatch(const PvrPresentationObservation& o) { if (pvrPresentation) pvrPresentation->write(o); }
	void dispatch(const GdromObservation& o) { if (gdrom) gdrom->write(o); }
	void dispatch(const GdromHardwareObservation& o) { if (gdrom) gdrom->write(o); }
	void dispatch(const AicaObservation& o) { if (audio) audio->write(o); }
	void dispatch(const CddaObservation& o) { if (audio) audio->write(o); }
};

struct WorkbenchRecorder::Subscriptions
{
	Sh4ObservationSubscription sh4 = 0;
	MapleObservationSubscription maple = 0;
	PvrTaObservationSubscription pvrTa = 0;
	PvrDrawObservationSubscription pvrDraw = 0;
	PvrPresentationObservationSubscription pvrPresentation = 0;
	GdromObservationSubscription gdrom = 0;
	GdromHardwareObservationSubscription gdromHardware = 0;
	AicaObservationSubscription aica = 0;
	CddaObservationSubscription cdda = 0;
};

namespace
{

constexpr std::size_t WriterBatch = 16384;
constexpr std::chrono::milliseconds WriterWait {50};

void createCommonSchema(Database& db)
{
	db.exec("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT)");
	db.exec("CREATE TABLE IF NOT EXISTS runs("
			"id INTEGER PRIMARY KEY, started_utc TEXT, stopped_utc TEXT,"
			" game_id TEXT, media_path TEXT, flycast_version TEXT, cpu_backend TEXT,"
			" dynarec_observation INTEGER, threaded_rendering INTEGER, rtc_seed INTEGER,"
			" config TEXT, note TEXT, written INTEGER, dropped INTEGER)");
	// Populated by the workbench tools from a Ghidra export; empty by default.
	db.exec("CREATE TABLE IF NOT EXISTS symbols("
			"address INTEGER PRIMARY KEY, name TEXT, size INTEGER, kind TEXT, namespace TEXT)");
	Statement meta = db.prepare("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)");
	meta.bindText(1, "schema_version").bindText(2, "1").execute();
	meta.bindText(1, "created_utc").bindText(2, utcNowIso8601()).execute();
}

std::int64_t insertRun(Database& db, const RunInfo& run, const RecorderConfig& config)
{
	Statement insert = db.prepare("INSERT INTO runs(started_utc, game_id, media_path,"
			" flycast_version, cpu_backend, dynarec_observation, threaded_rendering,"
			" rtc_seed, config, note, written, dropped)"
			" VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, 0, 0)");
	insert.bindText(1, utcNowIso8601())
			.bindText(2, run.gameId)
			.bindText(3, run.mediaPath)
			.bindText(4, run.flycastVersion)
			.bindText(5, run.cpuBackend)
			.bindBool(6, run.dynarecObservation)
			.bindBool(7, run.threadedRendering)
			.bindInt(8, run.rtcSeed)
			.bindText(9, recorderConfigToJson(config).dump())
			.bindText(10, config.note)
			.execute();
	return db.lastInsertRowId();
}

} // namespace

WorkbenchRecorder::WorkbenchRecorder() = default;

WorkbenchRecorder::~WorkbenchRecorder()
{
	try
	{
		stop();
	}
	catch (...)
	{
	}
}

bool WorkbenchRecorder::active() const noexcept
{
	return running.load(std::memory_order_acquire);
}

void WorkbenchRecorder::start(const std::filesystem::path& database,
		const RecorderConfig& config, const RunInfo& run)
{
	std::lock_guard<std::mutex> lock(mutex);
	if (running.load(std::memory_order_acquire))
		throw std::logic_error("workbench recorder is already active");
	if (config.buses == 0)
		throw std::invalid_argument("workbench recorder needs at least one bus");

	db = std::make_unique<Database>(database);
	try
	{
		// Recording favours throughput: WAL plus no fsync per commit. A crash
		// loses the last few batches, never the database.
		db->exec("PRAGMA synchronous=OFF");
		db->exec("PRAGMA cache_size=-65536");
		createCommonSchema(*db);
		if (config.buses & BusSh4) Sh4Rows::createTables(*db);
		if (config.buses & BusMaple) MapleRows::createTables(*db);
		if (config.buses & BusPvrTa) PvrTaRows::createTables(*db);
		if (config.buses & BusPvrDraw) PvrDrawRows::createTables(*db);
		if (config.buses & BusPvrPresentation) PvrPresentationRows::createTables(*db);
		if (config.buses & (BusGdrom | BusGdromHardware)) GdromRows::createTables(*db);
		if (config.buses & (BusAica | BusCdda)) AudioRows::createTables(*db);
		runId = insertRun(*db, run, config);

		rows = std::make_unique<RowWriters>();
		if (config.buses & BusSh4) rows->sh4 = std::make_unique<Sh4Rows>(*db, runId, config.rows);
		if (config.buses & BusMaple) rows->maple = std::make_unique<MapleRows>(*db, runId, config.rows);
		if (config.buses & BusPvrTa) rows->pvrTa = std::make_unique<PvrTaRows>(*db, runId, config.rows);
		if (config.buses & BusPvrDraw) rows->pvrDraw = std::make_unique<PvrDrawRows>(*db, runId, config.rows);
		if (config.buses & BusPvrPresentation)
			rows->pvrPresentation = std::make_unique<PvrPresentationRows>(*db, runId, config.rows);
		if (config.buses & (BusGdrom | BusGdromHardware))
			rows->gdrom = std::make_unique<GdromRows>(*db, runId, config.rows);
		if (config.buses & (BusAica | BusCdda))
			rows->audio = std::make_unique<AudioRows>(*db, runId, config.rows);

		queue = std::make_unique<EventQueue>(config.queueCapacity);
		databasePath = database;
		written.store(0, std::memory_order_relaxed);
		callbackDrops.store(0, std::memory_order_relaxed);
		failure.clear();
		running.store(true, std::memory_order_release);
		writer = std::thread([this] { writerLoop(); });
		subscribeAll(config);
	}
	catch (...)
	{
		unsubscribeAll();
		running.store(false, std::memory_order_release);
		if (queue)
			queue->close();
		if (writer.joinable())
			writer.join();
		queue.reset();
		rows.reset();
		db.reset();
		throw;
	}
}

void WorkbenchRecorder::subscribeAll(const RecorderConfig& config)
{
	subscriptions = std::make_unique<Subscriptions>();
	// Hardware events carry the owning SH-4 instruction only while instruction
	// ownership is retained. Do it for both backends like the old evidence
	// subscribers did, even when the SH-4 bus itself is not recorded.
	retainSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
	retainSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
	ownershipRetained = true;

	// SH-4 rows ride the bulk lane; everything else is priority so a flood of
	// instruction events cannot starve the hardware buses.
	const auto pushLane = [this](auto observation, Lane lane) noexcept {
		try
		{
			queue->push(WorkbenchEvent(std::move(observation)), lane);
		}
		catch (...)
		{
			callbackDrops.fetch_add(1, std::memory_order_relaxed);
		}
	};
	const auto push = [pushLane](auto observation) noexcept {
		pushLane(std::move(observation), Lane::Priority);
	};
	if (config.buses & BusSh4)
		subscriptions->sh4 = subscribeSh4Observations(config.sh4,
				[pushLane](const Sh4Observation& o) { pushLane(o, Lane::Bulk); });
	if (config.buses & BusMaple)
		subscriptions->maple = subscribeMapleObservations(config.maple,
				[push](const MapleObservation& o) { push(o); });
	if (config.buses & BusPvrTa)
		subscriptions->pvrTa = subscribePvrTaObservations(config.pvrTa,
				[push](const PvrTaObservation& o) { push(o); });
	if (config.buses & BusPvrDraw)
		subscriptions->pvrDraw = subscribePvrDrawObservations(
				[push](const PvrDrawObservation& o) { push(o); });
	if (config.buses & BusPvrPresentation)
	{
		const bool vramWrites = config.rows.recordVramWrites;
		subscriptions->pvrPresentation = subscribePvrPresentationObservations(
				[push, vramWrites](const PvrPresentationObservation& o) {
					if (!vramWrites && o.type == PvrPresentationObservationType::VramWrite)
						return;
					push(o);
				});
	}
	if (config.buses & BusGdrom)
		subscriptions->gdrom = subscribeGdromObservations(
				[push](const GdromObservation& o) { push(o); });
	if (config.buses & BusGdromHardware)
		subscriptions->gdromHardware = subscribeGdromHardwareObservations(
				[push](const GdromHardwareObservation& o) { push(o); });
	if (config.buses & BusAica)
	{
		const bool sampleFrames = config.rows.recordSampleFrames;
		const std::uint32_t writers = config.aica.writerMask != 0
				? config.aica.writerMask : defaultRecorderConfig().aica.writerMask;
		const std::uint32_t types = config.aica.typeMask;
		subscriptions->aica = subscribeAicaObservations(
				[push, sampleFrames, writers, types](const AicaObservation& o) {
					if (!sampleFrames && o.type == AicaObservationType::SampleFrame)
						return;
					if ((writers & (1u << static_cast<unsigned>(o.owner.writer))) == 0)
						return;
					if (types != 0 && (types & (1u << static_cast<unsigned>(o.type))) == 0)
						return;
					push(o);
				});
	}
	if (config.buses & BusCdda)
		subscriptions->cdda = subscribeCddaObservations(
				[push](const CddaObservation& o) { push(o); });
}

void WorkbenchRecorder::unsubscribeAll() noexcept
{
	if (subscriptions)
	{
		if (subscriptions->sh4) unsubscribeSh4Observations(subscriptions->sh4);
		if (subscriptions->maple) unsubscribeMapleObservations(subscriptions->maple);
		if (subscriptions->pvrTa) unsubscribePvrTaObservations(subscriptions->pvrTa);
		if (subscriptions->pvrDraw) unsubscribePvrDrawObservations(subscriptions->pvrDraw);
		if (subscriptions->pvrPresentation)
			unsubscribePvrPresentationObservations(subscriptions->pvrPresentation);
		if (subscriptions->gdrom) unsubscribeGdromObservations(subscriptions->gdrom);
		if (subscriptions->gdromHardware)
			unsubscribeGdromHardwareObservations(subscriptions->gdromHardware);
		if (subscriptions->aica) unsubscribeAicaObservations(subscriptions->aica);
		if (subscriptions->cdda) unsubscribeCddaObservations(subscriptions->cdda);
		subscriptions.reset();
	}
	if (ownershipRetained)
	{
		releaseSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
		releaseSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
		ownershipRetained = false;
	}
}

void WorkbenchRecorder::writerLoop()
{
	std::vector<WorkbenchEvent> batch;
	batch.reserve(WriterBatch);
	try
	{
		for (;;)
		{
			batch.clear();
			const std::size_t count = queue->drain(batch, WriterBatch, WriterWait);
			if (count == 0)
			{
				if (queue->closed())
					break;
				continue;
			}
			db->begin();
			for (const WorkbenchEvent& event : batch)
				rows->write(event);
			db->commit();
			written.fetch_add(count, std::memory_order_relaxed);
		}
	}
	catch (const std::exception& exception)
	{
		db->rollback();
		std::lock_guard<std::mutex> lock(mutex);
		failure = exception.what();
		// Keep draining so producers see a full queue as drops, not a hang.
		while (!queue->closed() || queue->size() != 0)
		{
			batch.clear();
			if (queue->drain(batch, WriterBatch, WriterWait) == 0 && queue->closed())
				break;
		}
	}
}

void WorkbenchRecorder::stop()
{
	std::unique_lock<std::mutex> lock(mutex);
	if (!running.load(std::memory_order_acquire) && !db)
		return;
	unsubscribeAll();
	running.store(false, std::memory_order_release);
	if (queue)
		queue->close();
	lock.unlock();
	if (writer.joinable())
		writer.join();
	lock.lock();
	if (db)
	{
		try
		{
			finalize();
		}
		catch (...)
		{
			rows.reset();
			db.reset();
			queue.reset();
			throw;
		}
	}
	rows.reset();
	db.reset();
	queue.reset();
}

void WorkbenchRecorder::finalize()
{
	const std::uint64_t dropped = (queue ? queue->dropped() : 0)
			+ callbackDrops.load(std::memory_order_relaxed);
	if (failure.empty())
	{
		db->begin();
		if (rows->sh4) Sh4Rows::createIndexes(*db);
		if (rows->maple) MapleRows::createIndexes(*db);
		if (rows->pvrTa) PvrTaRows::createIndexes(*db);
		if (rows->pvrDraw) PvrDrawRows::createIndexes(*db);
		if (rows->pvrPresentation) PvrPresentationRows::createIndexes(*db);
		if (rows->gdrom) GdromRows::createIndexes(*db);
		if (rows->audio) AudioRows::createIndexes(*db);
		db->commit();
	}
	Statement update = db->prepare("UPDATE runs SET stopped_utc = ?, written = ?, dropped = ?,"
			" note = CASE WHEN ? = '' THEN note ELSE note || ' [writer error: ' || ? || ']' END"
			" WHERE id = ?");
	update.bindText(1, utcNowIso8601())
			.bindUInt(2, written.load(std::memory_order_relaxed))
			.bindUInt(3, dropped)
			.bindText(4, failure)
			.bindText(5, failure)
			.bindInt(6, runId)
			.execute();
}

RecorderStatus WorkbenchRecorder::status() const
{
	std::lock_guard<std::mutex> lock(mutex);
	RecorderStatus status;
	status.active = running.load(std::memory_order_acquire);
	status.path = databasePath.string();
	status.runId = runId;
	status.written = written.load(std::memory_order_relaxed);
	status.queued = queue ? queue->size() : 0;
	status.dropped = (queue ? queue->dropped() : 0)
			+ callbackDrops.load(std::memory_order_relaxed);
	status.error = failure;
	return status;
}

} // namespace research::workbench

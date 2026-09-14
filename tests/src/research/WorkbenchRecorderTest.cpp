#include "research/workbench/workbench_db.h"
#include "research/workbench/workbench_queue.h"
#include "research/workbench/workbench_config.h"
#include "research/workbench/workbench_recorder.h"
#include "research/sh4_observation.h"
#include "research/maple_observation.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace wb = research::workbench;

namespace
{

class WorkbenchRecorderTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		const auto *info = ::testing::UnitTest::GetInstance()->current_test_info();
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		directory = std::filesystem::temp_directory_path()
				/ ("flycast-workbench-" + std::string(info->name()) + "-" + std::to_string(stamp));
		std::filesystem::create_directories(directory);
	}

	void TearDown() override
	{
		std::error_code ignored;
		std::filesystem::remove_all(directory, ignored);
	}

	std::filesystem::path databasePath() const { return directory / "workbench.sqlite"; }

	std::filesystem::path directory;
};

research::Sh4Observation sh4Event(research::Sh4ObservationType type, std::uint64_t tick)
{
	research::Sh4Observation observation;
	observation.type = type;
	observation.tick = tick;
	observation.instructionPc = 0x8c010000;
	return observation;
}

research::MapleTransactionEvent mapleTransaction()
{
	research::MapleTransactionEvent event;
	event.tick = 1234;
	event.descriptorAddress = 0x8c001000;
	event.destinationAddress = 0x8c002000;
	event.descriptorHeader1 = 1u << 16;
	event.descriptorHeader2 = event.destinationAddress;
	event.deviceType = 1;
	event.bus = 1;
	event.port = 2;
	event.command = 9;
	event.flags = research::MapleTransactionDevicePresent;
	event.request = {9, 0x20, 0x01, 0x00};
	event.response = {0x08, 0x01, 0x20, 0x00};
	return event;
}

wb::RunInfo testRun()
{
	wb::RunInfo run;
	run.gameId = "T-TEST";
	run.cpuBackend = "interpreter";
	run.flycastVersion = "test";
	return run;
}

std::int64_t tableCount(wb::Database& db, const char *table)
{
	return db.queryInt64(std::string("SELECT COUNT(*) FROM sqlite_master WHERE type = 'table'"
			" AND name = '") + table + "'");
}

} // namespace

TEST_F(WorkbenchRecorderTest, DatabaseOpensCreatesAndQueries)
{
	wb::Database db(databasePath());
	db.exec("CREATE TABLE probe(id INTEGER PRIMARY KEY, name TEXT, bytes BLOB,"
			" opt INTEGER, gen INTEGER)");
	wb::Statement insert = db.prepare(
			"INSERT INTO probe(id, name, bytes, opt, gen) VALUES(?, ?, ?, ?, ?)");
	const unsigned char blob[] = {0x01, 0x00, 0xff};
	insert.bindInt(1, 1).bindText(2, "present").bindBlob(3, blob, sizeof(blob))
			.bindOptionalU32(4, 0x1234).bindGeneration(5, 7).execute();
	insert.bindInt(1, 2).bindText(2, "absent").bindNull(3)
			.bindOptionalU32(4, UINT32_MAX).bindGeneration(5, 0).execute();
	EXPECT_EQ(2, db.lastInsertRowId());
	EXPECT_EQ(2, db.queryInt64("SELECT COUNT(*) FROM probe"));

	wb::Statement query = db.prepare("SELECT name, bytes, opt, gen FROM probe ORDER BY id");
	ASSERT_TRUE(query.step());
	EXPECT_EQ("present", query.columnText(0));
	EXPECT_EQ(std::string(reinterpret_cast<const char *>(blob), sizeof(blob)), query.columnBlob(1));
	EXPECT_FALSE(query.columnIsNull(2));
	EXPECT_EQ(0x1234, query.columnInt64(2));
	EXPECT_EQ(7, query.columnInt64(3));
	ASSERT_TRUE(query.step());
	EXPECT_EQ("absent", query.columnText(0));
	EXPECT_TRUE(query.columnIsNull(1));
	EXPECT_TRUE(query.columnIsNull(2));
	EXPECT_TRUE(query.columnIsNull(3));
	EXPECT_FALSE(query.step());
	query.reset();
	EXPECT_THROW(db.exec("SELECT * FROM missing_table"), std::runtime_error);
}

TEST_F(WorkbenchRecorderTest, EventQueueDropsWhenFullAndDrainsInOrder)
{
	wb::EventQueue queue(4);
	for (std::uint64_t tick = 1; tick <= 6; ++tick)
	{
		const bool accepted = queue.push(wb::WorkbenchEvent(
				sh4Event(research::Sh4ObservationType::InstructionEnd, tick)));
		EXPECT_EQ(tick <= 4, accepted);
	}
	EXPECT_EQ(2u, queue.dropped());
	EXPECT_EQ(4u, queue.size());

	std::vector<wb::WorkbenchEvent> drained;
	EXPECT_EQ(4u, queue.drain(drained, 16, std::chrono::milliseconds(0)));
	ASSERT_EQ(4u, drained.size());
	for (std::size_t index = 0; index < drained.size(); ++index)
	{
		ASSERT_TRUE(std::holds_alternative<research::Sh4Observation>(drained[index]));
		EXPECT_EQ(index + 1, std::get<research::Sh4Observation>(drained[index]).tick);
	}
	EXPECT_FALSE(queue.closed());
	queue.close();
	EXPECT_TRUE(queue.closed());
	drained.clear();
	EXPECT_EQ(0u, queue.drain(drained, 16, std::chrono::milliseconds(0)));
	EXPECT_FALSE(queue.push(wb::WorkbenchEvent(
			sh4Event(research::Sh4ObservationType::InstructionEnd, 7))));
	EXPECT_EQ(3u, queue.dropped());
}

TEST_F(WorkbenchRecorderTest, ConfigJsonRoundTripAndValidation)
{
	const nlohmann::json json {
		{"buses", "sh4,maple"},
		{"sh4", {{"types", "call,return,memory-write"}, {"backend", "interpreter"},
				{"pc_start", 0x8c010000u}, {"pc_end", 0x8c01ffffu}}},
		{"rows", {{"texture_bytes", true}}},
	};
	const wb::RecorderConfig config = wb::recorderConfigFromJson(json);
	EXPECT_EQ(wb::BusSh4 | wb::BusMaple, config.buses);
	EXPECT_EQ(research::sh4ObservationTypeBit(research::Sh4ObservationType::Call)
			| research::sh4ObservationTypeBit(research::Sh4ObservationType::Return)
			| research::sh4ObservationTypeBit(research::Sh4ObservationType::MemoryWrite),
			config.sh4.typeMask);
	EXPECT_EQ(research::sh4ObservationBackendBit(research::Sh4ObservationBackend::Interpreter),
			config.sh4.backendMask);
	EXPECT_TRUE(config.sh4.hasInstructionPcRange);
	EXPECT_EQ(0x8c010000u, config.sh4.instructionPcStart);
	EXPECT_EQ(0x8c020000u, config.sh4.instructionPcEndExclusive);
	EXPECT_FALSE(config.sh4.hasMemoryRange);
	EXPECT_TRUE(config.rows.storeTextureBytes);
	EXPECT_TRUE(config.rows.storeDrawVertices);

	const wb::RecorderConfig again = wb::recorderConfigFromJson(wb::recorderConfigToJson(config));
	EXPECT_EQ(config.buses, again.buses);
	EXPECT_EQ(config.sh4.typeMask, again.sh4.typeMask);
	EXPECT_EQ(config.sh4.backendMask, again.sh4.backendMask);
	EXPECT_TRUE(again.sh4.hasInstructionPcRange);
	EXPECT_EQ(config.sh4.instructionPcStart, again.sh4.instructionPcStart);
	EXPECT_EQ(config.sh4.instructionPcEndExclusive, again.sh4.instructionPcEndExclusive);
	EXPECT_EQ(config.rows.storeTextureBytes, again.rows.storeTextureBytes);

	EXPECT_THROW(wb::recorderConfigFromJson(nlohmann::json {{"buses", "sh4,warp-drive"}}),
			std::invalid_argument);
	EXPECT_THROW(wb::recorderConfigFromJson(nlohmann::json {{"busses", "sh4"}}),
			std::invalid_argument);
	EXPECT_THROW(wb::recorderConfigFromJson(nlohmann::json {{"sh4", {{"pc_start", 0x8c010000u}}}}),
			std::invalid_argument);
	EXPECT_THROW(wb::parseBusList("sh4,bogus"), std::invalid_argument);

	EXPECT_EQ(wb::AllWorkbenchBuses, wb::parseBusList("all"));
	EXPECT_EQ(wb::BusPvrTa | wb::BusGdromHardware, wb::parseBusList("pvr-ta,gdrom-hw"));
	EXPECT_EQ(research::sh4ObservationTypeBit(research::Sh4ObservationType::Call)
			| research::sh4ObservationTypeBit(research::Sh4ObservationType::Return)
			| research::sh4ObservationTypeBit(research::Sh4ObservationType::Exception),
			wb::defaultRecorderConfig().sh4.typeMask);
	EXPECT_EQ(wb::AllWorkbenchBuses, wb::defaultRecorderConfig().buses);
}

TEST_F(WorkbenchRecorderTest, RecorderWritesSh4RowsAndRunMetadata)
{
	ASSERT_EQ(0u, research::sh4ObservationSubscriberCount());
	wb::RecorderConfig config;
	config.buses = wb::BusSh4;
	config.sh4.typeMask = research::AllSh4ObservationTypes;
	wb::WorkbenchRecorder recorder;
	ASSERT_NO_THROW(recorder.start(databasePath(), config, testRun()));
	EXPECT_TRUE(recorder.active());
	EXPECT_TRUE(recorder.status().active);
	EXPECT_EQ(1u, research::sh4ObservationSubscriberCount());

	research::Sh4Observation end = sh4Event(research::Sh4ObservationType::InstructionEnd, 10);
	end.availableFields = research::Sh4Observation::HasNextPc | research::Sh4Observation::HasRegisters;
	end.nextPc = 0x8c010002;
	end.opcode = 0x0009;
	end.registers.r[3] = 0x1234;
	end.registers.pr = 0x8c000100;
	EXPECT_TRUE(research::publishSh4Observation(end));

	research::Sh4Observation write = sh4Event(research::Sh4ObservationType::MemoryWrite, 11);
	write.memoryAddress = 0x8c001000;
	write.memoryWidth = 4;
	write.memoryValue = 0xdeadbeef;
	EXPECT_TRUE(research::publishSh4Observation(write));

	research::Sh4Observation call = sh4Event(research::Sh4ObservationType::Call, 12);
	call.callKind = research::Sh4CallKind::Jsr;
	call.targetPc = 0x8c020000;
	call.returnPc = 0x8c010004;
	call.delaySlotPc = 0x8c010002;
	EXPECT_TRUE(research::publishSh4Observation(call));

	research::Sh4Observation exception = sh4Event(research::Sh4ObservationType::Exception, 13);
	exception.exceptionPc = 0x8c010000;
	exception.vectorPc = 0x8c000600;
	exception.exceptionCode = 0x1e0;
	EXPECT_TRUE(research::publishSh4Observation(exception));

	ASSERT_NO_THROW(recorder.stop());
	EXPECT_FALSE(recorder.active());
	EXPECT_FALSE(recorder.status().active);
	EXPECT_EQ(4u, recorder.status().written);
	EXPECT_EQ(0u, recorder.status().dropped);
	EXPECT_TRUE(recorder.status().error.empty());
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());

	wb::Database db(databasePath());
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM runs"));
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM runs WHERE game_id = 'T-TEST'"
			" AND cpu_backend = 'interpreter' AND stopped_utc IS NOT NULL AND written = 4"
			" AND dropped = 0"));
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM meta WHERE key = 'schema_version'"));
	EXPECT_EQ(4, db.queryInt64("SELECT COUNT(*) FROM sh4_events"));
	EXPECT_EQ(4, db.queryInt64("SELECT COUNT(*) FROM sh4_events WHERE run = 1"));

	wb::Statement rows = db.prepare("SELECT tick, has_regs, r3, next_pc, mem_addr, mem_value,"
			" call_kind, target_pc, return_pc FROM sh4_events ORDER BY ordinal");
	ASSERT_TRUE(rows.step()); // InstructionEnd
	EXPECT_EQ(10, rows.columnInt64(0));
	EXPECT_EQ(1, rows.columnInt64(1));
	EXPECT_EQ(0x1234, rows.columnInt64(2));
	EXPECT_FALSE(rows.columnIsNull(3));
	EXPECT_EQ(0x8c010002, rows.columnInt64(3));
	ASSERT_TRUE(rows.step()); // MemoryWrite
	EXPECT_EQ(11, rows.columnInt64(0));
	EXPECT_EQ(0, rows.columnInt64(1));
	EXPECT_EQ(0x8c001000, rows.columnInt64(4));
	EXPECT_EQ(0xdeadbeef, rows.columnInt64(5));
	ASSERT_TRUE(rows.step()); // Call
	EXPECT_EQ(12, rows.columnInt64(0));
	EXPECT_EQ(static_cast<int>(research::Sh4CallKind::Jsr), rows.columnInt64(6));
	EXPECT_EQ(0x8c020000, rows.columnInt64(7));
	EXPECT_EQ(0x8c010004, rows.columnInt64(8));
	ASSERT_TRUE(rows.step()); // Exception
	EXPECT_EQ(13, rows.columnInt64(0));
	EXPECT_TRUE(rows.columnIsNull(4));
	EXPECT_TRUE(rows.columnIsNull(6));
	EXPECT_FALSE(rows.step());
	rows.reset();
}

TEST_F(WorkbenchRecorderTest, RecorderRecordsMapleAndFiltersByBus)
{
	wb::RecorderConfig config;
	config.buses = wb::BusMaple;
	wb::WorkbenchRecorder recorder;
	ASSERT_NO_THROW(recorder.start(databasePath(), config, testRun()));
	EXPECT_TRUE(research::mapleObservationBusActive());
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());

	const std::uint64_t dma = research::beginMapleObservationDma();
	EXPECT_NE(UINT64_MAX, dma);
	EXPECT_TRUE(research::publishMapleTransactionObservations(dma, mapleTransaction()));
	// No SH-4 subscriber exists, so this is silently ignored by the bus.
	EXPECT_FALSE(research::publishSh4Observation(
			sh4Event(research::Sh4ObservationType::Call, 5)));

	ASSERT_NO_THROW(recorder.stop());
	EXPECT_FALSE(research::mapleObservationBusActive());
	EXPECT_EQ(2u, recorder.status().written);

	wb::Database db(databasePath());
	EXPECT_EQ(1, tableCount(db, "maple_events"));
	EXPECT_EQ(0, tableCount(db, "sh4_events"));
	EXPECT_EQ(2, db.queryInt64("SELECT COUNT(*) FROM maple_events"));
	EXPECT_EQ(2, db.queryInt64("SELECT COUNT(*) FROM maple_events WHERE bus = 1 AND port = 2"
			" AND command = 9 AND tick = 1234 AND length(payload) = 4"));
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(DISTINCT transaction_ordinal) FROM maple_events"));
	EXPECT_EQ(2, db.queryInt64("SELECT COUNT(DISTINCT type) FROM maple_events"));
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM runs WHERE written = 2"));
}

TEST_F(WorkbenchRecorderTest, RecorderRejectsDoubleStartAndEmptyBuses)
{
	wb::RecorderConfig config;
	config.buses = wb::BusSh4;
	wb::WorkbenchRecorder recorder;
	ASSERT_NO_THROW(recorder.start(databasePath(), config, testRun()));
	EXPECT_THROW(recorder.start(directory / "second.sqlite", config, testRun()), std::logic_error);
	EXPECT_TRUE(recorder.active());
	EXPECT_FALSE(std::filesystem::exists(directory / "second.sqlite"));
	ASSERT_NO_THROW(recorder.stop());
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());

	wb::WorkbenchRecorder empty;
	wb::RecorderConfig none;
	none.buses = 0;
	EXPECT_THROW(empty.start(directory / "empty.sqlite", none, testRun()), std::invalid_argument);
	EXPECT_FALSE(empty.active());
	EXPECT_NO_THROW(empty.stop());
	EXPECT_FALSE(empty.status().active);
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
	EXPECT_FALSE(research::mapleObservationBusActive());
}

TEST_F(WorkbenchRecorderTest, WriterErrorIsReportedNotThrown)
{
	wb::RecorderConfig config;
	config.buses = wb::BusSh4;
	wb::WorkbenchRecorder recorder;
	ASSERT_NO_THROW(recorder.start(databasePath(), config, testRun()));
	{
		// Sabotage the schema behind the writer's back; its next insert fails.
		wb::Database saboteur(databasePath());
		saboteur.exec("DROP TABLE sh4_events");
	}
	EXPECT_TRUE(research::publishSh4Observation(
			sh4Event(research::Sh4ObservationType::Call, 21)));
	ASSERT_NO_THROW(recorder.stop());
	EXPECT_FALSE(recorder.active());
	EXPECT_FALSE(recorder.status().error.empty());
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());

	wb::Database db(databasePath());
	EXPECT_EQ(0, tableCount(db, "sh4_events"));
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM runs WHERE stopped_utc IS NOT NULL"
			" AND written = 0 AND note LIKE '%writer error%'"));
}

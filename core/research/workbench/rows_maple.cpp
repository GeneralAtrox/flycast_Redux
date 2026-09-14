#include "research/workbench/workbench_rows.h"

#include <string>

namespace research::workbench
{

namespace
{

// Bind parameter positions in the maple_events INSERT (1-based).
constexpr int ParamRun = 1;
constexpr int ParamOrdinal = 2;
constexpr int ParamDmaOrdinal = 3;
constexpr int ParamTransactionOrdinal = 4;
constexpr int ParamType = 5;
constexpr int ParamTick = 6;
constexpr int ParamDescriptorAddr = 7;
constexpr int ParamDestAddr = 8;
constexpr int ParamHeader1 = 9;
constexpr int ParamHeader2 = 10;
constexpr int ParamDeviceType = 11;
constexpr int ParamBus = 12;
constexpr int ParamPort = 13;
constexpr int ParamCommand = 14;
constexpr int ParamFlags = 15;
constexpr int ParamPayload = 16;

const std::string MapleEventsCreate =
		std::string("CREATE TABLE IF NOT EXISTS maple_events(")
		+ "id INTEGER PRIMARY KEY, run INTEGER, ordinal INTEGER, dma_ordinal INTEGER,"
		+ " transaction_ordinal INTEGER, type INTEGER, tick INTEGER,"
		+ " descriptor_addr INTEGER, dest_addr INTEGER, header1 INTEGER, header2 INTEGER,"
		+ " device_type INTEGER, bus INTEGER, port INTEGER, command INTEGER, flags INTEGER,"
		+ " payload BLOB)";

const std::string MapleEventsInsert =
		std::string("INSERT INTO maple_events(")
		+ "run, ordinal, dma_ordinal, transaction_ordinal, type, tick,"
		+ " descriptor_addr, dest_addr, header1, header2,"
		+ " device_type, bus, port, command, flags, payload)"
		+ " VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

} // namespace

MapleRows::MapleRows(Database& db, std::int64_t run, const RowOptions& options)
	: run(run), insert(db.prepare(MapleEventsInsert))
{
	(void)options;
}

void MapleRows::createTables(Database& db)
{
	db.exec(MapleEventsCreate);
}

void MapleRows::createIndexes(Database& db)
{
	db.exec("CREATE INDEX IF NOT EXISTS maple_events_tick ON maple_events(tick)");
	db.exec("CREATE INDEX IF NOT EXISTS maple_events_bus_port ON maple_events(bus, port)");
	db.exec("CREATE INDEX IF NOT EXISTS maple_events_command ON maple_events(command)");
	db.exec("CREATE INDEX IF NOT EXISTS maple_events_dma_ordinal ON maple_events(dma_ordinal)");
}

void MapleRows::write(const MapleObservation& observation)
{
	insert.bindInt(ParamRun, run)
			.bindUInt(ParamOrdinal, observation.emissionOrdinal)
			.bindUInt(ParamDmaOrdinal, observation.dmaOrdinal)
			.bindUInt(ParamTransactionOrdinal, observation.transactionOrdinal)
			.bindInt(ParamType, static_cast<int>(observation.type))
			.bindUInt(ParamTick, observation.tick)
			.bindUInt(ParamDescriptorAddr, observation.descriptorAddress)
			.bindUInt(ParamDestAddr, observation.destinationAddress)
			.bindUInt(ParamHeader1, observation.descriptorHeader1)
			.bindUInt(ParamHeader2, observation.descriptorHeader2)
			.bindOptionalU32(ParamDeviceType, observation.deviceType)
			.bindUInt(ParamBus, observation.bus)
			.bindUInt(ParamPort, observation.port)
			.bindUInt(ParamCommand, observation.command)
			.bindUInt(ParamFlags, observation.flags)
			.bindBlob(ParamPayload, observation.payload.data(), observation.payload.size());
	insert.execute();
}

} // namespace research::workbench

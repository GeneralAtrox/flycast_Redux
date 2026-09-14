// GdromRows: the REIOS HLE GD-ROM path (gdrom_events) and the ATA/ATAPI
// hardware path (gdrom_hw_events). Both tables are wide, so the INSERT text
// and the bind chains carry a running 1-based parameter index per column.

#include "research/workbench/workbench_rows.h"

#include <cstddef>

namespace research::workbench
{

namespace
{

constexpr const char *InsertHleSql =
		"INSERT INTO gdrom_events("
		"run, ordinal, type, tick, command_gen, path,"                        // 1-6
		" init_gen, init_pc, init_pr, init_opcode, init_backend, init_depth," // 7-12
		" request_id, command, p0, p1, p2, p3,"                               // 13-18
		" chunk_ordinal, fad, sector_count, destination,"                     // 19-22
		" byte_count, bytes, completion, transferred_bytes)"                  // 23-26
		" VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
		" ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

constexpr const char *InsertHardwareSql =
		"INSERT INTO gdrom_hw_events("
		"run, ordinal, type, tick, command_gen, dma_gen,"                     // 1-6
		" ata_gen, ata_pc, ata_pr, ata_opcode, ata_backend, ata_depth,"       // 7-12
		" pkt_gen, pkt_pc, pkt_pr, pkt_opcode, pkt_backend, pkt_depth,"       // 13-18
		" packet, features, byte_count_reg, drive_state,"                     // 19-22
		" start_fad, sector_count, sector_bytes, delivery,"                   // 23-26
		" read_successful, stream_offset, destination,"                       // 27-29
		" dma_star, dma_length, dma_direction, dma_enabled,"                  // 30-33
		" pio_word, status_reg, byte_count, bytes,"                           // 34-37
		" abort_reason, transferred_bytes)"                                   // 38-39
		" VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
		" ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

} // namespace

GdromRows::GdromRows(Database& db, std::int64_t run, const RowOptions& options)
	: run(run),
	  options(options),
	  insertHle(db.prepare(InsertHleSql)),
	  insertHardware(db.prepare(InsertHardwareSql))
{
}

void GdromRows::createTables(Database& db)
{
	db.exec("CREATE TABLE IF NOT EXISTS gdrom_events("
			"id INTEGER PRIMARY KEY, run INTEGER, ordinal INTEGER, type INTEGER, tick INTEGER,"
			" command_gen INTEGER, path INTEGER,"
			" init_gen INTEGER, init_pc INTEGER, init_pr INTEGER, init_opcode INTEGER,"
			" init_backend INTEGER, init_depth INTEGER,"
			" request_id INTEGER, command INTEGER, p0 INTEGER, p1 INTEGER, p2 INTEGER, p3 INTEGER,"
			" chunk_ordinal INTEGER, fad INTEGER, sector_count INTEGER, destination INTEGER,"
			" byte_count INTEGER, bytes BLOB, completion INTEGER, transferred_bytes INTEGER)");
	db.exec("CREATE TABLE IF NOT EXISTS gdrom_hw_events("
			"id INTEGER PRIMARY KEY, run INTEGER, ordinal INTEGER, type INTEGER, tick INTEGER,"
			" command_gen INTEGER, dma_gen INTEGER,"
			" ata_gen INTEGER, ata_pc INTEGER, ata_pr INTEGER, ata_opcode INTEGER,"
			" ata_backend INTEGER, ata_depth INTEGER,"
			" pkt_gen INTEGER, pkt_pc INTEGER, pkt_pr INTEGER, pkt_opcode INTEGER,"
			" pkt_backend INTEGER, pkt_depth INTEGER,"
			" packet BLOB, features INTEGER, byte_count_reg INTEGER, drive_state INTEGER,"
			" start_fad INTEGER, sector_count INTEGER, sector_bytes INTEGER, delivery INTEGER,"
			" read_successful INTEGER, stream_offset INTEGER, destination INTEGER,"
			" dma_star INTEGER, dma_length INTEGER, dma_direction INTEGER, dma_enabled INTEGER,"
			" pio_word INTEGER, status_reg INTEGER, byte_count INTEGER, bytes BLOB,"
			" abort_reason INTEGER, transferred_bytes INTEGER)");
}

void GdromRows::createIndexes(Database& db)
{
	db.exec("CREATE INDEX IF NOT EXISTS gdrom_events_tick ON gdrom_events(tick)");
	db.exec("CREATE INDEX IF NOT EXISTS gdrom_events_command_gen ON gdrom_events(command_gen)");
	db.exec("CREATE INDEX IF NOT EXISTS gdrom_events_fad ON gdrom_events(fad)");
	db.exec("CREATE INDEX IF NOT EXISTS gdrom_events_type ON gdrom_events(type)");
	db.exec("CREATE INDEX IF NOT EXISTS gdrom_hw_events_tick ON gdrom_hw_events(tick)");
	db.exec("CREATE INDEX IF NOT EXISTS gdrom_hw_events_command_gen"
			" ON gdrom_hw_events(command_gen)");
	db.exec("CREATE INDEX IF NOT EXISTS gdrom_hw_events_start_fad ON gdrom_hw_events(start_fad)");
	db.exec("CREATE INDEX IF NOT EXISTS gdrom_hw_events_type ON gdrom_hw_events(type)");
	db.exec("CREATE INDEX IF NOT EXISTS gdrom_hw_events_ata_pc"
			" ON gdrom_hw_events(ata_pc) WHERE ata_pc IS NOT NULL");
}

void GdromRows::write(const GdromObservation& observation)
{
	insertHle.bindInt(1, run)
			.bindUInt(2, observation.emissionOrdinal)
			.bindInt(3, static_cast<int>(observation.type))
			.bindUInt(4, observation.tick)
			.bindGeneration(5, observation.commandGeneration)
			.bindInt(6, static_cast<int>(observation.path));
	bindOwnerToken(insertHle, 7, observation.initiator);                    // 7-12
	insertHle.bindUInt(13, observation.requestId)
			.bindUInt(14, observation.command)
			.bindUInt(15, observation.parameters[0])
			.bindUInt(16, observation.parameters[1])
			.bindUInt(17, observation.parameters[2])
			.bindUInt(18, observation.parameters[3])
			.bindUInt(19, observation.chunkOrdinal)
			.bindUInt(20, observation.fad)
			.bindUInt(21, observation.sectorCount)
			.bindUInt(22, observation.destination)
			.bindUInt(23, observation.bytes.size());
	if (options.storeSectorBytes)
		insertHle.bindBlob(24, observation.bytes.data(), observation.bytes.size());
	else
		insertHle.bindNull(24);
	insertHle.bindInt(25, static_cast<int>(observation.completion))
			.bindUInt(26, observation.transferredBytes);
	insertHle.execute();
}

void GdromRows::write(const GdromHardwareObservation& observation)
{
	insertHardware.bindInt(1, run)
			.bindUInt(2, observation.emissionOrdinal)
			.bindInt(3, static_cast<int>(observation.type))
			.bindUInt(4, observation.tick)
			.bindGeneration(5, observation.commandGeneration)
			.bindGeneration(6, observation.dmaGeneration);
	bindOwnerToken(insertHardware, 7, observation.ataOwner);                // 7-12
	bindOwnerToken(insertHardware, 13, observation.packetOwner);            // 13-18
	// The 12-byte ATAPI packet is only meaningful on PacketAccepted.
	if (observation.type == GdromHardwareObservationType::PacketAccepted)
		insertHardware.bindBlob(19, observation.packet.data(), observation.packet.size());
	else
		insertHardware.bindNull(19);
	insertHardware.bindUInt(20, observation.features)
			.bindUInt(21, observation.byteCountRegister)
			.bindUInt(22, observation.driveState)
			.bindUInt(23, observation.startFad)
			.bindUInt(24, observation.sectorCount)
			.bindUInt(25, observation.sectorBytes)
			.bindInt(26, static_cast<int>(observation.delivery))
			.bindBool(27, observation.readSuccessful)
			.bindUInt(28, observation.streamOffset)
			.bindUInt(29, observation.destination)
			.bindUInt(30, observation.dmaStar)
			.bindUInt(31, observation.dmaLength)
			.bindUInt(32, observation.dmaDirection)
			.bindUInt(33, observation.dmaEnabled)
			.bindUInt(34, observation.pioWord)
			.bindUInt(35, observation.statusRegister)
			.bindUInt(36, observation.bytes.size());
	if (options.storeSectorBytes)
		insertHardware.bindBlob(37, observation.bytes.data(), observation.bytes.size());
	else
		insertHardware.bindNull(37);
	insertHardware.bindInt(38, static_cast<int>(observation.abortReason))
			.bindUInt(39, observation.transferredBytes);
	insertHardware.execute();
}

} // namespace research::workbench

// AudioRows: AICA bus events (aica_events) and CD-DA control/sector events
// (cdda_events). CD-DA sectors join AICA frames through cdda_gen/aica_gen.
// Both tables are wide, so the INSERT text and the bind chains carry a
// running 1-based parameter index per column.

#include "research/workbench/workbench_rows.h"

#include <cstddef>

namespace research::workbench
{

namespace
{

constexpr const char *InsertAicaSql =
		"INSERT INTO aica_events("
		"run, ordinal, type, tick, writer,"                                   // 1-5
		" sh4_gen, sh4_pc, sh4_pr, sh4_opcode, sh4_backend, sh4_depth,"       // 6-11
		" arm7_pc, address, width, value, byte_count, bytes,"                 // 12-17
		" dma_gen, source_addr, dest_addr, transfer_length, ram_is_dest,"     // 18-22
		" channel, channel_regs, key_on_mask, key_off_mask, sample_cut_ordinal," // 23-27
		" cdda_gen, cdda_fad, cdda_status, cdda_repeats, cdda_read_ok,"       // 28-32
		" cdda_frame_index, suppression, sample_ordinal, active_channel_mask," // 33-36
		" dry_l, dry_r, cdda_in_l, cdda_in_r, cdda_c_l, cdda_c_r,"            // 37-42
		" dsp_enabled, dsp_c_l, dsp_c_r, dsp_inputs, dsp_effect_outputs,"     // 43-47
		" final_l, final_r)"                                                  // 48-49
		" VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
		" ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
		" ?, ?, ?, ?, ?, ?, ?, ?, ?)";

constexpr const char *InsertCddaSql =
		"INSERT INTO cdda_events("
		"run, ordinal, type, tick, control_gen, path,"                        // 1-6
		" init_gen, init_pc, init_pr, init_opcode, init_backend, init_depth," // 7-12
		" request_id, command, p0, p1, p2, p3,"                               // 13-18
		" before_status, before_repeats, before_current_fad,"                 // 19-21
		" before_start_fad, before_end_fad,"                                  // 22-23
		" after_status, after_repeats, after_current_fad,"                    // 24-26
		" after_start_fad, after_end_fad,"                                    // 27-28
		" applied_ok, aica_gen, fad, read_ok, byte_count, bytes)"             // 29-34
		" VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
		" ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

// Small RAM/register payloads are always kept; large ones (DMA bursts, CD-DA
// sectors) only when sector bytes were requested.
constexpr std::size_t AlwaysStoredByteLimit = 64;

} // namespace

AudioRows::AudioRows(Database& db, std::int64_t run, const RowOptions& options)
	: run(run),
	  options(options),
	  insertAica(db.prepare(InsertAicaSql)),
	  insertCdda(db.prepare(InsertCddaSql))
{
}

void AudioRows::createTables(Database& db)
{
	db.exec("CREATE TABLE IF NOT EXISTS aica_events("
			"id INTEGER PRIMARY KEY, run INTEGER, ordinal INTEGER, type INTEGER, tick INTEGER,"
			" writer INTEGER,"
			" sh4_gen INTEGER, sh4_pc INTEGER, sh4_pr INTEGER, sh4_opcode INTEGER,"
			" sh4_backend INTEGER, sh4_depth INTEGER, arm7_pc INTEGER,"
			" address INTEGER, width INTEGER, value INTEGER, byte_count INTEGER, bytes BLOB,"
			" dma_gen INTEGER, source_addr INTEGER, dest_addr INTEGER, transfer_length INTEGER,"
			" ram_is_dest INTEGER,"
			" channel INTEGER, channel_regs BLOB, key_on_mask INTEGER, key_off_mask INTEGER,"
			" sample_cut_ordinal INTEGER,"
			" cdda_gen INTEGER, cdda_fad INTEGER, cdda_status INTEGER, cdda_repeats INTEGER,"
			" cdda_read_ok INTEGER, cdda_frame_index INTEGER, suppression INTEGER,"
			" sample_ordinal INTEGER, active_channel_mask INTEGER,"
			" dry_l INTEGER, dry_r INTEGER, cdda_in_l INTEGER, cdda_in_r INTEGER,"
			" cdda_c_l INTEGER, cdda_c_r INTEGER,"
			" dsp_enabled INTEGER, dsp_c_l INTEGER, dsp_c_r INTEGER,"
			" dsp_inputs BLOB, dsp_effect_outputs BLOB, final_l INTEGER, final_r INTEGER)");
	db.exec("CREATE TABLE IF NOT EXISTS cdda_events("
			"id INTEGER PRIMARY KEY, run INTEGER, ordinal INTEGER, type INTEGER, tick INTEGER,"
			" control_gen INTEGER, path INTEGER,"
			" init_gen INTEGER, init_pc INTEGER, init_pr INTEGER, init_opcode INTEGER,"
			" init_backend INTEGER, init_depth INTEGER,"
			" request_id INTEGER, command INTEGER, p0 INTEGER, p1 INTEGER, p2 INTEGER, p3 INTEGER,"
			" before_status INTEGER, before_repeats INTEGER, before_current_fad INTEGER,"
			" before_start_fad INTEGER, before_end_fad INTEGER,"
			" after_status INTEGER, after_repeats INTEGER, after_current_fad INTEGER,"
			" after_start_fad INTEGER, after_end_fad INTEGER,"
			" applied_ok INTEGER, aica_gen INTEGER, fad INTEGER, read_ok INTEGER,"
			" byte_count INTEGER, bytes BLOB)");
}

void AudioRows::createIndexes(Database& db)
{
	db.exec("CREATE INDEX IF NOT EXISTS aica_events_tick ON aica_events(tick)");
	db.exec("CREATE INDEX IF NOT EXISTS aica_events_type ON aica_events(type)");
	db.exec("CREATE INDEX IF NOT EXISTS aica_events_address"
			" ON aica_events(address) WHERE address IS NOT NULL");
	db.exec("CREATE INDEX IF NOT EXISTS aica_events_channel ON aica_events(channel)");
	db.exec("CREATE INDEX IF NOT EXISTS aica_events_sample_ordinal ON aica_events(sample_ordinal)");
	db.exec("CREATE INDEX IF NOT EXISTS aica_events_cdda_gen ON aica_events(cdda_gen)");
	db.exec("CREATE INDEX IF NOT EXISTS cdda_events_tick ON cdda_events(tick)");
	db.exec("CREATE INDEX IF NOT EXISTS cdda_events_control_gen ON cdda_events(control_gen)");
	db.exec("CREATE INDEX IF NOT EXISTS cdda_events_aica_gen ON cdda_events(aica_gen)");
	db.exec("CREATE INDEX IF NOT EXISTS cdda_events_fad ON cdda_events(fad)");
}

void AudioRows::write(const AicaObservation& observation)
{
	const bool keyTransition = observation.type == AicaObservationType::KeyOn
			|| observation.type == AicaObservationType::KeyOff;
	const bool sampleFrame = observation.type == AicaObservationType::SampleFrame;
	const bool storeBytes = observation.bytes.size() <= AlwaysStoredByteLimit
			|| options.storeSectorBytes;

	insertAica.bindInt(1, run)
			.bindUInt(2, observation.emissionOrdinal)
			.bindInt(3, static_cast<int>(observation.type))
			.bindUInt(4, observation.tick)
			.bindInt(5, static_cast<int>(observation.owner.writer));
	bindOwnerToken(insertAica, 6, observation.owner.sh4);                   // 6-11
	if (observation.owner.arm7PcAvailable)
		insertAica.bindUInt(12, observation.owner.arm7Pc);
	else
		insertAica.bindNull(12);
	insertAica.bindUInt(13, observation.address)
			.bindUInt(14, observation.width)
			.bindUInt(15, observation.value)
			.bindUInt(16, observation.bytes.size());
	if (storeBytes)
		insertAica.bindBlob(17, observation.bytes.data(), observation.bytes.size());
	else
		insertAica.bindNull(17);
	insertAica.bindGeneration(18, observation.dmaGeneration)
			.bindUInt(19, observation.sourceAddress)
			.bindUInt(20, observation.destinationAddress)
			.bindUInt(21, observation.transferLength)
			.bindBool(22, observation.aicaRamIsDestination)
			.bindUInt(23, observation.channel);
	if (keyTransition)
		insertAica.bindBlob(24, observation.channelRegisters.data(),
				observation.channelRegisters.size());
	else
		insertAica.bindNull(24);
	insertAica.bindUInt(25, observation.keyOnMask)
			.bindUInt(26, observation.keyOffMask)
			.bindUInt(27, observation.sampleCutOrdinal)
			.bindGeneration(28, observation.cddaGeneration)
			.bindUInt(29, observation.cddaFad)
			.bindUInt(30, observation.cddaStatus)
			.bindUInt(31, observation.cddaRepeats)
			.bindBool(32, observation.cddaReadSuccessful)
			.bindUInt(33, observation.cddaFrameIndex)
			.bindInt(34, static_cast<int>(observation.suppression))
			.bindUInt(35, observation.sampleOrdinal)
			.bindUInt(36, observation.activeChannelMask)
			.bindInt(37, observation.dryLeft)
			.bindInt(38, observation.dryRight)
			.bindInt(39, observation.cddaInputLeft)
			.bindInt(40, observation.cddaInputRight)
			.bindInt(41, observation.cddaContributionLeft)
			.bindInt(42, observation.cddaContributionRight)
			.bindBool(43, observation.dspEnabled)
			.bindInt(44, observation.dspContributionLeft)
			.bindInt(45, observation.dspContributionRight);
	// Raw little-endian arrays: 16 x int32 inputs, 16 x int16 effect outputs.
	if (sampleFrame)
		insertAica.bindBlob(46, observation.dspInputs.data(),
						observation.dspInputs.size() * sizeof(std::int32_t))
				.bindBlob(47, observation.dspEffectOutputs.data(),
						observation.dspEffectOutputs.size() * sizeof(std::int16_t));
	else
		insertAica.bindNull(46).bindNull(47);
	insertAica.bindInt(48, observation.finalLeft)
			.bindInt(49, observation.finalRight);
	insertAica.execute();
}

void AudioRows::write(const CddaObservation& observation)
{
	insertCdda.bindInt(1, run)
			.bindUInt(2, observation.emissionOrdinal)
			.bindInt(3, static_cast<int>(observation.type))
			.bindUInt(4, observation.tick)
			.bindGeneration(5, observation.controlGeneration)
			.bindInt(6, static_cast<int>(observation.path));
	bindOwnerToken(insertCdda, 7, observation.initiator);                   // 7-12
	insertCdda.bindUInt(13, observation.requestId)
			.bindUInt(14, observation.command)
			.bindUInt(15, observation.parameters[0])
			.bindUInt(16, observation.parameters[1])
			.bindUInt(17, observation.parameters[2])
			.bindUInt(18, observation.parameters[3])
			.bindUInt(19, observation.before.status)
			.bindUInt(20, observation.before.repeats)
			.bindUInt(21, observation.before.currentFad)
			.bindUInt(22, observation.before.startFad)
			.bindUInt(23, observation.before.endFad)
			.bindUInt(24, observation.after.status)
			.bindUInt(25, observation.after.repeats)
			.bindUInt(26, observation.after.currentFad)
			.bindUInt(27, observation.after.startFad)
			.bindUInt(28, observation.after.endFad)
			.bindBool(29, observation.appliedSuccessfully)
			.bindGeneration(30, observation.aicaGeneration)
			.bindUInt(31, observation.fad)
			.bindBool(32, observation.readSuccessful)
			.bindUInt(33, observation.bytes.size());
	if (options.storeSectorBytes)
		insertCdda.bindBlob(34, observation.bytes.data(), observation.bytes.size());
	else
		insertCdda.bindNull(34);
	insertCdda.execute();
}

} // namespace research::workbench

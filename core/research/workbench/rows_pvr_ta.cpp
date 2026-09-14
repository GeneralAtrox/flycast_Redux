// PvrTaRows: one row per TA observation plus two child tables for the
// StartRender fan-out (selected contexts and the render-selection VRAM reads).

#include "research/workbench/workbench_rows.h"

#include <algorithm>
#include <cstddef>

namespace research::workbench
{

namespace
{

constexpr const char *InsertEventSql =
		"INSERT INTO pvr_ta_events("
		"run, ordinal, type, tick,"                                            // 1-4
		" init_gen, init_pc, init_pr, init_opcode, init_backend, init_depth,"  // 5-10
		" ctx_addr, ctx_gen, ctx_block_ordinal, render_pass,"                  // 11-14
		" list_before, list_after, parser_before, parser_after,"               // 15-18
		" source, source_addr, ta_addr, block,"                                // 19-22
		" render_gen, render_ctx_available, region_base, fpu_param_cfg,"       // 23-26
		" selected_count, selection_read_count)"                               // 27-28
		" VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
		" ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

// The row binder holds no database handle, so the parent id is taken from the
// table itself: this connection is the only writer and `id` is the rowid
// alias, so MAX(id) is the event inserted immediately before (an O(log n)
// rightmost-leaf descent, not a scan).
constexpr const char *InsertSelectedContextSql =
		"INSERT INTO pvr_ta_selected_contexts(event_id, idx, ctx_addr, ctx_gen, available)"
		" VALUES((SELECT MAX(id) FROM pvr_ta_events), ?, ?, ?, ?)";          // 1-4

constexpr const char *InsertSelectionReadSql =
		"INSERT INTO pvr_ta_selection_reads(event_id, idx, address, value)"
		" VALUES((SELECT MAX(id) FROM pvr_ta_events), ?, ?, ?)";             // 1-3

} // namespace

PvrTaRows::PvrTaRows(Database& db, std::int64_t run, const RowOptions&)
	: run(run),
	  insert(db.prepare(InsertEventSql)),
	  insertSelectedContext(db.prepare(InsertSelectedContextSql)),
	  insertSelectionRead(db.prepare(InsertSelectionReadSql))
{
}

void PvrTaRows::createTables(Database& db)
{
	db.exec("CREATE TABLE IF NOT EXISTS pvr_ta_events("
			"id INTEGER PRIMARY KEY, run INTEGER, ordinal INTEGER, type INTEGER, tick INTEGER,"
			" init_gen INTEGER, init_pc INTEGER, init_pr INTEGER, init_opcode INTEGER,"
			" init_backend INTEGER, init_depth INTEGER,"
			" ctx_addr INTEGER, ctx_gen INTEGER, ctx_block_ordinal INTEGER, render_pass INTEGER,"
			" list_before INTEGER, list_after INTEGER, parser_before INTEGER, parser_after INTEGER,"
			" source INTEGER, source_addr INTEGER, ta_addr INTEGER, block BLOB,"
			" render_gen INTEGER, render_ctx_available INTEGER, region_base INTEGER,"
			" fpu_param_cfg INTEGER, selected_count INTEGER, selection_read_count INTEGER)");
	db.exec("CREATE TABLE IF NOT EXISTS pvr_ta_selected_contexts("
			"event_id INTEGER, idx INTEGER, ctx_addr INTEGER, ctx_gen INTEGER, available INTEGER)");
	db.exec("CREATE TABLE IF NOT EXISTS pvr_ta_selection_reads("
			"event_id INTEGER, idx INTEGER, address INTEGER, value INTEGER)");
}

void PvrTaRows::createIndexes(Database& db)
{
	db.exec("CREATE INDEX IF NOT EXISTS pvr_ta_events_tick ON pvr_ta_events(tick)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_ta_events_render_gen ON pvr_ta_events(render_gen)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_ta_events_init_pc ON pvr_ta_events(init_pc)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_ta_events_ctx_gen ON pvr_ta_events(ctx_gen)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_ta_events_type ON pvr_ta_events(type)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_ta_selected_contexts_event"
			" ON pvr_ta_selected_contexts(event_id)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_ta_selection_reads_event"
			" ON pvr_ta_selection_reads(event_id)");
}

void PvrTaRows::write(const PvrTaObservation& observation)
{
	// readCount is bounded by the transcript array; clamp defensively anyway.
	const std::size_t readCount = std::min(observation.renderSelectionReadCount,
			observation.renderSelectionReads.size());

	insert.bindInt(1, run)                                           // 1  run
			.bindUInt(2, observation.emissionOrdinal)                // 2  ordinal
			.bindInt(3, static_cast<int>(observation.type))          // 3  type
			.bindUInt(4, observation.tick);                          // 4  tick
	bindOwnerToken(insert, 5, observation.initiator);                // 5-10 init_*
	insert.bindOptionalU32(11, observation.contextAddress)           // 11 ctx_addr
			.bindGeneration(12, observation.contextGeneration)       // 12 ctx_gen
			.bindUInt(13, observation.contextBlockOrdinal)           // 13 ctx_block_ordinal
			.bindUInt(14, observation.renderPass)                    // 14 render_pass
			.bindOptionalU32(15, observation.listTypeBefore)         // 15 list_before
			.bindOptionalU32(16, observation.listTypeAfter)          // 16 list_after
			.bindOptionalU32(17, observation.parserStateBefore)      // 17 parser_before
			.bindOptionalU32(18, observation.parserStateAfter)       // 18 parser_after
			.bindInt(19, static_cast<int>(observation.source))       // 19 source
			.bindOptionalU32(20, observation.sourceAddress)          // 20 source_addr
			.bindOptionalU32(21, observation.taAddress);             // 21 ta_addr
	if (observation.type == PvrTaObservationType::AcceptedBlock)
		insert.bindBlob(22, observation.block.data(), observation.block.size()); // 22 block
	else
		insert.bindNull(22);                                         // 22 block
	insert.bindGeneration(23, observation.renderGeneration)          // 23 render_gen
			.bindBool(24, observation.renderContextAvailable)        // 24 render_ctx_available
			.bindOptionalU32(25, observation.regionBase)             // 25 region_base
			.bindOptionalU32(26, observation.fpuParamCfg)            // 26 fpu_param_cfg
			.bindUInt(27, observation.selectedContexts.size())       // 27 selected_count
			.bindUInt(28, readCount)                                 // 28 selection_read_count
			.execute();

	std::int64_t index = 0;
	for (const PvrTaContextRef& context : observation.selectedContexts)
	{
		insertSelectedContext.bindInt(1, index++)                    // 1 idx
				.bindOptionalU32(2, context.address)                 // 2 ctx_addr
				.bindGeneration(3, context.generation)               // 3 ctx_gen
				.bindBool(4, context.available)                      // 4 available
				.execute();
	}

	for (std::size_t i = 0; i < readCount; i++)
	{
		const PvrTaVramRead& read = observation.renderSelectionReads[i];
		insertSelectionRead.bindInt(1, static_cast<std::int64_t>(i))  // 1 idx
				.bindOptionalU32(2, read.address)                    // 2 address
				.bindUInt(3, read.value)                             // 3 value
				.execute();
	}
}

} // namespace research::workbench

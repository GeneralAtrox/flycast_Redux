// PvrPresentationRows: one row per PVR presentation-path observation (register
// writes, VRAM writes, render lifecycle, framebuffer captures, presentation).
// The wide row is deliberately denormalised: every event type shares one table
// so a tick-ordered read of the presentation path needs no joins.

#include "research/workbench/workbench_rows.h"

#include <cstddef>

namespace research::workbench
{

namespace
{

constexpr const char *InsertEventSql =
		"INSERT INTO pvr_present_events("
		"run, ordinal, type, tick,"                                            // 1-4
		" init_gen, init_pc, init_pr, init_opcode, init_backend, init_depth,"  // 5-10
		" reg_paddr, reg_addr, requested, previous, effective, disposition,"   // 11-16
		" vram_source, logical_addr, physical_addr, byte_count, bytes,"        // 17-21
		" render_gen, render_kind, successful, fb_write_addr,"                 // 22-25
		" fb_gen, fb_source_render_gen, fb_kind, fb_w, fb_h, fb_row_bytes,"    // 26-31
		" fb_digest,"                                                          // 32
		" present_gen, present_source, source_gen,"                            // 33-35
		" cfg_fb_r_size, cfg_fb_r_ctrl, cfg_spg_ctrl, cfg_spg_status,"         // 36-39
		" cfg_sof1, cfg_sof2, cfg_video_ctrl, cfg_border)"                     // 40-43
		" VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,"
		" ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

bool storePayload(const PvrPresentationObservation& observation, const RowOptions& options)
{
	switch (observation.type)
	{
	case PvrPresentationObservationType::VramWrite:
		return options.storeVramWriteBytes;
	case PvrPresentationObservationType::FramebufferCaptured:
		return options.storeFramebufferBytes;
	case PvrPresentationObservationType::InitialRegisterState:
		// The PVR register file snapshot is small and is the baseline every
		// later RegisterWrite row is relative to; always keep it.
		return true;
	default:
		return false;
	}
}

} // namespace

PvrPresentationRows::PvrPresentationRows(Database& db, std::int64_t run,
		const RowOptions& options)
	: run(run),
	  options(options),
	  insert(db.prepare(InsertEventSql))
{
}

void PvrPresentationRows::createTables(Database& db)
{
	db.exec("CREATE TABLE IF NOT EXISTS pvr_present_events("
			"id INTEGER PRIMARY KEY, run INTEGER, ordinal INTEGER, type INTEGER, tick INTEGER,"
			" init_gen INTEGER, init_pc INTEGER, init_pr INTEGER, init_opcode INTEGER,"
			" init_backend INTEGER, init_depth INTEGER,"
			" reg_paddr INTEGER, reg_addr INTEGER, requested INTEGER, previous INTEGER,"
			" effective INTEGER, disposition INTEGER,"
			" vram_source INTEGER, logical_addr INTEGER, physical_addr INTEGER,"
			" byte_count INTEGER, bytes BLOB,"
			" render_gen INTEGER, render_kind INTEGER, successful INTEGER, fb_write_addr INTEGER,"
			" fb_gen INTEGER, fb_source_render_gen INTEGER, fb_kind INTEGER,"
			" fb_w INTEGER, fb_h INTEGER, fb_row_bytes INTEGER, fb_digest BLOB,"
			" present_gen INTEGER, present_source INTEGER, source_gen INTEGER,"
			" cfg_fb_r_size INTEGER, cfg_fb_r_ctrl INTEGER, cfg_spg_ctrl INTEGER,"
			" cfg_spg_status INTEGER, cfg_sof1 INTEGER, cfg_sof2 INTEGER,"
			" cfg_video_ctrl INTEGER, cfg_border INTEGER)");
}

void PvrPresentationRows::createIndexes(Database& db)
{
	db.exec("CREATE INDEX IF NOT EXISTS pvr_present_events_tick ON pvr_present_events(tick)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_present_events_render_gen"
			" ON pvr_present_events(render_gen)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_present_events_reg_addr"
			" ON pvr_present_events(reg_addr) WHERE reg_addr IS NOT NULL");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_present_events_type ON pvr_present_events(type)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_present_events_physical_addr"
			" ON pvr_present_events(physical_addr) WHERE physical_addr IS NOT NULL");
}

void PvrPresentationRows::write(const PvrPresentationObservation& observation)
{
	const PvrFramebufferConfig& cfg = observation.framebufferConfig;

	insert.bindInt(1, run)                                           // 1  run
			.bindUInt(2, observation.emissionOrdinal)                // 2  ordinal
			.bindInt(3, static_cast<int>(observation.type))          // 3  type
			.bindUInt(4, observation.tick);                          // 4  tick
	bindOwnerToken(insert, 5, observation.initiator);                // 5-10 init_*
	insert.bindOptionalU32(11, observation.registerPhysicalAddress)  // 11 reg_paddr
			.bindOptionalU32(12, observation.registerAddress)        // 12 reg_addr
			.bindUInt(13, observation.requestedValue)                // 13 requested
			.bindUInt(14, observation.previousValue)                 // 14 previous
			.bindUInt(15, observation.effectiveValue)                // 15 effective
			.bindInt(16, static_cast<int>(observation.registerDisposition)) // 16 disposition
			.bindInt(17, static_cast<int>(observation.vramSource))   // 17 vram_source
			.bindOptionalU32(18, observation.logicalAddress)         // 18 logical_addr
			.bindOptionalU32(19, observation.physicalAddress)        // 19 physical_addr
			.bindUInt(20, observation.bytes.size());                 // 20 byte_count
	if (storePayload(observation, options))
		insert.bindBlob(21, observation.bytes.data(), observation.bytes.size()); // 21 bytes
	else
		insert.bindNull(21);                                         // 21 bytes
	insert.bindGeneration(22, observation.renderGeneration)          // 22 render_gen
			.bindInt(23, static_cast<int>(observation.renderKind))   // 23 render_kind
			.bindBool(24, observation.successful)                    // 24 successful
			.bindOptionalU32(25, observation.framebufferWriteAddress) // 25 fb_write_addr
			.bindGeneration(26, observation.framebufferGeneration)   // 26 fb_gen
			.bindGeneration(27, observation.framebufferSourceRenderGeneration) // 27 fb_source_render_gen
			.bindInt(28, static_cast<int>(observation.framebufferKind)) // 28 fb_kind
			.bindUInt(29, observation.framebufferWidth)              // 29 fb_w
			.bindUInt(30, observation.framebufferHeight)             // 30 fb_h
			.bindUInt(31, observation.framebufferRowBytes);          // 31 fb_row_bytes
	if (observation.framebufferDigestAvailable)
		insert.bindBlob(32, observation.framebufferDigest.data(),
				observation.framebufferDigest.size());               // 32 fb_digest
	else
		insert.bindNull(32);                                         // 32 fb_digest
	insert.bindGeneration(33, observation.presentationGeneration)    // 33 present_gen
			.bindInt(34, static_cast<int>(observation.presentationSource)) // 34 present_source
			.bindGeneration(35, observation.sourceGeneration)        // 35 source_gen
			.bindUInt(36, cfg.fbReadSize)                            // 36 cfg_fb_r_size
			.bindUInt(37, cfg.fbReadControl)                         // 37 cfg_fb_r_ctrl
			.bindUInt(38, cfg.spgControl)                            // 38 cfg_spg_ctrl
			.bindUInt(39, cfg.spgStatus)                             // 39 cfg_spg_status
			.bindUInt(40, cfg.fbReadSof1)                            // 40 cfg_sof1
			.bindUInt(41, cfg.fbReadSof2)                            // 41 cfg_sof2
			.bindUInt(42, cfg.videoControl)                          // 42 cfg_video_ctrl
			.bindUInt(43, cfg.borderColor)                           // 43 cfg_border
			.execute();
}

} // namespace research::workbench

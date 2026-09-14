#pragma once

// Row binders: one class per observation family. Each owns its prepared
// statements, creates its tables at recorder start, and adds its indexes when
// the recording is finalized (indexes are deferred so inserts stay fast).
//
// Conventions shared by every table:
//   run            INTEGER  -> runs.id
//   ordinal        INTEGER  bus emission ordinal (per bus, per process)
//   tick           INTEGER  SH-4 scheduler tick
//   init_gen/pc/pr/opcode/backend/depth  the Sh4InstructionOwnerToken, NULL when
//                                        the token was not valid
//   UINT32_MAX "absent" sentinels are stored as NULL (Statement::bindOptionalU32)
//   zero generations are stored as NULL (Statement::bindGeneration)

#include "research/aica_observation.h"
#include "research/cdda_observation.h"
#include "research/gdrom_hardware_observation.h"
#include "research/gdrom_observation.h"
#include "research/maple_observation.h"
#include "research/pvr_draw_observation.h"
#include "research/pvr_presentation_observation.h"
#include "research/pvr_ta_observation.h"
#include "research/sh4_observation.h"
#include "research/sh4_observation_runtime.h"
#include "research/workbench/workbench_config.h"
#include "research/workbench/workbench_db.h"

#include <cstdint>

namespace research::workbench
{

// Binds the six owner-token columns starting at `firstIndex`
// (init_gen, init_pc, init_pr, init_opcode, init_backend, init_depth).
void bindOwnerToken(Statement& statement, int firstIndex,
		const Sh4InstructionOwnerToken& token);

class Sh4Rows
{
public:
	Sh4Rows(Database& db, std::int64_t run, const RowOptions& options);
	void write(const Sh4Observation& observation);
	static void createTables(Database& db);
	static void createIndexes(Database& db);

private:
	std::int64_t run;
	Statement insert;
};

class MapleRows
{
public:
	MapleRows(Database& db, std::int64_t run, const RowOptions& options);
	void write(const MapleObservation& observation);
	static void createTables(Database& db);
	static void createIndexes(Database& db);

private:
	std::int64_t run;
	Statement insert;
};

class PvrTaRows
{
public:
	PvrTaRows(Database& db, std::int64_t run, const RowOptions& options);
	void write(const PvrTaObservation& observation);
	static void createTables(Database& db);
	static void createIndexes(Database& db);

private:
	std::int64_t run;
	Statement insert;
	Statement insertSelectedContext;
	Statement insertSelectionRead;
};

class PvrDrawRows
{
public:
	PvrDrawRows(Database& db, std::int64_t run, const RowOptions& options);
	void write(const PvrDrawObservation& observation);
	static void createTables(Database& db);
	static void createIndexes(Database& db);

private:
	std::int64_t run;
	RowOptions options;
	Statement insert;
	Statement insertVertex;
	Statement insertBlock;
	Statement insertTexture;
	Statement insertConsumedPrimitive;
};

class PvrPresentationRows
{
public:
	PvrPresentationRows(Database& db, std::int64_t run, const RowOptions& options);
	void write(const PvrPresentationObservation& observation);
	static void createTables(Database& db);
	static void createIndexes(Database& db);

private:
	std::int64_t run;
	RowOptions options;
	Statement insert;
};

// Both the REIOS HLE path and the ATA/ATAPI hardware path.
class GdromRows
{
public:
	GdromRows(Database& db, std::int64_t run, const RowOptions& options);
	void write(const GdromObservation& observation);
	void write(const GdromHardwareObservation& observation);
	static void createTables(Database& db);
	static void createIndexes(Database& db);

private:
	std::int64_t run;
	RowOptions options;
	Statement insertHle;
	Statement insertHardware;
};

// AICA and CD-DA share one binder because CD-DA sectors join AICA frames.
class AudioRows
{
public:
	AudioRows(Database& db, std::int64_t run, const RowOptions& options);
	void write(const AicaObservation& observation);
	void write(const CddaObservation& observation);
	static void createTables(Database& db);
	static void createIndexes(Database& db);

private:
	std::int64_t run;
	RowOptions options;
	Statement insertAica;
	Statement insertCdda;
};

} // namespace research::workbench

#include "research/workbench/workbench_rows.h"

#include <string>

namespace research::workbench
{

namespace
{

// Bind parameter positions in the sh4_events INSERT (1-based).
constexpr int ParamRun = 1;
constexpr int ParamOrdinal = 2;
constexpr int ParamBackend = 3;
constexpr int ParamType = 4;
constexpr int ParamTick = 5;
constexpr int ParamPc = 6;
constexpr int ParamNextPc = 7;
constexpr int ParamOpcode = 8;
constexpr int ParamDelaySlotDepth = 9;
constexpr int ParamMemAddr = 10;
constexpr int ParamMemWidth = 11;
constexpr int ParamMemValue = 12;
constexpr int ParamExcPc = 13;
constexpr int ParamVectorPc = 14;
constexpr int ParamExcCode = 15;
constexpr int ParamCallKind = 16;
constexpr int ParamTargetPc = 17;
constexpr int ParamReturnPc = 18;
constexpr int ParamDelaySlotPc = 19;
constexpr int ParamHasRegs = 20;
constexpr int ParamR0 = 21;       // r0..r15 occupy 21..36
constexpr int ParamPr = 37;
constexpr int ParamGbr = 38;
constexpr int ParamVbr = 39;
constexpr int ParamMach = 40;
constexpr int ParamMacl = 41;
constexpr int ParamSr = 42;
constexpr int ParamFpul = 43;
constexpr int ParamFpscr = 44;
constexpr int ParamCount = 44;

const std::string Sh4EventsCreate =
		std::string("CREATE TABLE IF NOT EXISTS sh4_events(")
		+ "id INTEGER PRIMARY KEY, run INTEGER, ordinal INTEGER, backend INTEGER,"
		+ " type INTEGER, tick INTEGER,"
		+ " pc INTEGER, next_pc INTEGER, opcode INTEGER, delay_slot_depth INTEGER,"
		+ " mem_addr INTEGER, mem_width INTEGER, mem_value INTEGER,"
		+ " exc_pc INTEGER, vector_pc INTEGER, exc_code INTEGER,"
		+ " call_kind INTEGER, target_pc INTEGER, return_pc INTEGER, delay_slot_pc INTEGER,"
		+ " has_regs INTEGER,"
		+ " r0 INTEGER, r1 INTEGER, r2 INTEGER, r3 INTEGER,"
		+ " r4 INTEGER, r5 INTEGER, r6 INTEGER, r7 INTEGER,"
		+ " r8 INTEGER, r9 INTEGER, r10 INTEGER, r11 INTEGER,"
		+ " r12 INTEGER, r13 INTEGER, r14 INTEGER, r15 INTEGER,"
		+ " pr INTEGER, gbr INTEGER, vbr INTEGER, mach INTEGER, macl INTEGER,"
		+ " sr INTEGER, fpul INTEGER, fpscr INTEGER)";

std::string placeholders(int count)
{
	std::string result;
	for (int i = 0; i < count; i++)
		result += (i == 0) ? "?" : ", ?";
	return result;
}

const std::string Sh4EventsInsert =
		std::string("INSERT INTO sh4_events(")
		+ "run, ordinal, backend, type, tick,"
		+ " pc, next_pc, opcode, delay_slot_depth,"
		+ " mem_addr, mem_width, mem_value,"
		+ " exc_pc, vector_pc, exc_code,"
		+ " call_kind, target_pc, return_pc, delay_slot_pc,"
		+ " has_regs,"
		+ " r0, r1, r2, r3, r4, r5, r6, r7, r8, r9, r10, r11, r12, r13, r14, r15,"
		+ " pr, gbr, vbr, mach, macl, sr, fpul, fpscr)"
		+ " VALUES(" + placeholders(ParamCount) + ")";

void bindNullRange(Statement& statement, int first, int lastInclusive)
{
	for (int index = first; index <= lastInclusive; index++)
		statement.bindNull(index);
}

void bindMemory(Statement& statement, const Sh4Observation& observation)
{
	const bool present = observation.type == Sh4ObservationType::MemoryRead
			|| observation.type == Sh4ObservationType::MemoryWrite;
	if (!present)
	{
		bindNullRange(statement, ParamMemAddr, ParamMemValue);
		return;
	}
	statement.bindUInt(ParamMemAddr, observation.memoryAddress)
			.bindUInt(ParamMemWidth, observation.memoryWidth)
			.bindUInt(ParamMemValue, observation.memoryValue);
}

void bindException(Statement& statement, const Sh4Observation& observation)
{
	if (observation.type != Sh4ObservationType::Exception)
	{
		bindNullRange(statement, ParamExcPc, ParamExcCode);
		return;
	}
	statement.bindUInt(ParamExcPc, observation.exceptionPc)
			.bindUInt(ParamVectorPc, observation.vectorPc)
			.bindUInt(ParamExcCode, observation.exceptionCode);
}

void bindControlFlow(Statement& statement, const Sh4Observation& observation)
{
	const bool present = observation.type == Sh4ObservationType::Call
			|| observation.type == Sh4ObservationType::Return;
	if (!present)
	{
		bindNullRange(statement, ParamCallKind, ParamDelaySlotPc);
		return;
	}
	statement.bindInt(ParamCallKind, static_cast<int>(observation.callKind))
			.bindUInt(ParamTargetPc, observation.targetPc)
			.bindUInt(ParamReturnPc, observation.returnPc)
			.bindUInt(ParamDelaySlotPc, observation.delaySlotPc);
}

void bindRegisters(Statement& statement, const Sh4Observation& observation)
{
	const bool present = (observation.availableFields & Sh4Observation::HasRegisters) != 0;
	statement.bindBool(ParamHasRegs, present);
	if (!present)
	{
		bindNullRange(statement, ParamR0, ParamFpscr);
		return;
	}
	const Sh4RegisterSnapshot& regs = observation.registers;
	for (int i = 0; i < 16; i++)
		statement.bindUInt(ParamR0 + i, regs.r[static_cast<std::size_t>(i)]);
	statement.bindUInt(ParamPr, regs.pr)
			.bindUInt(ParamGbr, regs.gbr)
			.bindUInt(ParamVbr, regs.vbr)
			.bindUInt(ParamMach, regs.mach)
			.bindUInt(ParamMacl, regs.macl)
			.bindUInt(ParamSr, regs.sr)
			.bindUInt(ParamFpul, regs.fpul)
			.bindUInt(ParamFpscr, regs.fpscr);
}

} // namespace

void bindOwnerToken(Statement& statement, int firstIndex,
		const Sh4InstructionOwnerToken& token)
{
	if (!token.valid)
	{
		bindNullRange(statement, firstIndex, firstIndex + 5);
		return;
	}
	statement.bindGeneration(firstIndex, token.generation)
			.bindUInt(firstIndex + 1, token.pc)
			.bindUInt(firstIndex + 2, token.pr)
			.bindUInt(firstIndex + 3, token.opcode)
			.bindInt(firstIndex + 4, static_cast<int>(token.backend))
			.bindUInt(firstIndex + 5, token.delaySlotDepth);
}

Sh4Rows::Sh4Rows(Database& db, std::int64_t run, const RowOptions& options)
	: run(run), insert(db.prepare(Sh4EventsInsert))
{
	(void)options;
}

void Sh4Rows::createTables(Database& db)
{
	db.exec(Sh4EventsCreate);
}

void Sh4Rows::createIndexes(Database& db)
{
	db.exec("CREATE INDEX IF NOT EXISTS sh4_events_pc ON sh4_events(pc)");
	db.exec("CREATE INDEX IF NOT EXISTS sh4_events_tick ON sh4_events(tick)");
	db.exec("CREATE INDEX IF NOT EXISTS sh4_events_type ON sh4_events(type)");
	db.exec("CREATE INDEX IF NOT EXISTS sh4_events_mem_addr ON sh4_events(mem_addr)"
			" WHERE mem_addr IS NOT NULL");
	db.exec("CREATE INDEX IF NOT EXISTS sh4_events_target_pc ON sh4_events(target_pc)"
			" WHERE target_pc IS NOT NULL");
}

void Sh4Rows::write(const Sh4Observation& observation)
{
	insert.bindInt(ParamRun, run)
			.bindUInt(ParamOrdinal, observation.emissionOrdinal)
			.bindInt(ParamBackend, static_cast<int>(observation.backend))
			.bindInt(ParamType, static_cast<int>(observation.type))
			.bindUInt(ParamTick, observation.tick)
			.bindUInt(ParamPc, observation.instructionPc)
			.bindUInt(ParamOpcode, observation.opcode)
			.bindUInt(ParamDelaySlotDepth, observation.delaySlotDepth);
	if (observation.availableFields & Sh4Observation::HasNextPc)
		insert.bindUInt(ParamNextPc, observation.nextPc);
	else
		insert.bindNull(ParamNextPc);
	bindMemory(insert, observation);
	bindException(insert, observation);
	bindControlFlow(insert, observation);
	bindRegisters(insert, observation);
	insert.execute();
}

} // namespace research::workbench

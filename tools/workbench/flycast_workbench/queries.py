"""Canned reverse-engineering queries over a workbench recording.

All functions take an open sqlite3.Connection (see `open_db`) and return
plain lists of dicts so they serialize straight to JSON for the MCP server.
Type codes follow the C++ enums:
  sh4_events.type: 1 InstructionBegin 2 InstructionEnd 3 InstructionAbort
                   4 MemoryRead 5 MemoryWrite 6 Exception 7 Call 8 Return
"""

from __future__ import annotations

import sqlite3
from pathlib import Path
from typing import Any, Iterable, Optional

SH4_TYPE_NAMES = {1: "instruction-begin", 2: "instruction", 3: "instruction-abort",
                  4: "memory-read", 5: "memory-write", 6: "exception", 7: "call", 8: "return"}
CALL_KIND_NAMES = {1: "bsr", 2: "bsrf", 3: "jsr", 4: "jmp"}
MAX_ROWS = 5000


def open_db(path: str | Path, readonly: bool = True) -> sqlite3.Connection:
    uri = Path(path).resolve().as_uri()
    if readonly:
        uri += "?mode=ro"
    db = sqlite3.connect(uri, uri=True, timeout=10.0)
    db.row_factory = sqlite3.Row
    return db


def rows(db: sqlite3.Connection, sql: str, params: Iterable[Any] = (),
         limit: int = MAX_ROWS) -> list[dict]:
    cursor = db.execute(sql, tuple(params))
    out = [dict(row) for row in cursor.fetchmany(limit)]
    return out


def tables(db: sqlite3.Connection) -> dict[str, int]:
    names = [r[0] for r in db.execute(
        "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")]
    return {name: db.execute(f"SELECT COUNT(*) FROM {name}").fetchone()[0] for name in names}


def schema(db: sqlite3.Connection) -> dict[str, list[str]]:
    result = {}
    for name, in db.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"):
        result[name] = [row[1] for row in db.execute(f"PRAGMA table_info({name})")]
    return result


def runs(db: sqlite3.Connection) -> list[dict]:
    return rows(db, "SELECT * FROM runs ORDER BY id")


def symbol_name(db: sqlite3.Connection, address: Optional[int]) -> Optional[str]:
    if address is None:
        return None
    row = db.execute(
        "SELECT name, address FROM symbols WHERE address <= ? AND address + COALESCE(size, 0) > ? "
        "ORDER BY address DESC LIMIT 1", (address, address)).fetchone()
    if row is None:
        row = db.execute("SELECT name, address FROM symbols WHERE address = ?", (address,)).fetchone()
        return row["name"] if row else None
    offset = address - row["address"]
    return row["name"] if offset == 0 else f"{row['name']}+0x{offset:x}"


def _with_names(db: sqlite3.Connection, items: list[dict], *keys: str) -> list[dict]:
    if db.execute("SELECT COUNT(*) FROM symbols").fetchone()[0] == 0:
        return items
    for item in items:
        for key in keys:
            if item.get(key) is not None:
                item[f"{key}_name"] = symbol_name(db, item[key])
    return items


# ------------------------------------------------------------------ SH-4

def who_writes(db: sqlite3.Connection, start: int, end: Optional[int] = None,
               limit: int = 200) -> list[dict]:
    """Instructions that wrote inside [start, end], grouped by PC."""
    end = start if end is None else end
    items = rows(db, """
        SELECT pc, opcode, COUNT(*) AS writes, MIN(tick) AS first_tick, MAX(tick) AS last_tick,
               MIN(mem_addr) AS lowest_addr, MAX(mem_addr) AS highest_addr,
               GROUP_CONCAT(DISTINCT mem_width) AS widths
        FROM sh4_events WHERE type = 5 AND mem_addr BETWEEN ? AND ?
        GROUP BY pc ORDER BY writes DESC LIMIT ?""", (start, end, limit))
    return _with_names(db, items, "pc")


def who_reads(db: sqlite3.Connection, start: int, end: Optional[int] = None,
              limit: int = 200) -> list[dict]:
    end = start if end is None else end
    items = rows(db, """
        SELECT pc, opcode, COUNT(*) AS reads, MIN(tick) AS first_tick, MAX(tick) AS last_tick
        FROM sh4_events WHERE type = 4 AND mem_addr BETWEEN ? AND ?
        GROUP BY pc ORDER BY reads DESC LIMIT ?""", (start, end, limit))
    return _with_names(db, items, "pc")


def writes_timeline(db: sqlite3.Connection, start: int, end: Optional[int] = None,
                    tick_from: int = 0, tick_to: int = 1 << 62, limit: int = 500) -> list[dict]:
    """Each write to the range in order, with the writer PC and value."""
    end = start if end is None else end
    items = rows(db, """
        SELECT tick, pc, mem_addr, mem_width, mem_value, delay_slot_depth
        FROM sh4_events WHERE type = 5 AND mem_addr BETWEEN ? AND ? AND tick BETWEEN ? AND ?
        ORDER BY tick, ordinal LIMIT ?""", (start, end, tick_from, tick_to, limit))
    return _with_names(db, items, "pc")


def callers_of(db: sqlite3.Connection, target_pc: int, limit: int = 200) -> list[dict]:
    items = rows(db, """
        SELECT pc AS caller_pc, call_kind, COUNT(*) AS calls, MIN(tick) AS first_tick
        FROM sh4_events WHERE type = 7 AND target_pc = ?
        GROUP BY pc, call_kind ORDER BY calls DESC LIMIT ?""", (target_pc, limit))
    for item in items:
        item["call_kind"] = CALL_KIND_NAMES.get(item["call_kind"], item["call_kind"])
    return _with_names(db, items, "caller_pc")


def callees_of(db: sqlite3.Connection, start: int, end: int, limit: int = 200) -> list[dict]:
    """Call targets reached from instructions in [start, end] (a function body)."""
    items = rows(db, """
        SELECT target_pc, call_kind, COUNT(*) AS calls, MIN(pc) AS from_pc
        FROM sh4_events WHERE type = 7 AND pc BETWEEN ? AND ?
        GROUP BY target_pc, call_kind ORDER BY calls DESC LIMIT ?""", (start, end, limit))
    for item in items:
        item["call_kind"] = CALL_KIND_NAMES.get(item["call_kind"], item["call_kind"])
    return _with_names(db, items, "target_pc", "from_pc")


def hot_functions(db: sqlite3.Connection, limit: int = 100) -> list[dict]:
    """Most-called targets across the recording."""
    items = rows(db, """
        SELECT target_pc, COUNT(*) AS calls, COUNT(DISTINCT pc) AS distinct_callers
        FROM sh4_events WHERE type = 7 GROUP BY target_pc ORDER BY calls DESC LIMIT ?""", (limit,))
    return _with_names(db, items, "target_pc")


def indirect_call_targets(db: sqlite3.Connection, limit: int = 1000) -> list[dict]:
    """Observed targets of JSR/JMP (kinds 3 and 4): what Ghidra cannot resolve statically."""
    items = rows(db, """
        SELECT pc, call_kind, target_pc, COUNT(*) AS calls
        FROM sh4_events WHERE type = 7 AND call_kind IN (3, 4)
        GROUP BY pc, call_kind, target_pc ORDER BY pc, calls DESC LIMIT ?""", (limit,))
    for item in items:
        item["call_kind"] = CALL_KIND_NAMES.get(item["call_kind"], item["call_kind"])
    return _with_names(db, items, "pc", "target_pc")


def call_stack_at(db: sqlite3.Connection, tick: int, depth: int = 32) -> list[dict]:
    """Approximate call stack at `tick` from unmatched calls before it."""
    events = db.execute("""
        SELECT type, pc, target_pc, return_pc, tick FROM sh4_events
        WHERE type IN (7, 8) AND tick <= ? ORDER BY tick, ordinal""", (tick,)).fetchall()
    stack: list[dict] = []
    for event in events:
        if event["type"] == 7:
            stack.append({"call_pc": event["pc"], "target_pc": event["target_pc"],
                          "return_pc": event["return_pc"], "tick": event["tick"]})
        elif stack:
            stack.pop()
    return _with_names(db, stack[-depth:], "call_pc", "target_pc")


def exceptions(db: sqlite3.Connection, limit: int = 200) -> list[dict]:
    return rows(db, """
        SELECT tick, pc, exc_pc, vector_pc, exc_code, COUNT(*) OVER (PARTITION BY exc_code) AS same_code
        FROM sh4_events WHERE type = 6 ORDER BY tick LIMIT ?""", (limit,))


# ------------------------------------------------------------------ hardware

def maple_summary(db: sqlite3.Connection) -> list[dict]:
    return rows(db, """
        SELECT bus, port, command, type, COUNT(*) AS events, MIN(tick) AS first_tick,
               MAX(tick) AS last_tick, device_type
        FROM maple_events GROUP BY bus, port, command, type ORDER BY bus, port, command, type""")


def render_summary(db: sqlite3.Connection, limit: int = 200) -> list[dict]:
    """One row per PVR render generation: blocks submitted, submitting PCs, draws."""
    items = rows(db, """
        SELECT render_gen,
               SUM(type = 3) AS blocks,
               COUNT(DISTINCT init_pc) AS submitting_pcs,
               MIN(tick) AS first_tick, MAX(tick) AS last_tick
        FROM pvr_ta_events WHERE render_gen IS NOT NULL
        GROUP BY render_gen ORDER BY render_gen LIMIT ?""", (limit,))
    return items


def ta_submitters(db: sqlite3.Connection, render_gen: Optional[int] = None,
                  limit: int = 200) -> list[dict]:
    """Which SH-4 instructions pushed TA blocks (optionally for one render)."""
    where = "type = 3 AND init_pc IS NOT NULL"
    params: list[Any] = []
    if render_gen is not None:
        where += " AND render_gen = ?"
        params.append(render_gen)
    params.append(limit)
    items = rows(db, f"""
        SELECT init_pc, init_pr, source, COUNT(*) AS blocks, COUNT(DISTINCT ctx_gen) AS contexts,
               MIN(tick) AS first_tick
        FROM pvr_ta_events WHERE {where}
        GROUP BY init_pc, init_pr, source ORDER BY blocks DESC LIMIT ?""", params)
    return _with_names(db, items, "init_pc", "init_pr")


def draw_owners(db: sqlite3.Connection, render_gen: Optional[int] = None,
                limit: int = 200) -> list[dict]:
    """Decoded primitives grouped by the SH-4 instruction that owns them."""
    where = "type = 1"
    params: list[Any] = []
    if render_gen is not None:
        where += " AND render_gen = ?"
        params.append(render_gen)
    params.append(limit)
    items = rows(db, f"""
        SELECT owner_pc, owner_class, kind, COUNT(*) AS primitives, SUM(vertex_count) AS vertices,
               COUNT(DISTINCT tex_addr) AS textures
        FROM pvr_draw_events WHERE {where}
        GROUP BY owner_pc, owner_class, kind ORDER BY primitives DESC LIMIT ?""", params)
    return _with_names(db, items, "owner_pc")


def texture_users(db: sqlite3.Connection, tex_addr: int, limit: int = 100) -> list[dict]:
    items = rows(db, """
        SELECT owner_pc, render_gen, COUNT(*) AS primitives, tex_w, tex_h, tex_fmt
        FROM pvr_draw_events WHERE tex_addr = ? GROUP BY owner_pc, render_gen, tex_w, tex_h, tex_fmt
        ORDER BY render_gen LIMIT ?""", (tex_addr, limit))
    return _with_names(db, items, "owner_pc")


def register_writes(db: sqlite3.Connection, reg_addr: Optional[int] = None,
                    limit: int = 500) -> list[dict]:
    where = "type = 1"
    params: list[Any] = []
    if reg_addr is not None:
        where += " AND reg_addr = ?"
        params.append(reg_addr)
    params.append(limit)
    items = rows(db, f"""
        SELECT tick, init_pc, reg_addr, requested, previous, effective, disposition, render_gen
        FROM pvr_present_events WHERE {where} ORDER BY tick LIMIT ?""", params)
    return _with_names(db, items, "init_pc")


def gdrom_summary(db: sqlite3.Connection, limit: int = 500) -> list[dict]:
    return rows(db, """
        SELECT 'hle' AS path, command_gen, type, tick, init_pc, command, fad, sector_count, destination
        FROM gdrom_events
        UNION ALL
        SELECT 'hw', command_gen, type, tick, ata_pc, NULL, start_fad, sector_count, destination
        FROM gdrom_hw_events
        ORDER BY tick LIMIT ?""", (limit,))


def aica_writers(db: sqlite3.Connection, limit: int = 200) -> list[dict]:
    items = rows(db, """
        SELECT sh4_pc, writer, COUNT(*) AS writes, MIN(address) AS lowest, MAX(address) AS highest
        FROM aica_events WHERE type IN (1, 2) GROUP BY sh4_pc, writer ORDER BY writes DESC LIMIT ?""",
        (limit,))
    return _with_names(db, items, "sh4_pc")


# ------------------------------------------------------------------ symbols

def import_symbols(db: sqlite3.Connection, symbols: list[dict]) -> int:
    """symbols: [{address, name, size?, kind?, namespace?}] from the Ghidra exporter."""
    db.execute("CREATE TABLE IF NOT EXISTS symbols(address INTEGER PRIMARY KEY, name TEXT,"
               " size INTEGER, kind TEXT, namespace TEXT)")
    db.executemany(
        "INSERT OR REPLACE INTO symbols(address, name, size, kind, namespace) VALUES(?, ?, ?, ?, ?)",
        [(int(s["address"]), s["name"], s.get("size"), s.get("kind"), s.get("namespace"))
         for s in symbols])
    db.commit()
    return len(symbols)


def ghidra_facts(db: sqlite3.Connection) -> dict:
    """Runtime facts for tools/ghidra/ImportWorkbenchFacts.java."""
    return {
        "call_targets": rows(db, """
            SELECT target_pc AS address, COUNT(*) AS calls, COUNT(DISTINCT pc) AS callers
            FROM sh4_events WHERE type = 7 GROUP BY target_pc""", limit=1 << 20),
        "indirect_calls": rows(db, """
            SELECT pc AS address, target_pc, COUNT(*) AS calls
            FROM sh4_events WHERE type = 7 AND call_kind IN (3, 4) GROUP BY pc, target_pc""",
            limit=1 << 20),
        "memory_writers": rows(db, """
            SELECT pc AS address, mem_addr, mem_width, COUNT(*) AS writes
            FROM sh4_events WHERE type = 5 GROUP BY pc, mem_addr, mem_width""", limit=1 << 20),
        "ta_submitters": rows(db, """
            SELECT init_pc AS address, COUNT(*) AS blocks FROM pvr_ta_events
            WHERE type = 3 AND init_pc IS NOT NULL GROUP BY init_pc""", limit=1 << 20),
    }

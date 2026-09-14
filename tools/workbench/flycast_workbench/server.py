"""MCP server exposing the Flycast workbench to agents.

Run with:  python -m flycast_workbench.server
Configure in an MCP client as a stdio server with that command and the
working directory set to tools/workbench.

Two groups of tools: `flycast_*` drive a live emulator through its control
socket (launching one if needed), `db_*` query a recording. Every tool
returns JSON-serializable data; addresses are accepted as ints or hex
strings.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Optional

from mcp.server.fastmcp import FastMCP

from . import queries
from .control_client import ControlError, FlycastControl, hexdump
from .launcher import PROFILES, FlycastProcess, LaunchOptions, default_exe

mcp = FastMCP("flycast-workbench")

_process: Optional[FlycastProcess] = None
_control: Optional[FlycastControl] = None


def _addr(value: Any) -> int:
    return int(value, 0) if isinstance(value, str) else int(value)


def _ctl() -> FlycastControl:
    if _control is None:
        raise ControlError("no emulator connected; call flycast_launch or flycast_connect first")
    return _control


# ---------------------------------------------------------------- emulator

@mcp.tool()
def flycast_launch(game: str, exe: Optional[str] = None, profile: Optional[str] = None,
                   record_db: Optional[str] = None, autoload_slot: Optional[int] = None,
                   dynarec: Optional[bool] = None, maple_replay: Optional[str] = None,
                   rtc_seed: Optional[int] = None) -> dict:
    """Start Flycast with the control socket enabled and connect to it.

    profile: one of PROFILES (hardware, calls, memory, input-to-render, full);
    used only when record_db is given, to start recording at boot.
    autoload_slot: load that save state right after boot (savestate-anchored
    capture). Returns the initial status."""
    global _process, _control
    flycast_stop()
    exe_path = Path(exe) if exe else default_exe()
    if exe_path is None:
        raise ControlError("no flycast.exe found; pass exe=")
    options = LaunchOptions(exe=exe_path, game=Path(game), profile=profile,
                            record_db=Path(record_db) if record_db else None,
                            autoload_slot=autoload_slot, dynarec=dynarec,
                            maple_replay=Path(maple_replay) if maple_replay else None,
                            rtc_seed=rtc_seed)
    _process = FlycastProcess(options).start()
    _control = _process.control
    status = _control.wait_for_game(120)
    status["port"] = options.port
    status["token"] = options.token
    return status


@mcp.tool()
def flycast_connect(port: int, token: str = "") -> dict:
    """Connect to an already running Flycast started with research:ControlPort."""
    global _control
    _control = FlycastControl(port, token).connect()
    return _control.status()


@mcp.tool()
def flycast_stop() -> dict:
    """Exit the emulator started by flycast_launch (or just disconnect)."""
    global _process, _control
    code = None
    if _process is not None:
        code = _process.stop()
    elif _control is not None:
        try:
            _control.exit()
        except ControlError:
            pass
    _process, _control = None, None
    return {"exit_code": code}


@mcp.tool()
def flycast_status() -> dict:
    """Emulator, recorder, checkpoint, and bus status."""
    return _ctl().status()


@mcp.tool()
def flycast_pause() -> dict:
    """Pause emulation (memory and register reads are then coherent)."""
    _ctl().pause()
    return _ctl().wait_for_pause(10)


@mcp.tool()
def flycast_resume() -> dict:
    _ctl().resume()
    return _ctl().wait_for_game(10)


@mcp.tool()
def flycast_savestate(slot: int = 0) -> dict:
    """Save state to a slot (pauses and resumes around the save)."""
    _ctl().savestate(slot)
    return {"slot": slot}


@mcp.tool()
def flycast_loadstate(slot: int = 0) -> dict:
    """Load a save state; use to jump straight to the moment under study."""
    _ctl().loadstate(slot)
    return _ctl().status()


@mcp.tool()
def flycast_regs() -> dict:
    """SH-4 registers (r0-r15, banks, pc, pr, gbr, vbr, sr, fpscr, fr bits) and tick."""
    return _ctl().regs()


@mcp.tool()
def flycast_mem_read(addr: Any, length: int = 256, as_hexdump: bool = True) -> Any:
    """Read guest memory: system RAM (0x8c...), VRAM (0xa4...), AICA RAM (0x0080....).
    Up to 1 MiB. Returns a hexdump string, or a hex string when as_hexdump is false."""
    address = _addr(addr)
    data = _ctl().mem_read(address, length)
    return hexdump(data, address) if as_hexdump else data.hex()


@mcp.tool()
def flycast_mem_dump(addr: Any, length: int, path: str) -> dict:
    """Dump up to 64 MiB of guest memory to a file on disk (e.g. all 16 MiB of RAM)."""
    return _ctl().mem_dump(_addr(addr), length, path)


@mcp.tool()
def flycast_record_start(path: str, config: Optional[dict] = None,
                         profile: Optional[str] = None) -> dict:
    """Start recording observations into a SQLite database.

    config follows core/research/workbench/workbench_config.h, e.g.
    {"buses": "sh4,maple", "sh4": {"types": "call,return,memory-write",
    "mem_start": "0x8c100000", "mem_end": "0x8c10ffff"}}. profile picks a
    preset from PROFILES instead."""
    if config is None and profile is not None:
        config = PROFILES[profile]
    return _ctl().record_start(path, config)


@mcp.tool()
def flycast_record_stop() -> dict:
    """Stop recording; builds indexes and finalizes the runs row."""
    _ctl().record_stop()
    return _ctl().record_status()


@mcp.tool()
def flycast_record_status() -> dict:
    return _ctl().record_status()


@mcp.tool()
def flycast_checkpoint(pc: Any, gate_addr: Any = 0, gate_value: Any = 0,
                       wait_seconds: float = 0) -> dict:
    """Pause when the top-level instruction at pc completes (optionally only while
    the u32 at gate_addr equals gate_value). Requires non-threaded rendering, which
    flycast_launch sets. With wait_seconds > 0, blocks until the pause."""
    control = _ctl()
    control.checkpoint_set(_addr(pc), _addr(gate_addr), _addr(gate_value))
    if wait_seconds > 0:
        return control.wait_for_pause(wait_seconds)
    return control.status()


@mcp.tool()
def flycast_checkpoint_clear() -> dict:
    _ctl().checkpoint_clear()
    return _ctl().status()


@mcp.tool()
def flycast_profiles() -> dict:
    """Recorder presets usable with flycast_launch and flycast_record_start."""
    return PROFILES


# ---------------------------------------------------------------- database

@mcp.tool()
def db_tables(db: str) -> dict:
    """Row counts per table in a recording."""
    with queries.open_db(db) as conn:
        return queries.tables(conn)


@mcp.tool()
def db_schema(db: str) -> dict:
    """Column names per table."""
    with queries.open_db(db) as conn:
        return queries.schema(conn)


@mcp.tool()
def db_query(db: str, sql: str, limit: int = 500) -> list:
    """Run a read-only SQL query against a recording. Type codes: sh4_events.type
    1 begin 2 end 3 abort 4 mem-read 5 mem-write 6 exception 7 call 8 return."""
    with queries.open_db(db) as conn:
        return queries.rows(conn, sql, limit=limit)


@mcp.tool()
def db_who_writes(db: str, addr: Any, end: Any = None, limit: int = 200) -> list:
    """Instructions that wrote to [addr, end], most frequent first."""
    with queries.open_db(db) as conn:
        return queries.who_writes(conn, _addr(addr), _addr(end) if end is not None else None, limit)


@mcp.tool()
def db_who_reads(db: str, addr: Any, end: Any = None, limit: int = 200) -> list:
    with queries.open_db(db) as conn:
        return queries.who_reads(conn, _addr(addr), _addr(end) if end is not None else None, limit)


@mcp.tool()
def db_writes_timeline(db: str, addr: Any, end: Any = None, tick_from: int = 0,
                       tick_to: int = 1 << 62, limit: int = 500) -> list:
    """Every write to a range in order: tick, writer pc, address, width, value."""
    with queries.open_db(db) as conn:
        return queries.writes_timeline(conn, _addr(addr), _addr(end) if end is not None else None,
                                       tick_from, tick_to, limit)


@mcp.tool()
def db_callers_of(db: str, target_pc: Any, limit: int = 200) -> list:
    with queries.open_db(db) as conn:
        return queries.callers_of(conn, _addr(target_pc), limit)


@mcp.tool()
def db_callees_of(db: str, start: Any, end: Any, limit: int = 200) -> list:
    """Calls made from instructions in [start, end] (a function body)."""
    with queries.open_db(db) as conn:
        return queries.callees_of(conn, _addr(start), _addr(end), limit)


@mcp.tool()
def db_hot_functions(db: str, limit: int = 100) -> list:
    with queries.open_db(db) as conn:
        return queries.hot_functions(conn, limit)


@mcp.tool()
def db_indirect_calls(db: str, limit: int = 1000) -> list:
    """Observed JSR/JMP targets: resolves what static analysis cannot."""
    with queries.open_db(db) as conn:
        return queries.indirect_call_targets(conn, limit)


@mcp.tool()
def db_call_stack_at(db: str, tick: int, depth: int = 32) -> list:
    with queries.open_db(db) as conn:
        return queries.call_stack_at(conn, tick, depth)


@mcp.tool()
def db_render_summary(db: str, limit: int = 200) -> list:
    """Per PVR render: TA blocks, submitting PCs, tick span."""
    with queries.open_db(db) as conn:
        return queries.render_summary(conn, limit)


@mcp.tool()
def db_ta_submitters(db: str, render_gen: Optional[int] = None, limit: int = 200) -> list:
    """Which SH-4 instructions pushed geometry to the Tile Accelerator."""
    with queries.open_db(db) as conn:
        return queries.ta_submitters(conn, render_gen, limit)


@mcp.tool()
def db_draw_owners(db: str, render_gen: Optional[int] = None, limit: int = 200) -> list:
    """Decoded primitives grouped by owning SH-4 instruction."""
    with queries.open_db(db) as conn:
        return queries.draw_owners(conn, render_gen, limit)


@mcp.tool()
def db_maple_summary(db: str) -> list:
    with queries.open_db(db) as conn:
        return queries.maple_summary(conn)


@mcp.tool()
def db_gdrom_summary(db: str, limit: int = 500) -> list:
    with queries.open_db(db) as conn:
        return queries.gdrom_summary(conn, limit)


@mcp.tool()
def db_import_symbols(db: str, symbols_json: str) -> dict:
    """Load a symbols JSON (from tools/ghidra/ExportSymbols.java) so query results
    carry function names."""
    with open(symbols_json, "r", encoding="utf-8") as handle:
        symbols = json.load(handle)
    if isinstance(symbols, dict):
        symbols = symbols.get("symbols", [])
    with queries.open_db(db, readonly=False) as conn:
        return {"imported": queries.import_symbols(conn, symbols)}


@mcp.tool()
def db_export_ghidra_facts(db: str, out_json: str) -> dict:
    """Write runtime facts (call targets, indirect calls, memory writers, TA
    submitters) for tools/ghidra/ImportWorkbenchFacts.java."""
    with queries.open_db(db) as conn:
        facts = queries.ghidra_facts(conn)
    with open(out_json, "w", encoding="utf-8") as handle:
        json.dump(facts, handle)
    return {key: len(value) for key, value in facts.items()}


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()

"""Command-line front end for the workbench, for use without MCP.

    python -m flycast_workbench launch <game> [--exe X] [--profile P] [--record out.db]
    python -m flycast_workbench status|pause|resume|regs --port N --token T
    python -m flycast_workbench mem <addr> [len] --port N --token T
    python -m flycast_workbench record start <db> [--profile P] | stop | status
    python -m flycast_workbench checkpoint <pc> [--gate ADDR VALUE] [--wait S]
    python -m flycast_workbench db <recording.db> <query-name> [args...]
    python -m flycast_workbench sql <recording.db> "<select ...>"
"""

from __future__ import annotations

import argparse
import inspect
import json
import sys
from pathlib import Path
from typing import Any

from . import queries
from .control_client import FlycastControl, hexdump
from .launcher import PROFILES, FlycastProcess, LaunchOptions, default_exe


def _addr(value: str) -> int:
    return int(value, 0)


def _dump(value: Any) -> None:
    print(json.dumps(value, indent=2, default=str))


def _control(args: argparse.Namespace) -> FlycastControl:
    if not args.port:
        sys.exit("--port is required (printed by `launch`)")
    return FlycastControl(args.port, args.token or "").connect()


def cmd_launch(args: argparse.Namespace) -> int:
    exe = Path(args.exe) if args.exe else default_exe()
    if exe is None:
        sys.exit("no flycast.exe found; pass --exe")
    options = LaunchOptions(exe=exe, game=Path(args.game), profile=args.profile,
                            record_db=Path(args.record) if args.record else None,
                            autoload_slot=args.slot, dynarec=args.dynarec)
    process = FlycastProcess(options).start()
    status = process.control.wait_for_game(120)
    print(json.dumps({"port": options.port, "token": options.token, "pid": process.process.pid,
                      "game_id": status.get("game_id"), "backend": status.get("cpu_backend")}))
    # Detach: the emulator keeps running; later commands use --port/--token.
    process.control.close()
    return 0


def cmd_simple(args: argparse.Namespace) -> int:
    control = _control(args)
    with control:
        if args.command == "status":
            _dump(control.status())
        elif args.command == "pause":
            control.pause()
            _dump(control.wait_for_pause(10))
        elif args.command == "resume":
            control.resume()
            _dump(control.wait_for_game(10))
        elif args.command == "regs":
            regs = control.regs()
            for key in ("pc", "pr", "gbr", "vbr", "sr", "mach", "macl", "fpul", "fpscr"):
                print(f"{key:>5} = {regs[key]:08x}")
            for i, value in enumerate(regs["r"]):
                print(f"  r{i:<2} = {value:08x}", end="\n" if i % 4 == 3 else "")
            print(f" tick = {regs['tick']}")
        elif args.command == "exit":
            control.exit()
        elif args.command == "savestate":
            control.savestate(args.slot)
        elif args.command == "loadstate":
            control.loadstate(args.slot)
            _dump(control.status())
    return 0


def cmd_mem(args: argparse.Namespace) -> int:
    with _control(args) as control:
        address = _addr(args.addr)
        if args.out:
            _dump(control.mem_dump(address, args.length, args.out))
        else:
            print(hexdump(control.mem_read(address, args.length), address))
    return 0


def cmd_record(args: argparse.Namespace) -> int:
    with _control(args) as control:
        if args.action == "start":
            config = PROFILES[args.profile] if args.profile else None
            if args.config:
                config = json.loads(args.config)
            _dump(control.record_start(args.db, config))
        elif args.action == "stop":
            control.record_stop()
            _dump(control.record_status())
        else:
            _dump(control.record_status())
    return 0


def cmd_checkpoint(args: argparse.Namespace) -> int:
    with _control(args) as control:
        if args.clear:
            control.checkpoint_clear()
        else:
            gate_addr, gate_value = (_addr(args.gate[0]), _addr(args.gate[1])) if args.gate else (0, 0)
            control.checkpoint_set(_addr(args.pc), gate_addr, gate_value)
            if args.wait:
                _dump(control.wait_for_pause(args.wait))
                return 0
        _dump(control.status())
    return 0


def cmd_db(args: argparse.Namespace) -> int:
    function = getattr(queries, args.query, None)
    if function is None or args.query.startswith("_"):
        names = [n for n, f in inspect.getmembers(queries, inspect.isfunction)
                 if not n.startswith("_") and n not in ("open_db", "rows")]
        sys.exit("unknown query; available: " + ", ".join(names))
    converted = [_addr(a) if a.lower().startswith("0x") else (int(a) if a.isdigit() else a)
                 for a in args.args]
    with queries.open_db(args.db) as conn:
        _dump(function(conn, *converted))
    return 0


def cmd_sql(args: argparse.Namespace) -> int:
    with queries.open_db(args.db) as conn:
        _dump(queries.rows(conn, args.sql, limit=args.limit))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="flycast_workbench", description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--token", default="")
    sub = parser.add_subparsers(dest="command", required=True)

    launch = sub.add_parser("launch")
    launch.add_argument("game")
    launch.add_argument("--exe")
    launch.add_argument("--profile", choices=sorted(PROFILES))
    launch.add_argument("--record")
    launch.add_argument("--slot", type=int)
    launch.add_argument("--dynarec", type=lambda v: v.lower() in ("1", "yes", "true"), default=None)
    launch.set_defaults(func=cmd_launch)

    for name in ("status", "pause", "resume", "regs", "exit"):
        sub.add_parser(name).set_defaults(func=cmd_simple)
    for name in ("savestate", "loadstate"):
        state = sub.add_parser(name)
        state.add_argument("slot", type=int, nargs="?", default=0)
        state.set_defaults(func=cmd_simple)

    mem = sub.add_parser("mem")
    mem.add_argument("addr")
    mem.add_argument("length", type=int, nargs="?", default=256)
    mem.add_argument("--out", help="write a binary dump to this file instead of printing")
    mem.set_defaults(func=cmd_mem)

    record = sub.add_parser("record")
    record.add_argument("action", choices=["start", "stop", "status"])
    record.add_argument("db", nargs="?")
    record.add_argument("--profile", choices=sorted(PROFILES))
    record.add_argument("--config", help="recorder config as a JSON string")
    record.set_defaults(func=cmd_record)

    checkpoint = sub.add_parser("checkpoint")
    checkpoint.add_argument("pc", nargs="?")
    checkpoint.add_argument("--gate", nargs=2, metavar=("ADDR", "VALUE"))
    checkpoint.add_argument("--wait", type=float, default=0)
    checkpoint.add_argument("--clear", action="store_true")
    checkpoint.set_defaults(func=cmd_checkpoint)

    db = sub.add_parser("db")
    db.add_argument("db")
    db.add_argument("query")
    db.add_argument("args", nargs="*")
    db.set_defaults(func=cmd_db)

    sql = sub.add_parser("sql")
    sql.add_argument("db")
    sql.add_argument("sql")
    sql.add_argument("--limit", type=int, default=200)
    sql.set_defaults(func=cmd_sql)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

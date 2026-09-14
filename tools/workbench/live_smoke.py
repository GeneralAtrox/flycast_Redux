"""End-to-end smoke test against a real emulator and disc image.

    python tools/workbench/live_smoke.py <flycast.exe> <game.gdi> [out.db]

Boots the game with the control socket enabled, exercises every control
command that does not modify the user's save states, records a few seconds
into a workbench database, and prints row counts. Bounded: it kills the
emulator process tree if anything hangs.
"""

from __future__ import annotations

import os
import sqlite3
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from flycast_workbench import FlycastProcess, LaunchOptions  # noqa: E402
from flycast_workbench.control_client import hexdump  # noqa: E402


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe, game = Path(sys.argv[1]), Path(sys.argv[2])
    db_path = Path(sys.argv[3]) if len(sys.argv) > 3 else \
        Path(tempfile.gettempdir()) / f"flycast-smoke-{os.getpid()}.db"
    if db_path.exists():
        db_path.unlink()

    options = LaunchOptions(exe=exe, game=game, dynarec=True)
    started = time.monotonic()
    with FlycastProcess(options) as flycast:
        control = flycast.control
        caps = control.capabilities()
        print(f"[{elapsed(started)}] connected on port {options.port}; "
              f"{len(caps['commands'])} commands")
        status = control.wait_for_game(90)
        print(f"[{elapsed(started)}] game loaded: {status['game_id']!r} "
              f"backend={status['cpu_backend']} pc={status.get('pc', 0):08x}")
        # Let the boot sequence finish: SH-4 observation slows emulation about
        # tenfold, so start recording once the game is polling and rendering.
        control.wait_until(lambda s: s.get("tick", 0) > 2_400_000_000, 120)
        print(f"[{elapsed(started)}] past boot (tick {control.status()['tick']})")

        regs = control.regs()
        print(f"[{elapsed(started)}] regs: pc={regs['pc']:08x} pr={regs['pr']:08x} "
              f"r15={regs['r'][15]:08x} tick={regs['tick']}")
        ram = control.mem_read(0x8C000000, 64)
        print(hexdump(ram, 0x8C000000).splitlines()[0])
        vram = control.mem_read(0xA4000000, 16)
        aica = control.mem_read(0x00800000, 16)
        print(f"vram[0:16]={vram.hex()} aica[0:16]={aica.hex()}")

        buses = os.environ.get("SMOKE_BUSES",
                               "sh4,maple,pvr-ta,pvr-draw,pvr-present,gdrom,gdrom-hw,aica,cdda")
        config = {
            "buses": buses,
            "sh4": {"types": "call,return,exception,memory-write",
                    "mem_start": 0x8C000000, "mem_end": 0x8CFFFFFF},
            "note": "live smoke test",
        }
        control.record_start(str(db_path), config)
        print(f"[{elapsed(started)}] recording to {db_path}")
        time.sleep(5)
        rec = control.record_status()
        print(f"[{elapsed(started)}] recorder: written={rec['written']} queued={rec['queued']} "
              f"dropped={rec['dropped']} error={rec['error']!r}")

        control.pause()
        paused = control.wait_for_pause(10)
        assert paused["paused"], paused
        print(f"[{elapsed(started)}] paused at pc={paused.get('pc', 0):08x}")
        snapshot_a = control.mem_read(0x8C000000, 4096)
        snapshot_b = control.mem_read(0x8C000000, 4096)
        assert snapshot_a == snapshot_b, "paused memory must be stable"
        control.resume()
        control.wait_for_game(10)
        print(f"[{elapsed(started)}] resumed")

        # Arm a checkpoint on the current PC; a running game revisits its loops.
        target = control.status().get("pc", 0)
        if target:
            control.checkpoint_set(target)
            try:
                hit = control.wait_for_pause(20)
                print(f"[{elapsed(started)}] checkpoint hit: pc={hit.get('pc', 0):08x} "
                      f"triggered={hit['checkpoint']['triggered']}")
                control.resume()
            except Exception as error:  # noqa: BLE001
                print(f"[{elapsed(started)}] checkpoint at {target:08x} not hit: {error}")
                control.checkpoint_clear()

        control.record_stop()
        print(f"[{elapsed(started)}] recording stopped")
        flycast.stop()
        print(f"[{elapsed(started)}] emulator exited")

    with sqlite3.connect(db_path) as db:
        tables = [row[0] for row in db.execute(
            "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name")]
        print("tables:", ", ".join(tables))
        for table in tables:
            count = db.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0]
            print(f"  {table:<26} {count:>9}")
        run = db.execute("SELECT game_id, cpu_backend, written, dropped, note FROM runs").fetchone()
        print("run:", run)
        if "sh4_events" in tables:
            top = db.execute(
                "SELECT pc, COUNT(*) c FROM sh4_events WHERE type=7 GROUP BY pc ORDER BY c DESC LIMIT 5"
            ).fetchall()
            print("hottest call sites:", [(f"{pc:08x}", c) for pc, c in top])
        if "pvr_ta_events" in tables:
            print("ta by type:", db.execute(
                "SELECT type, COUNT(*) FROM pvr_ta_events GROUP BY type").fetchall())
            print("ta initiators:", db.execute(
                "SELECT init_pc, COUNT(*) c FROM pvr_ta_events WHERE init_pc IS NOT NULL "
                "GROUP BY init_pc ORDER BY c DESC LIMIT 3").fetchall())
        if "maple_events" in tables:
            print("maple by cmd:", db.execute(
                "SELECT command, COUNT(*) FROM maple_events GROUP BY command").fetchall())
    return 0


def elapsed(started: float) -> str:
    return f"{time.monotonic() - started:5.1f}s"


if __name__ == "__main__":
    sys.exit(main())

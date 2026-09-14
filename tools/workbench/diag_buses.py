"""Diagnostic: record with a given config for N seconds and print per-bus
counters from status() plus row counts. Usage:
    python diag_buses.py <exe> <game> <buses> <sh4types> [seconds]
"""
from __future__ import annotations

import os
import sqlite3
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from flycast_workbench import FlycastProcess, LaunchOptions  # noqa: E402

exe, game, buses, sh4types = sys.argv[1:5]
seconds = float(sys.argv[5]) if len(sys.argv) > 5 else 4.0
db_path = Path(os.environ["LOCALAPPDATA"]) / "Temp" / "flycast-diag.db"
if db_path.exists():
    db_path.unlink()

dynarec = os.environ.get("DIAG_DYNAREC", "1") == "1"
observation = os.environ.get("DIAG_OBS", "1") == "1"
with FlycastProcess(LaunchOptions(exe=Path(exe), game=Path(game), dynarec=dynarec,
                                  dynarec_observation=observation)) as flycast:
    control = flycast.control
    control.wait_for_game(90)
    time.sleep(float(os.environ.get("DIAG_WAIT", "2")))
    before = control.status()["buses"]
    s0 = control.status()
    print(f"pre-record: pc={s0.get('pc',0):08x} tick={s0.get('tick')} backend={s0['cpu_backend']}")
    control.record_start(str(db_path), {"buses": buses, "sh4": {"types": sh4types}})
    for i in range(int(seconds * 2)):
        time.sleep(0.5)
        s1 = control.status()
        print(f"  t+{(i+1)*0.5:.1f}s pc={s1.get('pc',0):08x} tick={s1.get('tick')} running={s1['running']}")
    mid = control.status()
    rec = control.record_status()
    control.record_stop()
    after = control.status()["buses"]
    flycast.stop()

print("buses before:", before)
print("buses during:", mid["buses"])
print("buses after :", after)
print("recorder    :", rec)
with sqlite3.connect(db_path) as db:
    for (name,) in db.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name"):
        count = db.execute(f"SELECT COUNT(*) FROM {name}").fetchone()[0]
        if count:
            print(f"  {name:<26} {count:>9}")

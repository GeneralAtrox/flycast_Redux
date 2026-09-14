"""Launch a workbench-enabled Flycast and connect its control socket.

Everything the emulator needs is passed as transient `-config` options, so
the user's saved emu.cfg is left untouched. Recorder presets live in
PROFILES; each is a recorder config JSON object as accepted by
`record_start` and by `research.Workbench*` options.
"""

from __future__ import annotations

import os
import secrets
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

from .control_client import ControlError, FlycastControl

PROFILES: dict[str, dict] = {
    # Everything except per-instruction SH-4 rows: what a game does at the
    # hardware level, joined to the SH-4 instruction that caused each effect.
    "hardware": {
        "buses": "maple,pvr-ta,pvr-draw,pvr-present,gdrom,gdrom-hw,aica,cdda",
    },
    # Control flow only: calls, returns, exceptions, plus Maple input.
    "calls": {
        "buses": "sh4,maple",
        "sh4": {"types": "call,return,exception"},
    },
    # Who writes main RAM. Heavy; narrow with sh4.mem_start/mem_end.
    "memory": {
        "buses": "sh4",
        "sh4": {"types": "memory-write", "mem_start": 0x8C000000, "mem_end": 0x8CFFFFFF},
    },
    # Input to pixels: Maple traffic through TA submission to draws.
    "input-to-render": {
        "buses": "maple,pvr-ta,pvr-draw,pvr-present,sh4",
        "sh4": {"types": "call,return"},
    },
    # Everything, including every SH-4 instruction. Very large databases.
    "full": {
        "buses": "all",
        "sh4": {"types": "all"},
        "rows": {"vram_writes": True, "sample_frames": True},
    },
}


def free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@dataclass
class LaunchOptions:
    exe: Path
    game: Optional[Path] = None
    port: int = 0                       # 0 = pick a free one
    token: str = ""                     # "" = generate
    threaded_rendering: bool = False    # required by the PC checkpoint
    dynarec: Optional[bool] = None      # None = keep emu.cfg
    dynarec_observation: bool = True    # exact per-instruction attribution under dynarec
    record_db: Optional[Path] = None    # start recording at launch
    profile: Optional[str] = None       # PROFILES key for the launch recording
    autoload_slot: Optional[int] = None # load this save state after boot
    maple_replay: Optional[Path] = None
    maple_record: Optional[Path] = None
    rtc_seed: Optional[int] = None
    extra: list[str] = field(default_factory=list)  # more section:key=value entries

    def config_entries(self) -> list[str]:
        entries = [
            f"research:ControlPort={self.port}",
            f"research:ControlToken={self.token}",
            f"config:rend.ThreadedRendering={'yes' if self.threaded_rendering else 'no'}",
            f"research:DynarecObservation={'yes' if self.dynarec_observation else 'no'}",
        ]
        if self.dynarec is not None:
            entries.append(f"config:Dynarec.Enabled={'yes' if self.dynarec else 'no'}")
        if self.record_db is not None:
            entries.append(f"research:WorkbenchRecord={_quote(str(self.record_db))}")
            profile = PROFILES.get(self.profile or "hardware", PROFILES["hardware"])
            entries.append(f"research:WorkbenchBuses={profile.get('buses', 'all')}")
            sh4 = profile.get("sh4", {})
            if "types" in sh4:
                entries.append(f"research:WorkbenchSh4Types={sh4['types']}")
            if "mem_start" in sh4:
                entries.append(f"research:WorkbenchSh4MemStart={sh4['mem_start']}")
                entries.append(f"research:WorkbenchSh4MemEnd={sh4['mem_end']}")
            if "pc_start" in sh4:
                entries.append(f"research:WorkbenchSh4PcStart={sh4['pc_start']}")
                entries.append(f"research:WorkbenchSh4PcEnd={sh4['pc_end']}")
            rows = profile.get("rows", {})
            if rows.get("vram_writes"):
                entries.append("research:WorkbenchVramWrites=yes")
            if rows.get("sample_frames"):
                entries.append("research:WorkbenchSampleFrames=yes")
            if rows.get("texture_bytes"):
                entries.append("research:WorkbenchTextureBytes=yes")
        if self.autoload_slot is not None:
            entries.append("config:Dreamcast.AutoLoadState=yes")
            entries.append(f"config:Dreamcast.SavestateSlot={self.autoload_slot}")
        else:
            entries.append("config:Dreamcast.AutoLoadState=no")
        if self.maple_replay is not None:
            entries.append(f"research:MapleReplay={_quote(str(self.maple_replay))}")
        if self.maple_record is not None:
            entries.append(f"research:MapleRecord={_quote(str(self.maple_record))}")
        if self.rtc_seed is not None:
            entries.append(f"research:DreamcastRtcSeed={self.rtc_seed}")
        entries.extend(self.extra)
        return entries

    def command_line(self) -> list[str]:
        args = [str(self.exe), "-config", ",".join(self.config_entries())]
        if self.game is not None:
            args.append(str(self.game))
        return args


def _quote(value: str) -> str:
    # Flycast's -config parser splits on ',' and accepts '...' or "..." values.
    if any(c in value for c in ",'\" "):
        return '"' + value.replace('"', "'") + '"'
    return value


class FlycastProcess:
    def __init__(self, options: LaunchOptions) -> None:
        if options.port == 0:
            options.port = free_port()
        if not options.token:
            options.token = secrets.token_hex(16)
        self.options = options
        self.process: Optional[subprocess.Popen] = None
        self.control = FlycastControl(options.port, options.token)

    def start(self, ready_timeout: float = 60.0) -> "FlycastProcess":
        exe = Path(self.options.exe)
        if not exe.is_file():
            raise FileNotFoundError(exe)
        creation = 0
        if sys.platform == "win32":
            creation = subprocess.CREATE_NEW_PROCESS_GROUP
        self.process = subprocess.Popen(
            self.options.command_line(), cwd=str(exe.parent), creationflags=creation,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        deadline = time.monotonic() + ready_timeout
        last_error: Optional[Exception] = None
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                raise ControlError(f"flycast exited early with code {self.process.returncode}")
            try:
                self.control.connect()
                self.control.capabilities()
                return self
            except (OSError, ControlError) as error:
                last_error = error
                self.control.close()
                time.sleep(0.5)
        self.kill()
        raise ControlError(f"control socket not reachable within {ready_timeout:.0f}s: {last_error}")

    def stop(self, timeout: float = 30.0) -> int:
        if self.process is None:
            return 0
        if self.process.poll() is None:
            try:
                self.control.exit()
            except ControlError:
                pass
            try:
                self.process.wait(timeout)
            except subprocess.TimeoutExpired:
                self.kill()
        self.control.close()
        return self.process.returncode if self.process.returncode is not None else -1

    def kill(self) -> None:
        if self.process is None or self.process.poll() is not None:
            return
        if sys.platform == "win32":
            subprocess.run(["taskkill", "/PID", str(self.process.pid), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        else:
            self.process.kill()
        try:
            self.process.wait(10)
        except subprocess.TimeoutExpired:
            pass

    @property
    def alive(self) -> bool:
        return self.process is not None and self.process.poll() is None

    def __enter__(self) -> "FlycastProcess":
        return self.start()

    def __exit__(self, *exc: object) -> None:
        self.stop()


def default_exe(repo_root: Optional[Path] = None) -> Optional[Path]:
    """Best-effort guess at a playable build in this repository."""
    root = repo_root or Path(__file__).resolve().parents[3]
    for candidate in ("build-research-runtime/Release/flycast.exe",
                      "build-research-runtime/RelWithDebInfo/flycast.exe",
                      "build/Release/flycast.exe", "build/flycast.exe", "build/flycast"):
        path = root / candidate
        if path.is_file():
            return path
    env = os.environ.get("FLYCAST_EXE")
    return Path(env) if env else None

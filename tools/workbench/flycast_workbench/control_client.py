"""JSON-lines client for the emulator's research control socket.

Protocol: one JSON object per line in each direction. See
core/research/control/control_protocol.h for the command inventory.
"""

from __future__ import annotations

import base64
import json
import socket
import time
from typing import Any, Callable, Optional


class ControlError(Exception):
    """The emulator answered ok=false or the connection failed."""


class FlycastControl:
    def __init__(self, port: int, token: str = "", host: str = "127.0.0.1",
                 timeout: float = 30.0) -> None:
        self.host = host
        self.port = port
        self.token = token
        self.timeout = timeout
        self._socket: Optional[socket.socket] = None
        self._reader = None
        self._next_id = 1

    # ------------------------------------------------------------ lifecycle
    def connect(self) -> "FlycastControl":
        sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        sock.settimeout(self.timeout)
        self._socket = sock
        self._reader = sock.makefile("rb")
        return self

    def close(self) -> None:
        if self._reader is not None:
            try:
                self._reader.close()
            finally:
                self._reader = None
        if self._socket is not None:
            try:
                self._socket.close()
            finally:
                self._socket = None

    def __enter__(self) -> "FlycastControl":
        return self.connect()

    def __exit__(self, *exc: object) -> None:
        self.close()

    @property
    def connected(self) -> bool:
        return self._socket is not None

    # -------------------------------------------------------------- request
    def request(self, cmd: str, args: Optional[dict] = None,
                timeout: Optional[float] = None) -> Any:
        if self._socket is None or self._reader is None:
            self.connect()
        assert self._socket is not None and self._reader is not None
        request_id = self._next_id
        self._next_id += 1
        message: dict[str, Any] = {"id": request_id, "cmd": cmd, "args": args or {}}
        if self.token:
            message["token"] = self.token
        payload = (json.dumps(message) + "\n").encode("utf-8")
        previous = self._socket.gettimeout()
        if timeout is not None:
            self._socket.settimeout(timeout)
        try:
            self._socket.sendall(payload)
            line = self._reader.readline()
        except OSError as error:
            self.close()
            raise ControlError(f"control connection failed: {error}") from error
        finally:
            if self._socket is not None:
                self._socket.settimeout(previous)
        if not line:
            self.close()
            raise ControlError("the emulator closed the control connection")
        response = json.loads(line.decode("utf-8"))
        if response.get("id") != request_id:
            raise ControlError(f"response id mismatch: {response!r}")
        if not response.get("ok"):
            raise ControlError(response.get("error", "unknown control error"))
        return response.get("result")

    # ------------------------------------------------------------- commands
    def capabilities(self) -> dict:
        return self.request("capabilities")

    def status(self) -> dict:
        return self.request("status")

    def pause(self) -> None:
        self.request("pause")

    def resume(self) -> None:
        self.request("resume")

    def savestate(self, slot: int = 0) -> None:
        self.request("savestate", {"slot": slot}, timeout=60.0)

    def loadstate(self, slot: int = 0) -> None:
        self.request("loadstate", {"slot": slot}, timeout=60.0)

    def mem_read(self, address: int, length: int) -> bytes:
        result = self.request("mem_read", {"addr": address, "len": length})
        return base64.b64decode(result["base64"])

    def mem_dump(self, address: int, length: int, path: str) -> dict:
        return self.request("mem_dump", {"addr": address, "len": length, "path": path},
                            timeout=300.0)

    def regs(self) -> dict:
        return self.request("regs")

    def record_start(self, path: str, config: Optional[dict] = None) -> dict:
        args: dict[str, Any] = {"path": path}
        if config:
            args["config"] = config
        return self.request("record_start", args, timeout=60.0)

    def record_stop(self) -> None:
        self.request("record_stop", timeout=600.0)

    def record_status(self) -> dict:
        return self.request("record_status")

    def checkpoint_set(self, pc: int, gate_addr: int = 0, gate_value: int = 0) -> None:
        args: dict[str, Any] = {"pc": pc}
        if gate_addr:
            args["gate_addr"] = gate_addr
            args["gate_value"] = gate_value
        self.request("checkpoint_set", args)

    def checkpoint_clear(self) -> None:
        self.request("checkpoint_clear")

    def exit(self) -> None:
        try:
            self.request("exit")
        finally:
            self.close()

    # -------------------------------------------------------------- waiting
    def wait_until(self, predicate: Callable[[dict], bool], timeout: float,
                   interval: float = 0.25) -> dict:
        deadline = time.monotonic() + timeout
        last: dict = {}
        while time.monotonic() < deadline:
            last = self.status()
            if predicate(last):
                return last
            time.sleep(interval)
        raise ControlError(f"condition not reached within {timeout:.0f}s; last status: {last}")

    def wait_for_game(self, timeout: float = 60.0) -> dict:
        return self.wait_until(lambda s: s.get("game_loaded") and s.get("running"), timeout)

    def wait_for_pause(self, timeout: float = 60.0) -> dict:
        return self.wait_until(lambda s: s.get("game_loaded") and not s.get("running"), timeout)


def hexdump(data: bytes, base: int = 0, width: int = 16) -> str:
    lines = []
    for offset in range(0, len(data), width):
        chunk = data[offset:offset + width]
        hexes = " ".join(f"{b:02x}" for b in chunk)
        text = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"{base + offset:08x}  {hexes:<{width * 3}} {text}")
    return "\n".join(lines)

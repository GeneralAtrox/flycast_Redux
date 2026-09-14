"""Flycast reverse-engineering workbench: control client, launcher, and
SQLite query helpers for recordings produced by the emulator's workbench
recorder. The MCP server in `server.py` exposes the same operations to
agents."""

from .control_client import ControlError, FlycastControl
from .launcher import PROFILES, FlycastProcess, LaunchOptions, free_port

__all__ = [
    "ControlError",
    "FlycastControl",
    "FlycastProcess",
    "LaunchOptions",
    "PROFILES",
    "free_port",
]

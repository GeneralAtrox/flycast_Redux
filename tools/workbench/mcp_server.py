"""Entry point for MCP clients: `python tools/workbench/mcp_server.py`.

Adds this directory to sys.path so the package imports without installation.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from flycast_workbench.server import main  # noqa: E402

if __name__ == "__main__":
    main()

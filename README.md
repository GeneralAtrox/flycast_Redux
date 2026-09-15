<img src="shell/linux/flycast.png" alt="flycast logo" width="150"/>

# flycast_Redux

A reverse-engineering workbench built on [Flycast](https://github.com/flyinghead/flycast).
Stock Flycast runs a Dreamcast game. This fork also records *why* things
happen: which SH-4 instruction wrote which bytes, which DMA carried them to
the PowerVR, which render consumed them, and what the Maple, GD-ROM, and AICA
traffic looked like at that moment. Everything lands in one SQLite database
you can query with SQL, from the command line, or from an agent over MCP.

It is game-agnostic. No game addresses or names live here.

## For agents: read this first

- All research features are inert unless enabled by `research.*` options.
  Never make one default-on. Upstream Flycast behaviour is unchanged.
- Start with [docs/workbench/Workbench.md](docs/workbench/Workbench.md). It
  covers launching, the control socket, the recorder, the database schema,
  the MCP tools, and the Ghidra loop.
- [docs/workbench/Architecture.md](docs/workbench/Architecture.md) explains
  the observation buses and how attribution to an SH-4 instruction works.
  The per-bus docs under `docs/research/` describe the event models and the
  Lua discovery API.
- Research-side source files stay under 400 lines. Split by responsibility
  when you touch an oversized file. Upstream Flycast files are exempt; keep
  diffs to them minimal so upstream merges stay cheap.
- The SH-4 bus, when recorded, slows emulation roughly tenfold. Prefer
  hardware-only profiles or PC and address ranges for long sessions.
- Windows is the primary platform. The Python tools are cross-platform.

## Quick start

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target flycast
python tools/workbench/live_smoke.py build/Release/flycast.exe "C:\games\game.gdi"
```

From an agent, add the MCP server in [.mcp.json](.mcp.json) (already present
for Claude Code) and call `flycast_launch`, `flycast_record_start`,
`db_who_writes`, and friends. From a shell:

```bash
python -m flycast_workbench launch "C:\games\game.gdi" --record run.db --profile hardware
python -m flycast_workbench --port 51234 --token ... mem 0x8c010000 64
python -m flycast_workbench db run.db who_writes 0x8c2a1f30
```

Run the Python commands from `tools/workbench`, or `pip install -e tools/workbench`.

## Layout

| Path | Contents |
| --- | --- |
| `core/research/*_observation.*` | Native observation buses: SH-4, Maple, PVR TA, PVR draw, PVR presentation, GD-ROM (HLE and hardware), AICA, CD-DA. |
| `core/research/sh4_observation_runtime*` | SH-4 instruction frames, call/return derivation, dynarec SHIL markers. |
| `core/research/maple_*` | Deterministic Maple record/replay. |
| `core/research/workbench/` | The SQLite recorder: config, queue, per-bus row binders, runtime glue. |
| `core/research/control/` | Loopback TCP JSON-lines control server and its emulator bindings. |
| `core/research/lua/`, `core/research/sh4_lua_subscriptions*` | `flycast.research` Lua discovery API. |
| `core/research/sh4_pc_checkpoint_runtime.*` | Pause exactly when a given instruction completes. |
| `core/debug/gdb_server.cpp` | Read-only, loopback-only GDB stub. |
| `tools/workbench/` | Python: control client, launcher, queries, CLI, MCP server, smoke test. |
| `tools/ghidra/` | Ghidra scripts: export symbols, import runtime facts. |
| `tools/research/lua/` | Ready-to-run Lua watchers (Maple, memory, causal slice, hardware, CD-DA). |
| `docs/workbench/`, `docs/research/` | Guide and architecture; bus and Lua model docs. |
| `tests/src/research/` | googletest suites for buses, runtime, recorder, control protocol. |

## Build and test

```powershell
cmake -S . -B build-research -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_RESEARCH_TOOLS=ON -DENABLE_CTEST=ON
cmake --build build-research --config Release
ctest --test-dir build-research -C Release -R flycast-research-tests --output-on-failure
```

`ENABLE_CTEST` turns the `flycast` binary into the in-tree test runner, so
keep a second build tree without it for playing. `flycast-research-tests`
is the standalone suite for the research code.

## Upstream

Based on flyinghead/flycast. Upstream README, license, and build notes for
other platforms apply unchanged. See [LICENSE](LICENSE).

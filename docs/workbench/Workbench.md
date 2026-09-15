# Flycast workbench guide

The workbench has three parts: a recorder inside the emulator that writes
observations to SQLite, a control socket that lets a tool drive the emulator,
and Python tooling (CLI and MCP server) that launches Flycast, talks to the
socket, and queries recordings.

## Launch options

Everything is passed as transient `-config` options so `emu.cfg` is untouched.
Section `research`, keys below. The launcher in `tools/workbench` sets them for you.

| Key | Meaning |
| --- | --- |
| `ControlPort` | Loopback TCP port for the control socket. 0 disables. |
| `ControlToken` | Optional shared secret every request must carry. |
| `DynarecObservation` | Compile per-instruction markers into the dynarec so SH-4 attribution is exact under dynarec. The launcher sets it. |
| `WorkbenchRecord` | SQLite path. Starts recording at boot. |
| `WorkbenchBuses` | Comma list: `sh4,maple,pvr-ta,pvr-draw,pvr-present,gdrom,gdrom-hw,aica,cdda` or `all`. |
| `WorkbenchSh4Types` | Comma list: `instruction-begin,instruction,instruction-abort,memory-read,memory-write,exception,call,return` or `all`. Default `call,return,exception`. |
| `WorkbenchSh4PcStart/End`, `WorkbenchSh4MemStart/End` | Inclusive filters on instruction PC and accessed address. |
| `WorkbenchVramWrites`, `WorkbenchSampleFrames`, `WorkbenchTextureBytes` | High-volume payload switches, off by default. |
| `Sh4PcCheckpoint`, `Sh4PcCheckpointU32Address/Value` | Pause when that top-level instruction completes, optionally gated on a RAM word. |
| `MapleRecord`, `MapleReplay`, `MapleDmaCheckpoint` | Deterministic input record and replay. See [MapleReplay.md](MapleReplay.md). |
| `DreamcastRtcSeed` | Fixed RTC. Record/replay pins one automatically. |

Checkpoints require `config:rend.ThreadedRendering=no`, which the launcher sets.

## Control socket

JSON lines over TCP on 127.0.0.1. One object per line each way.

```json
{"id": 1, "token": "...", "cmd": "mem_read", "args": {"addr": "0x8c010000", "len": 64}}
{"id": 1, "ok": true, "result": {"addr": 2348875776, "len": 64, "base64": "..."}}
```

Commands: `capabilities`, `status`, `pause`, `resume`, `savestate {slot}`,
`loadstate {slot}`, `mem_read {addr,len}` (up to 1 MiB, base64), `mem_dump
{addr,len,path}` (up to 64 MiB to a file), `regs`, `record_start {path,
config}`, `record_stop`, `record_status`, `checkpoint_set {pc, gate_addr,
gate_value}`, `checkpoint_clear`, `exit`. Errors come back as `{"ok": false,
"error": "..."}`; nothing raises. Memory and register reads are not
synchronized while the emulator runs. Pause first for a coherent snapshot.
Readable regions: system RAM (`0x8c...`), VRAM (`0xa4...`), AICA RAM
(`0x0080....`). Nothing is writable.

The old read-only GDB stub is still available with `config:Debug.GDBEnabled=yes`
for tools that speak GDB.

## Recorder

`record_start` takes a config object. All keys optional, unknown keys rejected.

```json
{
  "buses": "sh4,maple,pvr-ta",
  "sh4": {"types": "call,return,memory-write", "backend": "any",
          "pc_start": "0x8c010000", "pc_end": "0x8c01ffff",
          "mem_start": "0x8c100000", "mem_end": "0x8c10ffff"},
  "maple": {"bus": 0, "port": 5, "command": 9},
  "aica": {"writers": "sh4,g2dma,reios"},
  "rows": {"texture_bytes": false, "draw_vertices": true, "vram_writes": false,
           "framebuffer_bytes": false, "sector_bytes": false, "sample_frames": false},
  "queue_capacity": 262144,
  "note": "what this run is for"
}
```

Presets live in `tools/workbench/flycast_workbench/launcher.py` (`hardware`,
`calls`, `memory`, `input-to-render`, `full`). The recorder uses discovery
subscriptions, so Lua watchers can run at the same time.

Throughput: about 300k rows/s sustained. SH-4 rows ride a bulk lane and all
other buses a priority lane, so a flood of instruction events never starves
hardware rows. Drops are counted in `runs.dropped` and in `record_status`.
Recording the SH-4 bus at all slows emulation roughly tenfold; narrow with PC
and address ranges, or use a hardware-only profile, for long sessions.

## Database

One SQLite file, WAL mode, readable while recording. Tables:

| Table | Rows |
| --- | --- |
| `runs`, `meta` | One row per recording: game id, media path, Flycast version, backend, config JSON, written and dropped counts. |
| `sh4_events` | Instruction, memory, exception, call, return events with PC, tick, and a register snapshot where available. |
| `maple_events` | Request and response frames with bus, port, command, payload blob. |
| `pvr_ta_events` (+ `_selected_contexts`, `_selection_reads`) | List init, accepted 32-byte TA blocks with the submitting instruction, STARTRENDER, render done. |
| `pvr_draw_events` (+ `_vertices`, `_blocks`, `_consumed`), `textures` | Decoded primitives with owning instruction, vertices, texture identity. |
| `pvr_present_events` | PVR register writes, VRAM writes (opt-in), render and framebuffer generations, presentations. |
| `gdrom_events`, `gdrom_hw_events` | REIOS HLE and ATA/ATAPI paths. |
| `aica_events`, `cdda_events` | Sound register and RAM writes, key on/off, CD-DA sectors and control. |
| `symbols` | Empty until you import a Ghidra export. |

Conventions: `UINT32_MAX` sentinels are stored as NULL, generation 0 is NULL,
every hardware row carries `init_*` columns (generation, pc, pr, opcode,
backend, delay-slot depth) for the SH-4 instruction that caused it, and
`init_gen` joins to `sh4_events` frames when the SH-4 bus was also recorded.
`sh4_events.type`: 1 begin, 2 end, 3 abort, 4 read, 5 write, 6 exception,
7 call, 8 return. `call_kind`: 1 bsr, 2 bsrf, 3 jsr, 4 jmp.

Indexes are created when the recording stops.

## Queries and MCP

`tools/workbench/flycast_workbench/queries.py` holds the canned questions:
`who_writes`, `who_reads`, `writes_timeline`, `callers_of`, `callees_of`,
`hot_functions`, `indirect_call_targets`, `call_stack_at`, `render_summary`,
`ta_submitters`, `draw_owners`, `texture_users`, `register_writes`,
`maple_summary`, `gdrom_summary`, `aica_writers`. Results carry symbol names
once `symbols` is populated.

The MCP server exposes them as `db_*` tools and the control socket as
`flycast_*` tools. Start it with `python tools/workbench/mcp_server.py`
(needs the `mcp` package). The repository `.mcp.json` registers it for Claude
Code. A typical loop:

1. `flycast_launch(game, profile="hardware", record_db="boot.db")`
2. `flycast_loadstate(3)` to jump to the moment of interest
3. `flycast_record_start("scene.db", config={...})`, play, `flycast_record_stop()`
4. `db_who_writes("scene.db", "0x8c2a1f30")`, `db_ta_submitters("scene.db")`
5. `flycast_checkpoint(pc, wait_seconds=30)`, then `flycast_regs()` and
   `flycast_mem_read(...)` at that exact instruction boundary

## Ghidra loop

1. In Ghidra run `tools/ghidra/ExportSymbols.java` with an output path. It
   writes functions and labels as JSON.
2. `db_import_symbols(db, "symbols.json")`. Query results now show names.
3. `db_export_ghidra_facts(db, "facts.json")` collects observed call targets,
   indirect call targets, memory writers, and TA submitters.
4. Run `tools/ghidra/ImportWorkbenchFacts.java` on `facts.json`. It creates
   functions at observed call targets, adds computed-call references for
   indirect jumps, and leaves comments and bookmarks. Re-running is idempotent.

## Lua watchers

`tools/research/lua/` scripts subscribe to the same buses from Lua and emit
JSONL. They are useful for quick looks without a database. Their PowerShell
launchers in `tools/research/` need PowerShell 7 (`pwsh`); under Windows
PowerShell 5.1 they refuse to start rather than fail part-way. API reference:
[../research/LuaSubscriptionsV1.md](../research/LuaSubscriptionsV1.md).

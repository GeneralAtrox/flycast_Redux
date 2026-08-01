-- Game-agnostic, discovery-only SH-4 memory provenance watcher.
--
-- Exact accesses come from the canonical native observation bus. Optional
-- lifecycle snapshots are bounded main-RAM samples and are labeled separately.

local supplied = rawget(_G, "flycast_memory_watch") or {}

local function setting(name, environment, default)
  local value = supplied[name]
  if value == nil then value = os.getenv(environment) end
  if value == nil or value == "" then return default end
  return value
end

local function integer_setting(name, environment, minimum, maximum, default)
  local value = setting(name, environment, default)
  value = tonumber(value)
  assert(value and value == math.floor(value) and value >= minimum and value <= maximum,
    name .. " must be an integer in [" .. minimum .. ", " .. maximum .. "]")
  return value
end

local function optional_integer(name, environment, minimum, maximum)
  local value = setting(name, environment, nil)
  if value == nil then return nil end
  value = tonumber(value)
  assert(value and value == math.floor(value) and value >= minimum and value <= maximum,
    name .. " must be an integer in [" .. minimum .. ", " .. maximum .. "]")
  return value
end

local function boolean_setting(name, environment, default)
  local value = setting(name, environment, default)
  if type(value) == "boolean" then return value end
  value = tostring(value):lower()
  if value == "true" or value == "yes" or value == "1" then return true end
  if value == "false" or value == "no" or value == "0" then return false end
  error(name .. " must be true or false")
end

local function choice_setting(name, environment, choices, default)
  local value = tostring(setting(name, environment, default)):lower()
  assert(choices[value], name .. " has unsupported value: " .. value)
  return value
end

local output_path = assert(setting("output", "FLYCAST_MEMORY_WATCH_OUTPUT", nil),
  "flycast_memory_watch.output or FLYCAST_MEMORY_WATCH_OUTPUT is required")
local start_address = integer_setting("start_address",
  "FLYCAST_MEMORY_WATCH_START_ADDRESS", 0, 0xffffffff)
local length = integer_setting("length", "FLYCAST_MEMORY_WATCH_LENGTH",
  1, 1024 * 1024)
local end_address = start_address + length - 1
assert(end_address <= 0xffffffff, "memory range exceeds the 32-bit guest address space")
local access = choice_setting("access", "FLYCAST_MEMORY_WATCH_ACCESS",
  { read = true, write = true, both = true }, "write")
local backend = choice_setting("backend", "FLYCAST_MEMORY_WATCH_BACKEND",
  { interpreter = true, dynarec = true, any = true }, "interpreter")
local queue_capacity = integer_setting("queue_capacity",
  "FLYCAST_MEMORY_WATCH_QUEUE_CAPACITY", 1, 65536, 4096)
local max_events = integer_setting("max_events", "FLYCAST_MEMORY_WATCH_MAX_EVENTS",
  1, 100000000, 100000)
local auto_exit_frames = optional_integer("auto_exit_frames",
  "FLYCAST_MEMORY_WATCH_AUTO_EXIT_FRAMES", 1, 2147483647)
local start_pc = optional_integer("start_pc", "FLYCAST_MEMORY_WATCH_START_PC",
  0, 0xffffffff)
local end_pc = optional_integer("end_pc", "FLYCAST_MEMORY_WATCH_END_PC",
  0, 0xffffffff)
assert((start_pc == nil) == (end_pc == nil),
  "start_pc and end_pc must be supplied together")
if start_pc ~= nil then assert(start_pc <= end_pc, "start_pc must not exceed end_pc") end
local snapshots = boolean_setting("snapshots", "FLYCAST_MEMORY_WATCH_SNAPSHOTS", true)

if snapshots then
  assert(length <= 65536, "snapshot-enabled ranges are limited to 65536 bytes")
  local alias_start = math.floor(start_address / 0x20000000)
  local alias_end = math.floor(end_address / 0x20000000)
  local physical_start = start_address % 0x20000000
  local physical_end = end_address % 0x20000000
  assert(alias_start == alias_end and physical_start >= 0x0c000000
      and physical_end <= 0x0dffffff,
    "snapshots are restricted to one Dreamcast main-RAM alias")
end

local escapes = {
  ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f',
  ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t',
}

local function json_string(value)
  return '"' .. tostring(value):gsub('[%z\1-\31\\"]', function(character)
    return escapes[character] or string.format("\\u%04x", string.byte(character))
  end) .. '"'
end

local function json_boolean(value) return value and "true" or "false" end
local function json_nullable_number(value)
  return value == nil and "null" or tostring(value)
end

local function hex32(value) return string.format("0x%08x", value) end
local function hex16(value) return string.format("0x%04x", value) end

local prior_state = rawget(_G, "flycast_memory_watch_state")
if prior_state and prior_state.shutdown then prior_state.shutdown("reload", true) end

local callbacks = rawget(_G, "flycast_callbacks") or {}
local prior_callbacks
if prior_state and prior_state.prior_callbacks then
  prior_callbacks = prior_state.prior_callbacks
else
  prior_callbacks = {}
  for key, value in pairs(callbacks) do prior_callbacks[key] = value end
end

local state = {
  prior_callbacks = prior_callbacks,
  tokens = {},
  stats = {},
  output = nil,
  closed = true,
  frames = 0,
  accesses = 0,
  reads = 0,
  writes = 0,
  snapshot_count = 0,
  snapshot_errors = 0,
  final_snapshot_taken = false,
  stop_requested = false,
}
rawset(_G, "flycast_memory_watch_state", state)

local session_sequence = (rawget(_G, "flycast_memory_watch_session_sequence") or 0) + 1
rawset(_G, "flycast_memory_watch_session_sequence", session_sequence)

local function write_line(text)
  assert(state.output, "memory watcher output is closed")
  assert(state.output:write(text, "\n"))
  assert(state.output:flush())
end

local function new_stats()
  return { delivered = 0, dropped = 0, callback_errors = 0, queued = 0 }
end

local function copy_stats(target, value)
  if value then
    target.delivered = value.delivered
    target.dropped = value.dropped
    target.callback_errors = value.callback_errors
    target.queued = value.queued
  end
end

local function refresh_stats()
  for event, token in pairs(state.tokens) do
    copy_stats(state.stats[event], flycast.research.subscription_stats(token))
  end
end

local function event_json(event)
  return table.concat({
    '{"record_type":"access","discovery":true,"authoritative_evidence":false',
    ',"schema_version":', tostring(event.schema_version),
    ',"event":', json_string(event.event),
    ',"ordinal":', json_string(event.ordinal_decimal),
    ',"tick":', json_string(event.tick_decimal),
    ',"backend":', json_string(event.backend),
    ',"pc":', tostring(event.pc),
    ',"pc_hex":', json_string(hex32(event.pc)),
    ',"opcode":', tostring(event.opcode),
    ',"opcode_hex":', json_string(hex16(event.opcode)),
    ',"delay_slot_depth":', tostring(event.delay_slot_depth),
    ',"address":', tostring(event.address),
    ',"address_hex":', json_string(hex32(event.address)),
    ',"width_bytes":', tostring(event.width),
    ',"value_hex":', json_string(event.value_hex),
    '}'
  })
end

local function detach_at_event_limit(current_event)
  refresh_stats()
  -- Native delivery is committed after this callback returns. Account for
  -- the current successful record before detaching its subscription.
  state.stats[current_event].delivered = state.stats[current_event].delivered + 1
  for event, token in pairs(state.tokens) do
    flycast.research.unsubscribe(token)
    state.stats[event].queued = 0
  end
  state.tokens = {}
  state.stop_requested = true
end

local function access_callback(event)
  state.accesses = state.accesses + 1
  if event.event == "memory-read" then
    state.reads = state.reads + 1
  else
    state.writes = state.writes + 1
  end
  write_line(event_json(event))
  if state.accesses >= max_events then detach_at_event_limit(event.event) end
end

local function snapshot(point)
  if not snapshots or state.closed then return end
  local ok, result = pcall(function()
    local pieces = {}
    for offset = 0, length - 1 do
      pieces[offset + 1] = string.format("%02x",
        flycast.memory.read8(start_address + offset))
    end
    return table.concat(pieces)
  end)
  if not ok then
    state.snapshot_errors = state.snapshot_errors + 1
    write_line('{"record_type":"snapshot-error","discovery":true,'
      .. '"authoritative_evidence":false,"point":' .. json_string(point)
      .. ',"message":' .. json_string(result) .. '}')
    return
  end
  state.snapshot_count = state.snapshot_count + 1
  write_line('{"record_type":"snapshot","discovery":true,'
    .. '"authoritative_evidence":false,"point":' .. json_string(point)
    .. ',"coherent_with_access_stream":false'
    .. ',"start_address":' .. tostring(start_address)
    .. ',"start_address_hex":' .. json_string(hex32(start_address))
    .. ',"length":' .. tostring(length)
    .. ',"payload_hex":' .. json_string(result) .. '}')
end

local function filter(event)
  local value = {
    event = event,
    backend = backend,
    start_address = start_address,
    end_address = end_address,
    queue_capacity = queue_capacity,
  }
  if start_pc ~= nil then
    value.start_pc = start_pc
    value.end_pc = end_pc
  end
  return value
end

local function subscribe_one(event)
  state.stats[event] = new_stats()
  state.tokens[event] = flycast.research.subscribe(filter(event), access_callback)
end

local function subscribe()
  local ok, message = pcall(function()
    if access == "read" or access == "both" then subscribe_one("memory-read") end
    if access == "write" or access == "both" then subscribe_one("memory-write") end
  end)
  if not ok then
    for _, token in pairs(state.tokens) do flycast.research.unsubscribe(token) end
    state.tokens = {}
    error(message)
  end
end

local function open_session()
  state.output = assert(io.open(output_path, "ab"))
  state.closed = false
  state.frames = 0
  state.accesses = 0
  state.reads = 0
  state.writes = 0
  state.snapshot_count = 0
  state.snapshot_errors = 0
  state.final_snapshot_taken = false
  state.stop_requested = false
  state.tokens = {}
  state.stats = {}
  subscribe()
  write_line('{"record_type":"session-start","schema":"flycast-memory-watch-jsonl",'
    .. '"schema_version":1,"discovery":true,"authoritative_evidence":false'
    .. ',"session":' .. tostring(session_sequence)
    .. ',"start_address":' .. tostring(start_address)
    .. ',"start_address_hex":' .. json_string(hex32(start_address))
    .. ',"end_address":' .. tostring(end_address)
    .. ',"end_address_hex":' .. json_string(hex32(end_address))
    .. ',"length":' .. tostring(length)
    .. ',"access":' .. json_string(access)
    .. ',"backend":' .. json_string(backend)
    .. ',"start_pc":' .. json_nullable_number(start_pc)
    .. ',"end_pc":' .. json_nullable_number(end_pc)
    .. ',"queue_capacity":' .. tostring(queue_capacity)
    .. ',"max_events":' .. tostring(max_events)
    .. ',"snapshots":' .. json_boolean(snapshots) .. '}')
end

local function aggregate(field)
  local total = 0
  for _, value in pairs(state.stats) do total = total + value[field] end
  return total
end

function state.shutdown(reason, capture_final_snapshot)
  if state.closed then return end
  if capture_final_snapshot and not state.final_snapshot_taken then
    state.final_snapshot_taken = true
    snapshot("final")
  end
  refresh_stats()
  local dropped = aggregate("dropped")
  local callback_errors = aggregate("callback_errors")
  local queued = aggregate("queued")
  local incomplete = dropped > 0 or callback_errors > 0
    or state.snapshot_errors > 0 or queued > 0
  write_line('{"record_type":"summary","schema":"flycast-memory-watch-jsonl",'
    .. '"schema_version":1,"discovery":true,"authoritative_evidence":false'
    .. ',"reason":' .. json_string(reason)
    .. ',"complete":' .. json_boolean(not incomplete)
    .. ',"accesses":' .. tostring(state.accesses)
    .. ',"reads":' .. tostring(state.reads)
    .. ',"writes":' .. tostring(state.writes)
    .. ',"delivered":' .. tostring(aggregate("delivered"))
    .. ',"dropped":' .. tostring(dropped)
    .. ',"callback_errors":' .. tostring(callback_errors)
    .. ',"queued":' .. tostring(queued)
    .. ',"snapshots":' .. tostring(state.snapshot_count)
    .. ',"snapshot_errors":' .. tostring(state.snapshot_errors) .. '}')
  for _, token in pairs(state.tokens) do flycast.research.unsubscribe(token) end
  state.tokens = {}
  state.output:close()
  state.output = nil
  state.closed = true
end

local function call_prior(name)
  local callback = state.prior_callbacks[name]
  if type(callback) == "function" then callback() end
end

callbacks.start = function()
  if state.closed then
    session_sequence = session_sequence + 1
    rawset(_G, "flycast_memory_watch_session_sequence", session_sequence)
    open_session()
  end
  write_line('{"record_type":"lifecycle","event":"start","media":'
    .. json_string(flycast.state.media or "") .. ',"game_id":'
    .. json_string(flycast.state.gameId or "") .. '}')
  snapshot("start")
  call_prior("start")
end

callbacks.loadState = function()
  write_line('{"record_type":"lifecycle","event":"load-state"}')
  snapshot("load-state")
  call_prior("loadState")
end

callbacks.overlay = function()
  refresh_stats()
  state.frames = state.frames + 1
  call_prior("overlay")
  local reason
  if state.stop_requested then
    reason = "max-events"
  elseif auto_exit_frames and state.frames >= auto_exit_frames then
    auto_exit_frames = nil
    reason = "auto-exit"
  end
  if reason then
    state.shutdown(reason, true)
    flycast.emulator.requestExit()
  end
end

callbacks.terminate = function()
  -- Terminate is delivered after guest teardown, so reading main RAM here
  -- would produce a misleading post-unload image rather than a final sample.
  state.shutdown("terminate", false)
  call_prior("terminate")
end

flycast_callbacks = callbacks
open_session()

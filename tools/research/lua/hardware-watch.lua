-- Game-agnostic, discovery-only PowerVR, GD-ROM, CD-DA and AICA watcher.
--
-- Configure through `flycast_hardware_watch` before loading this file, or use
-- FLYCAST_HARDWARE_WATCH_OUTPUT and FLYCAST_HARDWARE_WATCH_EVENTS. Events are
-- comma-separated. Per-event native filters are available through
-- `flycast_hardware_watch.filters[event]`.

local supplied = rawget(_G, "flycast_hardware_watch") or {}

local function setting(name, environment, default)
  local value = supplied[name]
  if value == nil then value = os.getenv(environment) end
  if value == nil or value == "" then return default end
  return value
end

local function integer_setting(name, environment, minimum, maximum, default)
  local value = tonumber(setting(name, environment, default))
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

local supported = {
  ["pvr-ta-list-init"] = true, ["pvr-ta-list-continue"] = true,
  ["pvr-ta-block"] = true, ["pvr-start-render"] = true,
  ["pvr-render-done"] = true, ["pvr-ta-reset"] = true,
  ["pvr-register-write"] = true, ["pvr-vram-write"] = true,
  ["pvr-render-queued"] = true, ["pvr-render-completed"] = true,
  ["pvr-framebuffer"] = true, ["pvr-presentation"] = true,
  ["pvr-presentation-reset"] = true, ["pvr-primitive"] = true,
  ["pvr-draw"] = true, ["pvr-draw-render-completed"] = true,
  ["pvr-draw-reset"] = true, ["gdrom-command"] = true,
  ["gdrom-transfer"] = true, ["gdrom-complete"] = true,
  ["gdrom-abort"] = true, ["gdrom-reset"] = true,
  ["cdda-control-accepted"] = true, ["cdda-control-applied"] = true,
  ["cdda-sector"] = true, ["cdda-reset"] = true,
  ["aica-register-write"] = true, ["aica-ram-write"] = true,
  ["aica-dma-begin"] = true, ["aica-dma-transfer"] = true,
  ["aica-dma-complete"] = true, ["aica-key-on"] = true,
  ["aica-key-off"] = true, ["aica-sample"] = true,
  ["aica-reset"] = true, ["aica-key-batch"] = true,
  ["aica-cdda-sector"] = true, ["aica-sample-suppressed"] = true,
}

local output_path = assert(setting("output", "FLYCAST_HARDWARE_WATCH_OUTPUT", nil),
  "flycast_hardware_watch.output or FLYCAST_HARDWARE_WATCH_OUTPUT is required")
local configured_events = setting("events", "FLYCAST_HARDWARE_WATCH_EVENTS", nil)
assert(configured_events ~= nil,
  "flycast_hardware_watch.events or FLYCAST_HARDWARE_WATCH_EVENTS is required")
local queue_capacity = integer_setting("queue_capacity",
  "FLYCAST_HARDWARE_WATCH_QUEUE_CAPACITY", 1, 65536, 4096)
local max_events = integer_setting("max_events", "FLYCAST_HARDWARE_WATCH_MAX_EVENTS",
  1, 100000000, 100000)
local auto_exit_frames = optional_integer("auto_exit_frames",
  "FLYCAST_HARDWARE_WATCH_AUTO_EXIT_FRAMES", 1, 2147483647)
local filters = supplied.filters or {}
assert(type(filters) == "table", "flycast_hardware_watch.filters must be a table")

local events = {}
local seen = {}
if type(configured_events) == "string" then
  for event in configured_events:gmatch("[^,%s]+") do
    assert(supported[event], "unsupported hardware event: " .. event)
    assert(not seen[event], "duplicate hardware event: " .. event)
    seen[event] = true
    events[#events + 1] = event
  end
elseif type(configured_events) == "table" then
  for _, event in ipairs(configured_events) do
    assert(type(event) == "string" and supported[event],
      "unsupported hardware event: " .. tostring(event))
    assert(not seen[event], "duplicate hardware event: " .. event)
    seen[event] = true
    events[#events + 1] = event
  end
else
  error("events must be a comma-separated string or array")
end
assert(#events > 0, "at least one hardware event is required")

local escapes = {
  ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f',
  ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t',
}
local function json_string(value)
  return '"' .. tostring(value):gsub('[%z\1-\31\\"]', function(character)
    return escapes[character] or string.format("\\u%04x", string.byte(character))
  end) .. '"'
end

local raw_binary = {
  block = true, bytes = true, channel_registers = true, payload = true,
}
local function json(value, depth)
  depth = depth or 0
  assert(depth <= 12, "JSON nesting exceeds watcher limit")
  local kind = type(value)
  if kind == "nil" then return "null" end
  if kind == "boolean" then return value and "true" or "false" end
  if kind == "number" then
    assert(value == value and value ~= math.huge and value ~= -math.huge,
      "non-finite JSON number")
    return tostring(value)
  end
  if kind == "string" then return json_string(value) end
  assert(kind == "table", "unsupported JSON value: " .. kind)
  local array = true
  local count, maximum = 0, 0
  for key in pairs(value) do
    if type(key) ~= "number" or key < 1 or key ~= math.floor(key) then
      array = false
    else
      count = count + 1
      if key > maximum then maximum = key end
    end
  end
  if array and count == maximum then
    local pieces = {}
    for index = 1, maximum do pieces[index] = json(value[index], depth + 1) end
    return "[" .. table.concat(pieces, ",") .. "]"
  end
  local keys = {}
  for key in pairs(value) do
    assert(type(key) == "string", "JSON object keys must be strings")
    if not raw_binary[key] then keys[#keys + 1] = key end
  end
  table.sort(keys)
  local pieces = {}
  for index, key in ipairs(keys) do
    pieces[index] = json_string(key) .. ":" .. json(value[key], depth + 1)
  end
  return "{" .. table.concat(pieces, ",") .. "}"
end

local prior_state = rawget(_G, "flycast_hardware_watch_state")
if prior_state and prior_state.shutdown then prior_state.shutdown("reload", false) end
local callbacks = rawget(_G, "flycast_callbacks") or {}
local prior_callbacks = {}
if prior_state and prior_state.prior_callbacks then
  prior_callbacks = prior_state.prior_callbacks
else
  for key, value in pairs(callbacks) do prior_callbacks[key] = value end
end

local state = {
  prior_callbacks = prior_callbacks, tokens = {}, stats = {}, output = nil,
  closed = true, frames = 0, observed = 0, stop_requested = false,
}
rawset(_G, "flycast_hardware_watch_state", state)
local session = (rawget(_G, "flycast_hardware_watch_session_sequence") or 0) + 1
rawset(_G, "flycast_hardware_watch_session_sequence", session)

local function write_line(line)
  assert(state.output and state.output:write(line, "\n"))
  assert(state.output:flush())
end

local function new_stats()
  return { delivered = 0, dropped = 0, callback_errors = 0, queued = 0 }
end
local function refresh_stats()
  for event, token in pairs(state.tokens) do
    local value = flycast.research.subscription_stats(token)
    if value then
      state.stats[event] = {
        delivered = value.delivered, dropped = value.dropped,
        callback_errors = value.callback_errors, queued = value.queued,
      }
    end
  end
end
local function aggregate(field)
  local total = 0
  for _, value in pairs(state.stats) do total = total + value[field] end
  return total
end

local function event_callback(event)
  state.observed = state.observed + 1
  write_line(json({ record_type = "observation", observation = event }))
  if state.observed >= max_events then state.stop_requested = true end
end

local function subscription_filter(event)
  local configured = filters[event]
  assert(configured == nil or type(configured) == "table",
    "filter for " .. event .. " must be a table")
  local value = { event = event, queue_capacity = queue_capacity }
  if configured then
    for key, item in pairs(configured) do
      assert(key ~= "event" and key ~= "queue_capacity",
        "event and queue_capacity are controlled by the watcher")
      value[key] = item
    end
  end
  return value
end

local function subscribe_all()
  local ok, failure = pcall(function()
    for _, event in ipairs(events) do
      state.stats[event] = new_stats()
      state.tokens[event] = flycast.research.subscribe(
        subscription_filter(event), event_callback)
    end
  end)
  if not ok then
    for _, token in pairs(state.tokens) do flycast.research.unsubscribe(token) end
    state.tokens = {}
    error(failure)
  end
end

local function open_session()
  state.output = assert(io.open(output_path, "ab"))
  state.closed = false
  state.frames = 0
  state.observed = 0
  state.stop_requested = false
  state.tokens = {}
  state.stats = {}
  subscribe_all()
  write_line(json({ record_type = "session-start",
    schema = "flycast-hardware-watch-jsonl", schema_version = 1,
    discovery = true, authoritative_evidence = false, session = session,
    events = events, queue_capacity = queue_capacity, max_events = max_events }))
end

function state.shutdown(reason, controlled)
  if state.closed then return end
  refresh_stats()
  local dropped = aggregate("dropped")
  local callback_errors = aggregate("callback_errors")
  local queued = aggregate("queued")
  write_line(json({ record_type = "summary", schema = "flycast-hardware-watch-jsonl",
    schema_version = 1, discovery = true, authoritative_evidence = false,
    reason = reason, complete = controlled and dropped == 0
      and callback_errors == 0 and queued == 0,
    observations = state.observed, delivered = aggregate("delivered"),
    dropped = dropped, callback_errors = callback_errors, queued = queued }))
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
    session = session + 1
    rawset(_G, "flycast_hardware_watch_session_sequence", session)
    open_session()
  end
  write_line(json({ record_type = "lifecycle", event = "start",
    media = flycast.state.media or "", game_id = flycast.state.gameId or "" }))
  call_prior("start")
end
callbacks.loadState = function()
  write_line(json({ record_type = "lifecycle", event = "load-state" }))
  call_prior("loadState")
end
callbacks.overlay = function()
  refresh_stats()
  state.frames = state.frames + 1
  call_prior("overlay")
  local reason
  if state.stop_requested then reason = "max-events"
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
  state.shutdown("terminate", false)
  call_prior("terminate")
end
flycast_callbacks = callbacks
open_session()

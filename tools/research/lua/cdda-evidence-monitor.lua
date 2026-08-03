-- Long-running, discovery-only monitor for the first complete CD-DA causal chain.
-- It exits Flycast only after observing:
--   PLAY/PLAY2 accepted -> applied successfully -> successful sector ->
--   nonzero AICA CD-DA mixer contribution for that exact sector generation.

local supplied = rawget(_G, "flycast_cdda_evidence_monitor") or {}

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

local output_path = assert(setting("output", "FLYCAST_CDDA_MONITOR_OUTPUT", nil),
  "flycast_cdda_evidence_monitor.output or FLYCAST_CDDA_MONITOR_OUTPUT is required")
local queue_capacity = integer_setting("queue_capacity",
  "FLYCAST_CDDA_MONITOR_QUEUE_CAPACITY", 1, 65536, 4096)
local auto_exit_frames = setting("auto_exit_frames",
  "FLYCAST_CDDA_MONITOR_AUTO_EXIT_FRAMES", nil)
if auto_exit_frames ~= nil then
  auto_exit_frames = tonumber(auto_exit_frames)
  assert(auto_exit_frames and auto_exit_frames == math.floor(auto_exit_frames)
    and auto_exit_frames >= 1 and auto_exit_frames <= 2147483647,
    "auto_exit_frames must be an integer in [1, 2147483647]")
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
local function json(value, depth)
  depth = depth or 0
  assert(depth <= 8, "JSON nesting exceeds monitor limit")
  local kind = type(value)
  if kind == "nil" then return "null" end
  if kind == "boolean" then return value and "true" or "false" end
  if kind == "number" then return tostring(value) end
  if kind == "string" then return json_string(value) end
  assert(kind == "table", "unsupported JSON value: " .. kind)
  local keys = {}
  for key in pairs(value) do
    assert(type(key) == "string", "monitor JSON keys must be strings")
    keys[#keys + 1] = key
  end
  table.sort(keys)
  local pieces = {}
  for index, key in ipairs(keys) do
    pieces[index] = json_string(key) .. ":" .. json(value[key], depth + 1)
  end
  return "{" .. table.concat(pieces, ",") .. "}"
end

local prior_state = rawget(_G, "flycast_cdda_evidence_monitor_state")
if prior_state and prior_state.shutdown then prior_state.shutdown("reload", false) end
local callbacks = rawget(_G, "flycast_callbacks") or {}
local prior_callbacks = {}
if prior_state and prior_state.prior_callbacks then
  prior_callbacks = prior_state.prior_callbacks
else
  for key, value in pairs(callbacks) do prior_callbacks[key] = value end
end

local state = {
  prior_callbacks = prior_callbacks, output = nil, tokens = {}, stats = {},
  closed = true, stop_requested = false, hit = nil,
  accepted = nil, applied = nil, sector = nil, frames = 0,
}
rawset(_G, "flycast_cdda_evidence_monitor_state", state)

local function write(record)
  assert(state.output and state.output:write(json(record), "\n"))
  assert(state.output:flush())
end
local function clear_chain()
  state.accepted = nil
  state.applied = nil
  state.sector = nil
end
local function is_play(event)
  if event.path == 1 then return event.command == 0x14 or event.command == 0x15 end
  if event.path ~= 2 or event.command ~= 0x20
      or type(event.parameters) ~= "table" then return false end
  local parameter_type = math.floor((event.parameters[1] or 0) / 256) % 8
  return parameter_type == 1 or parameter_type == 2
end

local function observe(event)
  if event.event == "cdda-reset" then
    clear_chain()
    write({ record_type = "progress", stage = "reset", tick = event.tick_decimal })
    return
  end
  if event.event == "cdda-control-accepted" then
    if not is_play(event) then return end
    state.accepted = {
      generation = event.control_generation, path = event.path, command = event.command,
      request_id = event.request_id, tick = event.tick_decimal,
    }
    state.applied = nil
    state.sector = nil
    write({ record_type = "progress", stage = "play-accepted",
      path = event.path, command = event.command, control_generation = event.control_generation,
      request_id = event.request_id, tick = event.tick_decimal })
    return
  end
  if event.event == "cdda-control-applied" then
    if not is_play(event) or not event.applied_successfully then return end
    local accepted = state.accepted
    local joined = accepted ~= nil
      and accepted.generation == event.control_generation
      and accepted.path == event.path
      and accepted.command == event.command
      and accepted.request_id == event.request_id
    if joined then
      state.applied = {
        generation = event.control_generation, path = event.path, command = event.command,
        request_id = event.request_id,
      }
    end
    state.sector = nil
    write({ record_type = "progress", stage = "play-applied",
      joined_to_acceptance = joined, path = event.path, command = event.command,
      control_generation = event.control_generation,
      request_id = event.request_id, tick = event.tick_decimal })
    return
  end
  if event.event == "cdda-sector" then
    if not event.read_successful then return end
    local applied = state.applied
    local joined = applied ~= nil
      and applied.generation == event.control_generation
    if joined then
      state.sector = {
        control_generation = event.control_generation,
        aica_generation = event.aica_generation, fad = event.fad,
      }
    end
    write({ record_type = "progress", stage = "sector-read",
      joined_to_play = joined, control_generation = event.control_generation,
      aica_generation = event.aica_generation, fad = event.fad,
      tick = event.tick_decimal })
    return
  end
  if event.event == "aica-sample" then
    local sector = state.sector
    local joined = sector ~= nil
      and sector.aica_generation == event.cdda_generation
    if joined and not state.hit then
      state.hit = {
        command = state.applied.command,
        control_generation = sector.control_generation,
        aica_generation = sector.aica_generation, fad = sector.fad,
        sample_ordinal = event.sample_ordinal,
        cdda_frame_index = event.cdda_frame_index,
        contribution_left = event.cdda_contribution_left,
        contribution_right = event.cdda_contribution_right,
        tick = event.tick_decimal,
      }
      write({ record_type = "evidence-hit", stage = "nonzero-cdda-contribution",
        command = state.hit.command,
        control_generation = state.hit.control_generation,
        aica_generation = state.hit.aica_generation, fad = state.hit.fad,
        sample_ordinal = state.hit.sample_ordinal,
        cdda_frame_index = state.hit.cdda_frame_index,
        contribution_left = state.hit.contribution_left,
        contribution_right = state.hit.contribution_right,
        tick = state.hit.tick })
      state.stop_requested = true
    end
  end
end

local function subscribe(filter)
  local token = flycast.research.subscribe(filter, observe)
  state.tokens[#state.tokens + 1] = token
  state.stats[token] = { delivered = 0, dropped = 0, callback_errors = 0, queued = 0 }
end
local function refresh_stats()
  for _, token in ipairs(state.tokens) do
    local value = flycast.research.subscription_stats(token)
    if value then state.stats[token] = value end
  end
end
local function aggregate(field)
  local total = 0
  for _, value in pairs(state.stats) do total = total + value[field] end
  return total
end

local function open_session()
  state.output = assert(io.open(output_path, "wb"))
  state.closed = false
  state.stop_requested = false
  state.frames = 0
  state.hit = nil
  state.tokens = {}
  state.stats = {}
  clear_chain()
  local base = { queue_capacity = queue_capacity }
  subscribe({ event = "cdda-control-accepted", queue_capacity = base.queue_capacity })
  subscribe({ event = "cdda-control-applied", successful = true,
    queue_capacity = base.queue_capacity })
  subscribe({ event = "cdda-sector", successful = true,
    queue_capacity = base.queue_capacity })
  subscribe({ event = "aica-sample", nonzero_cdda_contribution = true,
    queue_capacity = base.queue_capacity })
  subscribe({ event = "cdda-reset", queue_capacity = base.queue_capacity })
  write({ record_type = "session-start", schema = "flycast-cdda-evidence-monitor-v1",
    discovery = true, authoritative_evidence = false,
    target = "accepted-play-applied-sector-nonzero-contribution" })
end

function state.shutdown(reason, controlled)
  if state.closed then return end
  refresh_stats()
  local dropped = aggregate("dropped")
  local callback_errors = aggregate("callback_errors")
  local queued = aggregate("queued")
  write({ record_type = "summary", schema = "flycast-cdda-evidence-monitor-v1",
    discovery = true, authoritative_evidence = false, reason = reason,
    hit = state.hit ~= nil,
    complete = controlled and dropped == 0
      and callback_errors == 0 and queued == 0,
    delivered = aggregate("delivered"), dropped = dropped,
    callback_errors = callback_errors, queued = queued })
  for _, token in ipairs(state.tokens) do flycast.research.unsubscribe(token) end
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
  write({ record_type = "lifecycle", event = "start",
    media = flycast.state.media or "", game_id = flycast.state.gameId or "" })
  call_prior("start")
end
callbacks.loadState = function()
  clear_chain()
  write({ record_type = "lifecycle", event = "load-state" })
  call_prior("loadState")
end
callbacks.overlay = function()
  refresh_stats()
  state.frames = state.frames + 1
  call_prior("overlay")
  local reason
  if state.stop_requested then reason = "evidence-hit"
  elseif auto_exit_frames and state.frames >= auto_exit_frames then
    reason = "probe-timeout"
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

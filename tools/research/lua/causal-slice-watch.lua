-- Game-agnostic, discovery-only SH-4 causal-slice watcher.
--
-- This is a targeted second-pass tool. A producer PC interval is required so
-- native filters can retain the exact instruction begin/access/end sequence
-- without queueing every guest instruction.
--
-- Entry script: settings, subscriptions, and lifecycle. Event matching lives
-- in causal-slice-model.lua and JSONL shaping in causal-slice-output.lua,
-- both loaded from this file's own directory.

local supplied = rawget(_G, "flycast_causal_slice_watch") or {}

local watcher_source = assert(debug.getinfo(1, "S").source)
local watcher_path = watcher_source:match("^@(.+)$")
assert(watcher_path, "causal-slice-watch.lua must be loaded from a file")
local watcher_directory = watcher_path:match("^(.*[\\/])") or ""
local output = assert(dofile(watcher_directory .. "causal-slice-output.lua"))
local model = assert(dofile(watcher_directory .. "causal-slice-model.lua"))

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

local output_path = assert(setting("output", "FLYCAST_CAUSAL_SLICE_OUTPUT", nil),
  "flycast_causal_slice_watch.output or FLYCAST_CAUSAL_SLICE_OUTPUT is required")
local config = {}
config.start_address = integer_setting("start_address",
  "FLYCAST_CAUSAL_SLICE_START_ADDRESS", 0, 0xffffffff)
config.length = integer_setting("length", "FLYCAST_CAUSAL_SLICE_LENGTH",
  1, 1024 * 1024)
config.end_address = config.start_address + config.length - 1
assert(config.end_address <= 0xffffffff,
  "memory range exceeds the 32-bit guest address space")
config.producer_start_pc = integer_setting("producer_start_pc",
  "FLYCAST_CAUSAL_SLICE_PRODUCER_START_PC", 0, 0xffffffff)
config.producer_end_pc = integer_setting("producer_end_pc",
  "FLYCAST_CAUSAL_SLICE_PRODUCER_END_PC", 0, 0xffffffff)
assert(config.producer_start_pc <= config.producer_end_pc,
  "producer_start_pc must not exceed producer_end_pc")
config.call_site_start_pc = integer_setting("call_site_start_pc",
  "FLYCAST_CAUSAL_SLICE_CALL_SITE_START_PC", 0, 0xffffffff)
config.call_site_end_pc = integer_setting("call_site_end_pc",
  "FLYCAST_CAUSAL_SLICE_CALL_SITE_END_PC", 0, 0xffffffff)
assert(config.call_site_start_pc <= config.call_site_end_pc,
  "call_site_start_pc must not exceed call_site_end_pc")
config.return_start_pc = integer_setting("return_start_pc",
  "FLYCAST_CAUSAL_SLICE_RETURN_START_PC", 0, 0xffffffff)
config.return_end_pc = integer_setting("return_end_pc",
  "FLYCAST_CAUSAL_SLICE_RETURN_END_PC", 0, 0xffffffff)
assert(config.return_start_pc <= config.return_end_pc,
  "return_start_pc must not exceed return_end_pc")
config.access = choice_setting("access", "FLYCAST_CAUSAL_SLICE_ACCESS",
  { read = true, write = true, both = true }, "write")
config.backend = choice_setting("backend", "FLYCAST_CAUSAL_SLICE_BACKEND",
  { interpreter = true, dynarec = true }, "interpreter")
config.queue_capacity = integer_setting("queue_capacity",
  "FLYCAST_CAUSAL_SLICE_QUEUE_CAPACITY", 1, 65536, 8192)
config.max_slices = integer_setting("max_slices", "FLYCAST_CAUSAL_SLICE_MAX_SLICES",
  1, 1000000, 100)
config.max_call_depth = integer_setting("max_call_depth",
  "FLYCAST_CAUSAL_SLICE_MAX_CALL_DEPTH", 1, 1024, 64)
config.wait_for_load_state = boolean_setting("wait_for_load_state",
  "FLYCAST_CAUSAL_SLICE_WAIT_FOR_LOAD_STATE", false)

local prior_state = rawget(_G, "flycast_causal_slice_watch_state")
if prior_state and prior_state.shutdown then prior_state.shutdown("reload", false) end

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
  output = nil,
  closed = true,
  armed = false,
  tokens = {},
  stats = {},
  retired = { delivered = 0, dropped = 0, callback_errors = 0 },
  intentionally_discarded = 0,
  active_frames = {},
  call_stack = {},
  pending_call = nil,
  context_epoch = 1,
  context_origin = config.wait_for_load_state and "awaiting-load-state" or "attachment",
  slices = 0,
  accesses = 0,
  completed = 0,
  aborted = 0,
  calls = 0,
  returns = 0,
  unmatched_returns = 0,
  context_resets = 0,
  anomalies = 0,
  stop_requested = false,
}
rawset(_G, "flycast_causal_slice_watch_state", state)

local session_sequence = (rawget(_G, "flycast_causal_slice_watch_session_sequence") or 0) + 1
rawset(_G, "flycast_causal_slice_watch_session_sequence", session_sequence)

local function write_line(text)
  assert(state.output, "causal-slice watcher output is closed")
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

local function aggregate(field)
  local total = state.retired[field] or 0
  for _, value in pairs(state.stats) do total = total + (value[field] or 0) end
  return total
end

local function anomaly(kind, event, detail)
  state.anomalies = state.anomalies + 1
  write_line(output.anomaly_json(kind, event, detail))
end

local function retire_subscriptions(current_event)
  refresh_stats()
  if current_event and state.stats[current_event] then
    -- Native delivery is counted after this callback returns.
    state.stats[current_event].delivered = state.stats[current_event].delivered + 1
  end
  local queued = 0
  for _, value in pairs(state.stats) do
    queued = queued + value.queued
    state.retired.delivered = state.retired.delivered + value.delivered
    state.retired.dropped = state.retired.dropped + value.dropped
    state.retired.callback_errors = state.retired.callback_errors + value.callback_errors
  end
  state.intentionally_discarded = state.intentionally_discarded + queued
  for _, token in pairs(state.tokens) do flycast.research.unsubscribe(token) end
  state.tokens = {}
  state.stats = {}
  state.armed = false
end

local function finish(reason, current_event, forced_incomplete)
  if state.closed then return end
  retire_subscriptions(current_event)
  local totals = {
    delivered = aggregate("delivered"),
    dropped = aggregate("dropped"),
    callback_errors = aggregate("callback_errors"),
  }
  write_line(output.summary_json(reason, state, totals, forced_incomplete))
  state.output:close()
  state.output = nil
  state.closed = true
  state.stop_requested = reason == "max-slices"
end

local function emit_slice(frame, outcome, event)
  write_line(output.slice_json(frame, outcome, event, state.slices))
  if state.slices >= config.max_slices then
    local ok, message = pcall(finish, "max-slices", event.event, false)
    if not ok and state.output then
      write_line(output.fatal_error_json(message))
      state.output:close()
      state.output = nil
      state.closed = true
      state.stop_requested = true
    end
  end
end

local handlers = model.new({
  state = state,
  max_call_depth = config.max_call_depth,
  anomaly = anomaly,
  emit_slice = emit_slice,
})

local function filter(event, start_pc, end_pc, memory_filter)
  local value = {
    event = event, backend = config.backend, queue_capacity = config.queue_capacity,
  }
  if start_pc then
    value.start_pc = start_pc
    value.end_pc = end_pc
  end
  if memory_filter then
    value.start_address = config.start_address
    value.end_address = config.end_address
  end
  return value
end

local function subscribe_one(event, callback, start_pc, end_pc, memory_filter)
  state.stats[event] = new_stats()
  state.tokens[event] = flycast.research.subscribe(
    filter(event, start_pc, end_pc, memory_filter), function(observation)
      local ok, message = pcall(callback, observation)
      if not ok then
        anomaly("watcher-callback-failure", observation,
          event .. " callback failed: " .. tostring(message))
      end
    end)
end

local function subscribe()
  if state.armed then return end
  local ok, message = pcall(function()
    subscribe_one("instruction-begin", handlers.begin,
      config.producer_start_pc, config.producer_end_pc, false)
    subscribe_one("instruction", handlers.finish,
      config.producer_start_pc, config.producer_end_pc, false)
    subscribe_one("instruction-abort", handlers.abort, nil, nil, false)
    subscribe_one("call", handlers.call,
      config.call_site_start_pc, config.call_site_end_pc, false)
    subscribe_one("return", handlers.ret,
      config.return_start_pc, config.return_end_pc, false)
    subscribe_one("exception", handlers.exception, nil, nil, false)
    if config.access == "read" or config.access == "both" then
      subscribe_one("memory-read", handlers.access,
        config.producer_start_pc, config.producer_end_pc, true)
    end
    if config.access == "write" or config.access == "both" then
      subscribe_one("memory-write", handlers.access,
        config.producer_start_pc, config.producer_end_pc, true)
    end
  end)
  if not ok then
    for _, token in pairs(state.tokens) do flycast.research.unsubscribe(token) end
    state.tokens = {}
    state.stats = {}
    error(message)
  end
  state.armed = true
end

local function reset_for_load_state()
  if state.armed then retire_subscriptions(nil) end
  state.active_frames = {}
  handlers.reset_context("load-state")
  subscribe()
end

function state.shutdown(reason, complete_boundary)
  finish(reason, nil, not complete_boundary)
end

local function call_prior(name)
  local callback = state.prior_callbacks[name]
  if type(callback) == "function" then callback() end
end

callbacks.start = function()
  if not state.closed then
    write_line(output.lifecycle_start_json(flycast.state.media, flycast.state.gameId))
  end
  call_prior("start")
end

callbacks.loadState = function()
  if not state.closed then
    reset_for_load_state()
    write_line(output.lifecycle_load_state_json(state.context_epoch))
  end
  call_prior("loadState")
end

callbacks.overlay = function()
  if not state.closed then refresh_stats() end
  call_prior("overlay")
  if state.stop_requested then
    state.stop_requested = false
    flycast.emulator.requestExit()
  end
end

callbacks.terminate = function()
  -- Native subscriptions and queued deliveries are cleared before this Lua
  -- callback. A manual close therefore cannot prove that its delivered prefix
  -- reached a clean queue boundary.
  if not state.closed then finish("terminate", nil, true) end
  call_prior("terminate")
end

flycast_callbacks = callbacks
state.output = assert(io.open(output_path, "ab"))
state.closed = false
write_line(output.session_start_json(config, session_sequence))
if not config.wait_for_load_state then subscribe() end

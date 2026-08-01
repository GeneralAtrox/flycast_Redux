-- Game-agnostic, discovery-only Maple request/response watcher.
--
-- Configure with a global `flycast_maple_watch` table before loading this
-- script, or with the equivalent FLYCAST_MAPLE_WATCH_* environment variables.
-- The only required setting is `output`.

local supplied = rawget(_G, "flycast_maple_watch") or {}

local watcher_source = assert(debug.getinfo(1, "S").source)
local watcher_path = watcher_source:match("^@(.+)$")
assert(watcher_path, "maple-watch.lua must be loaded from a file")
local watcher_directory = watcher_path:match("^(.*[\\/])") or ""
local maple_decoder = assert(dofile(watcher_directory .. "maple-decode.lua"))

local function setting(name, environment, default)
  local value = supplied[name]
  if value == nil then
    value = os.getenv(environment)
  end
  if value == nil or value == "" then
    return default
  end
  return value
end

local function optional_integer(name, environment, minimum, maximum)
  local value = setting(name, environment, nil)
  if value == nil then
    return nil
  end
  value = tonumber(value)
  assert(value and value == math.floor(value) and value >= minimum and value <= maximum,
    name .. " must be an integer in [" .. minimum .. ", " .. maximum .. "]")
  return value
end

local output_path = assert(setting("output", "FLYCAST_MAPLE_WATCH_OUTPUT", nil),
  "flycast_maple_watch.output or FLYCAST_MAPLE_WATCH_OUTPUT is required")
local queue_capacity = optional_integer("queue_capacity",
  "FLYCAST_MAPLE_WATCH_QUEUE_CAPACITY", 1, 65536) or 4096
local bus = optional_integer("bus", "FLYCAST_MAPLE_WATCH_BUS", 0, 3)
local port = optional_integer("port", "FLYCAST_MAPLE_WATCH_PORT", 0, 5)
local command = optional_integer("command", "FLYCAST_MAPLE_WATCH_COMMAND", 0, 255)
local auto_exit_frames = optional_integer("auto_exit_frames",
  "FLYCAST_MAPLE_WATCH_AUTO_EXIT_FRAMES", 1, 2147483647)

local escapes = {
  ['"'] = '\\"',
  ['\\'] = '\\\\',
  ['\b'] = '\\b',
  ['\f'] = '\\f',
  ['\n'] = '\\n',
  ['\r'] = '\\r',
  ['\t'] = '\\t',
}

local function json_string(value)
  return '"' .. tostring(value):gsub('[%z\1-\31\\"]', function(character)
    return escapes[character] or string.format("\\u%04x", string.byte(character))
  end) .. '"'
end

local function json_boolean(value)
  return value and "true" or "false"
end

local function json_nullable_number(value)
  return value == nil and "null" or tostring(value)
end

local function event_json(event)
  return table.concat({
    '{"event":', json_string(event.event),
    ',"ordinal":', json_string(event.ordinal_decimal),
    ',"tick":', json_string(event.tick_decimal),
    ',"dma_ordinal":', json_string(event.dma_ordinal_decimal),
    ',"transaction_ordinal":', json_string(event.transaction_ordinal_decimal),
    ',"descriptor_address":', tostring(event.descriptor_address),
    ',"destination_address":', tostring(event.destination_address),
    ',"descriptor_header_1":', tostring(event.descriptor_header_1),
    ',"descriptor_header_2":', tostring(event.descriptor_header_2),
    ',"bus":', tostring(event.bus),
    ',"port":', tostring(event.port),
    ',"command":', tostring(event.command),
    ',"device_present":', json_boolean(event.device_present),
    ',"device_type":', json_nullable_number(event.device_type),
    ',"frame_code":', json_nullable_number(event.frame_code),
    ',"response_code":', json_nullable_number(event.response_code),
    ',"byte_count":', tostring(event.byte_count),
    ',"payload_hex":', json_string(event.payload_hex),
    '}'
  })
end

local prior_state = rawget(_G, "flycast_maple_watch_state")
if prior_state and prior_state.shutdown then
  prior_state.shutdown("reload")
end

local callbacks = rawget(_G, "flycast_callbacks") or {}
local prior_callbacks
if prior_state and prior_state.prior_callbacks then
  prior_callbacks = prior_state.prior_callbacks
else
  prior_callbacks = {}
  for key, value in pairs(callbacks) do
    prior_callbacks[key] = value
  end
end

local state = {
  prior_callbacks = prior_callbacks,
  pending = {},
  request_token = nil,
  response_token = nil,
  output = nil,
  closed = true,
  frames = 0,
  requests = 0,
  responses = 0,
  paired = 0,
  unmatched_responses = 0,
  duplicate_requests = 0,
  pair_mismatches = 0,
  request_stats = { delivered = 0, dropped = 0, callback_errors = 0, queued = 0 },
  response_stats = { delivered = 0, dropped = 0, callback_errors = 0, queued = 0 },
}
rawset(_G, "flycast_maple_watch_state", state)

local session_sequence = (rawget(_G, "flycast_maple_watch_session_sequence") or 0) + 1
rawset(_G, "flycast_maple_watch_session_sequence", session_sequence)

local function write_line(text)
  assert(state.output, "Maple watcher output is closed")
  assert(state.output:write(text, "\n"))
  assert(state.output:flush())
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
  if state.request_token then
    copy_stats(state.request_stats,
      flycast.research.subscription_stats(state.request_token))
  end
  if state.response_token then
    copy_stats(state.response_stats,
      flycast.research.subscription_stats(state.response_token))
  end
end

local function pending_count()
  local count = 0
  for _ in pairs(state.pending) do
    count = count + 1
  end
  return count
end

local function same_transaction(request, response)
  return request.dma_ordinal_decimal == response.dma_ordinal_decimal
    and request.transaction_ordinal_decimal == response.transaction_ordinal_decimal
    and request.bus == response.bus
    and request.port == response.port
    and request.command == response.command
    and request.descriptor_address == response.descriptor_address
    and request.destination_address == response.destination_address
end

local function request_callback(event)
  local key = event.transaction_ordinal_decimal
  state.requests = state.requests + 1
  if state.pending[key] ~= nil then
    state.duplicate_requests = state.duplicate_requests + 1
    write_line('{"record_type":"duplicate-request","discovery":true,'
      .. '"authoritative_evidence":false,"request":' .. event_json(event) .. '}')
  end
  state.pending[key] = event
end

local function response_callback(event)
  local key = event.transaction_ordinal_decimal
  local request = state.pending[key]
  state.responses = state.responses + 1
  if request == nil then
    state.unmatched_responses = state.unmatched_responses + 1
    write_line('{"record_type":"unmatched-response","discovery":true,'
      .. '"authoritative_evidence":false,"response":' .. event_json(event) .. '}')
    return
  end
  state.pending[key] = nil
  local consistent = same_transaction(request, event)
  if not consistent then
    state.pair_mismatches = state.pair_mismatches + 1
  end
  state.paired = state.paired + 1
  local decoded = maple_decoder.decode(request, event)
  write_line('{"record_type":"transaction","discovery":true,'
    .. '"authoritative_evidence":false,"consistent":' .. json_boolean(consistent)
    .. ',"transaction_ordinal":' .. json_string(key)
    .. ',"request":' .. event_json(request)
    .. ',"response":' .. event_json(event)
    .. ',"decoded":' .. maple_decoder.encode_json(decoded) .. '}')
end

local function filter(event)
  local value = { event = event, queue_capacity = queue_capacity }
  if bus ~= nil then value.bus = bus end
  if port ~= nil then value.port = port end
  if command ~= nil then value.command = command end
  return value
end

local function subscribe()
  state.request_token = flycast.research.subscribe(filter("maple-request"),
    request_callback)
  local ok, token = pcall(function()
    return flycast.research.subscribe(filter("maple-response"), response_callback)
  end)
  if not ok then
    flycast.research.unsubscribe(state.request_token)
    state.request_token = nil
    error(token)
  end
  state.response_token = token
end

local function open_session()
  state.output = assert(io.open(output_path, "ab"))
  state.closed = false
  state.frames = 0
  state.pending = {}
  state.requests = 0
  state.responses = 0
  state.paired = 0
  state.unmatched_responses = 0
  state.duplicate_requests = 0
  state.pair_mismatches = 0
  state.request_stats = { delivered = 0, dropped = 0, callback_errors = 0, queued = 0 }
  state.response_stats = { delivered = 0, dropped = 0, callback_errors = 0, queued = 0 }
  subscribe()
  write_line('{"record_type":"session-start","schema":"flycast-maple-watch-jsonl",'
    .. '"schema_version":2,"discovery":true,"authoritative_evidence":false,'
    .. '"session":' .. tostring(session_sequence)
    .. ',"queue_capacity":' .. tostring(queue_capacity)
    .. ',"bus":' .. json_nullable_number(bus)
    .. ',"port":' .. json_nullable_number(port)
    .. ',"command":' .. json_nullable_number(command) .. '}')
end

function state.shutdown(reason)
  if state.closed then
    return
  end
  refresh_stats()
  local unmatched_requests = pending_count()
  local incomplete = state.request_stats.dropped > 0
    or state.response_stats.dropped > 0
    or state.request_stats.callback_errors > 0
    or state.response_stats.callback_errors > 0
    or unmatched_requests > 0
    or state.unmatched_responses > 0
    or state.duplicate_requests > 0
    or state.pair_mismatches > 0
  write_line('{"record_type":"summary","schema":"flycast-maple-watch-jsonl",'
    .. '"schema_version":2,"discovery":true,"authoritative_evidence":false,'
    .. '"reason":' .. json_string(reason)
    .. ',"complete":' .. json_boolean(not incomplete)
    .. ',"requests":' .. tostring(state.requests)
    .. ',"responses":' .. tostring(state.responses)
    .. ',"paired":' .. tostring(state.paired)
    .. ',"unmatched_requests":' .. tostring(unmatched_requests)
    .. ',"unmatched_responses":' .. tostring(state.unmatched_responses)
    .. ',"duplicate_requests":' .. tostring(state.duplicate_requests)
    .. ',"pair_mismatches":' .. tostring(state.pair_mismatches)
    .. ',"request_delivered":' .. tostring(state.request_stats.delivered)
    .. ',"response_delivered":' .. tostring(state.response_stats.delivered)
    .. ',"request_dropped":' .. tostring(state.request_stats.dropped)
    .. ',"response_dropped":' .. tostring(state.response_stats.dropped)
    .. ',"request_callback_errors":' .. tostring(state.request_stats.callback_errors)
    .. ',"response_callback_errors":' .. tostring(state.response_stats.callback_errors)
    .. ',"last_observed_request_queue":' .. tostring(state.request_stats.queued)
    .. ',"last_observed_response_queue":' .. tostring(state.response_stats.queued)
    .. '}')
  if state.request_token then
    flycast.research.unsubscribe(state.request_token)
  end
  if state.response_token then
    flycast.research.unsubscribe(state.response_token)
  end
  state.request_token = nil
  state.response_token = nil
  state.output:close()
  state.output = nil
  state.closed = true
end

local function call_prior(name)
  local callback = state.prior_callbacks[name]
  if type(callback) == "function" then
    callback()
  end
end

callbacks.start = function()
  if state.closed then
    session_sequence = session_sequence + 1
    rawset(_G, "flycast_maple_watch_session_sequence", session_sequence)
    open_session()
  end
  write_line('{"record_type":"lifecycle","event":"start","media":'
    .. json_string(flycast.state.media or "") .. ',"game_id":'
    .. json_string(flycast.state.gameId or "") .. '}')
  call_prior("start")
end

callbacks.loadState = function()
  write_line('{"record_type":"lifecycle","event":"load-state"}')
  call_prior("loadState")
end

callbacks.overlay = function()
  refresh_stats()
  state.frames = state.frames + 1
  call_prior("overlay")
  if auto_exit_frames and state.frames >= auto_exit_frames then
    auto_exit_frames = nil
    state.shutdown("auto-exit")
    flycast.emulator.requestExit()
  end
end

callbacks.terminate = function()
  state.shutdown("terminate")
  call_prior("terminate")
end

flycast_callbacks = callbacks
open_session()

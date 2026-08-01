-- Game-agnostic, discovery-only SH-4 causal-slice watcher.
--
-- This is a targeted second-pass tool. A producer PC interval is required so
-- native filters can retain the exact instruction begin/access/end sequence
-- without queueing every guest instruction.

local supplied = rawget(_G, "flycast_causal_slice_watch") or {}

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
local start_address = integer_setting("start_address",
  "FLYCAST_CAUSAL_SLICE_START_ADDRESS", 0, 0xffffffff)
local length = integer_setting("length", "FLYCAST_CAUSAL_SLICE_LENGTH",
  1, 1024 * 1024)
local end_address = start_address + length - 1
assert(end_address <= 0xffffffff, "memory range exceeds the 32-bit guest address space")
local producer_start_pc = integer_setting("producer_start_pc",
  "FLYCAST_CAUSAL_SLICE_PRODUCER_START_PC", 0, 0xffffffff)
local producer_end_pc = integer_setting("producer_end_pc",
  "FLYCAST_CAUSAL_SLICE_PRODUCER_END_PC", 0, 0xffffffff)
assert(producer_start_pc <= producer_end_pc,
  "producer_start_pc must not exceed producer_end_pc")
local call_site_start_pc = integer_setting("call_site_start_pc",
  "FLYCAST_CAUSAL_SLICE_CALL_SITE_START_PC", 0, 0xffffffff)
local call_site_end_pc = integer_setting("call_site_end_pc",
  "FLYCAST_CAUSAL_SLICE_CALL_SITE_END_PC", 0, 0xffffffff)
assert(call_site_start_pc <= call_site_end_pc,
  "call_site_start_pc must not exceed call_site_end_pc")
local return_start_pc = integer_setting("return_start_pc",
  "FLYCAST_CAUSAL_SLICE_RETURN_START_PC", 0, 0xffffffff)
local return_end_pc = integer_setting("return_end_pc",
  "FLYCAST_CAUSAL_SLICE_RETURN_END_PC", 0, 0xffffffff)
assert(return_start_pc <= return_end_pc,
  "return_start_pc must not exceed return_end_pc")
local access = choice_setting("access", "FLYCAST_CAUSAL_SLICE_ACCESS",
  { read = true, write = true, both = true }, "write")
local backend = choice_setting("backend", "FLYCAST_CAUSAL_SLICE_BACKEND",
  { interpreter = true, dynarec = true }, "interpreter")
local queue_capacity = integer_setting("queue_capacity",
  "FLYCAST_CAUSAL_SLICE_QUEUE_CAPACITY", 1, 65536, 8192)
local max_slices = integer_setting("max_slices", "FLYCAST_CAUSAL_SLICE_MAX_SLICES",
  1, 1000000, 100)
local max_call_depth = integer_setting("max_call_depth",
  "FLYCAST_CAUSAL_SLICE_MAX_CALL_DEPTH", 1, 1024, 64)
local wait_for_load_state = boolean_setting("wait_for_load_state",
  "FLYCAST_CAUSAL_SLICE_WAIT_FOR_LOAD_STATE", false)

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

local function registers_json(registers)
  if registers == nil then return "null" end
  local numeric = {}
  local hexadecimal = {}
  for index = 1, 16 do
    numeric[index] = tostring(registers.r[index])
    hexadecimal[index] = json_string(hex32(registers.r[index]))
  end
  return table.concat({
    '{"r":[', table.concat(numeric, ","), ']'
      .. ',"r_hex":[', table.concat(hexadecimal, ","), ']'
      .. ',"pr":', tostring(registers.pr), ',"pr_hex":', json_string(hex32(registers.pr))
      .. ',"gbr":', tostring(registers.gbr), ',"gbr_hex":', json_string(hex32(registers.gbr))
      .. ',"vbr":', tostring(registers.vbr), ',"vbr_hex":', json_string(hex32(registers.vbr))
      .. ',"mach":', tostring(registers.mach), ',"mach_hex":', json_string(hex32(registers.mach))
      .. ',"macl":', tostring(registers.macl), ',"macl_hex":', json_string(hex32(registers.macl))
      .. ',"sr":', tostring(registers.sr), ',"sr_hex":', json_string(hex32(registers.sr))
      .. ',"fpul":', tostring(registers.fpul), ',"fpul_hex":', json_string(hex32(registers.fpul))
      .. ',"fpscr":', tostring(registers.fpscr), ',"fpscr_hex":', json_string(hex32(registers.fpscr))
      .. '}'
  })
end

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
  context_origin = wait_for_load_state and "awaiting-load-state" or "attachment",
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
  write_line('{"record_type":"anomaly","discovery":true'
    .. ',"authoritative_evidence":false,"kind":' .. json_string(kind)
    .. ',"detail":' .. json_string(detail or "")
    .. ',"ordinal":' .. (event and json_string(event.ordinal_decimal) or "null")
    .. ',"pc_hex":' .. (event and json_string(hex32(event.pc)) or "null") .. '}')
end

local function reset_context(origin)
  state.call_stack = {}
  state.pending_call = nil
  state.context_epoch = state.context_epoch + 1
  state.context_origin = origin
  state.context_resets = state.context_resets + 1
end

local function same_owner(event, pending)
  return event.backend == pending.backend
    and event.pc == pending.pc
    and event.opcode == pending.opcode
    and event.delay_slot_depth == pending.delay_slot_depth
    and event.tick_decimal == pending.tick_decimal
end

local function commit_pending(event, reason)
  local pending = state.pending_call
  if not pending then return end
  if #state.call_stack >= max_call_depth then
    anomaly("call-depth-limit", event, "observed call depth exceeds configured maximum")
    reset_context("after-call-depth-limit")
    return
  end
  state.calls = state.calls + 1
  pending.invocation_id = state.calls
  pending.completion_reason = reason
  pending.completion_ordinal = event and event.ordinal_decimal or nil
  table.insert(state.call_stack, pending)
  state.pending_call = nil
end

local function advance_pending(event)
  local pending = state.pending_call
  if not pending then return end
  if same_owner(event, pending) then
    if event.event == "instruction" then
      commit_pending(event, "owner-instruction-completed")
    end
    return
  end
  if event.delay_slot_depth > pending.delay_slot_depth then return end
  commit_pending(event, "later-instruction-boundary-observed")
end

local function copy_call_path()
  local copy = {}
  for index, frame in ipairs(state.call_stack) do copy[index] = frame end
  return copy
end

local function call_frame_json(frame)
  local completion_ordinal = frame.completion_ordinal
    and json_string(frame.completion_ordinal) or "null"
  return table.concat({
    '{"invocation_id":', tostring(frame.invocation_id)
      .. ',"ordinal":', json_string(frame.ordinal_decimal)
      .. ',"tick":', json_string(frame.tick_decimal)
      .. ',"call_kind":', json_string(frame.call_kind)
      .. ',"call_pc":', tostring(frame.pc)
      .. ',"call_pc_hex":', json_string(hex32(frame.pc))
      .. ',"target_pc":', tostring(frame.target_pc)
      .. ',"target_pc_hex":', json_string(hex32(frame.target_pc))
      .. ',"return_pc":', tostring(frame.return_pc)
      .. ',"return_pc_hex":', json_string(hex32(frame.return_pc))
      .. ',"delay_slot_pc":', tostring(frame.delay_slot_pc)
      .. ',"delay_slot_pc_hex":', json_string(hex32(frame.delay_slot_pc))
      .. ',"completion_reason":', json_string(frame.completion_reason)
      .. ',"completion_ordinal":', completion_ordinal
      .. ',"registers":', registers_json(frame.registers)
      .. '}'
  })
end

local function call_path_json(frame)
  local entries = {}
  for index, call in ipairs(frame.call_path) do entries[index] = call_frame_json(call) end
  return table.concat({
    '{"root_complete":false,"scope":"selected-call-sites"'
      .. ',"epoch":', tostring(frame.context_epoch)
      .. ',"origin":', json_string(frame.context_origin)
      .. ',"frames":[', table.concat(entries, ","), ']}'
  })
end

local function access_json(event)
  return table.concat({
    '{"event":', json_string(event.event)
      .. ',"ordinal":', json_string(event.ordinal_decimal)
      .. ',"tick":', json_string(event.tick_decimal)
      .. ',"address":', tostring(event.address)
      .. ',"address_hex":', json_string(hex32(event.address))
      .. ',"width_bytes":', tostring(event.width)
      .. ',"value_hex":', json_string(event.value_hex)
      .. '}'
  })
end

local function exception_json(event)
  if not event then return "null" end
  return table.concat({
    '{"ordinal":', json_string(event.ordinal_decimal)
      .. ',"tick":', json_string(event.tick_decimal)
      .. ',"exception_pc":', tostring(event.exception_pc)
      .. ',"exception_pc_hex":', json_string(hex32(event.exception_pc))
      .. ',"vector_pc":', tostring(event.vector_pc)
      .. ',"vector_pc_hex":', json_string(hex32(event.vector_pc))
      .. ',"exception_code":', tostring(event.exception_code)
      .. ',"exception_code_hex":', json_string(hex32(event.exception_code))
      .. ',"registers":', registers_json(event.registers)
      .. '}'
  })
end

local function slice_json(frame, outcome, terminal)
  local accesses = {}
  for index, event in ipairs(frame.accesses) do accesses[index] = access_json(event) end
  local next_pc = terminal and terminal.next_pc or nil
  local end_ordinal = terminal and json_string(terminal.ordinal_decimal) or "null"
  local end_tick = terminal and json_string(terminal.tick_decimal) or "null"
  local next_pc_hex = next_pc and json_string(hex32(next_pc)) or "null"
  local registers_after = outcome == "completed"
    and registers_json(terminal.registers) or "null"
  return table.concat({
    '{"record_type":"causal-slice","schema":"flycast-causal-slice-jsonl"'
      .. ',"schema_version":1,"discovery":true,"authoritative_evidence":false'
      .. ',"slice_index":', tostring(state.slices)
      .. ',"backend":', json_string(frame.backend)
      .. ',"outcome":', json_string(outcome)
      .. ',"begin_ordinal":', json_string(frame.begin_ordinal)
      .. ',"begin_tick":', json_string(frame.begin_tick)
      .. ',"end_ordinal":', end_ordinal
      .. ',"end_tick":', end_tick
      .. ',"pc":', tostring(frame.pc)
      .. ',"pc_hex":', json_string(hex32(frame.pc))
      .. ',"opcode":', tostring(frame.opcode)
      .. ',"opcode_hex":', json_string(hex16(frame.opcode))
      .. ',"delay_slot_depth":', tostring(frame.delay_slot_depth)
      .. ',"next_pc":', json_nullable_number(next_pc)
      .. ',"next_pc_hex":', next_pc_hex
      .. ',"registers_before":', registers_json(frame.registers_before)
      .. ',"registers_after":', registers_after
      .. ',"accesses":[', table.concat(accesses, ","), ']'
      .. ',"exception":', exception_json(frame.exception)
      .. ',"call_path":', call_path_json(frame)
      .. ',"stack_memory_available":false}'
  })
end

local function frame_for(event)
  local frame = state.active_frames[event.delay_slot_depth]
  if frame and frame.pc == event.pc and frame.opcode == event.opcode
      and frame.backend == event.backend then
    return frame
  end
  return nil
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
  local incomplete = forced_incomplete or aggregate("dropped") > 0
    or aggregate("callback_errors") > 0 or state.anomalies > 0
  write_line('{"record_type":"summary","schema":"flycast-causal-slice-jsonl"'
    .. ',"schema_version":1,"discovery":true,"authoritative_evidence":false'
    .. ',"reason":' .. json_string(reason)
    .. ',"complete":' .. json_boolean(not incomplete)
    .. ',"slices":' .. tostring(state.slices)
    .. ',"accesses":' .. tostring(state.accesses)
    .. ',"completed_slices":' .. tostring(state.completed)
    .. ',"aborted_slices":' .. tostring(state.aborted)
    .. ',"committed_calls":' .. tostring(state.calls)
    .. ',"matched_returns":' .. tostring(state.returns)
    .. ',"unmatched_root_returns":' .. tostring(state.unmatched_returns)
    .. ',"context_resets":' .. tostring(state.context_resets)
    .. ',"anomalies":' .. tostring(state.anomalies)
    .. ',"delivered":' .. tostring(aggregate("delivered"))
    .. ',"dropped":' .. tostring(aggregate("dropped"))
    .. ',"callback_errors":' .. tostring(aggregate("callback_errors"))
    .. ',"intentionally_discarded_after_boundary":'
      .. tostring(state.intentionally_discarded)
    .. ',"manual_termination_delivery_unknown":'
      .. json_boolean(forced_incomplete and reason == "terminate") .. '}')
  state.output:close()
  state.output = nil
  state.closed = true
  state.stop_requested = reason == "max-slices"
end

local function close_frame(event, outcome)
  local frame = frame_for(event)
  if not frame then
    anomaly("unmatched-instruction-" .. outcome, event,
      "producer instruction terminal event has no matching begin")
    return
  end
  state.active_frames[event.delay_slot_depth] = nil
  if #frame.accesses == 0 then return end
  state.slices = state.slices + 1
  if outcome == "completed" then state.completed = state.completed + 1
  else state.aborted = state.aborted + 1 end
  write_line(slice_json(frame, outcome, event))
  if state.slices >= max_slices then
    local ok, message = pcall(finish, "max-slices", event.event, false)
    if not ok and state.output then
      write_line('{"record_type":"fatal-error","discovery":true'
        .. ',"authoritative_evidence":false,"message":' .. json_string(message) .. '}')
      state.output:close()
      state.output = nil
      state.closed = true
      state.stop_requested = true
    end
  end
end

local function begin_callback(event)
  advance_pending(event)
  if state.active_frames[event.delay_slot_depth] then
    anomaly("duplicate-instruction-begin", event,
      "producer depth already owns an open instruction")
  end
  state.active_frames[event.delay_slot_depth] = {
    backend = event.backend,
    pc = event.pc,
    opcode = event.opcode,
    delay_slot_depth = event.delay_slot_depth,
    begin_ordinal = event.ordinal_decimal,
    begin_tick = event.tick_decimal,
    registers_before = event.registers,
    accesses = {},
    exception = nil,
    context_epoch = state.context_epoch,
    context_origin = state.context_origin,
    call_path = copy_call_path(),
  }
end

local function end_callback(event)
  advance_pending(event)
  close_frame(event, "completed")
end

local function abort_callback(event)
  if state.pending_call and same_owner(event, state.pending_call) then
    state.pending_call = nil
  else
    advance_pending(event)
  end
  local frame = frame_for(event)
  if frame then close_frame(event, "aborted") end
end

local function access_callback(event)
  advance_pending(event)
  local frame = frame_for(event)
  if not frame then
    anomaly("access-without-producer-begin", event,
      "matching native access was delivered without its producer begin")
    return
  end
  if #frame.accesses >= 64 then
    anomaly("instruction-access-limit", event,
      "one instruction exceeded 64 matching accesses")
    return
  end
  table.insert(frame.accesses, event)
  state.accesses = state.accesses + 1
end

local function call_callback(event)
  advance_pending(event)
  if state.pending_call then
    anomaly("nested-pending-call", event,
      "a second call appeared before the first call resolved")
    reset_context("after-nested-pending-call")
  end
  state.pending_call = {
    backend = event.backend,
    pc = event.pc,
    opcode = event.opcode,
    delay_slot_depth = event.delay_slot_depth,
    ordinal_decimal = event.ordinal_decimal,
    tick_decimal = event.tick_decimal,
    call_kind = event.call_kind,
    target_pc = event.target_pc,
    return_pc = event.return_pc,
    delay_slot_pc = event.delay_slot_pc,
    registers = event.registers,
  }
end

local function return_callback(event)
  advance_pending(event)
  local top = state.call_stack[#state.call_stack]
  if not top then
    state.unmatched_returns = state.unmatched_returns + 1
    state.context_origin = "after-unobserved-root-return"
    return
  end
  if top.return_pc ~= event.target_pc then
    anomaly("return-mismatch", event,
      "RTS target does not close the latest observed call")
    reset_context("after-return-mismatch")
    return
  end
  table.remove(state.call_stack)
  state.returns = state.returns + 1
end

local function exception_callback(event)
  advance_pending(event)
  local frame = frame_for(event)
  if frame then frame.exception = event end
  reset_context("after-exception")
end

local function filter(event, start_pc, end_pc, memory_filter)
  local value = { event = event, backend = backend, queue_capacity = queue_capacity }
  if start_pc then
    value.start_pc = start_pc
    value.end_pc = end_pc
  end
  if memory_filter then
    value.start_address = start_address
    value.end_address = end_address
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
    subscribe_one("instruction-begin", begin_callback,
      producer_start_pc, producer_end_pc, false)
    subscribe_one("instruction", end_callback,
      producer_start_pc, producer_end_pc, false)
    subscribe_one("instruction-abort", abort_callback, nil, nil, false)
    subscribe_one("call", call_callback,
      call_site_start_pc, call_site_end_pc, false)
    subscribe_one("return", return_callback,
      return_start_pc, return_end_pc, false)
    subscribe_one("exception", exception_callback, nil, nil, false)
    if access == "read" or access == "both" then
      subscribe_one("memory-read", access_callback,
        producer_start_pc, producer_end_pc, true)
    end
    if access == "write" or access == "both" then
      subscribe_one("memory-write", access_callback,
        producer_start_pc, producer_end_pc, true)
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
  reset_context("load-state")
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
    write_line('{"record_type":"lifecycle","event":"start","media":'
      .. json_string(flycast.state.media or "") .. ',"game_id":'
      .. json_string(flycast.state.gameId or "") .. '}')
  end
  call_prior("start")
end

callbacks.loadState = function()
  if not state.closed then
    reset_for_load_state()
    write_line('{"record_type":"lifecycle","event":"load-state"'
      .. ',"context_epoch":' .. tostring(state.context_epoch) .. '}')
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
write_line('{"record_type":"session-start","schema":"flycast-causal-slice-jsonl"'
  .. ',"schema_version":1,"discovery":true,"authoritative_evidence":false'
  .. ',"session":' .. tostring(session_sequence)
  .. ',"start_address":' .. tostring(start_address)
  .. ',"start_address_hex":' .. json_string(hex32(start_address))
  .. ',"end_address":' .. tostring(end_address)
  .. ',"end_address_hex":' .. json_string(hex32(end_address))
  .. ',"length":' .. tostring(length)
  .. ',"access":' .. json_string(access)
  .. ',"backend":' .. json_string(backend)
  .. ',"producer_start_pc":' .. tostring(producer_start_pc)
  .. ',"producer_start_pc_hex":' .. json_string(hex32(producer_start_pc))
  .. ',"producer_end_pc":' .. tostring(producer_end_pc)
  .. ',"producer_end_pc_hex":' .. json_string(hex32(producer_end_pc))
  .. ',"call_site_start_pc":' .. tostring(call_site_start_pc)
  .. ',"call_site_start_pc_hex":' .. json_string(hex32(call_site_start_pc))
  .. ',"call_site_end_pc":' .. tostring(call_site_end_pc)
  .. ',"call_site_end_pc_hex":' .. json_string(hex32(call_site_end_pc))
  .. ',"return_start_pc":' .. tostring(return_start_pc)
  .. ',"return_start_pc_hex":' .. json_string(hex32(return_start_pc))
  .. ',"return_end_pc":' .. tostring(return_end_pc)
  .. ',"return_end_pc_hex":' .. json_string(hex32(return_end_pc))
  .. ',"queue_capacity":' .. tostring(queue_capacity)
  .. ',"max_slices":' .. tostring(max_slices)
  .. ',"max_call_depth":' .. tostring(max_call_depth)
  .. ',"wait_for_load_state":' .. json_boolean(wait_for_load_state)
  .. ',"call_path_root_complete":false'
  .. ',"stack_memory_available":false}')
if not wait_for_load_state then subscribe() end

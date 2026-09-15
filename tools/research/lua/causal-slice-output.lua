-- JSONL record shaping for the SH-4 causal-slice watcher.
--
-- Pure string builders: nothing here touches the emulator or the output
-- file. The entry script (causal-slice-watch.lua) writes what these return.

local output = {}

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

output.json_string = json_string
output.json_boolean = json_boolean
output.hex32 = hex32

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

function output.slice_json(frame, outcome, terminal, slice_index)
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
      .. ',"slice_index":', tostring(slice_index)
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

function output.anomaly_json(kind, event, detail)
  return '{"record_type":"anomaly","discovery":true'
    .. ',"authoritative_evidence":false,"kind":' .. json_string(kind)
    .. ',"detail":' .. json_string(detail or "")
    .. ',"ordinal":' .. (event and json_string(event.ordinal_decimal) or "null")
    .. ',"pc_hex":' .. (event and json_string(hex32(event.pc)) or "null") .. '}'
end

function output.fatal_error_json(message)
  return '{"record_type":"fatal-error","discovery":true'
    .. ',"authoritative_evidence":false,"message":' .. json_string(message) .. '}'
end

function output.lifecycle_start_json(media, game_id)
  return '{"record_type":"lifecycle","event":"start","media":'
    .. json_string(media or "") .. ',"game_id":' .. json_string(game_id or "") .. '}'
end

function output.lifecycle_load_state_json(context_epoch)
  return '{"record_type":"lifecycle","event":"load-state"'
    .. ',"context_epoch":' .. tostring(context_epoch) .. '}'
end

-- Completeness: any native drop, callback error, or watcher anomaly, or a
-- boundary the caller could not prove clean, marks the session incomplete.
function output.summary_json(reason, state, totals, forced_incomplete)
  local incomplete = forced_incomplete or totals.dropped > 0
    or totals.callback_errors > 0 or state.anomalies > 0
  return '{"record_type":"summary","schema":"flycast-causal-slice-jsonl"'
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
    .. ',"delivered":' .. tostring(totals.delivered)
    .. ',"dropped":' .. tostring(totals.dropped)
    .. ',"callback_errors":' .. tostring(totals.callback_errors)
    .. ',"intentionally_discarded_after_boundary":'
      .. tostring(state.intentionally_discarded)
    .. ',"manual_termination_delivery_unknown":'
      .. json_boolean(forced_incomplete and reason == "terminate") .. '}'
end

function output.session_start_json(config, session_sequence)
  return '{"record_type":"session-start","schema":"flycast-causal-slice-jsonl"'
    .. ',"schema_version":1,"discovery":true,"authoritative_evidence":false'
    .. ',"session":' .. tostring(session_sequence)
    .. ',"start_address":' .. tostring(config.start_address)
    .. ',"start_address_hex":' .. json_string(hex32(config.start_address))
    .. ',"end_address":' .. tostring(config.end_address)
    .. ',"end_address_hex":' .. json_string(hex32(config.end_address))
    .. ',"length":' .. tostring(config.length)
    .. ',"access":' .. json_string(config.access)
    .. ',"backend":' .. json_string(config.backend)
    .. ',"producer_start_pc":' .. tostring(config.producer_start_pc)
    .. ',"producer_start_pc_hex":' .. json_string(hex32(config.producer_start_pc))
    .. ',"producer_end_pc":' .. tostring(config.producer_end_pc)
    .. ',"producer_end_pc_hex":' .. json_string(hex32(config.producer_end_pc))
    .. ',"call_site_start_pc":' .. tostring(config.call_site_start_pc)
    .. ',"call_site_start_pc_hex":' .. json_string(hex32(config.call_site_start_pc))
    .. ',"call_site_end_pc":' .. tostring(config.call_site_end_pc)
    .. ',"call_site_end_pc_hex":' .. json_string(hex32(config.call_site_end_pc))
    .. ',"return_start_pc":' .. tostring(config.return_start_pc)
    .. ',"return_start_pc_hex":' .. json_string(hex32(config.return_start_pc))
    .. ',"return_end_pc":' .. tostring(config.return_end_pc)
    .. ',"return_end_pc_hex":' .. json_string(hex32(config.return_end_pc))
    .. ',"queue_capacity":' .. tostring(config.queue_capacity)
    .. ',"max_slices":' .. tostring(config.max_slices)
    .. ',"max_call_depth":' .. tostring(config.max_call_depth)
    .. ',"wait_for_load_state":' .. json_boolean(config.wait_for_load_state)
    .. ',"call_path_root_complete":false'
    .. ',"stack_memory_available":false}'
end

return output

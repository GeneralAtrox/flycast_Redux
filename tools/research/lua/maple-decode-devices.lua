-- Maple device identity and per-function payload decoders.
--
-- Loaded by maple-decode.lua. Each decoder fills `result.request_data` /
-- `result.response_data` and records malformed frames through `add_error`.

local module_source = assert(debug.getinfo(1, "S").source)
local module_path = module_source:match("^@(.+)$")
assert(module_path, "maple-decode-devices.lua must be loaded from a file")
local module_directory = module_path:match("^(.*[\\/])") or ""
local tables = assert(dofile(module_directory .. "maple-decode-tables.lua"))

local array = tables.array
local has_bit = tables.has_bit
local bytes_hex = tables.bytes_hex
local u16le = tables.u16le
local u32le = tables.u32le
local trim_ascii = tables.trim_ascii

local devices = {}

local function decode_functions(value)
  local names = array()
  local known = 0
  for _, definition in ipairs(tables.function_definitions) do
    if has_bit(value, definition.mask) then
      names[#names + 1] = definition.name
      known = known + definition.mask
    end
  end
  local unknown = value - known
  return {
    mask = value,
    mask_hex = string.format("0x%08x", value),
    names = names,
    unknown_mask = unknown,
    unknown_mask_hex = string.format("0x%08x", unknown),
  }
end

devices.decode_functions = decode_functions

local function add_error(result, message)
  result.errors[#result.errors + 1] = message
  result.status = "malformed"
end

devices.add_error = add_error

local function require_data_length(result, frame, expected, description)
  if frame.data_bytes ~= expected then
    add_error(result, description .. " requires " .. expected
      .. " data bytes; observed " .. frame.data_bytes)
    return false
  end
  return true
end

local function decode_parameter(bytes, offset)
  return {
    raw_hex = bytes_hex(bytes, offset, offset + 3),
    partition = bytes[offset],
    phase = bytes[offset + 1],
    block = bytes[offset + 2] * 0x100 + bytes[offset + 3],
  }
end

local function device_class(functions)
  if functions.mask == 0x01000000 then return "controller" end
  if functions.mask == 0x0e000000 then return "vmu" end
  return "maple-device"
end

function devices.decode_device_status(result, frame)
  local minimum = tables.device_status_data_bytes
  if frame.data_bytes < minimum then
    add_error(result, "device status requires at least " .. minimum
      .. " data bytes; observed " .. frame.data_bytes)
    return
  end
  local bytes = frame.bytes
  local functions = decode_functions(u32le(bytes, 5))
  result.response_data = {
    device_class = device_class(functions),
    functions = functions,
    function_capabilities_hex = array({
      string.format("0x%08x", u32le(bytes, 9)),
      string.format("0x%08x", u32le(bytes, 13)),
      string.format("0x%08x", u32le(bytes, 17)),
    }),
    area_code = bytes[21],
    direction = bytes[22],
    product_name = trim_ascii(bytes, 23, 30),
    product_license = trim_ascii(bytes, 53, 60),
    standby_current_ma = u16le(bytes, 113),
    maximum_current_ma = u16le(bytes, 115),
  }
  if frame.code == 0x05 and frame.data_bytes ~= minimum then
    add_error(result, "device-status response has unexpected trailing data")
  elseif frame.code == 0x06 and frame.data_bytes > minimum then
    result.status = "partial"
    result.response_data.trailing_data_hex = bytes_hex(bytes, 117, #bytes)
  end
end

function devices.decode_controller_condition(result, request_frame, response_frame,
    device_type)
  if not require_data_length(result, request_frame,
      tables.controller_condition_request_bytes,
      "controller get-condition request") then return end
  result.request_data = {
    functions = decode_functions(u32le(request_frame.bytes, 5)),
  }
  if response_frame.code ~= 0x08 then return end
  if not require_data_length(result, response_frame,
      tables.controller_condition_response_bytes,
      "controller condition response") then return end
  local bytes = response_frame.bytes
  local functions = decode_functions(u32le(bytes, 5))
  if functions.mask ~= 0x01000000 then
    add_error(result, "controller condition response has a non-controller function")
    return
  end
  local buttons = u16le(bytes, 9)
  if device_type ~= 0 then
    result.status = "partial"
    result.response_data = {
      functions = functions,
      buttons_active_low = buttons,
      buttons_active_low_hex = string.format("0x%04x", buttons),
      analog_bytes = array({ bytes[11], bytes[12], bytes[13], bytes[14],
        bytes[15], bytes[16] }),
      semantic_note = "axis and button names require the standard controller device type",
    }
    return
  end
  local pressed = array()
  for _, definition in ipairs(tables.button_definitions) do
    if not has_bit(buttons, definition.mask) then
      pressed[#pressed + 1] = definition.name
    end
  end
  result.response_data = {
    functions = functions,
    buttons_active_low = buttons,
    buttons_active_low_hex = string.format("0x%04x", buttons),
    pressed_buttons = pressed,
    right_trigger = bytes[11],
    left_trigger = bytes[12],
    joystick_x = bytes[13],
    joystick_y = bytes[14],
    joystick_x_offset = bytes[13] - 0x80,
    joystick_y_offset = bytes[14] - 0x80,
    reserved_analog_hex = bytes_hex(bytes, 15, 16),
  }
end

local function classify_unknown_payload(result, functions)
  if functions.unknown_mask ~= 0 or #functions.names ~= 1 then
    result.status = "unknown"
  else
    result.status = "partial"
  end
end

local function decode_block_write(result, request_frame, function_value, functions,
    parameter)
  local bytes = request_frame.bytes
  local header = tables.block_request_header_bytes
  result.request_data.payload_byte_count = request_frame.data_bytes - header
  result.request_data.payload_hex = bytes_hex(bytes, header + 5, #bytes)
  if function_value == 0x02000000 then
    local write_offset = parameter.block * 512 + parameter.phase * 128
    result.request_data.write_offset = write_offset
    if write_offset + result.request_data.payload_byte_count > tables.vmu_storage_bytes then
      add_error(result, "storage block-write exceeds the 128 KiB VMU storage range")
    end
    return
  end
  local expected = tables.block_write_payload_bytes[function_value]
  if expected == nil then
    classify_unknown_payload(result, functions)
  elseif result.request_data.payload_byte_count ~= expected then
    add_error(result, "block-write payload for " .. functions.names[1]
      .. " requires " .. expected .. " bytes; observed "
      .. result.request_data.payload_byte_count)
  end
end

local function decode_block_read(result, request_frame, response_frame, function_value,
    functions)
  local header = tables.block_request_header_bytes
  if request_frame.data_bytes ~= header then
    add_error(result, "block-read request has unexpected trailing data")
    return
  end
  if response_frame.code ~= 0x08 then return end
  if response_frame.data_bytes < header then
    add_error(result, "block-read response requires a function and parameter word")
    return
  end
  local response_bytes = response_frame.bytes
  local response_function = u32le(response_bytes, 5)
  local response_functions = decode_functions(response_function)
  result.response_data = {
    functions = response_functions,
    parameter = decode_parameter(response_bytes, 9),
    payload_byte_count = response_frame.data_bytes - header,
    payload_hex = bytes_hex(response_bytes, header + 5, #response_bytes),
  }
  if response_function ~= function_value then
    add_error(result, "block-read response function does not match request")
    return
  end
  local expected = tables.block_read_payload_bytes[function_value]
  if expected == nil then
    classify_unknown_payload(result, functions)
  elseif result.response_data.payload_byte_count ~= expected then
    add_error(result, "block-read payload for " .. functions.names[1]
      .. " requires " .. expected .. " bytes; observed "
      .. result.response_data.payload_byte_count)
  end
end

function devices.decode_block_request(result, request_frame, response_frame, write)
  if request_frame.data_bytes < tables.block_request_header_bytes then
    add_error(result, "block request requires a function and parameter word")
    return
  end
  local bytes = request_frame.bytes
  local function_value = u32le(bytes, 5)
  local functions = decode_functions(function_value)
  local parameter = decode_parameter(bytes, 9)
  result.request_data = {
    functions = functions,
    parameter = parameter,
  }
  if write then
    decode_block_write(result, request_frame, function_value, functions, parameter)
    return
  end
  decode_block_read(result, request_frame, response_frame, function_value, functions)
end

return devices

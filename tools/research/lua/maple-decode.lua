-- Game-agnostic Maple protocol decoder for discovery output.
--
-- This module never replaces the raw frames. It adds a fail-closed semantic
-- view: malformed lengths and unsupported commands/functions remain explicit.

local decoder = {}

local array_metatable = { __maple_json_array = true }

local function array(values)
  return setmetatable(values or {}, array_metatable)
end

decoder.array = array

local command_names = {
  [0x01] = "device-request",
  [0x02] = "all-status-request",
  [0x03] = "device-reset",
  [0x04] = "device-kill",
  [0x05] = "device-status",
  [0x06] = "device-all-status",
  [0x09] = "get-condition",
  [0x0a] = "get-media-info",
  [0x0b] = "block-read",
  [0x0c] = "block-write",
  [0x0d] = "get-last-error",
  [0x0e] = "set-condition",
  [0x0f] = "microphone-control",
  [0x10] = "argun-control",
  [0x80] = "jvs-command-0x80",
  [0x82] = "jvs-command-0x82",
  [0x84] = "jvs-command-0x84",
  [0x86] = "jvs-command-0x86",
}

local response_names = {
  [0x00] = "jvs-none",
  [0x05] = "device-status",
  [0x06] = "device-status-all",
  [0x07] = "device-reply",
  [0x08] = "data-transfer",
  [0x83] = "jvs-response-0x83",
  [0x85] = "jvs-response-0x85",
  [0x87] = "jvs-response-0x87",
  [0xf9] = "argun-error",
  [0xfa] = "lcd-error",
  [0xfb] = "file-error",
  [0xfc] = "transmit-again",
  [0xfd] = "unknown-command",
  [0xfe] = "unknown-function",
}

local function_definitions = {
  { mask = 0x80000000, name = "light-gun" },
  { mask = 0x40000000, name = "keyboard" },
  { mask = 0x20000000, name = "arcade-gun" },
  { mask = 0x10000000, name = "microphone" },
  { mask = 0x08000000, name = "clock" },
  { mask = 0x04000000, name = "lcd" },
  { mask = 0x02000000, name = "storage" },
  { mask = 0x01000000, name = "controller" },
  { mask = 0x00080000, name = "camera" },
  { mask = 0x00040000, name = "storage-extension" },
  { mask = 0x00020000, name = "mouse" },
  { mask = 0x00010000, name = "vibration" },
}

local button_definitions = {
  { mask = 0x0001, name = "c" },
  { mask = 0x0002, name = "b" },
  { mask = 0x0004, name = "a" },
  { mask = 0x0008, name = "start" },
  { mask = 0x0010, name = "dpad-up" },
  { mask = 0x0020, name = "dpad-down" },
  { mask = 0x0040, name = "dpad-left" },
  { mask = 0x0080, name = "dpad-right" },
  { mask = 0x0100, name = "z" },
  { mask = 0x0200, name = "y" },
  { mask = 0x0400, name = "x" },
  { mask = 0x0800, name = "d" },
  { mask = 0x1000, name = "dpad2-up" },
  { mask = 0x2000, name = "dpad2-down" },
  { mask = 0x4000, name = "dpad2-left" },
  { mask = 0x8000, name = "dpad2-right" },
}

local function has_bit(value, mask)
  return math.floor(value / mask) % 2 == 1
end

local function bytes_hex(bytes, first, last)
  local pieces = {}
  for index = first, last do
    pieces[#pieces + 1] = string.format("%02x", bytes[index])
  end
  return table.concat(pieces)
end

local function u16le(bytes, offset)
  return bytes[offset] + bytes[offset + 1] * 0x100
end

local function u32le(bytes, offset)
  return bytes[offset] + bytes[offset + 1] * 0x100
    + bytes[offset + 2] * 0x10000 + bytes[offset + 3] * 0x1000000
end

local function trim_ascii(bytes, first, length)
  local characters = {}
  for index = first, first + length - 1 do
    local value = bytes[index]
    if value == 0 then break end
    characters[#characters + 1] = string.char(value)
  end
  return table.concat(characters):gsub("%s+$", "")
end

local function parse_frame(event, response)
  local payload = event and event.payload
  if type(payload) ~= "string" then
    return nil, "payload is not a byte string"
  end
  local bytes = { string.byte(payload, 1, #payload) }
  if response and #bytes == 4 and bytes_hex(bytes, 1, 4) == "ffffffff"
      and event.device_present == false then
    return { bytes = bytes, code = 0xff, no_device = true }
  end
  if #bytes < 4 then
    return nil, "frame is shorter than the four-byte Maple header"
  end
  if #bytes % 4 ~= 0 then
    return nil, "frame byte count is not word aligned"
  end
  local expected = (bytes[4] + 1) * 4
  if expected ~= #bytes then
    return nil, "header word count does not match frame byte count"
  end
  local reported_code = response and event.response_code or event.command
  if reported_code ~= nil and bytes[1] ~= reported_code then
    return nil, "frame code does not match event metadata"
  end
  if event.byte_count ~= nil and event.byte_count ~= #bytes then
    return nil, "frame byte count does not match event metadata"
  end
  return {
    bytes = bytes,
    code = bytes[1],
    destination = bytes[2],
    source = bytes[3],
    data_words = bytes[4],
    data_bytes = #bytes - 4,
  }
end

local function decode_functions(value)
  local names = array()
  local known = 0
  for _, definition in ipairs(function_definitions) do
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

local function add_error(result, message)
  result.errors[#result.errors + 1] = message
  result.status = "malformed"
end

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

local function decode_device_status(result, frame)
  if frame.data_bytes < 112 then
    add_error(result, "device status requires at least 112 data bytes; observed "
      .. frame.data_bytes)
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
  if frame.code == 0x05 and frame.data_bytes ~= 112 then
    add_error(result, "device-status response has unexpected trailing data")
  elseif frame.code == 0x06 and frame.data_bytes > 112 then
    result.status = "partial"
    result.response_data.trailing_data_hex = bytes_hex(bytes, 117, #bytes)
  end
end

local function decode_controller_condition(result, request_frame, response_frame,
    device_type)
  if not require_data_length(result, request_frame, 4,
      "controller get-condition request") then return end
  result.request_data = {
    functions = decode_functions(u32le(request_frame.bytes, 5)),
  }
  if response_frame.code ~= 0x08 then return end
  if not require_data_length(result, response_frame, 12,
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
  for _, definition in ipairs(button_definitions) do
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

local function decode_block_request(result, request_frame, response_frame, write)
  if request_frame.data_bytes < 8 then
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
    result.request_data.payload_byte_count = request_frame.data_bytes - 8
    result.request_data.payload_hex = bytes_hex(bytes, 13, #bytes)
    if function_value == 0x02000000 then
      local write_offset = parameter.block * 512 + parameter.phase * 128
      result.request_data.write_offset = write_offset
      if write_offset + result.request_data.payload_byte_count > 128 * 1024 then
        add_error(result, "storage block-write exceeds the 128 KiB VMU storage range")
      end
      return
    end
    local expected = ({
      [0x04000000] = 192,
      [0x08000000] = 8,
    })[function_value]
    if expected == nil then
      if functions.unknown_mask ~= 0 or #functions.names ~= 1 then
        result.status = "unknown"
      else
        result.status = "partial"
      end
    elseif result.request_data.payload_byte_count ~= expected then
      add_error(result, "block-write payload for " .. functions.names[1]
        .. " requires " .. expected .. " bytes; observed "
        .. result.request_data.payload_byte_count)
    end
    return
  end
  if request_frame.data_bytes ~= 8 then
    add_error(result, "block-read request has unexpected trailing data")
    return
  end
  if response_frame.code ~= 0x08 then return end
  if response_frame.data_bytes < 8 then
    add_error(result, "block-read response requires a function and parameter word")
    return
  end
  local response_bytes = response_frame.bytes
  local response_function = u32le(response_bytes, 5)
  local response_functions = decode_functions(response_function)
  result.response_data = {
    functions = response_functions,
    parameter = decode_parameter(response_bytes, 9),
    payload_byte_count = response_frame.data_bytes - 8,
    payload_hex = bytes_hex(response_bytes, 13, #response_bytes),
  }
  if response_function ~= function_value then
    add_error(result, "block-read response function does not match request")
    return
  end
  local expected = ({
    [0x02000000] = 512,
    [0x04000000] = 192,
    [0x08000000] = 8,
  })[function_value]
  if expected == nil then
    if functions.unknown_mask ~= 0 or #functions.names ~= 1 then
      result.status = "unknown"
    else
      result.status = "partial"
    end
  elseif result.response_data.payload_byte_count ~= expected then
    add_error(result, "block-read payload for " .. functions.names[1]
      .. " requires " .. expected .. " bytes; observed "
      .. result.response_data.payload_byte_count)
  end
end

local function is_error_response(code)
  return code >= 0xf9 and code <= 0xfe
end

function decoder.decode(request_event, response_event)
  local result = {
    status = "decoded",
    errors = array(),
  }
  local request_frame, request_error = parse_frame(request_event, false)
  local response_frame, response_error = parse_frame(response_event, true)
  local command_code = request_frame and request_frame.code
    or (request_event and request_event.command)
  local response_code = response_frame and response_frame.code
    or (response_event and response_event.response_code)
  result.command = {
    code = command_code,
    name = command_names[command_code] or "unknown",
  }
  result.response = {
    code = response_code,
    name = response_names[response_code] or "unknown",
  }
  result.operation = result.command.name
  if request_error then add_error(result, "request: " .. request_error) end
  if response_error then add_error(result, "response: " .. response_error) end
  if not request_frame or not response_frame then return result end

  if response_frame.no_device then
    result.response.name = "no-device"
    result.response_data = { device_present = false }
    return result
  end
  if not command_names[request_frame.code] or not response_names[response_frame.code] then
    result.status = "unknown"
  end

  if response_frame.code == 0x05 or response_frame.code == 0x06 then
    decode_device_status(result, response_frame)
  elseif request_frame.code == 0x09 then
    if response_frame.code ~= 0x08 and not is_error_response(response_frame.code) then
      add_error(result, "get-condition returned an unexpected response code")
      return result
    end
    if request_frame.data_bytes ~= 4 then
      add_error(result, "get-condition request requires one function word")
    else
      local function_value = u32le(request_frame.bytes, 5)
      if function_value == 0x01000000 then
        decode_controller_condition(result, request_frame, response_frame,
          response_event.device_type)
      else
        result.request_data = { functions = decode_functions(function_value) }
        result.status = "unknown"
      end
    end
  elseif request_frame.code == 0x0b then
    if response_frame.code ~= 0x08 and not is_error_response(response_frame.code) then
      add_error(result, "block-read returned an unexpected response code")
      return result
    end
    decode_block_request(result, request_frame, response_frame, false)
  elseif request_frame.code == 0x0c then
    if response_frame.code ~= 0x07 and not is_error_response(response_frame.code) then
      add_error(result, "block-write returned an unexpected response code")
      return result
    end
    decode_block_request(result, request_frame, response_frame, true)
  end
  return result
end

local escapes = {
  ['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f',
  ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t',
}

local function encode_string(value)
  return '"' .. value:gsub('[%z\1-\31\\"]', function(character)
    return escapes[character] or string.format("\\u%04x", string.byte(character))
  end) .. '"'
end

local function encode_json(value)
  local kind = type(value)
  if kind == "string" then return encode_string(value) end
  if kind == "number" then
    assert(value == value and value ~= math.huge and value ~= -math.huge,
      "cannot encode a non-finite JSON number")
    return tostring(value)
  end
  if kind == "boolean" then return value and "true" or "false" end
  if kind ~= "table" then error("unsupported JSON value type: " .. kind) end
  if getmetatable(value) == array_metatable then
    local items = {}
    for index = 1, #value do items[index] = encode_json(value[index]) end
    return "[" .. table.concat(items, ",") .. "]"
  end
  local keys = {}
  for key in pairs(value) do
    assert(type(key) == "string", "JSON object keys must be strings")
    keys[#keys + 1] = key
  end
  table.sort(keys)
  local fields = {}
  for index, key in ipairs(keys) do
    fields[index] = encode_string(key) .. ":" .. encode_json(value[key])
  end
  return "{" .. table.concat(fields, ",") .. "}"
end

decoder.encode_json = encode_json

return decoder

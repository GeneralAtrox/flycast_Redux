-- Game-agnostic Maple protocol decoder for discovery output.
--
-- This module never replaces the raw frames. It adds a fail-closed semantic
-- view: malformed lengths and unsupported commands/functions remain explicit.
--
-- Public API: `decode(request_event, response_event)`, `encode_json(value)`,
-- and `array(values)`. Protocol tables live in maple-decode-tables.lua and
-- device/function decoders in maple-decode-devices.lua, both loaded from
-- this file's own directory.

local decoder_source = assert(debug.getinfo(1, "S").source)
local decoder_path = decoder_source:match("^@(.+)$")
assert(decoder_path, "maple-decode.lua must be loaded from a file")
local decoder_directory = decoder_path:match("^(.*[\\/])") or ""
local tables = assert(dofile(decoder_directory .. "maple-decode-tables.lua"))
local devices = assert(dofile(decoder_directory .. "maple-decode-devices.lua"))

local decoder = {}

local array = tables.array
local array_metatable = tables.array_metatable
local bytes_hex = tables.bytes_hex
local u32le = tables.u32le
local command_names = tables.command_names
local response_names = tables.response_names
local is_error_response = tables.is_error_response
local add_error = devices.add_error

decoder.array = array

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

local function decode_get_condition(result, request_frame, response_frame, response_event)
  if response_frame.code ~= 0x08 and not is_error_response(response_frame.code) then
    add_error(result, "get-condition returned an unexpected response code")
    return
  end
  if request_frame.data_bytes ~= 4 then
    add_error(result, "get-condition request requires one function word")
    return
  end
  local function_value = u32le(request_frame.bytes, 5)
  if function_value == 0x01000000 then
    devices.decode_controller_condition(result, request_frame, response_frame,
      response_event.device_type)
  else
    result.request_data = { functions = devices.decode_functions(function_value) }
    result.status = "unknown"
  end
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
    devices.decode_device_status(result, response_frame)
  elseif request_frame.code == 0x09 then
    decode_get_condition(result, request_frame, response_frame, response_event)
  elseif request_frame.code == 0x0b then
    if response_frame.code ~= 0x08 and not is_error_response(response_frame.code) then
      add_error(result, "block-read returned an unexpected response code")
      return result
    end
    devices.decode_block_request(result, request_frame, response_frame, false)
  elseif request_frame.code == 0x0c then
    if response_frame.code ~= 0x07 and not is_error_response(response_frame.code) then
      add_error(result, "block-write returned an unexpected response code")
      return result
    end
    devices.decode_block_request(result, request_frame, response_frame, true)
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
  -- Each dofile() of maple-decode-tables.lua creates a fresh marker table, so
  -- match on the marker field rather than the metatable's identity: arrays
  -- built inside maple-decode-devices.lua carry a different table instance.
  local metatable = getmetatable(value)
  if metatable ~= nil and metatable.__maple_json_array then
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

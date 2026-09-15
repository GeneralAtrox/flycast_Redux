-- Maple protocol tables shared by the decoder modules.
--
-- Command/response names, function and button bit definitions, per-command
-- length rules, and the little-endian byte primitives used to read frames.

local tables = {}

tables.array_metatable = { __maple_json_array = true }

function tables.array(values)
  return setmetatable(values or {}, tables.array_metatable)
end

tables.command_names = {
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

tables.response_names = {
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

tables.function_definitions = {
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

tables.button_definitions = {
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

-- Per-command length rules (data bytes, excluding the four-byte header).
tables.device_status_data_bytes = 112
tables.controller_condition_request_bytes = 4
tables.controller_condition_response_bytes = 12
tables.block_request_header_bytes = 8
tables.vmu_storage_bytes = 128 * 1024

-- Expected block payload sizes keyed by the single requested function mask.
tables.block_write_payload_bytes = {
  [0x04000000] = 192,
  [0x08000000] = 8,
}

tables.block_read_payload_bytes = {
  [0x02000000] = 512,
  [0x04000000] = 192,
  [0x08000000] = 8,
}

function tables.is_error_response(code)
  return code >= 0xf9 and code <= 0xfe
end

function tables.has_bit(value, mask)
  return math.floor(value / mask) % 2 == 1
end

function tables.bytes_hex(bytes, first, last)
  local pieces = {}
  for index = first, last do
    pieces[#pieces + 1] = string.format("%02x", bytes[index])
  end
  return table.concat(pieces)
end

function tables.u16le(bytes, offset)
  return bytes[offset] + bytes[offset + 1] * 0x100
end

function tables.u32le(bytes, offset)
  return bytes[offset] + bytes[offset + 1] * 0x100
    + bytes[offset + 2] * 0x10000 + bytes[offset + 3] * 0x1000000
end

function tables.trim_ascii(bytes, first, length)
  local characters = {}
  for index = first, first + length - 1 do
    local value = bytes[index]
    if value == 0 then break end
    characters[#characters + 1] = string.char(value)
  end
  return table.concat(characters):gsub("%s+$", "")
end

return tables

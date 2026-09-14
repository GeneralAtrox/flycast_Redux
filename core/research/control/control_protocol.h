#pragma once

// JSON-lines control protocol, independent of the transport and of the
// emulator so it can be unit-tested with fake actions.
//
// Request (one line):  {"id": <any>, "token": "<secret>", "cmd": "<name>",
//                       "args": {...}}
// Response (one line): {"id": <echo>, "ok": true, "result": {...}}
//                  or  {"id": <echo>, "ok": false, "error": "<message>"}

#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace research::control
{

using json = nlohmann::json;

// Everything the protocol can ask the emulator to do. Each callback may throw;
// the message becomes the error string of the response.
struct ControlActions
{
	std::function<json()> status;
	std::function<void()> pause;
	std::function<void()> resume;
	std::function<void(int slot)> saveState;
	std::function<void(int slot)> loadState;
	std::function<std::vector<std::uint8_t>(std::uint32_t address, std::uint32_t length)> readMemory;
	std::function<json()> readRegisters;
	std::function<void(const std::string& path, const json& config)> recordStart;
	std::function<void()> recordStop;
	std::function<json()> recordStatus;
	std::function<void(std::uint32_t pc, std::uint32_t gateAddress, std::uint32_t gateValue)> checkpointSet;
	std::function<void()> checkpointClear;
	std::function<void()> requestExit;
};

struct ControlLimits
{
	std::size_t maxRequestBytes = 64 * 1024;
	std::uint32_t maxReadBytes = 1u << 20;   // mem_read payload (base64 in the reply)
	std::uint32_t maxDumpBytes = 64u << 20;  // mem_dump written to a file
};

// Handles one request line. Never throws: transport, auth, parse, and action
// failures all become {"ok": false, "error": ...}. `expectedToken` empty
// disables authentication (the server only ever binds to loopback).
std::string handleControlRequest(const std::string& line, const std::string& expectedToken,
		const ControlActions& actions, const ControlLimits& limits = {});

// The fixed command inventory and limits, also returned by "capabilities".
json controlCapabilities(const ControlLimits& limits = {});

std::string base64Encode(const std::uint8_t *bytes, std::size_t size);

} // namespace research::control

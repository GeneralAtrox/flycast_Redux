#include "research/control/control_protocol.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace research::control
{
namespace
{

constexpr int ProtocolVersion = 1;

const char *const Commands[] = {
	"capabilities", "status", "pause", "resume", "savestate", "loadstate",
	"mem_read", "mem_dump", "regs", "record_start", "record_stop", "record_status",
	"checkpoint_set", "checkpoint_clear", "exit",
};

std::uint64_t argUnsigned(const json& args, const char *key, std::uint64_t maximum,
		bool required, std::uint64_t fallback = 0)
{
	if (!args.contains(key))
	{
		if (required)
			throw std::invalid_argument(std::string("missing argument: ") + key);
		return fallback;
	}
	const json& value = args.at(key);
	std::uint64_t result;
	if (value.is_number_unsigned())
		result = value.get<std::uint64_t>();
	else if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
		result = static_cast<std::uint64_t>(value.get<std::int64_t>());
	else if (value.is_string())
	{
		try
		{
			result = std::stoull(value.get<std::string>(), nullptr, 0);
		}
		catch (const std::exception&)
		{
			throw std::invalid_argument(std::string(key) + " is not a number");
		}
	}
	else
		throw std::invalid_argument(std::string(key) + " must be a non-negative integer");
	if (result > maximum)
		throw std::invalid_argument(std::string(key) + " is out of range");
	return result;
}

std::string argString(const json& args, const char *key)
{
	if (!args.contains(key) || !args.at(key).is_string())
		throw std::invalid_argument(std::string("missing string argument: ") + key);
	return args.at(key).get<std::string>();
}

template<typename F>
void require(const F& action, const char *name)
{
	if (!action)
		throw std::runtime_error(std::string(name) + " is not available in this build");
}

json dispatch(const std::string& command, const json& args, const ControlActions& actions,
		const ControlLimits& limits)
{
	if (command == "capabilities")
		return controlCapabilities(limits);
	if (command == "status")
	{
		require(actions.status, "status");
		return actions.status();
	}
	if (command == "pause")
	{
		require(actions.pause, "pause");
		actions.pause();
		return json::object();
	}
	if (command == "resume")
	{
		require(actions.resume, "resume");
		actions.resume();
		return json::object();
	}
	if (command == "savestate" || command == "loadstate")
	{
		const int slot = static_cast<int>(argUnsigned(args, "slot", 9, false, 0));
		if (command == "savestate")
		{
			require(actions.saveState, "savestate");
			actions.saveState(slot);
		}
		else
		{
			require(actions.loadState, "loadstate");
			actions.loadState(slot);
		}
		return json {{"slot", slot}};
	}
	if (command == "mem_read")
	{
		require(actions.readMemory, "mem_read");
		const auto address = static_cast<std::uint32_t>(argUnsigned(args, "addr", 0xffffffffu, true));
		const auto length = static_cast<std::uint32_t>(argUnsigned(args, "len", limits.maxReadBytes, true));
		const std::vector<std::uint8_t> bytes = actions.readMemory(address, length);
		return json {{"addr", address}, {"len", bytes.size()},
				{"base64", base64Encode(bytes.data(), bytes.size())}};
	}
	if (command == "mem_dump")
	{
		require(actions.readMemory, "mem_dump");
		const auto address = static_cast<std::uint32_t>(argUnsigned(args, "addr", 0xffffffffu, true));
		const auto length = static_cast<std::uint32_t>(argUnsigned(args, "len", limits.maxDumpBytes, true));
		const std::string path = argString(args, "path");
		std::ofstream output(path, std::ios::binary | std::ios::trunc);
		if (!output)
			throw std::runtime_error("cannot create dump file: " + path);
		std::uint32_t written = 0;
		while (written < length)
		{
			const std::uint32_t chunk = std::min<std::uint32_t>(length - written, limits.maxReadBytes);
			const std::vector<std::uint8_t> bytes = actions.readMemory(address + written, chunk);
			output.write(reinterpret_cast<const char *>(bytes.data()),
					static_cast<std::streamsize>(bytes.size()));
			written += static_cast<std::uint32_t>(bytes.size());
			if (bytes.size() != chunk)
				break;
		}
		if (!output)
			throw std::runtime_error("write to dump file failed: " + path);
		return json {{"addr", address}, {"len", written}, {"path", path}};
	}
	if (command == "regs")
	{
		require(actions.readRegisters, "regs");
		return actions.readRegisters();
	}
	if (command == "record_start")
	{
		require(actions.recordStart, "record_start");
		const std::string path = argString(args, "path");
		const json config = args.contains("config") ? args.at("config") : json::object();
		if (!config.is_object())
			throw std::invalid_argument("config must be an object");
		actions.recordStart(path, config);
		return json {{"path", path}};
	}
	if (command == "record_stop")
	{
		require(actions.recordStop, "record_stop");
		actions.recordStop();
		return json::object();
	}
	if (command == "record_status")
	{
		require(actions.recordStatus, "record_status");
		return actions.recordStatus();
	}
	if (command == "checkpoint_set")
	{
		require(actions.checkpointSet, "checkpoint_set");
		const auto pc = static_cast<std::uint32_t>(argUnsigned(args, "pc", 0xffffffffu, true));
		const auto gateAddress = static_cast<std::uint32_t>(argUnsigned(args, "gate_addr", 0xffffffffu, false));
		const auto gateValue = static_cast<std::uint32_t>(argUnsigned(args, "gate_value", 0xffffffffu, false));
		actions.checkpointSet(pc, gateAddress, gateValue);
		return json {{"pc", pc}};
	}
	if (command == "checkpoint_clear")
	{
		require(actions.checkpointClear, "checkpoint_clear");
		actions.checkpointClear();
		return json::object();
	}
	if (command == "exit")
	{
		require(actions.requestExit, "exit");
		actions.requestExit();
		return json::object();
	}
	throw std::invalid_argument("unknown command: " + command);
}

} // namespace

json controlCapabilities(const ControlLimits& limits)
{
	json commands = json::array();
	for (const char *command : Commands)
		commands.push_back(command);
	return json {
		{"protocol", ProtocolVersion},
		{"commands", commands},
		{"memory_mutation", false},
		{"max_request_bytes", limits.maxRequestBytes},
		{"max_read_bytes", limits.maxReadBytes},
		{"max_dump_bytes", limits.maxDumpBytes},
		{"note", "memory and register reads are unsynchronized while the emulator runs; pause first for a coherent snapshot"},
	};
}

std::string handleControlRequest(const std::string& line, const std::string& expectedToken,
		const ControlActions& actions, const ControlLimits& limits)
{
	json id;
	try
	{
		if (line.size() > limits.maxRequestBytes)
			throw std::invalid_argument("request exceeds the size limit");
		const json request = json::parse(line);
		if (!request.is_object())
			throw std::invalid_argument("request must be a JSON object");
		for (auto it = request.begin(); it != request.end(); ++it)
			if (it.key() != "id" && it.key() != "token" && it.key() != "cmd" && it.key() != "args")
				throw std::invalid_argument("unknown request key: " + it.key());
		if (request.contains("id"))
			id = request.at("id");
		if (!expectedToken.empty())
		{
			if (!request.contains("token") || !request.at("token").is_string()
					|| request.at("token").get<std::string>() != expectedToken)
				throw std::invalid_argument("authentication failed");
		}
		if (!request.contains("cmd") || !request.at("cmd").is_string())
			throw std::invalid_argument("cmd must be a string");
		const json args = request.contains("args") ? request.at("args") : json::object();
		if (!args.is_object())
			throw std::invalid_argument("args must be an object");
		const json result = dispatch(request.at("cmd").get<std::string>(), args, actions, limits);
		return json {{"id", id}, {"ok", true}, {"result", result}}.dump();
	}
	catch (const std::exception& exception)
	{
		return json {{"id", id}, {"ok", false}, {"error", exception.what()}}.dump();
	}
}

std::string base64Encode(const std::uint8_t *bytes, std::size_t size)
{
	static const char Alphabet[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string out;
	out.reserve((size + 2) / 3 * 4);
	std::size_t i = 0;
	for (; i + 2 < size; i += 3)
	{
		const std::uint32_t triple = (bytes[i] << 16) | (bytes[i + 1] << 8) | bytes[i + 2];
		out.push_back(Alphabet[(triple >> 18) & 63]);
		out.push_back(Alphabet[(triple >> 12) & 63]);
		out.push_back(Alphabet[(triple >> 6) & 63]);
		out.push_back(Alphabet[triple & 63]);
	}
	if (i < size)
	{
		std::uint32_t triple = bytes[i] << 16;
		if (i + 1 < size)
			triple |= bytes[i + 1] << 8;
		out.push_back(Alphabet[(triple >> 18) & 63]);
		out.push_back(Alphabet[(triple >> 12) & 63]);
		out.push_back(i + 1 < size ? Alphabet[(triple >> 6) & 63] : '=');
		out.push_back('=');
	}
	return out;
}

} // namespace research::control

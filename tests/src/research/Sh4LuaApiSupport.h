#pragma once

#ifdef USE_LUA

#include "cfg/option.h"
#include "json.hpp"
#include "research/maple_observation.h"
#include "hw/sh4/sh4_if.h"
#include "research/sh4_observation.h"
#include "stdclass.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace sh4_lua_test
{

using json = nlohmann::json;

class LuaTestDirectory
{
public:
	LuaTestDirectory()
	{
		path = std::filesystem::temp_directory_path()
				/ ("flycast-lua-research-"
						+ std::to_string(std::chrono::high_resolution_clock::now()
								.time_since_epoch().count()));
		std::filesystem::create_directories(path);
	}

	~LuaTestDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	std::filesystem::path file(const char *name) const { return path / name; }
	const std::filesystem::path& directory() const { return path; }

private:
	std::filesystem::path path;
};

// Detaches the SH-4 control block for the lifetime of the guard so a test can
// exercise the "scheduler is not initialized" path. Without this the test only
// passes when it runs before any suite that calls emu.init(), because the
// control block stays initialized for the rest of the process.
class DetachedSchedulerGuard
{
public:
	DetachedSchedulerGuard() : saved(p_sh4rcb) { p_sh4rcb = nullptr; }
	~DetachedSchedulerGuard() { p_sh4rcb = saved; }
	DetachedSchedulerGuard(const DetachedSchedulerGuard&) = delete;
	DetachedSchedulerGuard& operator=(const DetachedSchedulerGuard&) = delete;

private:
	Sh4RCB *saved;
};

class LuaRuntimeGuard
{
public:
	~LuaRuntimeGuard()
	{
		lua::term();
		config::LuaFileName.set(previousLuaFile);
		set_user_config_dir("");
	}

	std::string previousLuaFile = config::LuaFileName.get();
};

inline void writeText(const std::filesystem::path& path, const std::string& text)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(output.is_open());
	output << text;
	ASSERT_TRUE(output.good());
}

inline std::string readText(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
}

inline std::string luaPath(const std::filesystem::path& path)
{
	return path.generic_u8string();
}

inline std::filesystem::path mapleWatcherPath()
{
	return std::filesystem::path(FLYCAST_TEST_FILES).parent_path().parent_path()
			/ "tools" / "research" / "lua" / "maple-watch.lua";
}

inline std::filesystem::path mapleDecoderPath()
{
	return mapleWatcherPath().parent_path() / "maple-decode.lua";
}

inline std::filesystem::path memoryWatcherPath()
{
	return mapleWatcherPath().parent_path() / "memory-watch.lua";
}

inline std::filesystem::path causalSliceWatcherPath()
{
	return mapleWatcherPath().parent_path() / "causal-slice-watch.lua";
}

inline std::vector<json> readJsonLines(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	std::vector<json> lines;
	std::string line;
	while (std::getline(input, line))
	{
		if (!line.empty())
			lines.push_back(json::parse(line));
	}
	return lines;
}

inline research::Sh4Observation memoryWrite(std::uint32_t address,
		std::uint64_t value)
{
	research::Sh4Observation observation;
	observation.backend = research::Sh4ObservationBackend::Interpreter;
	observation.type = research::Sh4ObservationType::MemoryWrite;
	observation.tick = 0x12345678;
	observation.instructionPc = 0x8c010000;
	observation.opcode = 0x2102;
	observation.delaySlotDepth = 1;
	observation.memoryAddress = address;
	observation.memoryWidth = 8;
	observation.memoryValue = value;
	return observation;
}

inline research::Sh4Observation memoryRead(std::uint32_t address,
		std::uint64_t value)
{
	research::Sh4Observation observation = memoryWrite(address, value);
	observation.type = research::Sh4ObservationType::MemoryRead;
	observation.opcode = 0x6014;
	return observation;
}

inline research::Sh4Observation instructionObservation(research::Sh4ObservationType type,
		std::uint32_t pc, std::uint16_t opcode, std::uint64_t tick,
		std::uint32_t registerMarker = 0)
{
	research::Sh4Observation observation;
	observation.backend = research::Sh4ObservationBackend::Interpreter;
	observation.type = type;
	observation.availableFields = research::Sh4Observation::HasNextPc
			| research::Sh4Observation::HasRegisters;
	observation.tick = tick;
	observation.instructionPc = pc;
	observation.nextPc = pc + 2;
	observation.opcode = opcode;
	for (std::size_t index = 0; index < observation.registers.r.size(); ++index)
		observation.registers.r[index] = registerMarker + static_cast<std::uint32_t>(index);
	observation.registers.pr = registerMarker + 0x100;
	observation.registers.gbr = registerMarker + 0x104;
	observation.registers.vbr = registerMarker + 0x108;
	observation.registers.mach = registerMarker + 0x10c;
	observation.registers.macl = registerMarker + 0x110;
	observation.registers.sr = registerMarker + 0x114;
	observation.registers.fpul = registerMarker + 0x118;
	observation.registers.fpscr = registerMarker + 0x11c;
	return observation;
}

inline research::Sh4Observation callObservation(std::uint32_t pc,
		std::uint32_t targetPc, std::uint64_t tick)
{
	research::Sh4Observation observation = instructionObservation(
			research::Sh4ObservationType::Call, pc, 0x410b, tick, 0x2000);
	observation.callKind = research::Sh4CallKind::Jsr;
	observation.targetPc = targetPc;
	observation.returnPc = pc + 4;
	observation.delaySlotPc = pc + 2;
	return observation;
}

inline research::Sh4Observation returnObservation(std::uint32_t pc,
		std::uint32_t targetPc, std::uint64_t tick)
{
	research::Sh4Observation observation = instructionObservation(
			research::Sh4ObservationType::Return, pc, 0x000b, tick, 0x2800);
	observation.targetPc = targetPc;
	observation.returnPc = targetPc;
	observation.delaySlotPc = pc + 2;
	return observation;
}

inline research::MapleTransactionEvent mapleTransaction(std::uint8_t bus = 1,
		std::uint8_t port = 2, std::uint8_t command = 9)
{
	research::MapleTransactionEvent transaction;
	transaction.tick = 0x12345678;
	transaction.descriptorAddress = 0x8c001000;
	transaction.destinationAddress = 0x8c002000;
	transaction.descriptorHeader1 = static_cast<std::uint32_t>(bus) << 16;
	transaction.descriptorHeader2 = transaction.destinationAddress;
	transaction.deviceType = 1;
	transaction.bus = bus;
	transaction.port = port;
	transaction.command = command;
	transaction.flags = research::MapleTransactionDevicePresent;
	transaction.request = {command, 0x20, 0x01, 0x00};
	transaction.response = {0x08, 0x01, 0x20, 0x00};
	return transaction;
}

inline research::MapleTransactionEvent controllerConditionTransaction()
{
	research::MapleTransactionEvent transaction = mapleTransaction(1, 2, 9);
	transaction.deviceType = 0;
	transaction.request = { 0x09, 0x20, 0x01, 0x01,
			0x00, 0x00, 0x00, 0x01 };
	transaction.response = { 0x08, 0x01, 0x20, 0x03,
			0x00, 0x00, 0x00, 0x01,
			0xfa, 0xff, 0x40, 0x20, 0x90, 0x70, 0x80, 0x80 };
	return transaction;
}

} // namespace sh4_lua_test

#endif // USE_LUA

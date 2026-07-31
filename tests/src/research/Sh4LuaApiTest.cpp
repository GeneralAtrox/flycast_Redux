#include "lua/lua.h"

#ifdef USE_LUA

#include "cfg/option.h"
#include "emulator.h"
#include "research/sh4_observation.h"
#include "stdclass.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace
{

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

void writeText(const std::filesystem::path& path, const std::string& text)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(output.is_open());
	output << text;
	ASSERT_TRUE(output.good());
}

std::string readText(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
}

std::string luaPath(const std::filesystem::path& path)
{
	return path.generic_u8string();
}

research::Sh4Observation memoryWrite(std::uint32_t address,
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

TEST(ResearchSh4LuaApi, DeliversTypedDiscoveryTableAndReportsOverflow)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto resultPath = directory.file("result.txt");
	std::ostringstream script;
	script << "local token\n"
			<< "token = flycast.research.subscribe({"
			<< "event='memory-write', backend='interpreter', "
			<< "start_address=0x2000, end_address=0x2007, queue_capacity=2"
			<< "}, function(event)\n"
			<< "  assert(event.discovery == true)\n"
			<< "  assert(event.authoritative_evidence == false)\n"
			<< "  assert(event.event == 'memory-write')\n"
			<< "  assert(event.backend == 'interpreter')\n"
			<< "  assert(type(event.ordinal_decimal) == 'string')\n"
			<< "  assert(event.tick == 0x12345678)\n"
			<< "  assert(event.pc == 0x8c010000 and event.opcode == 0x2102)\n"
			<< "  assert(event.delay_slot_depth == 1)\n"
			<< "  assert(event.address == 0x2000 and event.width == 8)\n"
			<< "  assert(event.value_hex == '0xffffffffffffffff')\n"
			<< "  local stats = flycast.research.subscription_stats(token)\n"
			<< "  assert(stats.discovery and stats.queued == 1 and stats.dropped == 1)\n"
			<< "  local output = assert(io.open([[" << luaPath(resultPath)
			<< "]], 'wb'))\n"
			<< "  output:write(event.ordinal_decimal)\n"
			<< "  output:close()\n"
			<< "  assert(flycast.research.unsubscribe(token))\n"
			<< "end)\n";
	writeText(directory.file("init.lua"), script.str());
	lua::init();

	// The range is inclusive. One non-match is filtered before the bounded
	// queue; the third match is dropped, preserving the first two as a prefix.
	research::publishSh4Observation(memoryWrite(0x2008, 1));
	research::publishSh4Observation(memoryWrite(0x2000,
			0xffffffffffffffffull));
	research::publishSh4Observation(memoryWrite(0x2000, 2));
	research::publishSh4Observation(memoryWrite(0x2000, 3));
	lua::overlay();

	ASSERT_TRUE(std::filesystem::exists(resultPath));
	EXPECT_FALSE(readText(resultPath).empty());
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
}

TEST(ResearchSh4LuaApi, ReloadAndTerminateRemoveOldQueuedCallbacks)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto oldPath = directory.file("old.txt");
	const auto newPath = directory.file("new.txt");
	const auto latePath = directory.file("late.txt");
	writeText(directory.file("init.lua"),
			"flycast.research.subscribe({event='instruction'}, function() "
			"local f=assert(io.open([[" + luaPath(oldPath)
			+ "]], 'wb')); f:write('old'); f:close() end)\n");
	writeText(directory.file("reload.lua"),
			"flycast.research.subscribe({event='instruction'}, function() "
			"local f=assert(io.open([[" + luaPath(newPath)
			+ "]], 'wb')); f:write('new'); f:close() end)\n");
	lua::init();

	research::Sh4Observation observation;
	observation.backend = research::Sh4ObservationBackend::Interpreter;
	observation.type = research::Sh4ObservationType::InstructionEnd;
	observation.instructionPc = 0x8c010000;
	observation.opcode = 0x0009;
	research::publishSh4Observation(observation);
	lua::exec("reload.lua");
	research::publishSh4Observation(observation);
	lua::overlay();
	EXPECT_FALSE(std::filesystem::exists(oldPath));
	EXPECT_EQ("new", readText(newPath));

	writeText(directory.file("late.lua"),
			"flycast.research.subscribe({event='instruction'}, function() "
			"local f=assert(io.open([[" + luaPath(latePath)
			+ "]], 'wb')); f:write('late'); f:close() end)\n");
	lua::exec("late.lua");
	research::publishSh4Observation(observation);
	EventManager::event(Event::Terminate);
	lua::overlay();
	EXPECT_FALSE(std::filesystem::exists(latePath));
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
}

} // namespace

#endif // USE_LUA

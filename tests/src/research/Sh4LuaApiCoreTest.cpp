#include "lua/lua.h"

#ifdef USE_LUA

#include "Sh4LuaApiSupport.h"

#include "emulator.h"
#include "research/maple_observation.h"
#include "research/sh4_observation.h"
#include "stdclass.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

namespace
{

using namespace sh4_lua_test;

TEST(ResearchSh4LuaApi, CurrentSh4TickFailsCleanlyBeforeSchedulerInitialization)
{
	DetachedSchedulerGuard detached;
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto resultPath = directory.file("tick.txt");
	std::ostringstream script;
	script << "local ok, message = pcall(function() "
			<< "return flycast.research.current_sh4_tick_decimal() end)\n"
			<< "assert(not ok and message:match('scheduler is not initialized'))\n"
			<< "assert(not pcall(function() "
			<< "flycast.research.current_sh4_tick_decimal(1) end))\n"
			<< "local output = assert(io.open([[" << luaPath(resultPath)
			<< "]], 'wb'))\n"
			<< "output:write('clean-error')\n"
			<< "output:close()\n";
	writeText(directory.file("init.lua"), script.str());
	lua::init();

	ASSERT_TRUE(std::filesystem::exists(resultPath));
	EXPECT_EQ("clean-error", readText(resultPath));
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
			+ "]], 'wb')); f:write('late'); f:close() end)\n"
			"flycast.research.subscribe({event='maple-response'}, function() "
			"local f=assert(io.open([[" + luaPath(latePath)
			+ "]], 'ab')); f:write('maple'); f:close() end)\n");
	lua::exec("late.lua");
	research::publishSh4Observation(observation);
	research::publishMapleTransactionObservations(
			research::beginMapleObservationDma(), mapleTransaction());
	EventManager::event(Event::Terminate);
	lua::overlay();
	EXPECT_FALSE(std::filesystem::exists(latePath));
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
	EXPECT_EQ(0u, research::mapleObservationSubscriberCount());
}

TEST(ResearchSh4LuaApi, ExposesInstructionLifecycleEventsWithNativePcFilters)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto resultPath = directory.file("lifecycle.txt");
	std::ostringstream script;
	script << "local output_path = [[" << luaPath(resultPath) << "]]\n"
			<< "local function append(value)\n"
			<< "  local f=assert(io.open(output_path, 'ab')); f:write(value); f:close()\n"
			<< "end\n"
			<< "local begin_token\n"
			<< "begin_token=flycast.research.subscribe({event='instruction-begin', "
			<< "start_pc=0x8c010100, end_pc=0x8c0101ff}, function(event)\n"
			<< "  assert(event.event == 'instruction-begin' and event.pc == 0x8c010100)\n"
			<< "  append('begin\\n'); assert(flycast.research.unsubscribe(begin_token))\n"
			<< "end)\n"
			<< "local abort_token\n"
			<< "abort_token=flycast.research.subscribe({event='instruction-abort', "
			<< "start_pc=0x8c010200, end_pc=0x8c0102ff}, function(event)\n"
			<< "  assert(event.event == 'instruction-abort' and event.pc == 0x8c010200)\n"
			<< "  append('abort\\n'); assert(flycast.research.unsubscribe(abort_token))\n"
			<< "end)\n";
	writeText(directory.file("init.lua"), script.str());
	lua::init();

	research::Sh4Observation outside;
	outside.type = research::Sh4ObservationType::InstructionBegin;
	outside.instructionPc = 0x8c010000;
	research::publishSh4Observation(outside);
	research::Sh4Observation begin = outside;
	begin.instructionPc = 0x8c010100;
	research::publishSh4Observation(begin);
	research::Sh4Observation abort = outside;
	abort.type = research::Sh4ObservationType::InstructionAbort;
	abort.instructionPc = 0x8c010200;
	research::publishSh4Observation(abort);
	lua::overlay();

	EXPECT_EQ("begin\nabort\n", readText(resultPath));
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
}

} // namespace

#endif // USE_LUA

#include "lua/lua.h"

#ifdef USE_LUA

#include "Sh4LuaApiSupport.h"

#include "emulator.h"
#include "research/sh4_observation.h"
#include "stdclass.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

namespace
{

using namespace sh4_lua_test;

TEST(ResearchSh4LuaApi, MemoryWatcherWritesFilteredProvenanceAndCompleteSummary)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto outputPath = directory.file("memory-watch.jsonl");
	std::ostringstream script;
	script << "flycast_memory_watch={output=[[" << luaPath(outputPath)
			<< "]], start_address=0x2000, length=8, access='both', "
			<< "backend='interpreter', start_pc=0x8c010000, end_pc=0x8c010000, "
			<< "queue_capacity=4, max_events=10, snapshots=false}\n"
			<< "dofile([[" << luaPath(memoryWatcherPath()) << "]])\n";
	writeText(directory.file("init.lua"), script.str());
	ASSERT_NO_THROW(lua::init());
	EXPECT_EQ(2u, research::sh4ObservationSubscriberCount());

	EventManager::event(Event::Start);
	research::publishSh4Observation(memoryWrite(0x2008, 1));
	research::Sh4Observation outsidePc = memoryWrite(0x2000, 2);
	outsidePc.instructionPc = 0x8c010002;
	research::publishSh4Observation(outsidePc);
	research::publishSh4Observation(memoryWrite(0x2000, 0xab));
	research::publishSh4Observation(memoryRead(0x2007, 0xcd));
	lua::overlay();
	EventManager::event(Event::LoadState);
	EventManager::event(Event::Terminate);

	const std::vector<json> lines = readJsonLines(outputPath);
	ASSERT_EQ(6u, lines.size());
	EXPECT_EQ("session-start", lines[0].at("record_type"));
	EXPECT_EQ("flycast-memory-watch-jsonl", lines[0].at("schema"));
	EXPECT_EQ("0x00002000", lines[0].at("start_address_hex"));
	EXPECT_EQ("0x00002007", lines[0].at("end_address_hex"));
	EXPECT_EQ("start", lines[1].at("event"));
	EXPECT_EQ("access", lines[2].at("record_type"));
	EXPECT_EQ("memory-write", lines[2].at("event"));
	EXPECT_EQ("0x8c010000", lines[2].at("pc_hex"));
	EXPECT_EQ("0x00002000", lines[2].at("address_hex"));
	EXPECT_EQ(8, lines[2].at("width_bytes"));
	EXPECT_EQ("0x00000000000000ab", lines[2].at("value_hex"));
	EXPECT_EQ("memory-read", lines[3].at("event"));
	EXPECT_EQ("0x6014", lines[3].at("opcode_hex"));
	EXPECT_EQ("load-state", lines[4].at("event"));
	EXPECT_EQ("summary", lines[5].at("record_type"));
	EXPECT_EQ(true, lines[5].at("complete"));
	EXPECT_EQ(2, lines[5].at("accesses"));
	EXPECT_EQ(2, lines[5].at("delivered"));
	EXPECT_EQ(1, lines[5].at("reads"));
	EXPECT_EQ(1, lines[5].at("writes"));
	EXPECT_EQ(0, lines[5].at("dropped"));
	EXPECT_EQ(0, lines[5].at("snapshot_errors"));
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
}

TEST(ResearchSh4LuaApi, MemoryWatcherReportsQueueOverflowAsIncomplete)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto outputPath = directory.file("memory-watch-overflow.jsonl");
	std::ostringstream script;
	script << "flycast_memory_watch={output=[[" << luaPath(outputPath)
			<< "]], start_address=0x2000, length=1, access='write', "
			<< "backend='interpreter', queue_capacity=1, max_events=10, snapshots=false}\n"
			<< "dofile([[" << luaPath(memoryWatcherPath()) << "]])\n";
	writeText(directory.file("init.lua"), script.str());
	ASSERT_NO_THROW(lua::init());

	research::publishSh4Observation(memoryWrite(0x2000, 1));
	research::publishSh4Observation(memoryWrite(0x2000, 2));
	lua::overlay();
	EventManager::event(Event::Terminate);

	const std::vector<json> lines = readJsonLines(outputPath);
	ASSERT_EQ(3u, lines.size());
	EXPECT_EQ("access", lines[1].at("record_type"));
	EXPECT_EQ("summary", lines[2].at("record_type"));
	EXPECT_EQ(false, lines[2].at("complete"));
	EXPECT_EQ(1, lines[2].at("accesses"));
	EXPECT_EQ(1, lines[2].at("dropped"));
	EXPECT_EQ(0, lines[2].at("queued"));
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
}

} // namespace

#endif // USE_LUA

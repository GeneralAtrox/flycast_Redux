#include "lua/lua.h"

#ifdef USE_LUA

#include "cfg/option.h"
#include "emulator.h"
#include "json.hpp"
#include "research/maple_observation.h"
#include "research/sh4_observation.h"
#include "stdclass.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
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

std::filesystem::path mapleWatcherPath()
{
	return std::filesystem::path(FLYCAST_TEST_FILES).parent_path().parent_path()
			/ "tools" / "research" / "lua" / "maple-watch.lua";
}

std::filesystem::path mapleDecoderPath()
{
	return mapleWatcherPath().parent_path() / "maple-decode.lua";
}

std::filesystem::path memoryWatcherPath()
{
	return mapleWatcherPath().parent_path() / "memory-watch.lua";
}

std::filesystem::path causalSliceWatcherPath()
{
	return mapleWatcherPath().parent_path() / "causal-slice-watch.lua";
}

std::vector<json> readJsonLines(const std::filesystem::path& path)
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

research::Sh4Observation memoryRead(std::uint32_t address,
		std::uint64_t value)
{
	research::Sh4Observation observation = memoryWrite(address, value);
	observation.type = research::Sh4ObservationType::MemoryRead;
	observation.opcode = 0x6014;
	return observation;
}

research::Sh4Observation instructionObservation(research::Sh4ObservationType type,
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

research::Sh4Observation callObservation(std::uint32_t pc,
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

research::Sh4Observation returnObservation(std::uint32_t pc,
		std::uint32_t targetPc, std::uint64_t tick)
{
	research::Sh4Observation observation = instructionObservation(
			research::Sh4ObservationType::Return, pc, 0x000b, tick, 0x2800);
	observation.targetPc = targetPc;
	observation.returnPc = targetPc;
	observation.delaySlotPc = pc + 2;
	return observation;
}

research::MapleTransactionEvent mapleTransaction(std::uint8_t bus = 1,
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

research::MapleTransactionEvent controllerConditionTransaction()
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

TEST(ResearchSh4LuaApi, CurrentSh4TickFailsCleanlyBeforeSchedulerInitialization)
{
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

TEST(ResearchSh4LuaApi, CausalSliceWatcherJoinsRegistersAccessAndObservedCall)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto outputPath = directory.file("causal-slice.jsonl");
	std::ostringstream script;
	script << "flycast_causal_slice_watch={output=[[" << luaPath(outputPath)
			<< "]], start_address=0x2000, length=4, access='write', "
			<< "backend='interpreter', producer_start_pc=0x8c010100, "
			<< "producer_end_pc=0x8c010100, call_site_start_pc=0x8c000100, "
			<< "call_site_end_pc=0x8c000100, return_start_pc=0x8c0100fe, "
			<< "return_end_pc=0x8c0100fe, queue_capacity=16, max_slices=1}\n"
			<< "dofile([[" << luaPath(causalSliceWatcherPath()) << "]])\n";
	writeText(directory.file("init.lua"), script.str());
	ASSERT_NO_THROW(lua::init());

	for (std::uint32_t index = 0; index < 10000; ++index)
	{
		research::publishSh4Observation(callObservation(
				0x8c020000 + index * 2, 0x8c030000, index));
		research::publishSh4Observation(returnObservation(
				0x8c040000 + index * 2, 0x8c050000, index));
	}
	research::publishSh4Observation(callObservation(0x8c000100, 0x8c010100, 10));
	research::publishSh4Observation(instructionObservation(
			research::Sh4ObservationType::InstructionBegin,
			0x8c010100, 0x2102, 20, 0x3000));
	research::Sh4Observation access = memoryWrite(0x2000, 0x11223344);
	access.instructionPc = 0x8c010100;
	access.tick = 20;
	access.delaySlotDepth = 0;
	access.memoryWidth = 4;
	research::publishSh4Observation(access);
	research::Sh4Observation end = instructionObservation(
			research::Sh4ObservationType::InstructionEnd,
			0x8c010100, 0x2102, 24, 0x4000);
	end.nextPc = 0x8c010102;
	research::publishSh4Observation(end);
	lua::overlay();

	const std::vector<json> lines = readJsonLines(outputPath);
	ASSERT_EQ(3u, lines.size()) << readText(outputPath);
	SCOPED_TRACE(readText(outputPath));
	EXPECT_EQ("session-start", lines[0].at("record_type"));
	const json& slice = lines[1];
	EXPECT_EQ("causal-slice", slice.at("record_type"));
	EXPECT_EQ("completed", slice.at("outcome"));
	EXPECT_EQ("0x8c010100", slice.at("pc_hex"));
	EXPECT_EQ("0x0000000011223344",
			slice.at("accesses").at(0).at("value_hex"));
	EXPECT_EQ(0x3000, slice.at("registers_before").at("r").at(0));
	EXPECT_EQ(0x4000, slice.at("registers_after").at("r").at(0));
	EXPECT_EQ("0x8c010102", slice.at("next_pc_hex"));
	EXPECT_EQ(false, slice.at("call_path").at("root_complete"));
	EXPECT_EQ("selected-call-sites", slice.at("call_path").at("scope"));
	ASSERT_EQ(1u, slice.at("call_path").at("frames").size());
	EXPECT_EQ("0x8c000100",
			slice.at("call_path").at("frames").at(0).at("call_pc_hex"));
	EXPECT_EQ("0x8c010100",
			slice.at("call_path").at("frames").at(0).at("target_pc_hex"));
	EXPECT_EQ(true, lines[2].at("complete"));
	EXPECT_EQ(0, lines[2].at("dropped"));
	EXPECT_EQ("0x8c000100", lines[0].at("call_site_start_pc_hex"));
	EXPECT_EQ("0x8c0100fe", lines[0].at("return_start_pc_hex"));
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
}

TEST(ResearchSh4LuaApi, CausalSliceWatcherDoesNotCommitAbortedCall)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto outputPath = directory.file("causal-slice-call-abort.jsonl");
	std::ostringstream script;
	script << "flycast_causal_slice_watch={output=[[" << luaPath(outputPath)
			<< "]], start_address=0x2000, length=4, access='write', "
			<< "backend='interpreter', producer_start_pc=0x8c010100, "
			<< "producer_end_pc=0x8c010100, call_site_start_pc=0x8c000100, "
			<< "call_site_end_pc=0x8c000100, return_start_pc=0x8c0100fe, "
			<< "return_end_pc=0x8c0100fe, queue_capacity=16, max_slices=1}\n"
			<< "dofile([[" << luaPath(causalSliceWatcherPath()) << "]])\n";
	writeText(directory.file("init.lua"), script.str());
	ASSERT_NO_THROW(lua::init());

	const research::Sh4Observation call = callObservation(
			0x8c000100, 0x8c010100, 10);
	research::publishSh4Observation(call);
	research::Sh4Observation callAbort;
	callAbort.backend = research::Sh4ObservationBackend::Interpreter;
	callAbort.type = research::Sh4ObservationType::InstructionAbort;
	callAbort.tick = call.tick;
	callAbort.instructionPc = call.instructionPc;
	callAbort.opcode = call.opcode;
	research::publishSh4Observation(callAbort);
	research::publishSh4Observation(instructionObservation(
			research::Sh4ObservationType::InstructionBegin,
			0x8c010100, 0x2102, 20, 0x3000));
	research::Sh4Observation access = memoryWrite(0x2000, 1);
	access.instructionPc = 0x8c010100;
	access.tick = 20;
	access.delaySlotDepth = 0;
	research::publishSh4Observation(access);
	research::publishSh4Observation(instructionObservation(
			research::Sh4ObservationType::InstructionEnd,
			0x8c010100, 0x2102, 24, 0x4000));
	lua::overlay();

	const std::vector<json> lines = readJsonLines(outputPath);
	ASSERT_EQ(3u, lines.size());
	EXPECT_TRUE(lines[1].at("call_path").at("frames").empty());
	EXPECT_EQ(0, lines[2].at("committed_calls"));
	EXPECT_EQ(true, lines[2].at("complete"));
}

TEST(ResearchSh4LuaApi, CausalSliceWatcherRetainsExceptionAndAbortOutcome)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto outputPath = directory.file("causal-slice-abort.jsonl");
	std::ostringstream script;
	script << "flycast_causal_slice_watch={output=[[" << luaPath(outputPath)
			<< "]], start_address=0x2000, length=4, access='write', "
			<< "backend='interpreter', producer_start_pc=0x8c010100, "
			<< "producer_end_pc=0x8c010100, call_site_start_pc=0x8c000100, "
			<< "call_site_end_pc=0x8c000100, return_start_pc=0x8c0100fe, "
			<< "return_end_pc=0x8c0100fe, queue_capacity=16, max_slices=1}\n"
			<< "dofile([[" << luaPath(causalSliceWatcherPath()) << "]])\n";
	writeText(directory.file("init.lua"), script.str());
	ASSERT_NO_THROW(lua::init());

	research::publishSh4Observation(instructionObservation(
			research::Sh4ObservationType::InstructionBegin,
			0x8c010100, 0x2102, 20, 0x3000));
	research::Sh4Observation access = memoryWrite(0x2000, 0x55);
	access.instructionPc = 0x8c010100;
	access.tick = 20;
	access.delaySlotDepth = 0;
	research::publishSh4Observation(access);
	research::Sh4Observation exception = instructionObservation(
			research::Sh4ObservationType::Exception,
			0x8c010100, 0x2102, 20, 0x3500);
	exception.exceptionPc = 0x8c010100;
	exception.vectorPc = 0x8c000100;
	exception.exceptionCode = 0x0e0;
	research::publishSh4Observation(exception);
	research::Sh4Observation abort;
	abort.backend = research::Sh4ObservationBackend::Interpreter;
	abort.type = research::Sh4ObservationType::InstructionAbort;
	abort.tick = 20;
	abort.instructionPc = 0x8c010100;
	abort.opcode = 0x2102;
	research::publishSh4Observation(abort);
	lua::overlay();

	const std::vector<json> lines = readJsonLines(outputPath);
	ASSERT_EQ(3u, lines.size());
	EXPECT_EQ("aborted", lines[1].at("outcome"));
	EXPECT_TRUE(lines[1].at("registers_after").is_null());
	EXPECT_EQ("0x000000e0", lines[1].at("exception").at("exception_code_hex"));
	EXPECT_EQ(1, lines[2].at("aborted_slices"));
	EXPECT_EQ(true, lines[2].at("complete"));
}

TEST(ResearchSh4LuaApi, CausalSliceWatcherReportsOverflowAsIncomplete)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto outputPath = directory.file("causal-slice-overflow.jsonl");
	std::ostringstream script;
	script << "flycast_causal_slice_watch={output=[[" << luaPath(outputPath)
			<< "]], start_address=0x2000, length=4, access='write', "
			<< "backend='interpreter', producer_start_pc=0x8c010100, "
			<< "producer_end_pc=0x8c010100, call_site_start_pc=0x8c000100, "
			<< "call_site_end_pc=0x8c000100, return_start_pc=0x8c0100fe, "
			<< "return_end_pc=0x8c0100fe, queue_capacity=1, max_slices=1}\n"
			<< "dofile([[" << luaPath(causalSliceWatcherPath()) << "]])\n";
	writeText(directory.file("init.lua"), script.str());
	ASSERT_NO_THROW(lua::init());

	research::publishSh4Observation(instructionObservation(
			research::Sh4ObservationType::InstructionBegin,
			0x8c010100, 0x2102, 20, 0x3000));
	for (std::uint64_t value : {1u, 2u})
	{
		research::Sh4Observation access = memoryWrite(0x2000, value);
		access.instructionPc = 0x8c010100;
		access.tick = 20;
		access.delaySlotDepth = 0;
		research::publishSh4Observation(access);
	}
	research::publishSh4Observation(instructionObservation(
			research::Sh4ObservationType::InstructionEnd,
			0x8c010100, 0x2102, 24, 0x4000));
	lua::overlay();

	const std::vector<json> lines = readJsonLines(outputPath);
	ASSERT_EQ(3u, lines.size());
	ASSERT_EQ(1u, lines[1].at("accesses").size());
	EXPECT_EQ(false, lines[2].at("complete"));
	EXPECT_EQ(1, lines[2].at("dropped"));
}

TEST(ResearchSh4LuaApi, CausalSliceWatcherArmsAfterRequestedStateLoad)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto outputPath = directory.file("causal-slice-state.jsonl");
	std::ostringstream script;
	script << "flycast_causal_slice_watch={output=[[" << luaPath(outputPath)
			<< "]], start_address=0x2000, length=4, access='write', "
			<< "backend='interpreter', producer_start_pc=0x8c010100, "
			<< "producer_end_pc=0x8c010100, call_site_start_pc=0x8c000100, "
			<< "call_site_end_pc=0x8c000100, return_start_pc=0x8c0100fe, "
			<< "return_end_pc=0x8c0100fe, queue_capacity=16, max_slices=1, "
			<< "wait_for_load_state=true}\n"
			<< "dofile([[" << luaPath(causalSliceWatcherPath()) << "]])\n";
	writeText(directory.file("init.lua"), script.str());
	ASSERT_NO_THROW(lua::init());
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());

	EventManager::event(Event::LoadState);
	EXPECT_EQ(7u, research::sh4ObservationSubscriberCount());
	research::publishSh4Observation(instructionObservation(
			research::Sh4ObservationType::InstructionBegin,
			0x8c010100, 0x2102, 20, 0x3000));
	research::Sh4Observation access = memoryWrite(0x2000, 7);
	access.instructionPc = 0x8c010100;
	access.tick = 20;
	access.delaySlotDepth = 0;
	research::publishSh4Observation(access);
	research::publishSh4Observation(instructionObservation(
			research::Sh4ObservationType::InstructionEnd,
			0x8c010100, 0x2102, 24, 0x4000));
	lua::overlay();

	const std::vector<json> lines = readJsonLines(outputPath);
	ASSERT_EQ(4u, lines.size());
	EXPECT_EQ("load-state", lines[1].at("event"));
	EXPECT_EQ("causal-slice", lines[2].at("record_type"));
	EXPECT_EQ(true, lines[3].at("complete"));
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
}

TEST(ResearchSh4LuaApi, CausalSliceWatcherMarksManualTerminationIncomplete)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto outputPath = directory.file("causal-slice-terminate.jsonl");
	std::ostringstream script;
	script << "flycast_causal_slice_watch={output=[[" << luaPath(outputPath)
			<< "]], start_address=0x2000, length=4, access='write', "
			<< "backend='interpreter', producer_start_pc=0x8c010100, "
			<< "producer_end_pc=0x8c010100, call_site_start_pc=0x8c000100, "
			<< "call_site_end_pc=0x8c000100, return_start_pc=0x8c0100fe, "
			<< "return_end_pc=0x8c0100fe, queue_capacity=16, max_slices=10}\n"
			<< "dofile([[" << luaPath(causalSliceWatcherPath()) << "]])\n";
	writeText(directory.file("init.lua"), script.str());
	ASSERT_NO_THROW(lua::init());
	EXPECT_EQ(7u, research::sh4ObservationSubscriberCount());

	EventManager::event(Event::Terminate);
	const std::vector<json> lines = readJsonLines(outputPath);
	ASSERT_EQ(2u, lines.size());
	EXPECT_EQ("summary", lines[1].at("record_type"));
	EXPECT_EQ("terminate", lines[1].at("reason"));
	EXPECT_EQ(false, lines[1].at("complete"));
	EXPECT_EQ(true, lines[1].at("manual_termination_delivery_unknown"));
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
}

TEST(ResearchSh4LuaApi, DeliversFilteredMapleRequestAndResponse)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto resultPath = directory.file("maple.txt");
	std::ostringstream script;
	script << "local output_path = [[" << luaPath(resultPath) << "]]\n"
			<< "local function append(value)\n"
			<< "  local f=assert(io.open(output_path, 'ab')); f:write(value); f:close()\n"
			<< "end\n"
			<< "local transaction\n"
			<< "local request_token\n"
			<< "request_token=flycast.research.subscribe({event='maple-request', "
			<< "bus=1, port=2, command=9}, function(event)\n"
			<< "  assert(event.discovery and not event.authoritative_evidence)\n"
			<< "  assert(event.event == 'maple-request' and event.bus == 1 "
			<< "and event.port == 2 and event.command == 9)\n"
			<< "  assert(event.byte_count == 4 and #event.payload == 4 "
			<< "and event.payload_hex == '09200100' and string.byte(event.payload, 1) == 9)\n"
			<< "  assert(event.device_present and event.device_type == 1)\n"
			<< "  transaction=event.transaction_ordinal_decimal; append('request\\n')\n"
			<< "  assert(flycast.research.unsubscribe(request_token))\n"
			<< "end)\n"
			<< "local response_token\n"
			<< "response_token=flycast.research.subscribe({event='maple-response', "
			<< "bus=1, port=2, command=9}, function(event)\n"
			<< "  assert(event.event == 'maple-response' and event.response_code == 8)\n"
			<< "  assert(event.payload_hex == '08012000' "
			<< "and event.transaction_ordinal_decimal == transaction)\n"
			<< "  local stats=assert(flycast.research.subscription_stats(response_token))\n"
			<< "  assert(stats.discovery and stats.queued == 0 and stats.dropped == 0)\n"
			<< "  append('response\\n'); assert(flycast.research.unsubscribe(response_token))\n"
			<< "end)\n";
	writeText(directory.file("init.lua"), script.str());
	lua::init();

	research::publishMapleTransactionObservations(
			research::beginMapleObservationDma(), mapleTransaction(0, 2, 9));
	research::publishMapleTransactionObservations(
			research::beginMapleObservationDma(), mapleTransaction());
	lua::overlay();

	EXPECT_EQ("request\nresponse\n", readText(resultPath));
	EXPECT_EQ(0u, research::mapleObservationSubscriberCount());
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
}

TEST(ResearchSh4LuaApi, RejectsInvalidMapleFilters)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	writeText(directory.file("init.lua"),
			"local function rejected(filter)\n"
			"  local ok=pcall(function() flycast.research.subscribe(filter, function() end) end)\n"
			"  assert(not ok)\n"
			"end\n"
			"rejected({event='maple-request', bus=4})\n"
			"rejected({event='maple-request', port=6})\n"
			"rejected({event='maple-response', command=256})\n"
			"rejected({event='maple-response', backend='interpreter'})\n"
			"rejected({event='instruction', bus=0})\n"
			"rejected({event='maple-response', queue_capacity=0})\n");
	EXPECT_NO_THROW(lua::init());
	EXPECT_EQ(0u, research::mapleObservationSubscriberCount());
}

TEST(ResearchSh4LuaApi, MapleWatcherWritesPairedCompleteJsonl)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto outputPath = directory.file("maple-watch.jsonl");
	std::ostringstream script;
	script << "flycast_maple_watch={output=[[" << luaPath(outputPath)
			<< "]], queue_capacity=4, bus=1, port=2, command=9}\n"
			<< "dofile([[" << luaPath(mapleWatcherPath()) << "]])\n";
	writeText(directory.file("init.lua"), script.str());
	ASSERT_NO_THROW(lua::init());

	EventManager::event(Event::Start);
	research::publishMapleTransactionObservations(
			research::beginMapleObservationDma(), controllerConditionTransaction());
	lua::overlay();
	EventManager::event(Event::LoadState);
	EventManager::event(Event::Terminate);

	const std::vector<json> lines = readJsonLines(outputPath);
	ASSERT_EQ(5u, lines.size());
	EXPECT_EQ("session-start", lines[0].at("record_type"));
	EXPECT_EQ(true, lines[0].at("discovery"));
	EXPECT_EQ(false, lines[0].at("authoritative_evidence"));
	EXPECT_EQ("start", lines[1].at("event"));
	EXPECT_EQ("transaction", lines[2].at("record_type"));
	EXPECT_EQ(true, lines[2].at("consistent"));
	EXPECT_EQ("0920010100000001", lines[2].at("request").at("payload_hex"));
	EXPECT_EQ("0801200300000001faff402090708080",
			lines[2].at("response").at("payload_hex"));
	EXPECT_EQ(lines[2].at("transaction_ordinal"),
			lines[2].at("response").at("transaction_ordinal"));
	EXPECT_EQ("decoded", lines[2].at("decoded").at("status"));
	EXPECT_EQ("get-condition", lines[2].at("decoded").at("operation"));
	EXPECT_EQ(json({ "c", "a" }), lines[2].at("decoded").at("response_data")
			.at("pressed_buttons"));
	EXPECT_EQ(64, lines[2].at("decoded").at("response_data").at("right_trigger"));
	EXPECT_EQ(32, lines[2].at("decoded").at("response_data").at("left_trigger"));
	EXPECT_EQ(16, lines[2].at("decoded").at("response_data").at("joystick_x_offset"));
	EXPECT_EQ(-16, lines[2].at("decoded").at("response_data").at("joystick_y_offset"));
	EXPECT_EQ("load-state", lines[3].at("event"));
	EXPECT_EQ("summary", lines[4].at("record_type"));
	EXPECT_EQ(true, lines[4].at("complete"));
	EXPECT_EQ(1, lines[4].at("paired"));
	EXPECT_EQ(0, lines[4].at("request_dropped"));
	EXPECT_EQ(0, lines[4].at("response_dropped"));
	EXPECT_EQ(0u, research::mapleObservationSubscriberCount());
}

TEST(ResearchSh4LuaApi, MapleDecoderHandlesCapturedVmuAndFailClosedFrames)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	std::ostringstream script;
	script << "local decoder=assert(dofile([[" << luaPath(mapleDecoderPath())
			<< "]]))\n"
			<< "local function from_hex(hex) return (hex:gsub('..', function(byte) "
			<< "return string.char(tonumber(byte, 16)) end)) end\n"
			<< "local function event(hex, command, response_code, present)\n"
			<< "  local payload=from_hex(hex)\n"
			<< "  return {payload=payload, byte_count=#payload, command=command, "
			<< "response_code=response_code, device_present=present}\n"
			<< "end\n"
			// Exact VMU device-status response observed during the bounded Lodoss boot.
			<< "local status_hex='0500011c0000000e7e7e3f4000051000000f4100ff00'"
			<< "..'56697375616c204d656d6f7279202020202020202020202020202020202020'"
			<< "..'50726f6475636564204279206f7220556e646572204c6963656e7365204672'"
			<< "..'6f6d205345474120454e5445525052495345532c4c54442e20202020207c008200'\n"
			<< "local identity=decoder.decode(event('01010000', 1, nil, true), "
			<< "event(status_hex, 1, 5, true))\n"
			<< "assert(identity.status=='decoded')\n"
			<< "assert(identity.response_data.device_class=='vmu')\n"
			<< "assert(identity.response_data.product_name=='Visual Memory')\n"
			<< "assert(identity.response_data.functions.mask_hex=='0x0e000000')\n"
			// Exact request shape observed in Lodoss: LCD function, zero parameter,
			// and one 192-byte monochrome VMU screen image.
			<< "local write_payload=string.char(0x0c,1,0,0x32)"
			<< "..string.char(0,0,0,4)..string.char(0,0,0,0)..string.rep('\\0',192)\n"
			<< "local write=decoder.decode({payload=write_payload,byte_count=#write_payload,"
			<< "command=12,device_present=true}, event('07000100',12,7,true))\n"
			<< "assert(write.status=='decoded' and write.operation=='block-write')\n"
			<< "assert(write.request_data.functions.names[1]=='lcd')\n"
			<< "assert(write.request_data.parameter.block==0 "
			<< "and write.request_data.parameter.phase==0)\n"
			<< "assert(write.request_data.payload_byte_count==192 "
			<< "and #write.request_data.payload_hex==384)\n"
			<< "local unknown=decoder.decode(event('11200100',17,nil,true), "
			<< "event('07012000',17,7,true))\n"
			<< "assert(unknown.status=='unknown' and unknown.command.name=='unknown')\n"
			<< "local malformed=decoder.decode(event('09200101000000',9,nil,true), "
			<< "event('08012000',9,8,true))\n"
			<< "assert(malformed.status=='malformed' and #malformed.errors==1)\n"
			<< "assert(decoder.encode_json(malformed):find('word aligned',1,true))\n";
	writeText(directory.file("init.lua"), script.str());
	ASSERT_NO_THROW(lua::init());
	EXPECT_EQ(0u, research::mapleObservationSubscriberCount());
}

TEST(ResearchSh4LuaApi, MapleWatcherReportsOverflowAsIncomplete)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto outputPath = directory.file("maple-watch-overflow.jsonl");
	std::ostringstream script;
	script << "flycast_maple_watch={output=[[" << luaPath(outputPath)
			<< "]], queue_capacity=1}\n"
			<< "dofile([[" << luaPath(mapleWatcherPath()) << "]])\n";
	writeText(directory.file("init.lua"), script.str());
	ASSERT_NO_THROW(lua::init());

	research::publishMapleTransactionObservations(
			research::beginMapleObservationDma(), mapleTransaction());
	research::publishMapleTransactionObservations(
			research::beginMapleObservationDma(), mapleTransaction());
	lua::overlay();
	EventManager::event(Event::Terminate);

	const std::vector<json> lines = readJsonLines(outputPath);
	ASSERT_EQ(3u, lines.size());
	EXPECT_EQ("transaction", lines[1].at("record_type"));
	EXPECT_EQ("summary", lines[2].at("record_type"));
	EXPECT_EQ(false, lines[2].at("complete"));
	EXPECT_EQ(1, lines[2].at("paired"));
	EXPECT_EQ(1, lines[2].at("request_dropped"));
	EXPECT_EQ(1, lines[2].at("response_dropped"));
	EXPECT_EQ(0, lines[2].at("unmatched_requests"));
	EXPECT_EQ(0, lines[2].at("unmatched_responses"));
	EXPECT_EQ(0u, research::mapleObservationSubscriberCount());
}

TEST(ResearchSh4LuaApi, MapleWatcherReloadAndResetOwnExactlyTwoSubscriptions)
{
	LuaTestDirectory directory;
	LuaRuntimeGuard runtime;
	set_user_config_dir(directory.directory().u8string());
	config::LuaFileName.set("init.lua");
	const auto outputPath = directory.file("maple-watch-lifecycle.jsonl");
	std::ostringstream script;
	script << "flycast_maple_watch={output=[[" << luaPath(outputPath)
			<< "]], queue_capacity=4}\n"
			<< "dofile([[" << luaPath(mapleWatcherPath()) << "]])\n";
	writeText(directory.file("init.lua"), script.str());
	writeText(directory.file("reload.lua"), script.str());
	ASSERT_NO_THROW(lua::init());
	EXPECT_EQ(2u, research::mapleObservationSubscriberCount());

	lua::exec("reload.lua");
	EXPECT_EQ(2u, research::mapleObservationSubscriberCount());
	EventManager::event(Event::Terminate);
	EXPECT_EQ(0u, research::mapleObservationSubscriberCount());
	EventManager::event(Event::Start);
	EXPECT_EQ(2u, research::mapleObservationSubscriberCount());
	research::publishMapleTransactionObservations(
			research::beginMapleObservationDma(), mapleTransaction());
	lua::overlay();
	EventManager::event(Event::Terminate);
	EXPECT_EQ(0u, research::mapleObservationSubscriberCount());

	const std::vector<json> lines = readJsonLines(outputPath);
	ASSERT_EQ(8u, lines.size());
	EXPECT_EQ("session-start", lines[0].at("record_type"));
	EXPECT_EQ("reload", lines[1].at("reason"));
	EXPECT_EQ("session-start", lines[2].at("record_type"));
	EXPECT_EQ("terminate", lines[3].at("reason"));
	EXPECT_EQ("session-start", lines[4].at("record_type"));
	EXPECT_EQ("start", lines[5].at("event"));
	EXPECT_EQ("transaction", lines[6].at("record_type"));
	EXPECT_EQ("terminate", lines[7].at("reason"));
}

} // namespace

#endif // USE_LUA

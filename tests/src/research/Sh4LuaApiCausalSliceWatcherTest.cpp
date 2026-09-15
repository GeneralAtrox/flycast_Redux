#include "lua/lua.h"

#ifdef USE_LUA

#include "Sh4LuaApiSupport.h"

#include "emulator.h"
#include "research/sh4_observation.h"
#include "stdclass.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace
{

using namespace sh4_lua_test;

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

} // namespace

#endif // USE_LUA

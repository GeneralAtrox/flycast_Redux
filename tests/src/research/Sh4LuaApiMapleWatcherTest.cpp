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

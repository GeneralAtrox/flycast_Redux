#include "research/control/control_protocol.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using research::control::ControlActions;
using research::control::ControlLimits;
using research::control::handleControlRequest;
using research::control::json;

constexpr const char *Token = "s3cret";

json call(const ControlActions& actions, const std::string& command,
		const json& args = json::object(), const std::string& token = Token,
		const ControlLimits& limits = {})
{
	json request {{"id", 7}, {"cmd", command}, {"args", args}};
	if (!token.empty())
		request["token"] = token;
	return json::parse(handleControlRequest(request.dump(), Token, actions, limits));
}

struct FakeEmulator
{
	bool paused = false;
	int savedSlot = -1;
	int loadedSlot = -1;
	std::string recordPath;
	json recordConfig;
	bool recordStopped = false;
	std::uint32_t checkpointPc = 0;
	std::uint32_t gateAddress = 0;
	std::uint32_t gateValue = 0;
	bool exited = false;

	ControlActions actions()
	{
		ControlActions a;
		a.status = [this] { return json {{"running", !paused}}; };
		a.pause = [this] { paused = true; };
		a.resume = [this] { paused = false; };
		a.saveState = [this](int slot) { savedSlot = slot; };
		a.loadState = [this](int slot) {
			if (slot == 9)
				throw std::runtime_error("slot 9 is empty");
			loadedSlot = slot;
		};
		a.readMemory = [](std::uint32_t address, std::uint32_t length) {
			std::vector<std::uint8_t> bytes(length);
			for (std::uint32_t i = 0; i < length; ++i)
				bytes[i] = static_cast<std::uint8_t>((address + i) & 0xff);
			return bytes;
		};
		a.readRegisters = [] { return json {{"pc", 0x8c010000u}}; };
		a.recordStart = [this](const std::string& path, const json& config) {
			recordPath = path;
			recordConfig = config;
		};
		a.recordStop = [this] { recordStopped = true; };
		a.recordStatus = [] { return json {{"active", false}}; };
		a.checkpointSet = [this](std::uint32_t pc, std::uint32_t gate, std::uint32_t value) {
			checkpointPc = pc;
			gateAddress = gate;
			gateValue = value;
		};
		a.checkpointClear = [this] { checkpointPc = 0; };
		a.requestExit = [this] { exited = true; };
		return a;
	}
};

TEST(ResearchControl, CapabilitiesListEveryCommandAndEchoTheId)
{
	FakeEmulator fake;
	const json response = call(fake.actions(), "capabilities");
	EXPECT_TRUE(response.at("ok"));
	EXPECT_EQ(7, response.at("id"));
	const json& commands = response.at("result").at("commands");
	EXPECT_NE(commands.end(), std::find(commands.begin(), commands.end(), "mem_read"));
	EXPECT_NE(commands.end(), std::find(commands.begin(), commands.end(), "record_start"));
	EXPECT_FALSE(response.at("result").at("memory_mutation"));
}

TEST(ResearchControl, RequiresTheTokenWhenOneIsConfigured)
{
	FakeEmulator fake;
	EXPECT_FALSE(call(fake.actions(), "status", json::object(), "").at("ok"));
	EXPECT_FALSE(call(fake.actions(), "status", json::object(), "wrong").at("ok"));
	EXPECT_TRUE(call(fake.actions(), "status").at("ok"));
	// No token configured: anything goes.
	const json open = json::parse(handleControlRequest(
			json {{"cmd", "status"}}.dump(), "", fake.actions()));
	EXPECT_TRUE(open.at("ok"));
}

TEST(ResearchControl, LifecycleCommandsReachTheActions)
{
	FakeEmulator fake;
	const ControlActions actions = fake.actions();
	EXPECT_TRUE(call(actions, "pause").at("ok"));
	EXPECT_TRUE(fake.paused);
	EXPECT_TRUE(call(actions, "resume").at("ok"));
	EXPECT_FALSE(fake.paused);
	EXPECT_TRUE(call(actions, "savestate", {{"slot", 3}}).at("ok"));
	EXPECT_EQ(3, fake.savedSlot);
	EXPECT_TRUE(call(actions, "loadstate").at("ok"));
	EXPECT_EQ(0, fake.loadedSlot);
	const json failed = call(actions, "loadstate", {{"slot", 9}});
	EXPECT_FALSE(failed.at("ok"));
	EXPECT_EQ("slot 9 is empty", failed.at("error"));
	EXPECT_FALSE(call(actions, "savestate", {{"slot", 10}}).at("ok"));
	EXPECT_TRUE(call(actions, "exit").at("ok"));
	EXPECT_TRUE(fake.exited);
}

TEST(ResearchControl, MemoryReadIsBoundedAndBase64Encoded)
{
	FakeEmulator fake;
	const ControlActions actions = fake.actions();
	const json response = call(actions, "mem_read", {{"addr", "0x8c000000"}, {"len", 3}});
	ASSERT_TRUE(response.at("ok")) << response.dump();
	EXPECT_EQ(0x8c000000u, response.at("result").at("addr"));
	EXPECT_EQ(3, response.at("result").at("len"));
	EXPECT_EQ("AAEC", response.at("result").at("base64"));
	ControlLimits limits;
	limits.maxReadBytes = 16;
	EXPECT_FALSE(call(actions, "mem_read", {{"addr", 0}, {"len", 17}}, Token, limits).at("ok"));
	EXPECT_FALSE(call(actions, "mem_read", {{"len", 4}}).at("ok"));
	EXPECT_TRUE(call(actions, "regs").at("ok"));
}

TEST(ResearchControl, RecordingAndCheckpointCommandsPassArguments)
{
	FakeEmulator fake;
	const ControlActions actions = fake.actions();
	const json config {{"buses", "sh4"}, {"sh4", {{"types", "call"}}}};
	EXPECT_TRUE(call(actions, "record_start", {{"path", "run.db"}, {"config", config}}).at("ok"));
	EXPECT_EQ("run.db", fake.recordPath);
	EXPECT_EQ(config, fake.recordConfig);
	EXPECT_FALSE(call(actions, "record_start", {{"config", config}}).at("ok"));
	EXPECT_TRUE(call(actions, "record_stop").at("ok"));
	EXPECT_TRUE(fake.recordStopped);
	EXPECT_TRUE(call(actions, "record_status").at("ok"));
	EXPECT_TRUE(call(actions, "checkpoint_set",
			{{"pc", 0x8c012344u}, {"gate_addr", 0x8c001000u}, {"gate_value", 1}}).at("ok"));
	EXPECT_EQ(0x8c012344u, fake.checkpointPc);
	EXPECT_EQ(0x8c001000u, fake.gateAddress);
	EXPECT_EQ(1u, fake.gateValue);
	EXPECT_TRUE(call(actions, "checkpoint_clear").at("ok"));
	EXPECT_EQ(0u, fake.checkpointPc);
}

TEST(ResearchControl, MalformedRequestsAreErrorsNotExceptions)
{
	FakeEmulator fake;
	const ControlActions actions = fake.actions();
	EXPECT_FALSE(json::parse(handleControlRequest("not json", Token, actions)).at("ok"));
	EXPECT_FALSE(json::parse(handleControlRequest("[1,2]", Token, actions)).at("ok"));
	EXPECT_FALSE(call(actions, "bogus").at("ok"));
	json extra {{"cmd", "status"}, {"token", Token}, {"unexpected", 1}};
	EXPECT_FALSE(json::parse(handleControlRequest(extra.dump(), Token, actions)).at("ok"));
	ControlLimits limits;
	limits.maxRequestBytes = 32;
	const json big = json::parse(handleControlRequest(std::string(64, 'x'), Token, actions, limits));
	EXPECT_FALSE(big.at("ok"));
	// A build without an action reports it instead of crashing.
	ControlActions partial;
	EXPECT_FALSE(call(partial, "pause").at("ok"));
}

} // namespace

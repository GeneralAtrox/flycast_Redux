#include "research/research_control.h"

#include "json.hpp"

#include <gtest/gtest.h>

#include <string>

namespace
{
using json = nlohmann::json;

constexpr const char *Nonce =
		"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

std::string request(const std::string& command)
{
	return json({{"schema", "flycast-research-control-request"},
			{"schema_version", 1}, {"nonce", Nonce}, {"command", command}}).dump();
}

TEST(ResearchControl, ExposesOnlyResearchToolingCommands)
{
	research::ResearchControlStatus status;
	bool cleanExit = false;
	const json response = json::parse(research::handleResearchControlRequest(
			request("capabilities"), Nonce, status, cleanExit));
	EXPECT_TRUE(response.at("ok"));
	EXPECT_EQ("reverse-engineering-tooling", response.at("scope"));
	EXPECT_FALSE(response.at("emulator_debug_control"));
	EXPECT_FALSE(response.at("memory_mutation"));
	EXPECT_EQ(json::array({"capabilities", "status", "request-clean-exit"}),
			response.at("commands"));
	EXPECT_FALSE(cleanExit);
}

TEST(ResearchControl, ReportsBoundedLifecycleStatus)
{
	research::ResearchControlStatus status;
	status.gameLoaded = true;
	status.running = false;
	status.paused = true;
	status.lifecycleGeneration = 17;
	status.gameId = "T7012D  05";
	status.mediaPath = "C:\\game.gdi";
	status.cpuBackend = "interpreter";
	bool cleanExit = false;
	const json response = json::parse(research::handleResearchControlRequest(
			request("status"), Nonce, status, cleanExit));
	EXPECT_TRUE(response.at("ok"));
	EXPECT_TRUE(response.at("game_loaded"));
	EXPECT_FALSE(response.at("running"));
	EXPECT_TRUE(response.at("paused"));
	EXPECT_EQ(17, response.at("lifecycle_generation"));
	EXPECT_EQ("T7012D  05", response.at("game_id"));
	EXPECT_EQ("interpreter", response.at("cpu_backend"));
	EXPECT_TRUE(response.at("configured_tools").is_array());
	EXPECT_TRUE(response.at("observation_buses").is_object());
	EXPECT_FALSE(cleanExit);
}

TEST(ResearchControl, CleanExitIsExplicitAndRequiresLoadedGame)
{
	research::ResearchControlStatus status;
	bool cleanExit = false;
	EXPECT_THROW(research::handleResearchControlRequest(request("request-clean-exit"),
			Nonce, status, cleanExit), std::invalid_argument);
	EXPECT_FALSE(cleanExit);
	status.gameLoaded = true;
	const json response = json::parse(research::handleResearchControlRequest(
			request("request-clean-exit"), Nonce, status, cleanExit));
	EXPECT_TRUE(response.at("accepted"));
	EXPECT_EQ("frontend-clean-exit", response.at("effect"));
	EXPECT_TRUE(cleanExit);
}

TEST(ResearchControl, RejectsUnauthenticatedMalformedAndUnknownRequests)
{
	research::ResearchControlStatus status;
	bool cleanExit = false;
	json wrongNonce = json::parse(request("status"));
	wrongNonce["nonce"] = std::string(64, 'f');
	EXPECT_THROW(research::handleResearchControlRequest(wrongNonce.dump(), Nonce,
			status, cleanExit), std::invalid_argument);
	json extra = json::parse(request("status"));
	extra["unexpected"] = true;
	EXPECT_THROW(research::handleResearchControlRequest(extra.dump(), Nonce,
			status, cleanExit), std::invalid_argument);
	EXPECT_THROW(research::handleResearchControlRequest(request("pause"), Nonce,
			status, cleanExit), std::invalid_argument);
	EXPECT_THROW(research::handleResearchControlRequest(std::string(16 * 1024 + 1, 'x'),
			Nonce, status, cleanExit), std::invalid_argument);
}

} // namespace

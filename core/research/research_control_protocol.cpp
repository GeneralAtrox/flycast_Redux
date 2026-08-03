#include "research/research_control.h"

#include "json.hpp"

#include <stdexcept>

namespace research
{
namespace
{
using json = nlohmann::json;
constexpr std::size_t MaximumRequestBytes = 16 * 1024;

json responseBase()
{
	return {{"schema", "flycast-research-control-response"},
			{"schema_version", 1}};
}
} // namespace

std::string handleResearchControlRequest(const std::string& request,
		const std::string& expectedNonce, const ResearchControlStatus& status,
		bool& requestCleanExit)
{
	requestCleanExit = false;
	if (request.empty() || request.size() > MaximumRequestBytes)
		throw std::invalid_argument("request size is outside the protocol limit");
	const json value = json::parse(request);
	if (!value.is_object() || value.size() != 4
			|| !value.contains("schema") || !value.contains("schema_version")
			|| !value.contains("nonce") || !value.contains("command")
			|| !value.at("schema").is_string()
			|| value.at("schema").get<std::string>() != "flycast-research-control-request"
			|| !value.at("schema_version").is_number_unsigned()
			|| value.at("schema_version").get<std::uint64_t>() != 1
			|| !value.at("nonce").is_string() || !value.at("command").is_string())
		throw std::invalid_argument("request contract is invalid");
	if (value.at("nonce").get<std::string>() != expectedNonce)
		throw std::invalid_argument("request nonce is invalid");
	const std::string command = value.at("command").get<std::string>();
	json response = responseBase();
	response["ok"] = true;
	response["command"] = command;
	if (command == "capabilities")
	{
		response["commands"] = {"capabilities", "status", "request-clean-exit"};
		response["scope"] = "reverse-engineering-tooling";
		response["emulator_debug_control"] = false;
		response["memory_mutation"] = false;
	}
	else if (command == "status")
	{
		response["game_loaded"] = status.gameLoaded;
		response["running"] = status.running;
		response["paused"] = status.paused;
		response["exit_requested"] = status.exitRequested;
		response["lifecycle_generation"] = status.lifecycleGeneration;
		response["game_id"] = status.gameId;
		response["media_path"] = status.mediaPath;
		response["cpu_backend"] = status.cpuBackend;
		response["configured_tools"] = status.configuredTools;
		response["observation_buses"] = {
				{"sh4", {{"subscribers", status.sh4Subscribers}}},
				{"pvr_ta", {{"active", status.pvrTaActive},
						{"dropped", status.pvrTaDropped}}},
				{"pvr_presentation", {{"active", status.pvrPresentationActive},
						{"dropped", status.pvrPresentationDropped}}},
				{"pvr_draw", {{"active", status.pvrDrawActive},
						{"dropped", status.pvrDrawDropped}}},
				{"gdrom", {{"active", status.gdromActive},
						{"dropped", status.gdromDropped}}},
				{"aica", {{"active", status.aicaActive},
						{"dropped", status.aicaDropped}}}};
	}
	else if (command == "request-clean-exit")
	{
		if (!status.gameLoaded)
			throw std::invalid_argument("no loaded game can be cleanly exited");
		requestCleanExit = true;
		response["accepted"] = true;
		response["effect"] = "frontend-clean-exit";
	}
	else
	{
		throw std::invalid_argument("command is not supported");
	}
	return response.dump();
}

} // namespace research

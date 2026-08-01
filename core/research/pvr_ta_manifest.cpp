#include "research/pvr_ta_manifest.h"

#include "research/pvr_ta_artifact.h"
#include "json.hpp"

#include <set>
#include <stdexcept>
#include <string>

namespace research
{
namespace
{

using json = nlohmann::json;

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid PowerVR TA manifest v1: " + reason);
}

void requireKeys(const json& value, const std::set<std::string>& keys,
		const std::string& field)
{
	if (!value.is_object())
		invalid(field + " must be an object");
	for (const auto& item : value.items())
		if (keys.count(item.key()) == 0)
			invalid(field + " has unknown field '" + item.key() + "'");
	for (const std::string& key : keys)
		if (!value.contains(key))
			invalid(field + "." + key + " is missing");
}

std::uint64_t unsignedValue(const json& parent, const char *name,
		const std::string& field)
{
	if (!parent.at(name).is_number_unsigned())
		invalid(field + "." + name + " must be an unsigned integer");
	return parent.at(name).get<std::uint64_t>();
}

std::string stringValue(const json& parent, const char *name,
		const std::string& field, std::size_t maximum = 128)
{
	if (!parent.at(name).is_string())
		invalid(field + "." + name + " must be a string");
	const std::string result = parent.at(name).get<std::string>();
	if (result.empty() || result.size() > maximum)
		invalid(field + "." + name + " has an invalid length");
	return result;
}

} // namespace

PvrTaManifest loadPvrTaManifest(const std::filesystem::path& path)
{
	PvrTaManifest result;
	result.path = path;
	result.bytes = readFileExact(path, MaxPvrTaManifestBytes);
	if (result.bytes.empty())
		invalid("file is empty");
	json root;
	try
	{
		std::vector<std::set<std::string>> keys;
		auto callback = [&keys](int depth, json::parse_event_t event, json& parsed) {
			if (depth < 0 || depth > 32)
				invalid("JSON depth is invalid");
			const std::size_t level = static_cast<std::size_t>(depth);
			if (event == json::parse_event_t::object_start)
			{
				if (keys.size() <= level)
					keys.resize(level + 1);
				keys[level].clear();
			}
			else if (event == json::parse_event_t::key)
			{
				if (level == 0 || keys.size() < level || !parsed.is_string()
						|| !keys[level - 1].insert(parsed.get<std::string>()).second)
					invalid("JSON contains a duplicate key");
			}
			return true;
		};
		root = json::parse(result.bytes.begin(), result.bytes.end(), callback);
	}
	catch (const json::exception& exception)
	{
		invalid(std::string("JSON parse error: ") + exception.what());
	}
	requireKeys(root, {"capture", "limits", "manifest_id", "schema",
			"schema_version", "static_analysis"}, "manifest");
	if (root.at("schema") != "flycast-research-pvr-ta-capture-manifest"
			|| !root.at("schema_version").is_number_unsigned()
			|| root.at("schema_version").get<std::uint64_t>() != 1)
		invalid("unsupported schema");
	result.manifestId = stringValue(root, "manifest_id", "manifest");
	const json& capture = root.at("capture");
	requireKeys(capture, {"render_done_count", "start_dma"}, "manifest.capture");
	result.startDma = unsignedValue(capture, "start_dma", "manifest.capture");
	result.renderDoneCount = unsignedValue(capture, "render_done_count",
			"manifest.capture");
	if (result.startDma > MaximumMapleDmaCheckpoint)
		invalid("capture.start_dma exceeds 10000000");
	if (result.renderDoneCount == 0 || result.renderDoneCount > 1'000'000)
		invalid("capture.render_done_count is outside [1, 1000000]");
	const json& limits = root.at("limits");
	requireKeys(limits, {"maximum_bytes", "maximum_events"}, "manifest.limits");
	result.maximumBytes = unsignedValue(limits, "maximum_bytes", "manifest.limits");
	result.maximumEvents = unsignedValue(limits, "maximum_events", "manifest.limits");
	if (result.maximumBytes < PvrTaArtifactHeaderSize
			|| result.maximumBytes > DefaultMaximumPvrTaArtifactBytes
			|| result.maximumEvents == 0
			|| result.maximumEvents > DefaultMaximumPvrTaArtifactEvents)
		invalid("manifest limits are outside the v1 admission bounds");
	const json& staticAnalysis = root.at("static_analysis");
	requireKeys(staticAnalysis, {"executable_sha256", "export_sha256"},
			"manifest.static_analysis");
	const std::string exportDigest = stringValue(staticAnalysis, "export_sha256",
			"manifest.static_analysis", 64);
	if (!sha256FromHex(exportDigest, result.staticAnalysisDigest)
			|| sha256ToHex(result.staticAnalysisDigest) != exportDigest)
		invalid("static_analysis.export_sha256 is not lowercase SHA-256");
	const std::string executable = stringValue(staticAnalysis, "executable_sha256",
			"manifest.static_analysis", 64);
	if (!sha256FromHex(executable, result.executableDigest)
			|| sha256ToHex(result.executableDigest) != executable)
		invalid("static_analysis.executable_sha256 is not lowercase SHA-256");
	result.digest = sha256(result.bytes.data(), result.bytes.size());
	return result;
}

void requirePvrTaManifestIdentity(const PvrTaManifest& manifest,
		const IdentityManifest& identity)
{
	requireSh4EquivalenceIdentityV2(identity);
	if (!identity.hasStaticAnalysis)
		invalid("identity has no static-analysis authority");
	if (manifest.startDma != identity.runtimeConfiguration.pvrTaStartDma)
		invalid("capture.start_dma differs from identity runtime configuration");
	if (!sha256Equal(manifest.executableDigest, identity.bootExecutableDigest))
		invalid("static executable digest differs from identity");
	if (!sha256Equal(manifest.staticAnalysisDigest,
			identity.staticAnalysisExportDigest))
		invalid("static-analysis export digest differs from identity");
}

} // namespace research

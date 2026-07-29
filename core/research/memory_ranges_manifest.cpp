#include "research/memory_ranges_manifest.h"

#include "json.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace research
{
namespace
{

using json = nlohmann::json;

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid memory-ranges manifest: " + reason);
}

void requireAllowedKeys(const json& value, const std::set<std::string>& allowed,
		const std::string& field)
{
	if (!value.is_object())
		invalid(field + " must be an object");
	for (const auto& item : value.items())
		if (allowed.find(item.key()) == allowed.end())
			invalid(field + " has unknown field '" + item.key() + "'");
}

const json& requiredObject(const json& parent, const char *name, const std::string& field)
{
	if (!parent.contains(name) || !parent.at(name).is_object())
		invalid(field + "." + name + " must be an object");
	return parent.at(name);
}

const json& requiredArray(const json& parent, const char *name, const std::string& field)
{
	if (!parent.contains(name) || !parent.at(name).is_array())
		invalid(field + "." + name + " must be an array");
	return parent.at(name);
}

std::string requiredString(const json& parent, const char *name, const std::string& field)
{
	if (!parent.contains(name) || !parent.at(name).is_string())
		invalid(field + "." + name + " must be a string");
	return parent.at(name).get<std::string>();
}

std::uint64_t requiredUnsigned(const json& parent, const char *name,
		const std::string& field)
{
	if (!parent.contains(name) || !parent.at(name).is_number_unsigned())
		invalid(field + "." + name + " must be an unsigned integer");
	return parent.at(name).get<std::uint64_t>();
}

bool requiredBool(const json& parent, const char *name, const std::string& field)
{
	if (!parent.contains(name) || !parent.at(name).is_boolean())
		invalid(field + "." + name + " must be a boolean");
	return parent.at(name).get<bool>();
}

bool asciiAlphaNumeric(char value)
{
	return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z')
			|| (value >= '0' && value <= '9');
}

void validateId(const std::string& value, const std::string& field)
{
	if (value.empty() || value.size() > 128 || !asciiAlphaNumeric(value.front()))
		invalid(field + " is not a valid identifier");
	for (const char character : value)
		if (!asciiAlphaNumeric(character) && character != '.' && character != '_'
				&& character != '-')
			invalid(field + " is not a valid identifier");
}

Sha256Digest parseDigest(const std::string& value, const std::string& field)
{
	Sha256Digest digest {};
	if (!sha256FromHex(value, digest))
		invalid(field + " must be a lowercase SHA-256 string");
	return digest;
}

std::uint32_t parseAddress(const std::string& value, const std::string& field)
{
	if (value.size() != 10 || value[0] != '0' || value[1] != 'x')
		invalid(field + " must use lowercase 0x00000000 form");
	std::uint32_t result = 0;
	for (std::size_t index = 2; index < value.size(); ++index)
	{
		const char character = value[index];
		unsigned digit = 0;
		if (character >= '0' && character <= '9')
			digit = static_cast<unsigned>(character - '0');
		else if (character >= 'a' && character <= 'f')
			digit = static_cast<unsigned>(character - 'a' + 10);
		else
			invalid(field + " must use lowercase 0x00000000 form");
		result = (result << 4) | digit;
	}
	return result;
}

json parseRejectingDuplicateKeys(const std::vector<std::uint8_t>& bytes)
{
	std::vector<std::set<std::string>> objectKeys;
	auto callback = [&objectKeys](int depth, json::parse_event_t event, json& parsed) {
		if (depth < 0)
			invalid("JSON parser reported a negative depth");
		const std::size_t level = static_cast<std::size_t>(depth);
		switch (event)
		{
		case json::parse_event_t::object_start:
			if (objectKeys.size() <= level)
				objectKeys.resize(level + 1);
			objectKeys[level].clear();
			break;
		case json::parse_event_t::key:
			if (level == 0 || objectKeys.size() < level || !parsed.is_string())
				invalid("JSON parser key depth is inconsistent");
			if (!objectKeys[level - 1].insert(parsed.get<std::string>()).second)
				invalid("duplicate JSON key '" + parsed.get<std::string>() + "'");
			break;
		case json::parse_event_t::object_end:
			if (objectKeys.size() > level)
				objectKeys[level].clear();
			break;
		default:
			break;
		}
		return true;
	};
	return json::parse(bytes.begin(), bytes.end(), callback);
}

MemoryRangesManifest validateManifest(const json& root)
{
	requireAllowedKeys(root, {"schema", "schema_version", "manifest_id", "address_space",
			"bindings", "trigger", "ranges", "limits", "acceptance"}, "root");
	if (requiredString(root, "schema", "root") != "flycast-research-memory-ranges-manifest")
		invalid("unsupported schema");
	if (requiredUnsigned(root, "schema_version", "root") != 1)
		invalid("unsupported schema_version");
	if (requiredString(root, "address_space", "root") != "flycast-sh4-virtual")
		invalid("unsupported address_space");

	MemoryRangesManifest manifest;
	manifest.id = requiredString(root, "manifest_id", "root");
	validateId(manifest.id, "root.manifest_id");

	const json& bindings = requiredObject(root, "bindings", "root");
	requireAllowedKeys(bindings, {"executable_sha256", "static_analysis_id",
			"static_analysis_sha256", "hook_manifest_id", "hook_manifest_sha256"},
			"root.bindings");
	manifest.bindings.executableDigest = parseDigest(
			requiredString(bindings, "executable_sha256", "root.bindings"),
			"root.bindings.executable_sha256");
	manifest.bindings.staticAnalysisId = requiredString(bindings, "static_analysis_id",
			"root.bindings");
	validateId(manifest.bindings.staticAnalysisId, "root.bindings.static_analysis_id");
	manifest.bindings.staticAnalysisDigest = parseDigest(
			requiredString(bindings, "static_analysis_sha256", "root.bindings"),
			"root.bindings.static_analysis_sha256");
	manifest.bindings.hookManifestId = requiredString(bindings, "hook_manifest_id",
			"root.bindings");
	validateId(manifest.bindings.hookManifestId, "root.bindings.hook_manifest_id");
	manifest.bindings.hookManifestDigest = parseDigest(
			requiredString(bindings, "hook_manifest_sha256", "root.bindings"),
			"root.bindings.hook_manifest_sha256");

	const json& trigger = requiredObject(root, "trigger", "root");
	requireAllowedKeys(trigger, {"kind", "start_address", "end_address_exclusive"},
			"root.trigger");
	if (requiredString(trigger, "kind", "root.trigger") != "guest-pc-enters-range")
		invalid("unsupported trigger kind");
	manifest.triggerStart = parseAddress(requiredString(trigger, "start_address", "root.trigger"),
			"root.trigger.start_address");
	manifest.triggerEndExclusive = parseAddress(
			requiredString(trigger, "end_address_exclusive", "root.trigger"),
			"root.trigger.end_address_exclusive");
	if (manifest.triggerStart >= manifest.triggerEndExclusive)
		invalid("trigger interval must be non-empty and non-wrapping");

	const json& ranges = requiredArray(root, "ranges", "root");
	if (ranges.empty() || ranges.size() > MaxMemoryRangeCount)
		invalid("root.ranges count is outside [1, 64]");
	std::set<std::string> rangeIds;
	std::vector<std::pair<std::uint64_t, std::uint64_t>> intervals;
	std::uint64_t totalBytes = 0;
	manifest.ranges.reserve(ranges.size());
	intervals.reserve(ranges.size());
	for (std::size_t index = 0; index < ranges.size(); ++index)
	{
		const json& range = ranges.at(index);
		const std::string field = "root.ranges[" + std::to_string(index) + "]";
		requireAllowedKeys(range, {"range_id", "address", "length", "expected_sha256"}, field);
		MemoryRangeDefinition definition;
		definition.id = requiredString(range, "range_id", field);
		validateId(definition.id, field + ".range_id");
		if (!rangeIds.insert(definition.id).second)
			invalid("duplicate range_id '" + definition.id + "'");
		definition.address = parseAddress(requiredString(range, "address", field),
				field + ".address");
		const std::uint64_t length = requiredUnsigned(range, "length", field);
		if (length == 0 || length > MaxMemoryRangeLength)
			invalid(field + ".length is outside [1, 16777216]");
		definition.length = static_cast<std::uint32_t>(length);
		const std::uint64_t end = static_cast<std::uint64_t>(definition.address) + length;
		if (end > (std::uint64_t {1} << 32))
			invalid(field + " wraps the 32-bit address space");
		intervals.emplace_back(definition.address, end);
		if (totalBytes > std::numeric_limits<std::uint64_t>::max() - length)
			invalid("range byte total overflows");
		totalBytes += length;
		definition.expectedDigest = parseDigest(
				requiredString(range, "expected_sha256", field), field + ".expected_sha256");
		manifest.ranges.push_back(std::move(definition));
	}
	std::sort(intervals.begin(), intervals.end());
	for (std::size_t index = 1; index < intervals.size(); ++index)
		if (intervals[index].first < intervals[index - 1].second)
			invalid("memory ranges overlap");

	const json& limits = requiredObject(root, "limits", "root");
	requireAllowedKeys(limits, {"maximum_total_bytes"}, "root.limits");
	manifest.maximumTotalBytes = requiredUnsigned(limits, "maximum_total_bytes", "root.limits");
	if (manifest.maximumTotalBytes == 0
			|| manifest.maximumTotalBytes > MaxMemoryRangesTotalBytes)
		invalid("root.limits.maximum_total_bytes is outside [1, 1073741824]");
	if (totalBytes > manifest.maximumTotalBytes)
		invalid("range byte total exceeds maximum_total_bytes");

	const json& acceptance = requiredObject(root, "acceptance", "root");
	requireAllowedKeys(acceptance, {"snapshot_consistency", "expected_trigger_count",
			"expected_event_count", "zero_dropped_events", "natural_exit"},
			"root.acceptance");
	if (requiredString(acceptance, "snapshot_consistency", "root.acceptance")
			!= "single-interpreter-boundary")
		invalid("unsupported snapshot_consistency");
	if (requiredUnsigned(acceptance, "expected_trigger_count", "root.acceptance") != 1)
		invalid("expected_trigger_count must be 1");
	if (requiredUnsigned(acceptance, "expected_event_count", "root.acceptance")
			!= manifest.ranges.size())
		invalid("expected_event_count must equal the range count");
	if (!requiredBool(acceptance, "zero_dropped_events", "root.acceptance"))
		invalid("zero_dropped_events must be true");
	if (!requiredBool(acceptance, "natural_exit", "root.acceptance"))
		invalid("natural_exit must be true");
	return manifest;
}

} // namespace

MemoryRangesManifest loadMemoryRangesManifest(const std::filesystem::path& path)
{
	std::vector<std::uint8_t> bytes = readFileExact(path, MaxMemoryRangesManifestBytes);
	if (bytes.empty())
		invalid("file is empty");
	MemoryRangesManifest manifest;
	try
	{
		manifest = validateManifest(parseRejectingDuplicateKeys(bytes));
	}
	catch (const nlohmann::json::exception& exception)
	{
		invalid(std::string("JSON parse/type error: ") + exception.what());
	}
	manifest.path = path;
	manifest.bytes = std::move(bytes);
	manifest.digest = sha256(manifest.bytes.data(), manifest.bytes.size());
	return manifest;
}

void requireMemoryRangesIdentity(const MemoryRangesManifest& manifest,
		const IdentityManifest& identity)
{
	if (!sha256Equal(manifest.bindings.executableDigest, identity.bootExecutableDigest))
		invalid("bindings.executable_sha256 does not match identity boot executable");
	if (!identity.hasStaticAnalysis)
		invalid("identity has no static_analysis binding");
	if (!sha256Equal(identity.staticAnalysisProgramDigest, identity.bootExecutableDigest)
			|| !sha256Equal(identity.staticAnalysisProgramDigest,
					manifest.bindings.executableDigest))
		invalid("identity static-analysis program does not match the boot executable");
	if (!sha256Equal(manifest.bindings.staticAnalysisDigest,
			identity.staticAnalysisExportDigest))
		invalid("bindings.static_analysis_sha256 does not match identity export");
	if (!identity.hasHookManifestDigest)
		invalid("identity has no hook-manifest digest");
	if (!sha256Equal(manifest.bindings.hookManifestDigest, identity.hookManifestDigest))
		invalid("bindings.hook_manifest_sha256 does not match identity hook manifest");
}

} // namespace research

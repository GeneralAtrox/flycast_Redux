#include "research/sh4_events_manifest.h"

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
	throw std::runtime_error("invalid SH-4 events manifest: " + reason);
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

std::int64_t requiredSigned(const json& parent, const char *name,
		const std::string& field)
{
	if (!parent.contains(name) || !parent.at(name).is_number_integer())
		invalid(field + "." + name + " must be an integer");
	try
	{
		return parent.at(name).get<std::int64_t>();
	}
	catch (const json::exception&)
	{
		invalid(field + "." + name + " is outside the signed 64-bit range");
	}
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

Sh4SnapshotPhase parsePhase(const std::string& value, const std::string& field)
{
	if (value == "call")
		return Sh4SnapshotPhase::Call;
	if (value == "return")
		return Sh4SnapshotPhase::Return;
	invalid(field + " must be 'call' or 'return'");
}

Sh4EventsManifest validateManifest(const json& root)
{
	requireAllowedKeys(root, {"schema", "schema_version", "manifest_id", "address_space",
			"bindings", "hooks", "watch_ranges", "limits", "acceptance"}, "root");
	if (requiredString(root, "schema", "root") != "flycast-research-sh4-events-manifest")
		invalid("unsupported schema");
	if (requiredUnsigned(root, "schema_version", "root") != 1)
		invalid("unsupported schema_version");
	if (requiredString(root, "address_space", "root") != "flycast-sh4-virtual")
		invalid("unsupported address_space");

	Sh4EventsManifest manifest;
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

	const json& hooks = requiredArray(root, "hooks", "root");
	if (hooks.size() > MaxSh4HookCount)
		invalid("root.hooks count exceeds 64");
	std::set<std::string> hookIds;
	std::set<std::string> snapshotIds;
	std::vector<std::pair<std::uint64_t, std::uint64_t>> hookIntervals;
	std::size_t snapshotCount = 0;
	manifest.hooks.reserve(hooks.size());
	for (std::size_t hookIndex = 0; hookIndex < hooks.size(); ++hookIndex)
	{
		const json& hook = hooks.at(hookIndex);
		const std::string field = "root.hooks[" + std::to_string(hookIndex) + "]";
		requireAllowedKeys(hook, {"hook_id", "entry_pc", "end_address_exclusive",
				"snapshots"}, field);
		Sh4HookDefinition definition;
		definition.id = requiredString(hook, "hook_id", field);
		validateId(definition.id, field + ".hook_id");
		if (!hookIds.insert(definition.id).second)
			invalid("duplicate hook_id '" + definition.id + "'");
		definition.entryPc = parseAddress(requiredString(hook, "entry_pc", field),
				field + ".entry_pc");
		definition.endPcExclusive = parseAddress(
				requiredString(hook, "end_address_exclusive", field),
				field + ".end_address_exclusive");
		if ((definition.entryPc & 1u) != 0 || (definition.endPcExclusive & 1u) != 0
				|| definition.entryPc >= definition.endPcExclusive)
			invalid(field + " instruction interval must be even, non-empty, and non-wrapping");
		hookIntervals.emplace_back(definition.entryPc, definition.endPcExclusive);

		const json& snapshots = requiredArray(hook, "snapshots", field);
		definition.snapshots.reserve(snapshots.size());
		for (std::size_t snapshotIndex = 0; snapshotIndex < snapshots.size(); ++snapshotIndex)
		{
			if (++snapshotCount > MaxSh4SnapshotCount)
				invalid("snapshot definition count exceeds 128");
			const json& snapshot = snapshots.at(snapshotIndex);
			const std::string snapshotField = field + ".snapshots["
					+ std::to_string(snapshotIndex) + "]";
			requireAllowedKeys(snapshot, {"snapshot_id", "phase", "source", "length",
					"required"}, snapshotField);
			Sh4SnapshotDefinition snapshotDefinition;
			snapshotDefinition.id = requiredString(snapshot, "snapshot_id", snapshotField);
			validateId(snapshotDefinition.id, snapshotField + ".snapshot_id");
			if (!snapshotIds.insert(snapshotDefinition.id).second)
				invalid("duplicate snapshot_id '" + snapshotDefinition.id + "'");
			snapshotDefinition.phase = parsePhase(
					requiredString(snapshot, "phase", snapshotField), snapshotField + ".phase");
			const std::uint64_t length = requiredUnsigned(snapshot, "length", snapshotField);
			if (length == 0 || length > MaxSh4SnapshotLength)
				invalid(snapshotField + ".length is outside [1, 4096]");
			snapshotDefinition.length = static_cast<std::uint32_t>(length);
			snapshotDefinition.required = requiredBool(snapshot, "required", snapshotField);
			const json& source = requiredObject(snapshot, "source", snapshotField);
			const std::string sourceKind = requiredString(source, "kind", snapshotField + ".source");
			if (sourceKind == "absolute")
			{
				requireAllowedKeys(source, {"kind", "address"}, snapshotField + ".source");
				snapshotDefinition.sourceKind = Sh4SnapshotSourceKind::Absolute;
				snapshotDefinition.absoluteAddress = parseAddress(
						requiredString(source, "address", snapshotField + ".source"),
						snapshotField + ".source.address");
				const std::uint64_t end = static_cast<std::uint64_t>(
						snapshotDefinition.absoluteAddress) + length;
				if (end > (std::uint64_t {1} << 32))
					invalid(snapshotField + " absolute snapshot wraps the address space");
			}
			else if (sourceKind == "register-relative")
			{
				requireAllowedKeys(source, {"kind", "register_index", "offset"},
						snapshotField + ".source");
				snapshotDefinition.sourceKind = Sh4SnapshotSourceKind::RegisterRelative;
				const std::uint64_t registerIndex = requiredUnsigned(source, "register_index",
						snapshotField + ".source");
				if (registerIndex > 15)
					invalid(snapshotField + ".source.register_index is outside [0, 15]");
				snapshotDefinition.registerIndex = static_cast<std::uint8_t>(registerIndex);
				const std::int64_t offset = requiredSigned(source, "offset",
						snapshotField + ".source");
				if (offset < std::numeric_limits<std::int32_t>::min()
						|| offset > std::numeric_limits<std::int32_t>::max())
					invalid(snapshotField + ".source.offset is outside signed 32-bit range");
				snapshotDefinition.offset = static_cast<std::int32_t>(offset);
			}
			else
			{
				invalid(snapshotField + ".source.kind is unsupported");
			}
			definition.snapshots.push_back(std::move(snapshotDefinition));
		}
		manifest.hooks.push_back(std::move(definition));
	}
	std::sort(hookIntervals.begin(), hookIntervals.end());
	for (std::size_t index = 1; index < hookIntervals.size(); ++index)
		if (hookIntervals[index].first < hookIntervals[index - 1].second)
			invalid("hook instruction intervals overlap");

	const json& watchRanges = requiredArray(root, "watch_ranges", "root");
	if (watchRanges.size() > MaxSh4WatchRangeCount)
		invalid("root.watch_ranges count exceeds 64");
	std::set<std::string> watchIds;
	std::vector<std::pair<std::uint64_t, std::uint64_t>> watchIntervals;
	manifest.watchRanges.reserve(watchRanges.size());
	for (std::size_t index = 0; index < watchRanges.size(); ++index)
	{
		const json& range = watchRanges.at(index);
		const std::string field = "root.watch_ranges[" + std::to_string(index) + "]";
		requireAllowedKeys(range, {"watch_id", "address", "length", "access"}, field);
		Sh4WatchRangeDefinition definition;
		definition.id = requiredString(range, "watch_id", field);
		validateId(definition.id, field + ".watch_id");
		if (!watchIds.insert(definition.id).second)
			invalid("duplicate watch_id '" + definition.id + "'");
		definition.address = parseAddress(requiredString(range, "address", field),
				field + ".address");
		const std::uint64_t length = requiredUnsigned(range, "length", field);
		if (length == 0 || length > 16 * 1024 * 1024)
			invalid(field + ".length is outside [1, 16777216]");
		definition.length = static_cast<std::uint32_t>(length);
		const std::uint64_t end = static_cast<std::uint64_t>(definition.address) + length;
		if (end > (std::uint64_t {1} << 32))
			invalid(field + " wraps the 32-bit address space");
		watchIntervals.emplace_back(definition.address, end);
		const json& access = requiredArray(range, "access", field);
		if (access.empty() || access.size() > 2)
			invalid(field + ".access must contain read and/or write");
		for (const json& item : access)
		{
			if (!item.is_string())
				invalid(field + ".access entries must be strings");
			const std::string value = item.get<std::string>();
			const std::uint8_t flag = value == "read" ? Sh4WatchRead
					: value == "write" ? Sh4WatchWrite : 0;
			if (flag == 0 || (definition.access & flag) != 0)
				invalid(field + ".access contains an unsupported or duplicate value");
			definition.access |= flag;
		}
		manifest.watchRanges.push_back(std::move(definition));
	}
	std::sort(watchIntervals.begin(), watchIntervals.end());
	for (std::size_t index = 1; index < watchIntervals.size(); ++index)
		if (watchIntervals[index].first < watchIntervals[index - 1].second)
			invalid("watch ranges overlap");

	const json& limits = requiredObject(root, "limits", "root");
	requireAllowedKeys(limits, {"maximum_events", "maximum_snapshot_bytes_per_event",
			"maximum_total_snapshot_bytes", "maximum_open_invocations"}, "root.limits");
	manifest.maximumEvents = requiredUnsigned(limits, "maximum_events", "root.limits");
	manifest.maximumSnapshotBytesPerEvent = requiredUnsigned(limits,
			"maximum_snapshot_bytes_per_event", "root.limits");
	manifest.maximumTotalSnapshotBytes = requiredUnsigned(limits,
			"maximum_total_snapshot_bytes", "root.limits");
	const std::uint64_t maximumOpen = requiredUnsigned(limits,
			"maximum_open_invocations", "root.limits");
	if (manifest.maximumEvents == 0 || manifest.maximumEvents > MaxSh4EventCount)
		invalid("root.limits.maximum_events is outside [1, 10000000]");
	if (manifest.maximumSnapshotBytesPerEvent > MaxSh4SnapshotBytesPerEvent)
		invalid("root.limits.maximum_snapshot_bytes_per_event exceeds 1048576");
	if (manifest.maximumTotalSnapshotBytes > MaxSh4TotalSnapshotBytes)
		invalid("root.limits.maximum_total_snapshot_bytes exceeds 1073741824");
	if (maximumOpen == 0 || maximumOpen > MaxSh4OpenInvocations)
		invalid("root.limits.maximum_open_invocations is outside [1, 1024]");
	manifest.maximumOpenInvocations = static_cast<std::uint32_t>(maximumOpen);
	for (std::size_t hookIndex = 0; hookIndex < manifest.hooks.size(); ++hookIndex)
		for (const Sh4SnapshotPhase phase : {Sh4SnapshotPhase::Call,
				Sh4SnapshotPhase::Return})
		{
			std::uint64_t bytes = 0;
			for (const Sh4SnapshotDefinition& snapshot : manifest.hooks[hookIndex].snapshots)
				if (snapshot.phase == phase)
					bytes += snapshot.length;
			if (bytes > manifest.maximumSnapshotBytesPerEvent)
				invalid("hook snapshot bytes exceed maximum_snapshot_bytes_per_event");
		}

	const json& acceptance = requiredObject(root, "acceptance", "root");
	requireAllowedKeys(acceptance, {"backend", "minimum_call_events",
			"minimum_watch_events", "require_balanced_calls", "zero_dropped_events",
			"natural_exit"}, "root.acceptance");
	if (requiredString(acceptance, "backend", "root.acceptance") != "interpreter")
		invalid("root.acceptance.backend must be interpreter");
	manifest.minimumCallEvents = requiredUnsigned(acceptance, "minimum_call_events",
			"root.acceptance");
	manifest.minimumWatchEvents = requiredUnsigned(acceptance, "minimum_watch_events",
			"root.acceptance");
	if (manifest.minimumCallEvents > manifest.maximumEvents
			|| manifest.minimumWatchEvents > manifest.maximumEvents
			|| manifest.minimumCallEvents + manifest.minimumWatchEvents == 0)
		invalid("acceptance minimum events are inconsistent with maximum_events");
	if (manifest.minimumCallEvents
			> (manifest.maximumEvents - manifest.minimumWatchEvents) / 2)
		invalid("balanced call/return and watch minimums exceed maximum_events");
	if (manifest.hooks.empty() && manifest.minimumCallEvents != 0)
		invalid("minimum_call_events requires at least one hook");
	if (manifest.watchRanges.empty() && manifest.minimumWatchEvents != 0)
		invalid("minimum_watch_events requires at least one watch range");
	if (!requiredBool(acceptance, "require_balanced_calls", "root.acceptance"))
		invalid("require_balanced_calls must be true");
	if (!requiredBool(acceptance, "zero_dropped_events", "root.acceptance"))
		invalid("zero_dropped_events must be true");
	if (!requiredBool(acceptance, "natural_exit", "root.acceptance"))
		invalid("natural_exit must be true");
	return manifest;
}

} // namespace

Sh4EventsManifest loadSh4EventsManifest(const std::filesystem::path& path)
{
	std::vector<std::uint8_t> bytes = readFileExact(path, MaxSh4EventsManifestBytes);
	if (bytes.empty())
		invalid("file is empty");
	Sh4EventsManifest manifest;
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

void requireSh4EventsIdentity(const Sh4EventsManifest& manifest,
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

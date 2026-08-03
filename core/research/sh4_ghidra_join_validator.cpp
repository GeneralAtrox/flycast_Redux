#include "research/sh4_ghidra_join.h"

#include "json.hpp"
#include "research/ghidra_export.h"
#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/sh4_profile_artifact.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace research
{
namespace
{

using json = nlohmann::json;
constexpr std::uint64_t AddressSpaceSize = std::uint64_t {1} << 32;

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid SH-4/Ghidra semantic join: " + reason);
}

std::string hex(std::uint64_t value, unsigned digits)
{
	constexpr char alphabet[] = "0123456789abcdef";
	std::string result(2 + digits, '0');
	result[1] = 'x';
	for (unsigned index = 0; index < digits; ++index)
		result[1 + digits - index] = alphabet[(value >> (index * 4)) & 0xf];
	return result;
}

const char *kindName(Sh4DynarecBranchKind kind)
{
	switch (kind)
	{
	case Sh4DynarecBranchKind::None: return "none";
	case Sh4DynarecBranchKind::Conditional: return "conditional";
	case Sh4DynarecBranchKind::Call: return "call";
	case Sh4DynarecBranchKind::Jump: return "jump";
	case Sh4DynarecBranchKind::Return: return "return";
	}
	invalid("source profile branch kind is unsupported");
}

json parseRejectingDuplicateKeys(const std::vector<std::uint8_t>& bytes)
{
	std::vector<std::set<std::string>> keys;
	auto callback = [&keys](int depth, json::parse_event_t event, json& parsed) {
		if (depth < 0)
			invalid("JSON parser reported negative depth");
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
				invalid("candidate has a duplicate or inconsistent JSON key");
		}
		return true;
	};
	try
	{
		return json::parse(bytes.begin(), bytes.end(), callback);
	}
	catch (const json::exception& exception)
	{
		invalid(std::string("candidate JSON is malformed: ") + exception.what());
	}
}

struct Interval
{
	std::uint64_t start = 0;
	std::uint64_t end = 0;
	std::size_t function = 0;
};

struct Sources
{
	IdentityManifest identity;
	GhidraExport ghidra;
	Sh4DynarecProfileArtifactSummary profile;
	Sha256Digest replayDigest {};
	Sha256Digest profileDigest {};
	std::vector<std::uint8_t> program;
	std::vector<Interval> intervals;
	std::uint64_t start = 0;
	std::uint64_t end = 0;
};

Sources loadSources(const std::filesystem::path& profilePath,
		const std::filesystem::path& identityPath,
		const std::filesystem::path& replayPath,
		const std::filesystem::path& exportPath,
		const std::filesystem::path& programPath,
		const std::filesystem::path& scriptPath)
{
	Sources source;
	source.identity = loadIdentityManifest(identityPath);
	if (source.identity.schemaVersion == 2)
		requireSh4DynarecProfileIdentityV2(source.identity);
	else
		requireSh4DynarecProfileRecordIdentityV3(source.identity);
	const Sha256Digest& replayIdentity = source.identity.schemaVersion == 2
			? source.identity.mapleReplayIdentityDigest : source.identity.digest;
	validateProductionMapleTraceFile(replayPath, replayIdentity);
	source.replayDigest = hashFileExact(replayPath, DefaultMaximumMapleTraceBytes);
	source.profileDigest = hashFileExact(profilePath,
			DefaultMaximumSh4DynarecProfileArtifactBytes);
	Sh4DynarecProfileArtifactBinding binding;
	binding.identityDigest = source.identity.digest;
	binding.replayDigest = source.replayDigest;
	binding.configurationDigest = source.identity.configurationDigest;
	source.profile = validateSh4DynarecProfileArtifact(profilePath, binding);
	if (source.profile.incompleteByteBlocks != 0)
		invalid("source profile has incomplete exact bytes");
	source.ghidra = loadGhidraExport(exportPath);
	requireGhidraExportIdentity(source.ghidra, source.identity, programPath, scriptPath);
	source.program = readFileExact(programPath, MaxGhidraExportBytes);
	source.start = source.ghidra.imageBase;
	if (source.program.size() > AddressSpaceSize - source.start)
		invalid("authenticated program wraps the address space");
	source.end = source.start + source.program.size();
	for (std::size_t function = 0; function < source.ghidra.functions.size(); ++function)
		for (const auto& range : source.ghidra.functions[function].bodyRanges)
			source.intervals.push_back({range.startAddress,
					std::uint64_t(range.startAddress) + range.length, function});
	std::sort(source.intervals.begin(), source.intervals.end(),
			[](const Interval& left, const Interval& right) {
				return left.start < right.start;
			});
	return source;
}

bool inProgram(const Sources& source, std::uint64_t start, std::uint64_t end)
{
	return start >= source.start && end >= start && end <= source.end;
}

bool overlapsProgram(const Sources& source, std::uint64_t start, std::uint64_t end)
{
	return start < source.end && end > source.start;
}

bool executable(const Sources& source, std::uint64_t start, std::uint64_t end)
{
	for (const auto& block : source.ghidra.memoryBlocks)
		if (block.initialized && block.execute && start >= block.startAddress
				&& end <= std::uint64_t(block.startAddress) + block.length)
			return true;
	return false;
}

const Interval *at(const Sources& source, std::uint64_t point)
{
	auto iterator = std::upper_bound(source.intervals.begin(), source.intervals.end(),
			point, [](std::uint64_t value, const Interval& interval) {
				return value < interval.start;
			});
	if (iterator == source.intervals.begin())
		return nullptr;
	--iterator;
	return point < iterator->end ? &*iterator : nullptr;
}

struct Mapping
{
	std::string classification;
	const GhidraFunction *function = nullptr;
};

Mapping pointMapping(const Sources& source, std::uint32_t point)
{
	if (!inProgram(source, point, std::uint64_t(point) + 1))
		return {"runtime-only", nullptr};
	if (!executable(source, point, std::uint64_t(point) + 1))
		return {"authenticated-program-non-executable", nullptr};
	const Interval *interval = at(source, point);
	if (interval == nullptr)
		return {"static-executable-unassigned", nullptr};
	const auto& function = source.ghidra.functions[interval->function];
	return {point == function.entryAddress ? "function-entry" : "function-interior",
			&function};
}

Mapping blockMapping(const Sources& source, std::uint32_t start, std::uint32_t size)
{
	const std::uint64_t end = std::uint64_t(start) + size;
	if (!inProgram(source, start, end))
		return {"runtime-only", nullptr};
	const Interval *interval = at(source, start);
	if (interval == nullptr || end > interval->end)
		return {"static-executable-unassigned", nullptr};
	const auto& function = source.ghidra.functions[interval->function];
	return {start == function.entryAddress ? "function-entry" : "function-interior",
			&function};
}

json mapping(const Mapping& value)
{
	return {{"class", value.classification},
			{"function_entry", value.function == nullptr
					? json(nullptr) : json(hex(value.function->entryAddress, 8))}};
}

json function(const GhidraFunction& value)
{
	json ranges = json::array();
	for (const auto& range : value.bodyRanges)
		ranges.push_back({{"start_address", hex(range.startAddress, 8)},
				{"length", range.length}});
	return {{"entry_address", hex(value.entryAddress, 8)}, {"name", value.name},
			{"namespace", value.nameSpace},
			{"calling_convention", value.callingConvention},
			{"return_type", value.returnType}, {"parameter_types", value.parameterTypes},
			{"body_ranges", std::move(ranges)}, {"thunk", value.thunk},
			{"no_return", value.noReturn}};
}

struct Reconstruction
{
	json root;
	Sh4GhidraSemanticJoinSummary summary;
};

Reconstruction reconstruct(const Sources& source)
{
	Reconstruction result;
	result.summary.identityDigest = source.identity.digest;
	result.summary.replayDigest = source.replayDigest;
	result.summary.profileDigest = source.profileDigest;
	result.summary.profilePayloadDigest = source.profile.payloadDigest;
	result.summary.ghidraExportDigest = source.ghidra.digest;
	result.summary.programDigest = source.ghidra.executableDigest;
	result.summary.blockCount = source.profile.blocks.size();
	result.summary.edgeCount = source.profile.branches.size();
	json blocks = json::array();
	json edges = json::array();
	std::set<std::uint32_t> usedFunctions;
	std::set<std::uint32_t> symbolAddresses;
	std::map<std::string, std::uint64_t> sourceClasses, destinationClasses,
			sourceOccurrences, destinationOccurrences;

	for (const auto& block : source.profile.blocks)
	{
		const std::uint64_t start = block.definition.virtualAddress;
		const std::uint64_t end = start + block.definition.guestCodeSize;
		const bool authenticated = inProgram(source, start, end);
		if (!authenticated && overlapsProgram(source, start, end))
			invalid("source block partially overlaps authenticated program");
		if (authenticated)
		{
			if (!executable(source, start, end))
				invalid("source program block is not in initialized executable memory");
			const std::size_t offset = static_cast<std::size_t>(start - source.start);
			if (!std::equal(block.definition.guestBytes.begin(),
					block.definition.guestBytes.end(), source.program.begin() + offset))
				invalid("source block bytes differ from authenticated executable");
			++result.summary.authenticatedProgramBlocks;
		}
		const Mapping owner = blockMapping(source, block.definition.virtualAddress,
				block.definition.guestCodeSize);
		if (owner.function != nullptr)
		{
			++result.summary.functionOwnedBlocks;
			usedFunctions.insert(owner.function->entryAddress);
		}
		else if (owner.classification == "runtime-only")
			++result.summary.runtimeOnlyBlocks;
		else
			++result.summary.staticExecutableUnassignedBlocks;
		symbolAddresses.insert(block.definition.virtualAddress);
		json branch = nullptr;
		if (block.definition.branchKind != Sh4DynarecBranchKind::None)
			branch = {{"kind", kindName(block.definition.branchKind)},
					{"source", hex(block.definition.branchSource, 8)},
					{"opcode", hex(block.definition.branchOpcode, 4)},
					{"target", block.definition.branchTarget == UINT32_MAX
							? json(nullptr) : json(hex(block.definition.branchTarget, 8))},
					{"fallthrough", block.definition.fallthroughTarget == UINT32_MAX
							? json(nullptr) : json(hex(block.definition.fallthroughTarget, 8))}};
		const Sha256Digest bytes = sha256(block.definition.guestBytes.data(),
				block.definition.guestBytes.size());
		blocks.push_back({{"generation", block.generation},
				{"virtual_address", hex(block.definition.virtualAddress, 8)},
				{"physical_address", hex(block.definition.physicalAddress, 8)},
				{"fpu_configuration", block.definition.fpuConfiguration},
				{"guest_code_size", block.definition.guestCodeSize},
				{"guest_cycles", block.definition.guestCycles},
				{"guest_opcodes", block.definition.guestOpcodes},
				{"guest_bytes_sha256", sha256ToHex(bytes)},
				{"entered", block.enteredCount}, {"completed", block.completedCount},
				{"aborted", block.abortedCount}, {"total_cycles", block.totalCycles},
				{"first_entry_tick", block.firstEntryTick},
				{"last_exit_tick", block.lastExitTick}, {"branch", std::move(branch)},
				{"mapping", mapping(owner)}});
	}

	for (const auto& edge : source.profile.branches)
	{
		const Mapping from = pointMapping(source, edge.source);
		const Mapping to = pointMapping(source, edge.destination);
		if (from.function != nullptr) usedFunctions.insert(from.function->entryAddress);
		if (to.function != nullptr) usedFunctions.insert(to.function->entryAddress);
		symbolAddresses.insert(edge.source);
		symbolAddresses.insert(edge.destination);
		++sourceClasses[from.classification];
		++destinationClasses[to.classification];
		if (sourceOccurrences[from.classification]
				> std::numeric_limits<std::uint64_t>::max() - edge.count
				|| destinationOccurrences[to.classification]
						> std::numeric_limits<std::uint64_t>::max() - edge.count
				|| result.summary.edgeOccurrences
						> std::numeric_limits<std::uint64_t>::max() - edge.count)
			invalid("source edge occurrence totals overflow");
		sourceOccurrences[from.classification] += edge.count;
		destinationOccurrences[to.classification] += edge.count;
		result.summary.edgeOccurrences += edge.count;
		edges.push_back({{"source_generation", edge.sourceGeneration},
				{"source", hex(edge.source, 8)},
				{"destination", hex(edge.destination, 8)},
				{"opcode", hex(edge.opcode, 4)}, {"kind", kindName(edge.kind)},
				{"taken", edge.taken}, {"count", edge.count},
				{"first_boundary_tick", edge.firstBoundaryTick},
				{"last_boundary_tick", edge.lastBoundaryTick},
				{"source_mapping", mapping(from)},
				{"destination_mapping", mapping(to)}});
	}

	json functions = json::array();
	for (const auto& item : source.ghidra.functions)
		if (usedFunctions.find(item.entryAddress) != usedFunctions.end())
		{
			functions.push_back(function(item));
			symbolAddresses.insert(item.entryAddress);
		}
	json symbols = json::array();
	std::map<std::uint32_t, json> symbolsByAddress;
	for (std::uint32_t relevant : symbolAddresses)
		symbolsByAddress.emplace(relevant, json::array());
	for (const auto& symbol : source.ghidra.symbols)
	{
		auto values = symbolsByAddress.find(symbol.address);
		if (values != symbolsByAddress.end())
			values->second.push_back({{"name", symbol.name},
					{"namespace", symbol.nameSpace}, {"kind", symbol.kind},
					{"source", symbol.source}, {"primary", symbol.primary}});
	}
	for (auto& [relevant, values] : symbolsByAddress)
	{
		if (!values.empty())
			symbols.push_back({{"address", hex(relevant, 8)},
					{"symbols", std::move(values)}});
	}
	auto countObject = [](const auto& counts) {
		json value = json::object();
		for (const auto& [name, count] : counts) value[name] = count;
		return value;
	};
	result.root = {
		{"schema", "flycast-research-sh4-ghidra-semantic-join"},
		{"schema_version", Sh4GhidraSemanticJoinSchemaVersion},
		{"evidence_class", "static-dynamic-join"},
		{"bindings", {{"identity_sha256", sha256ToHex(source.identity.digest)},
				{"replay_sha256", sha256ToHex(source.replayDigest)},
				{"profile_sha256", sha256ToHex(source.profileDigest)},
				{"profile_payload_sha256", sha256ToHex(source.profile.payloadDigest)},
				{"configuration_sha256", sha256ToHex(source.identity.configurationDigest)},
				{"ghidra_export_id", source.ghidra.exportId},
				{"ghidra_export_sha256", sha256ToHex(source.ghidra.digest)},
				{"program_sha256", sha256ToHex(source.ghidra.executableDigest)},
				{"exporter_script_sha256", sha256ToHex(source.ghidra.exporterScriptDigest)},
				{"image_base", hex(source.ghidra.imageBase, 8)},
				{"program_size", source.program.size()}}},
		{"coverage", {{"block_count", result.summary.blockCount},
				{"edge_count", result.summary.edgeCount},
				{"edge_occurrences", result.summary.edgeOccurrences},
				{"authenticated_program_blocks", result.summary.authenticatedProgramBlocks},
				{"exact_byte_blocks", result.summary.authenticatedProgramBlocks},
				{"function_owned_blocks", result.summary.functionOwnedBlocks},
				{"static_executable_unassigned_blocks",
						result.summary.staticExecutableUnassignedBlocks},
				{"runtime_only_blocks", result.summary.runtimeOnlyBlocks},
				{"edge_source_classes", countObject(sourceClasses)},
				{"edge_destination_classes", countObject(destinationClasses)},
				{"edge_source_occurrences", countObject(sourceOccurrences)},
				{"edge_destination_occurrences", countObject(destinationOccurrences)}}},
		{"functions", std::move(functions)}, {"symbol_addresses", std::move(symbols)},
		{"blocks", std::move(blocks)}, {"edges", std::move(edges)}};
	return result;
}

} // namespace

Sh4GhidraSemanticJoinSummary validateSh4GhidraSemanticJoin(
		const std::filesystem::path& candidate,
		const std::filesystem::path& profile,
		const std::filesystem::path& identity,
		const std::filesystem::path& replay,
		const std::filesystem::path& ghidraExport,
		const std::filesystem::path& program,
		const std::filesystem::path& exporterScript,
		std::uint64_t maximumBytes)
{
	const std::vector<std::uint8_t> bytes = readFileExact(candidate, maximumBytes);
	if (bytes.empty())
		invalid("candidate is empty");
	const json actual = parseRejectingDuplicateKeys(bytes);
	const Reconstruction expected = reconstruct(loadSources(profile, identity, replay,
			ghidraExport, program, exporterScript));
	if (actual != expected.root)
		invalid("candidate semantics differ from independent source reconstruction");
	const std::string canonical = actual.dump(2) + "\n";
	if (bytes.size() != canonical.size()
			|| !std::equal(bytes.begin(), bytes.end(), canonical.begin()))
		invalid("candidate is not in canonical JSON encoding");
	Sh4GhidraSemanticJoinSummary summary = expected.summary;
	summary.artifactDigest = sha256(bytes.data(), bytes.size());
	return summary;
}

} // namespace research

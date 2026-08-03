#include "research/sh4_ghidra_join.h"

#include "json.hpp"
#include "research/ghidra_export.h"
#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/sh4_profile_artifact.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

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

std::string address(std::uint32_t value)
{
	constexpr char digits[] = "0123456789abcdef";
	std::string result = "0x00000000";
	for (unsigned index = 0; index < 8; ++index)
		result[9 - index] = digits[(value >> (index * 4)) & 0xf];
	return result;
}

std::string opcode(std::uint16_t value)
{
	constexpr char digits[] = "0123456789abcdef";
	std::string result = "0x0000";
	for (unsigned index = 0; index < 4; ++index)
		result[5 - index] = digits[(value >> (index * 4)) & 0xf];
	return result;
}

const char *branchKind(Sh4DynarecBranchKind kind)
{
	switch (kind)
	{
	case Sh4DynarecBranchKind::None: return "none";
	case Sh4DynarecBranchKind::Conditional: return "conditional";
	case Sh4DynarecBranchKind::Call: return "call";
	case Sh4DynarecBranchKind::Jump: return "jump";
	case Sh4DynarecBranchKind::Return: return "return";
	}
	invalid("profile contains an unsupported branch kind");
}

struct FunctionInterval
{
	std::uint64_t start = 0;
	std::uint64_t end = 0;
	std::size_t functionIndex = 0;
};

struct Inputs
{
	IdentityManifest identity;
	GhidraExport ghidra;
	Sh4DynarecProfileArtifactSummary profile;
	Sha256Digest replayDigest {};
	Sha256Digest profileDigest {};
	std::vector<std::uint8_t> program;
	std::vector<FunctionInterval> functionIntervals;
	std::uint64_t programStart = 0;
	std::uint64_t programEnd = 0;
};

Inputs loadInputs(const std::filesystem::path& profilePath,
		const std::filesystem::path& identityPath,
		const std::filesystem::path& replayPath,
		const std::filesystem::path& ghidraExportPath,
		const std::filesystem::path& programPath,
		const std::filesystem::path& exporterScriptPath)
{
	Inputs inputs;
	inputs.identity = loadIdentityManifest(identityPath);
	if (inputs.identity.schemaVersion == 2)
		requireSh4DynarecProfileIdentityV2(inputs.identity);
	else
		requireSh4DynarecProfileRecordIdentityV3(inputs.identity);

	const Sha256Digest& mapleIdentity = inputs.identity.schemaVersion == 2
			? inputs.identity.mapleReplayIdentityDigest : inputs.identity.digest;
	validateProductionMapleTraceFile(replayPath, mapleIdentity);
	inputs.replayDigest = hashFileExact(replayPath, DefaultMaximumMapleTraceBytes);
	inputs.profileDigest = hashFileExact(profilePath,
			DefaultMaximumSh4DynarecProfileArtifactBytes);

	Sh4DynarecProfileArtifactBinding profileBinding;
	profileBinding.identityDigest = inputs.identity.digest;
	profileBinding.replayDigest = inputs.replayDigest;
	profileBinding.configurationDigest = inputs.identity.configurationDigest;
	inputs.profile = validateSh4DynarecProfileArtifact(profilePath, profileBinding);
	if (inputs.profile.incompleteByteBlocks != 0)
		invalid("profile contains blocks without complete exact guest bytes");

	inputs.ghidra = loadGhidraExport(ghidraExportPath);
	requireGhidraExportIdentity(inputs.ghidra, inputs.identity, programPath,
			exporterScriptPath);
	inputs.program = readFileExact(programPath, MaxGhidraExportBytes);
	inputs.programStart = inputs.ghidra.imageBase;
	if (inputs.program.size() > AddressSpaceSize - inputs.programStart)
		invalid("authenticated program range wraps the SH-4 address space");
	inputs.programEnd = inputs.programStart + inputs.program.size();

	for (std::size_t functionIndex = 0;
			functionIndex < inputs.ghidra.functions.size(); ++functionIndex)
	{
		for (const GhidraFunctionBodyRange& range :
				inputs.ghidra.functions[functionIndex].bodyRanges)
		{
			inputs.functionIntervals.push_back(FunctionInterval {range.startAddress,
					std::uint64_t(range.startAddress) + range.length, functionIndex});
		}
	}
	std::sort(inputs.functionIntervals.begin(), inputs.functionIntervals.end(),
			[](const FunctionInterval& left, const FunctionInterval& right) {
				return left.start < right.start;
			});
	return inputs;
}

bool inAuthenticatedProgram(const Inputs& inputs, std::uint64_t start,
		std::uint64_t end)
{
	return start >= inputs.programStart && end >= start && end <= inputs.programEnd;
}

bool overlapsAuthenticatedProgram(const Inputs& inputs, std::uint64_t start,
		std::uint64_t end)
{
	return start < inputs.programEnd && end > inputs.programStart;
}

bool inInitializedExecutable(const Inputs& inputs, std::uint64_t start,
		std::uint64_t end)
{
	for (const GhidraMemoryBlock& block : inputs.ghidra.memoryBlocks)
		if (block.initialized && block.execute && start >= block.startAddress
				&& end <= std::uint64_t(block.startAddress) + block.length)
			return true;
	return false;
}

const FunctionInterval *functionAt(const Inputs& inputs, std::uint64_t point)
{
	auto iterator = std::upper_bound(inputs.functionIntervals.begin(),
			inputs.functionIntervals.end(), point,
			[](std::uint64_t value, const FunctionInterval& interval) {
				return value < interval.start;
			});
	if (iterator == inputs.functionIntervals.begin())
		return nullptr;
	--iterator;
	return point < iterator->end ? &*iterator : nullptr;
}

const FunctionInterval *functionContaining(const Inputs& inputs,
		std::uint64_t start, std::uint64_t end)
{
	const FunctionInterval *interval = functionAt(inputs, start);
	return interval != nullptr && end <= interval->end ? interval : nullptr;
}

struct AddressMapping
{
	std::string classification;
	const GhidraFunction *function = nullptr;
};

AddressMapping mapPoint(const Inputs& inputs, std::uint32_t point)
{
	if (!inAuthenticatedProgram(inputs, point, std::uint64_t(point) + 1))
		return {"runtime-only", nullptr};
	if (!inInitializedExecutable(inputs, point, std::uint64_t(point) + 1))
		return {"authenticated-program-non-executable", nullptr};
	const FunctionInterval *interval = functionAt(inputs, point);
	if (interval == nullptr)
		return {"static-executable-unassigned", nullptr};
	const GhidraFunction& function = inputs.ghidra.functions[interval->functionIndex];
	return {point == function.entryAddress ? "function-entry" : "function-interior",
			&function};
}

AddressMapping mapBlock(const Inputs& inputs, std::uint32_t start,
		std::uint32_t size)
{
	const std::uint64_t end = std::uint64_t(start) + size;
	if (!inAuthenticatedProgram(inputs, start, end))
		return {"runtime-only", nullptr};
	const FunctionInterval *interval = functionContaining(inputs, start, end);
	if (interval == nullptr)
		return {"static-executable-unassigned", nullptr};
	const GhidraFunction& function = inputs.ghidra.functions[interval->functionIndex];
	return {start == function.entryAddress ? "function-entry" : "function-interior",
			&function};
}

json mappingJson(const AddressMapping& mapping)
{
	json result { {"class", mapping.classification} };
	if (mapping.function == nullptr)
		result["function_entry"] = nullptr;
	else
		result["function_entry"] = address(mapping.function->entryAddress);
	return result;
}

json functionJson(const GhidraFunction& function)
{
	json ranges = json::array();
	for (const GhidraFunctionBodyRange& range : function.bodyRanges)
		ranges.push_back({{"start_address", address(range.startAddress)},
				{"length", range.length}});
	return {
		{"entry_address", address(function.entryAddress)},
		{"name", function.name},
		{"namespace", function.nameSpace},
		{"calling_convention", function.callingConvention},
		{"return_type", function.returnType},
		{"parameter_types", function.parameterTypes},
		{"body_ranges", std::move(ranges)},
		{"thunk", function.thunk},
		{"no_return", function.noReturn},
	};
}

struct BuiltJoin
{
	std::vector<std::uint8_t> bytes;
	Sh4GhidraSemanticJoinSummary summary;
};

BuiltJoin buildJoin(const Inputs& inputs, std::uint64_t maximumBytes)
{
	json blocks = json::array();
	json edges = json::array();
	std::set<std::uint32_t> referencedFunctions;
	std::set<std::uint32_t> relevantSymbolAddresses;
	std::map<std::string, std::uint64_t> sourceClassCounts;
	std::map<std::string, std::uint64_t> destinationClassCounts;
	std::map<std::string, std::uint64_t> sourceClassOccurrences;
	std::map<std::string, std::uint64_t> destinationClassOccurrences;
	BuiltJoin built;
	built.summary.identityDigest = inputs.identity.digest;
	built.summary.replayDigest = inputs.replayDigest;
	built.summary.profileDigest = inputs.profileDigest;
	built.summary.profilePayloadDigest = inputs.profile.payloadDigest;
	built.summary.ghidraExportDigest = inputs.ghidra.digest;
	built.summary.programDigest = inputs.ghidra.executableDigest;
	built.summary.blockCount = inputs.profile.blocks.size();
	built.summary.edgeCount = inputs.profile.branches.size();

	for (const Sh4DynarecBlockExecution& block : inputs.profile.blocks)
	{
		const std::uint64_t start = block.definition.virtualAddress;
		const std::uint64_t end = start + block.definition.guestCodeSize;
		const bool inProgram = inAuthenticatedProgram(inputs, start, end);
		if (!inProgram && overlapsAuthenticatedProgram(inputs, start, end))
			invalid("profile block partially overlaps the authenticated program range");
		if (inProgram)
		{
			if (!inInitializedExecutable(inputs, start, end))
				invalid("authenticated program block is outside Ghidra initialized executable memory");
			const std::size_t offset = static_cast<std::size_t>(start - inputs.programStart);
			if (!std::equal(block.definition.guestBytes.begin(),
					block.definition.guestBytes.end(), inputs.program.begin() + offset))
				invalid("profile block bytes differ from the authenticated executable");
			++built.summary.authenticatedProgramBlocks;
		}

		const AddressMapping mapping = mapBlock(inputs,
				block.definition.virtualAddress, block.definition.guestCodeSize);
		if (mapping.function != nullptr)
		{
			++built.summary.functionOwnedBlocks;
			referencedFunctions.insert(mapping.function->entryAddress);
		}
		else if (mapping.classification == "runtime-only")
			++built.summary.runtimeOnlyBlocks;
		else
			++built.summary.staticExecutableUnassignedBlocks;
		relevantSymbolAddresses.insert(block.definition.virtualAddress);

		json branch = nullptr;
		if (block.definition.branchKind != Sh4DynarecBranchKind::None)
		{
			branch = {
				{"kind", branchKind(block.definition.branchKind)},
				{"source", address(block.definition.branchSource)},
				{"opcode", opcode(block.definition.branchOpcode)},
				{"target", block.definition.branchTarget == UINT32_MAX
						? json(nullptr) : json(address(block.definition.branchTarget))},
				{"fallthrough", block.definition.fallthroughTarget == UINT32_MAX
						? json(nullptr) : json(address(block.definition.fallthroughTarget))},
			};
		}
		const Sha256Digest byteDigest = sha256(block.definition.guestBytes.data(),
				block.definition.guestBytes.size());
		blocks.push_back({
			{"generation", block.generation},
			{"virtual_address", address(block.definition.virtualAddress)},
			{"physical_address", address(block.definition.physicalAddress)},
			{"fpu_configuration", block.definition.fpuConfiguration},
			{"guest_code_size", block.definition.guestCodeSize},
			{"guest_cycles", block.definition.guestCycles},
			{"guest_opcodes", block.definition.guestOpcodes},
			{"guest_bytes_sha256", sha256ToHex(byteDigest)},
			{"entered", block.enteredCount},
			{"completed", block.completedCount},
			{"aborted", block.abortedCount},
			{"total_cycles", block.totalCycles},
			{"first_entry_tick", block.firstEntryTick},
			{"last_exit_tick", block.lastExitTick},
			{"branch", std::move(branch)},
			{"mapping", mappingJson(mapping)},
		});
	}

	for (const Sh4DynarecBranchExecution& edge : inputs.profile.branches)
	{
		const AddressMapping source = mapPoint(inputs, edge.source);
		const AddressMapping destination = mapPoint(inputs, edge.destination);
		if (source.function != nullptr)
			referencedFunctions.insert(source.function->entryAddress);
		if (destination.function != nullptr)
			referencedFunctions.insert(destination.function->entryAddress);
		relevantSymbolAddresses.insert(edge.source);
		relevantSymbolAddresses.insert(edge.destination);
		++sourceClassCounts[source.classification];
		++destinationClassCounts[destination.classification];
		sourceClassOccurrences[source.classification] += edge.count;
		destinationClassOccurrences[destination.classification] += edge.count;
		if (built.summary.edgeOccurrences
				> std::numeric_limits<std::uint64_t>::max() - edge.count)
			invalid("edge occurrence total overflowed");
		built.summary.edgeOccurrences += edge.count;
		edges.push_back({
			{"source_generation", edge.sourceGeneration},
			{"source", address(edge.source)},
			{"destination", address(edge.destination)},
			{"opcode", opcode(edge.opcode)},
			{"kind", branchKind(edge.kind)},
			{"taken", edge.taken},
			{"count", edge.count},
			{"first_boundary_tick", edge.firstBoundaryTick},
			{"last_boundary_tick", edge.lastBoundaryTick},
			{"source_mapping", mappingJson(source)},
			{"destination_mapping", mappingJson(destination)},
		});
	}

	json functions = json::array();
	for (const GhidraFunction& function : inputs.ghidra.functions)
		if (referencedFunctions.find(function.entryAddress) != referencedFunctions.end())
		{
			functions.push_back(functionJson(function));
			relevantSymbolAddresses.insert(function.entryAddress);
		}

	json symbols = json::array();
	std::uint32_t currentAddress = 0;
	bool haveCurrentAddress = false;
	json currentSymbols = json::array();
	auto emitSymbols = [&]() {
		if (haveCurrentAddress)
			symbols.push_back({{"address", address(currentAddress)},
					{"symbols", std::move(currentSymbols)}});
		currentSymbols = json::array();
	};
	for (const GhidraSymbol& symbol : inputs.ghidra.symbols)
	{
		if (relevantSymbolAddresses.find(symbol.address) == relevantSymbolAddresses.end())
			continue;
		if (!haveCurrentAddress || symbol.address != currentAddress)
		{
			emitSymbols();
			currentAddress = symbol.address;
			haveCurrentAddress = true;
		}
		currentSymbols.push_back({
			{"name", symbol.name},
			{"namespace", symbol.nameSpace},
			{"kind", symbol.kind},
			{"source", symbol.source},
			{"primary", symbol.primary},
		});
	}
	emitSymbols();

	auto countsJson = [](const std::map<std::string, std::uint64_t>& counts) {
		json result = json::object();
		for (const auto& [name, count] : counts)
			result[name] = count;
		return result;
	};

	json root {
		{"schema", "flycast-research-sh4-ghidra-semantic-join"},
		{"schema_version", Sh4GhidraSemanticJoinSchemaVersion},
		{"evidence_class", "static-dynamic-join"},
		{"bindings", {
			{"identity_sha256", sha256ToHex(inputs.identity.digest)},
			{"replay_sha256", sha256ToHex(inputs.replayDigest)},
			{"profile_sha256", sha256ToHex(inputs.profileDigest)},
			{"profile_payload_sha256", sha256ToHex(inputs.profile.payloadDigest)},
			{"configuration_sha256", sha256ToHex(inputs.identity.configurationDigest)},
			{"ghidra_export_id", inputs.ghidra.exportId},
			{"ghidra_export_sha256", sha256ToHex(inputs.ghidra.digest)},
			{"program_sha256", sha256ToHex(inputs.ghidra.executableDigest)},
			{"exporter_script_sha256", sha256ToHex(inputs.ghidra.exporterScriptDigest)},
			{"image_base", address(inputs.ghidra.imageBase)},
			{"program_size", inputs.program.size()},
		}},
		{"coverage", {
			{"block_count", built.summary.blockCount},
			{"edge_count", built.summary.edgeCount},
			{"edge_occurrences", built.summary.edgeOccurrences},
			{"authenticated_program_blocks", built.summary.authenticatedProgramBlocks},
			{"exact_byte_blocks", built.summary.authenticatedProgramBlocks},
			{"function_owned_blocks", built.summary.functionOwnedBlocks},
			{"static_executable_unassigned_blocks",
					built.summary.staticExecutableUnassignedBlocks},
			{"runtime_only_blocks", built.summary.runtimeOnlyBlocks},
			{"edge_source_classes", countsJson(sourceClassCounts)},
			{"edge_destination_classes", countsJson(destinationClassCounts)},
			{"edge_source_occurrences", countsJson(sourceClassOccurrences)},
			{"edge_destination_occurrences", countsJson(destinationClassOccurrences)},
		}},
		{"functions", std::move(functions)},
		{"symbol_addresses", std::move(symbols)},
		{"blocks", std::move(blocks)},
		{"edges", std::move(edges)},
	};
	const std::string text = root.dump(2) + "\n";
	if (text.size() > maximumBytes)
		invalid("canonical artifact exceeds its configured byte bound");
	built.bytes.assign(text.begin(), text.end());
	built.summary.artifactDigest = sha256(built.bytes.data(), built.bytes.size());
	return built;
}

class ExclusiveFile
{
public:
	explicit ExclusiveFile(const std::filesystem::path& path)
	{
		std::error_code error;
		if (path.parent_path().empty()
				|| !std::filesystem::is_directory(path.parent_path(), error) || error)
			invalid("output directory does not exist");
#ifdef _WIN32
		handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
				FILE_ATTRIBUTE_NORMAL, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			invalid("cannot create output exclusively");
#else
		fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
		if (fd < 0)
			invalid("cannot create output exclusively");
#endif
	}
	~ExclusiveFile()
	{
#ifdef _WIN32
		if (handle != INVALID_HANDLE_VALUE)
			CloseHandle(handle);
#else
		if (fd >= 0)
			::close(fd);
#endif
	}
	void write(const std::vector<std::uint8_t>& bytes)
	{
		const std::uint8_t *data = bytes.data();
		std::size_t remaining = bytes.size();
		while (remaining != 0)
		{
#ifdef _WIN32
			const DWORD chunk = static_cast<DWORD>((std::min<std::size_t>)(remaining,
					std::numeric_limits<DWORD>::max()));
			DWORD written = 0;
			if (!WriteFile(handle, data, chunk, &written, nullptr) || written != chunk)
				invalid("cannot write output");
#else
			const ssize_t written = ::write(fd, data, remaining);
			if (written <= 0)
				invalid("cannot write output");
#endif
			data += written;
			remaining -= static_cast<std::size_t>(written);
		}
#ifdef _WIN32
		if (!FlushFileBuffers(handle))
			invalid("cannot flush output");
#else
		if (::fsync(fd) != 0)
			invalid("cannot flush output");
#endif
	}
private:
#ifdef _WIN32
	HANDLE handle = INVALID_HANDLE_VALUE;
#else
	int fd = -1;
#endif
	};

} // namespace

Sh4GhidraSemanticJoinSummary writeSh4GhidraSemanticJoin(
		const std::filesystem::path& output,
		const std::filesystem::path& profile,
		const std::filesystem::path& identity,
		const std::filesystem::path& replay,
		const std::filesystem::path& ghidraExport,
		const std::filesystem::path& program,
		const std::filesystem::path& exporterScript,
		std::uint64_t maximumBytes)
{
	const BuiltJoin built = buildJoin(loadInputs(profile, identity, replay,
			ghidraExport, program, exporterScript), maximumBytes);
	ExclusiveFile outputFile(output);
	outputFile.write(built.bytes);
	return built.summary;
}

} // namespace research

#include "research/ghidra_export.h"

#include "research/sh4_events_manifest.h"
#include "research/memory_ranges_manifest.h"

#include "json.hpp"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace research
{
namespace
{

using json = nlohmann::json;
constexpr std::uint64_t AddressSpaceSize = std::uint64_t {1} << 32;
constexpr std::size_t MaxShortText = 4096;
constexpr std::size_t MaxDefinitionText = 65536;

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid Ghidra export: " + reason);
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

std::string requiredString(const json& parent, const char *name, const std::string& field,
		std::size_t maximum = MaxShortText, bool allowEmpty = false)
{
	if (!parent.contains(name) || !parent.at(name).is_string())
		invalid(field + "." + name + " must be a string");
	const std::string value = parent.at(name).get<std::string>();
	if ((!allowEmpty && value.empty()) || value.size() > maximum)
		invalid(field + "." + name + " has an invalid length");
	return value;
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

bool validIdentifier(const std::string& value)
{
	if (value.empty() || value.size() > 128)
		return false;
	const char first = value.front();
	if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z')
			|| (first >= '0' && first <= '9')))
		return false;
	for (const char character : value)
		if (!((character >= 'a' && character <= 'z')
				|| (character >= 'A' && character <= 'Z')
				|| (character >= '0' && character <= '9')
				|| character == '.' || character == '_' || character == '-'))
			return false;
	return true;
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

struct MemoryBlock
{
	std::uint64_t start = 0;
	std::uint64_t end = 0;
	std::string name;
	bool initialized = false;
	bool executable = false;
};

bool intervalContained(const std::vector<MemoryBlock>& blocks, std::uint64_t start,
		std::uint64_t end, bool requireInitialized, bool requireExecutable)
{
	for (const MemoryBlock& block : blocks)
		if (start >= block.start && end <= block.end
				&& (!requireInitialized || block.initialized)
				&& (!requireExecutable || block.executable))
			return true;
	return false;
}

GhidraExport validateExport(const json& root)
{
	requireAllowedKeys(root, {"schema", "schema_version", "export_id", "evidence_class",
			"producer", "program", "memory_blocks", "functions", "symbols", "data_types",
			"counts"}, "root");
	if (requiredString(root, "schema", "root") != "flycast-research-ghidra-export"
			|| requiredUnsigned(root, "schema_version", "root") != 1)
		invalid("unsupported schema or schema_version");
	if (requiredString(root, "evidence_class", "root") != "static-analysis")
		invalid("root.evidence_class must be static-analysis");

	GhidraExport result;
	result.exportId = requiredString(root, "export_id", "root", 128);
	if (!validIdentifier(result.exportId))
		invalid("root.export_id is not a valid identifier");

	const json& producer = requiredObject(root, "producer", "root");
	requireAllowedKeys(producer, {"name", "version", "exporter_id", "exporter_sha256"},
			"root.producer");
	if (requiredString(producer, "name", "root.producer") != "Ghidra")
		invalid("root.producer.name must be Ghidra");
	result.ghidraVersion = requiredString(producer, "version", "root.producer", 64);
	if (requiredString(producer, "exporter_id", "root.producer")
			!= "flycast-ghidra-export-v1")
		invalid("root.producer.exporter_id is unsupported");
	result.exporterScriptDigest = parseDigest(requiredString(producer, "exporter_sha256",
			"root.producer", 64), "root.producer.exporter_sha256");

	const json& program = requiredObject(root, "program", "root");
	requireAllowedKeys(program, {"name", "executable_sha256", "executable_size",
			"language_id", "compiler_spec_id", "endian", "address_size", "address_space",
			"ghidra_image_base", "image_base", "image_base_source", "minimum_address",
			"maximum_address"}, "root.program");
	result.programName = requiredString(program, "name", "root.program");
	result.executableDigest = parseDigest(requiredString(program, "executable_sha256",
			"root.program", 64), "root.program.executable_sha256");
	result.executableSize = requiredUnsigned(program, "executable_size", "root.program");
	if (result.executableSize == 0)
		invalid("root.program.executable_size must be positive");
	if (requiredString(program, "language_id", "root.program")
			!= "SuperH4:LE:32:default")
		invalid("root.program.language_id is not the authoritative Dreamcast SH-4 language");
	requiredString(program, "compiler_spec_id", "root.program", 256);
	if (requiredString(program, "endian", "root.program") != "little"
			|| requiredUnsigned(program, "address_size", "root.program") != 32)
		invalid("root.program must be 32-bit little-endian");
	requiredString(program, "address_space", "root.program", 128);
	const std::uint32_t ghidraImageBase = parseAddress(
			requiredString(program, "ghidra_image_base", "root.program"),
			"root.program.ghidra_image_base");
	result.imageBase = parseAddress(requiredString(program, "image_base", "root.program"),
			"root.program.image_base");
	const std::string imageBaseSource = requiredString(program, "image_base_source",
			"root.program", 64);
	if (imageBaseSource != "ghidra-program"
			&& imageBaseSource != "minimum-initialized-executable-block")
		invalid("root.program.image_base_source is unsupported");
	const std::uint32_t minimumAddress = parseAddress(
			requiredString(program, "minimum_address", "root.program"),
			"root.program.minimum_address");
	const std::uint32_t maximumAddress = parseAddress(
			requiredString(program, "maximum_address", "root.program"),
			"root.program.maximum_address");
	if (minimumAddress > maximumAddress)
		invalid("root.program address bounds are inconsistent");

	const json& blocks = requiredArray(root, "memory_blocks", "root");
	if (blocks.empty() || blocks.size() > MaxGhidraMemoryBlocks)
		invalid("root.memory_blocks count is outside [1, 4096]");
	std::vector<MemoryBlock> parsedBlocks;
	parsedBlocks.reserve(blocks.size());
	for (std::size_t index = 0; index < blocks.size(); ++index)
	{
		const json& block = blocks.at(index);
		const std::string field = "root.memory_blocks[" + std::to_string(index) + "]";
		requireAllowedKeys(block, {"name", "start_address", "length", "read", "write",
				"execute", "initialized"}, field);
		MemoryBlock parsed;
		parsed.name = requiredString(block, "name", field);
		parsed.start = parseAddress(requiredString(block, "start_address", field),
				field + ".start_address");
		const std::uint64_t length = requiredUnsigned(block, "length", field);
		if (length == 0 || length > AddressSpaceSize - parsed.start)
			invalid(field + " has an empty or wrapping range");
		parsed.end = parsed.start + length;
		requiredBool(block, "read", field);
		requiredBool(block, "write", field);
		parsed.executable = requiredBool(block, "execute", field);
		parsed.initialized = requiredBool(block, "initialized", field);
		if (!parsedBlocks.empty())
		{
			const MemoryBlock& previous = parsedBlocks.back();
			if (std::tie(parsed.start, parsed.name) <= std::tie(previous.start, previous.name))
				invalid("root.memory_blocks are not in canonical start/name order");
			if (parsed.start < previous.end)
				invalid("root.memory_blocks overlap");
		}
		parsedBlocks.push_back(std::move(parsed));
	}
	if (parsedBlocks.front().start != minimumAddress
			|| parsedBlocks.back().end - 1 != maximumAddress)
		invalid("root.program min/max addresses do not match memory blocks");
	if (!intervalContained(parsedBlocks, result.imageBase,
			static_cast<std::uint64_t>(result.imageBase) + 1, true, true))
		invalid("root.program.image_base is not inside initialized executable memory");
	std::uint64_t firstInitializedExecutable = AddressSpaceSize;
	for (const MemoryBlock& block : parsedBlocks)
		if (block.initialized && block.executable)
			firstInitializedExecutable = (std::min)(firstInitializedExecutable, block.start);
	if (imageBaseSource == "ghidra-program" && result.imageBase != ghidraImageBase)
		invalid("ghidra-program image-base source does not match ghidra_image_base");
	if (imageBaseSource == "minimum-initialized-executable-block"
			&& result.imageBase != firstInitializedExecutable)
		invalid("derived image base is not the lowest initialized executable block");
	if (imageBaseSource == "minimum-initialized-executable-block"
			&& intervalContained(parsedBlocks, ghidraImageBase,
					static_cast<std::uint64_t>(ghidraImageBase) + 1, true, true))
		invalid("derived image-base source is invalid when ghidra_image_base is executable");
	result.memoryBlockCount = parsedBlocks.size();

	const json& functions = requiredArray(root, "functions", "root");
	if (functions.empty() || functions.size() > MaxGhidraFunctions)
		invalid("root.functions count is outside [1, 250000]");
	std::vector<std::pair<std::uint64_t, std::uint64_t>> allFunctionRanges;
	std::uint64_t previousFunctionEntry = 0;
	bool havePreviousFunction = false;
	std::size_t bodyRangeCount = 0;
	for (std::size_t index = 0; index < functions.size(); ++index)
	{
		const json& function = functions.at(index);
		const std::string field = "root.functions[" + std::to_string(index) + "]";
		requireAllowedKeys(function, {"entry_address", "name", "namespace",
				"calling_convention", "return_type", "parameter_types", "body_ranges", "thunk",
				"no_return"}, field);
		const std::uint64_t entry = parseAddress(requiredString(function, "entry_address", field),
				field + ".entry_address");
		if ((entry & 1u) != 0 || (havePreviousFunction && entry <= previousFunctionEntry))
			invalid("root.functions are not uniquely ordered by even entry address");
		havePreviousFunction = true;
		previousFunctionEntry = entry;
		requiredString(function, "name", field);
		requiredString(function, "namespace", field, MaxShortText, true);
		requiredString(function, "calling_convention", field, 256);
		requiredString(function, "return_type", field);
		requiredBool(function, "thunk", field);
		requiredBool(function, "no_return", field);
		const json& parameters = requiredArray(function, "parameter_types", field);
		if (parameters.size() > MaxGhidraFunctionParameters)
			invalid(field + ".parameter_types has more than 64 entries");
		for (const json& parameter : parameters)
			if (!parameter.is_string() || parameter.get<std::string>().empty()
					|| parameter.get<std::string>().size() > MaxShortText)
				invalid(field + ".parameter_types contains an invalid type");
		const json& ranges = requiredArray(function, "body_ranges", field);
		if (ranges.empty())
			invalid(field + ".body_ranges is empty");
		bool entryCovered = false;
		std::uint64_t previousEnd = 0;
		for (std::size_t rangeIndex = 0; rangeIndex < ranges.size(); ++rangeIndex)
		{
			if (++bodyRangeCount > MaxGhidraFunctionBodyRanges)
				invalid("function body range count exceeds 1000000");
			const json& range = ranges.at(rangeIndex);
			const std::string rangeField = field + ".body_ranges["
					+ std::to_string(rangeIndex) + "]";
			requireAllowedKeys(range, {"start_address", "length"}, rangeField);
			const std::uint64_t start = parseAddress(
					requiredString(range, "start_address", rangeField),
					rangeField + ".start_address");
			const std::uint64_t length = requiredUnsigned(range, "length", rangeField);
			if ((start & 1u) != 0 || length == 0 || (length & 1u) != 0
					|| length > AddressSpaceSize - start)
				invalid(rangeField + " is not an even, non-empty, non-wrapping SH-4 range");
			const std::uint64_t end = start + length;
			if (rangeIndex != 0 && start < previousEnd)
				invalid(field + ".body_ranges are not ordered and disjoint");
			if (!intervalContained(parsedBlocks, start, end, true, true))
				invalid(rangeField + " is outside initialized executable memory");
			entryCovered = entryCovered || (entry >= start && entry < end);
			previousEnd = end;
			allFunctionRanges.emplace_back(start, end);
		}
		if (!entryCovered)
			invalid(field + ".entry_address is outside its body");
	}
	std::sort(allFunctionRanges.begin(), allFunctionRanges.end());
	for (std::size_t index = 1; index < allFunctionRanges.size(); ++index)
		if (allFunctionRanges[index].first < allFunctionRanges[index - 1].second)
			invalid("function bodies overlap");
	result.functionCount = functions.size();

	const json& symbols = requiredArray(root, "symbols", "root");
	if (symbols.empty() || symbols.size() > MaxGhidraSymbols)
		invalid("root.symbols count is outside [1, 1000000]");
	std::tuple<std::uint64_t, std::string, std::string, std::string> previousSymbol;
	bool havePreviousSymbol = false;
	for (std::size_t index = 0; index < symbols.size(); ++index)
	{
		const json& symbol = symbols.at(index);
		const std::string field = "root.symbols[" + std::to_string(index) + "]";
		requireAllowedKeys(symbol, {"address", "name", "namespace", "kind", "source",
				"primary"}, field);
		const std::uint64_t address = parseAddress(requiredString(symbol, "address", field),
				field + ".address");
		const std::string name = requiredString(symbol, "name", field);
		const std::string nameSpace = requiredString(symbol, "namespace", field,
				MaxShortText, true);
		const std::string kind = requiredString(symbol, "kind", field, 128);
		const std::string source = requiredString(symbol, "source", field, 128);
		if (!validIdentifier(kind) || !validIdentifier(source))
			invalid(field + " kind/source is not a valid identifier");
		requiredBool(symbol, "primary", field);
		if (!intervalContained(parsedBlocks, address, address + 1, false, false))
			invalid(field + ".address is outside exported memory");
		const auto key = std::make_tuple(address, kind, nameSpace, name);
		if (havePreviousSymbol && key <= previousSymbol)
			invalid("root.symbols are not uniquely ordered by address/kind/namespace/name");
		havePreviousSymbol = true;
		previousSymbol = key;
	}
	result.symbolCount = symbols.size();

	const json& dataTypes = requiredArray(root, "data_types", "root");
	if (dataTypes.size() > MaxGhidraDataTypes)
		invalid("root.data_types count exceeds 100000");
	std::string previousTypePath;
	for (std::size_t index = 0; index < dataTypes.size(); ++index)
	{
		const json& dataType = dataTypes.at(index);
		const std::string field = "root.data_types[" + std::to_string(index) + "]";
		requireAllowedKeys(dataType, {"path", "kind", "length", "dynamic", "definition"},
				field);
		const std::string path = requiredString(dataType, "path", field);
		if (index != 0 && path <= previousTypePath)
			invalid("root.data_types are not uniquely ordered by path");
		previousTypePath = path;
		const std::string kind = requiredString(dataType, "kind", field, 128);
		if (!validIdentifier(kind))
			invalid(field + ".kind is not a valid identifier");
		if (requiredUnsigned(dataType, "length", field) > AddressSpaceSize)
			invalid(field + ".length exceeds the 32-bit address-space size");
		requiredBool(dataType, "dynamic", field);
		requiredString(dataType, "definition", field, MaxDefinitionText);
	}
	result.dataTypeCount = dataTypes.size();

	const json& counts = requiredObject(root, "counts", "root");
	requireAllowedKeys(counts, {"memory_blocks", "functions", "symbols", "data_types"},
			"root.counts");
	if (requiredUnsigned(counts, "memory_blocks", "root.counts") != result.memoryBlockCount
			|| requiredUnsigned(counts, "functions", "root.counts") != result.functionCount
			|| requiredUnsigned(counts, "symbols", "root.counts") != result.symbolCount
			|| requiredUnsigned(counts, "data_types", "root.counts") != result.dataTypeCount)
		invalid("root.counts do not match exported arrays");
	return result;
}

std::uint64_t fileSize(const std::filesystem::path& path)
{
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error || size > std::numeric_limits<std::uint64_t>::max())
		throw std::runtime_error("cannot stat static-analysis input '" + path.string() + "'");
	return static_cast<std::uint64_t>(size);
}

} // namespace

GhidraExport loadGhidraExport(const std::filesystem::path& path)
{
	std::vector<std::uint8_t> bytes = readFileExact(path, MaxGhidraExportBytes);
	if (bytes.empty())
		invalid("file is empty");
	GhidraExport result;
	try
	{
		result = validateExport(parseRejectingDuplicateKeys(bytes));
	}
	catch (const nlohmann::json::exception& exception)
	{
		invalid(std::string("JSON parse/type error: ") + exception.what());
	}
	result.path = path;
	result.bytes = std::move(bytes);
	result.digest = sha256(result.bytes.data(), result.bytes.size());
	return result;
}

void requireGhidraExportIdentity(const GhidraExport& exportArtifact,
		const IdentityManifest& identity, const std::filesystem::path& executable,
		const std::filesystem::path& exporterScript)
{
	if (!identity.hasStaticAnalysis)
		invalid("identity has no static_analysis binding");
	if (!sha256Equal(exportArtifact.executableDigest, identity.bootExecutableDigest)
			|| !sha256Equal(exportArtifact.executableDigest,
					identity.staticAnalysisProgramDigest))
		invalid("program digest does not match identity boot/static-analysis identity");
	if (!sha256Equal(exportArtifact.digest, identity.staticAnalysisExportDigest))
		invalid("exact export bytes do not match identity static_analysis.export_sha256");
	if (exportArtifact.imageBase != identity.staticAnalysisImageBase)
		invalid("program image base does not match identity static_analysis.image_base");
	if (fileSize(executable) != exportArtifact.executableSize
			|| !sha256Equal(hashFileExact(executable, exportArtifact.executableSize),
					exportArtifact.executableDigest))
		invalid("supplied executable does not match exported program identity");
	if (!sha256Equal(hashFileExact(exporterScript, 16 * 1024 * 1024),
			exportArtifact.exporterScriptDigest))
		invalid("supplied exporter script does not match producer identity");
}

void requireGhidraExportSh4Join(const GhidraExport& exportArtifact,
		const Sh4EventsManifest& manifest)
{
	if (manifest.bindings.staticAnalysisId != exportArtifact.exportId)
		invalid("SH-4 manifest static_analysis_id does not match export_id");
	if (!sha256Equal(manifest.bindings.staticAnalysisDigest, exportArtifact.digest))
		invalid("SH-4 manifest static_analysis_sha256 does not match exact export bytes");
	if (!sha256Equal(manifest.bindings.executableDigest, exportArtifact.executableDigest))
		invalid("SH-4 manifest executable_sha256 does not match exported program");
}

void requireGhidraExportMemoryRangesJoin(const GhidraExport& exportArtifact,
		const MemoryRangesManifest& manifest)
{
	if (manifest.bindings.staticAnalysisId != exportArtifact.exportId)
		invalid("memory-ranges manifest static_analysis_id does not match export_id");
	if (!sha256Equal(manifest.bindings.staticAnalysisDigest, exportArtifact.digest))
		invalid("memory-ranges manifest static_analysis_sha256 does not match exact export bytes");
	if (!sha256Equal(manifest.bindings.executableDigest, exportArtifact.executableDigest))
		invalid("memory-ranges manifest executable_sha256 does not match exported program");
}

} // namespace research

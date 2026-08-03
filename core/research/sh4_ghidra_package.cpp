#include "research/sh4_ghidra_package.h"

#include "research/identity_manifest.h"
#include "research/sh4_ghidra_join.h"
#include "research/sha256.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace research
{
namespace
{

using json = nlohmann::json;

struct Blob
{
	std::string path;
	std::uint64_t size = 0;
	Sha256Digest digest {};
};

struct Validated
{
	std::filesystem::path root;
	std::filesystem::path accepted;
	std::string packageId;
	std::vector<Blob> lockedEntries;
	Blob packagedValidator;
	json receipt;
};

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid SH-4/Ghidra package v1: " + reason);
}

void require(bool condition, const std::string& reason)
{
	if (!condition)
		invalid(reason);
}

void keys(const json& value, const std::set<std::string>& expected,
		const std::string& field)
{
	require(value.is_object(), field + " must be an object");
	for (const auto& item : value.items())
		require(expected.count(item.key()) != 0,
				field + " has unknown field '" + item.key() + "'");
	for (const std::string& name : expected)
		require(value.contains(name), field + "." + name + " is missing");
}

std::string text(const json& value, const char *name, const std::string& field,
		std::size_t maximum = 4096)
{
	require(value.contains(name) && value.at(name).is_string(),
			field + "." + name + " must be a string");
	const std::string result = value.at(name).get<std::string>();
	require(!result.empty() && result.size() <= maximum,
			field + "." + name + " length is invalid");
	return result;
}

std::uint64_t number(const json& value, const char *name,
		const std::string& field)
{
	require(value.contains(name) && value.at(name).is_number_unsigned(),
			field + "." + name + " must be unsigned");
	return value.at(name).get<std::uint64_t>();
}

bool uuid(const std::string& value)
{
	if (value.size() != 36)
		return false;
	for (std::size_t index = 0; index < value.size(); ++index)
	{
		if (index == 8 || index == 13 || index == 18 || index == 23)
		{
			if (value[index] != '-')
				return false;
		}
		else if (!((value[index] >= '0' && value[index] <= '9')
				|| (value[index] >= 'a' && value[index] <= 'f')))
			return false;
	}
	return true;
}

bool safeEntry(const std::string& value)
{
	if (value.empty() || value.size() > 4096 || value.front() == '/'
			|| value.front() == '\\' || value.find('\\') != std::string::npos)
		return false;
	const std::filesystem::path path = std::filesystem::u8path(value);
	if (path.is_absolute() || path.has_root_path())
		return false;
	for (const auto& component : path)
	{
		const std::string item = component.u8string();
		if (item.empty() || item == "." || item == ".."
				|| !std::all_of(item.begin(), item.end(), [](unsigned char character) {
					return std::isalnum(character) || character == '.'
							|| character == '_' || character == '-';
				}))
			return false;
	}
	return true;
}

std::filesystem::path canonical(const std::filesystem::path& path,
		const std::string& field)
{
	std::error_code error;
	const auto absolute = std::filesystem::absolute(path, error);
	require(!error, field + " cannot be made absolute");
	const auto result = std::filesystem::weakly_canonical(absolute, error);
	require(!error && result.is_absolute(), field + " cannot be canonicalized");
	return result;
}

bool samePath(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
{
	std::error_code leftError, rightError;
	auto left = std::filesystem::weakly_canonical(lhs, leftError).native();
	auto right = std::filesystem::weakly_canonical(rhs, rightError).native();
	if (leftError || rightError)
		return false;
#ifdef _WIN32
	if (left.size() > INT_MAX || right.size() > INT_MAX)
		return false;
	return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
			right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
#else
	return left == right;
#endif
}

Blob fileIdentity(const std::filesystem::path& path, const std::string& pathText,
		const std::string& field)
{
	std::error_code error;
	const auto status = std::filesystem::symlink_status(path, error);
	require(!error && std::filesystem::is_regular_file(status)
			&& !std::filesystem::is_symlink(status),
			field + " is missing, linked, or non-regular");
	const std::uintmax_t raw = std::filesystem::file_size(path, error);
	require(!error && raw != 0
			&& raw <= std::numeric_limits<std::uint64_t>::max(),
			field + " size is invalid");
	const auto size = static_cast<std::uint64_t>(raw);
	return {pathText, size, hashFileExact(path, size)};
}

void equal(const Blob& actual, const Blob& expected, const std::string& field)
{
	require(actual.size == expected.size
			&& sha256Equal(actual.digest, expected.digest),
			field + " bytes differ from their declaration");
}

json blobJson(const Blob& value)
{
	return {{"path", value.path}, {"size", value.size},
			{"sha256", sha256ToHex(value.digest)}};
}

json parseJson(const std::filesystem::path& path, const std::string& field)
{
	const auto bytes = readFileExact(path, MaxSh4GhidraPackageJsonBytes);
	require(!bytes.empty(), field + " is empty");
	std::vector<std::set<std::string>> objectKeys;
	auto callback = [&objectKeys, &field](int depth, json::parse_event_t event,
			json& parsed) {
		require(depth >= 0 && depth <= 64, field + " JSON depth is invalid");
		const auto level = static_cast<std::size_t>(depth);
		if (event == json::parse_event_t::object_start)
		{
			if (objectKeys.size() <= level)
				objectKeys.resize(level + 1);
			objectKeys[level].clear();
		}
		else if (event == json::parse_event_t::key)
		{
			require(level != 0 && objectKeys.size() >= level && parsed.is_string(),
					field + " parser key depth is invalid");
			require(objectKeys[level - 1].insert(parsed.get<std::string>()).second,
					field + " contains a duplicate object key");
		}
		return true;
	};
	try
	{
		return json::parse(bytes.begin(), bytes.end(), callback);
	}
	catch (const json::exception& exception)
	{
		invalid(field + " JSON parse error: " + exception.what());
	}
}

Blob declaredBlob(const json& value, const std::string& field,
		bool absolutePath)
{
	keys(value, {"path", "size", "sha256"}, field);
	Blob result;
	result.path = text(value, "path", field);
	if (absolutePath)
		require(std::filesystem::u8path(result.path).is_absolute(),
				field + ".path must be absolute");
	result.size = number(value, "size", field);
	require(result.size != 0, field + ".size must be positive");
	const std::string digest = text(value, "sha256", field, 64);
	require(sha256FromHex(digest, result.digest)
			&& sha256ToHex(result.digest) == digest,
			field + ".sha256 must be lowercase SHA-256");
	return result;
}

void packaged(const std::filesystem::path& root, const std::string& name,
		const Blob& source, const std::string& field)
{
	equal(fileIdentity(root / name, name, field), source, field);
}

std::set<std::string> inventory(const std::filesystem::path& root)
{
	std::error_code error;
	std::set<std::string> files;
	for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
			iterator != end; iterator.increment(error))
	{
		require(!error, "cannot enumerate package");
		const auto status = iterator->symlink_status(error);
		require(!error && !std::filesystem::is_symlink(status),
				"package contains a linked entry");
		const std::string relative = iterator->path().lexically_relative(root)
				.generic_u8string();
		require(!std::filesystem::is_directory(status),
				"package contains an unexpected directory");
		require(std::filesystem::is_regular_file(status) && safeEntry(relative)
				&& files.insert(relative).second,
				"package contains an invalid entry: " + relative);
	}
	require(!error, "cannot enumerate package");
	return files;
}

void setBlobPath(json& root, const json::json_pointer& pointer,
		const std::string& path)
{
	require(root.contains(pointer) && root.at(pointer).is_object()
			&& root.at(pointer).contains("path"),
			"source job is missing a required blob mapping");
	root.at(pointer).at("path") = path;
}

bool requireJob(const json& job, const std::string& field)
{
	std::set<std::string> expected {"schema", "schema_version", "package_id",
			"output", "identity", "emulator", "maple_replay", "profile",
			"ghidra_export", "program", "exporter_script", "semantic_join",
			"publisher", "semantic_validator", "package_validator", "limits",
			"metadata"};
	const bool hasInitialState = job.contains("initial_state");
	if (hasInitialState)
		expected.insert("initial_state");
	keys(job, expected, field);
	require(job.at("schema") == "flycast-research-sh4-ghidra-package-job"
			&& job.at("schema_version").is_number_unsigned()
			&& job.at("schema_version").get<std::uint64_t>() == 1,
			"unsupported job schema");
	keys(job.at("output"), {"accepted_directory"}, field + ".output");
	keys(job.at("profile"), {"candidate"}, field + ".profile");
	keys(job.at("semantic_join"), {"candidate", "maximum_bytes"},
			field + ".semantic_join");
	keys(job.at("publisher"), {"script"}, field + ".publisher");
	keys(job.at("semantic_validator"), {"executable"},
			field + ".semantic_validator");
	keys(job.at("package_validator"), {"executable"},
			field + ".package_validator");
	keys(job.at("limits"), {"validator_timeout_seconds"}, field + ".limits");
	require(job.at("metadata").is_object() && job.at("metadata").size() <= 64,
			field + ".metadata must be a bounded object");
	return hasInitialState;
}

void requireLocalizedJob(const std::filesystem::path& root,
		bool hasInitialState)
{
	json expected = parseJson(root / "source-job.json", "source job");
	const json localized = parseJson(root / "job.json", "localized job");
	setBlobPath(expected, json::json_pointer("/identity"), "identity.json");
	if (hasInitialState)
		setBlobPath(expected, json::json_pointer("/initial_state"),
				"initial-state.state");
	setBlobPath(expected, json::json_pointer("/emulator"), "emulator.bin");
	setBlobPath(expected, json::json_pointer("/maple_replay"), "maple-replay.fcmt");
	setBlobPath(expected, json::json_pointer("/profile/candidate"),
			"sh4-profile.fcsh4profile");
	setBlobPath(expected, json::json_pointer("/ghidra_export"), "ghidra-export.json");
	setBlobPath(expected, json::json_pointer("/program"), "program.bin");
	setBlobPath(expected, json::json_pointer("/exporter_script"),
			"exporter-script.java");
	setBlobPath(expected, json::json_pointer("/semantic_join/candidate"),
			"sh4-ghidra-join.json");
	setBlobPath(expected, json::json_pointer("/publisher/script"), "publisher.ps1");
	setBlobPath(expected, json::json_pointer("/semantic_validator/executable"),
			"semantic-validator.exe");
	setBlobPath(expected, json::json_pointer("/package_validator/executable"),
			"package-validator.exe");
	require(localized == expected,
			"localized job differs from source job outside fixed blob paths");
}

Validated validate(const std::filesystem::path& package, bool issuing)
{
	Validated result;
	result.root = canonical(package, "package");
	std::error_code error;
	const auto rootStatus = std::filesystem::symlink_status(result.root, error);
	require(!error && std::filesystem::is_directory(rootStatus)
			&& !std::filesystem::is_symlink(rootStatus),
			"package is missing, linked, or not a directory");

	const json manifest = parseJson(result.root / "package.json", "package manifest");
	keys(manifest, {"schema", "schema_version", "package_id", "locked_entries"},
			"package manifest");
	require(manifest.at("schema") == "flycast-research-sh4-ghidra-package"
			&& manifest.at("schema_version").is_number_unsigned()
			&& manifest.at("schema_version").get<std::uint64_t>() == 1,
			"unsupported package manifest schema");
	result.packageId = text(manifest, "package_id", "package manifest", 36);
	require(uuid(result.packageId), "package_id is not a lowercase UUID");
	require(manifest.at("locked_entries").is_array()
			&& !manifest.at("locked_entries").empty()
			&& manifest.at("locked_entries").size() <= 32,
			"locked_entries count is outside [1, 32]");
	std::string previous;
	for (std::size_t index = 0; index < manifest.at("locked_entries").size(); ++index)
	{
		Blob entry = declaredBlob(manifest.at("locked_entries").at(index),
				"package manifest locked entry", false);
		require(safeEntry(entry.path) && entry.path != "package.json"
				&& entry.path != "package-validation.json"
				&& (previous.empty() || entry.path > previous),
				"locked entry paths are unsafe or not uniquely ordered");
		previous = entry.path;
		result.lockedEntries.push_back(std::move(entry));
	}
	std::set<std::string> expected {"package.json"};
	if (!issuing)
		expected.insert("package-validation.json");
	for (const Blob& entry : result.lockedEntries)
		expected.insert(entry.path);
	require(inventory(result.root) == expected,
			"package entries do not exactly match package manifest");
	for (const Blob& entry : result.lockedEntries)
		equal(fileIdentity(result.root / entry.path, entry.path,
				"locked entry " + entry.path), entry, "locked entry " + entry.path);

	const json sourceJob = parseJson(result.root / "source-job.json", "source job");
	const bool hasInitialState = requireJob(sourceJob, "source job");
	require(text(sourceJob, "package_id", "source job", 36) == result.packageId,
			"source job package_id differs from package manifest");
	std::set<std::string> mandatory {"source-job.json", "job.json", "identity.json",
			"emulator.bin", "maple-replay.fcmt", "sh4-profile.fcsh4profile",
			"ghidra-export.json", "program.bin", "exporter-script.java",
			"sh4-ghidra-join.json", "publisher.ps1", "semantic-validator.exe",
			"package-validator.exe"};
	if (hasInitialState)
		mandatory.insert("initial-state.state");
	std::set<std::string> lockedNames;
	for (const Blob& entry : result.lockedEntries)
		lockedNames.insert(entry.path);
	for (const std::string& name : mandatory)
		require(lockedNames.count(name) == 1,
				"package is missing mandatory locked entry " + name);
	keys(sourceJob.at("output"), {"accepted_directory"}, "source job.output");
	result.accepted = canonical(std::filesystem::u8path(text(sourceJob.at("output"),
			"accepted_directory", "source job.output")), "accepted directory");
	const auto candidate = result.accepted.parent_path()
			/ (".flycast-research-sh4-ghidra-candidate-" + result.packageId);
	if (issuing)
		require(samePath(result.root, candidate)
				&& !std::filesystem::exists(result.accepted),
				"receipt may only be issued in the private candidate");
	else
		require(samePath(result.root, result.accepted),
				"published package is not at its accepted path");

	requireLocalizedJob(result.root, hasInitialState);
	const Blob identitySource = declaredBlob(sourceJob.at("identity"),
			"source job.identity", true);
	Blob initialStateSource;
	if (hasInitialState)
		initialStateSource = declaredBlob(sourceJob.at("initial_state"),
				"source job.initial_state", true);
	const Blob emulatorSource = declaredBlob(sourceJob.at("emulator"),
			"source job.emulator", true);
	const Blob replaySource = declaredBlob(sourceJob.at("maple_replay"),
			"source job.maple_replay", true);
	const Blob profileSource = declaredBlob(sourceJob.at("profile").at("candidate"),
			"source job.profile.candidate", true);
	const Blob exportSource = declaredBlob(sourceJob.at("ghidra_export"),
			"source job.ghidra_export", true);
	const Blob programSource = declaredBlob(sourceJob.at("program"),
			"source job.program", true);
	const Blob scriptSource = declaredBlob(sourceJob.at("exporter_script"),
			"source job.exporter_script", true);
	const Blob joinSource = declaredBlob(sourceJob.at("semantic_join").at("candidate"),
			"source job.semantic_join.candidate", true);
	const Blob publisherSource = declaredBlob(sourceJob.at("publisher").at("script"),
			"source job.publisher.script", true);
	const Blob semanticValidatorSource = declaredBlob(
			sourceJob.at("semantic_validator").at("executable"),
			"source job.semantic_validator.executable", true);
	const Blob packageValidatorSource = declaredBlob(
			sourceJob.at("package_validator").at("executable"),
			"source job.package_validator.executable", true);
	for (const auto& item : std::vector<std::tuple<std::string, Blob, std::string>> {
			{"identity.json", identitySource, "identity"},
			{"emulator.bin", emulatorSource, "emulator"},
			{"maple-replay.fcmt", replaySource, "Maple replay"},
			{"sh4-profile.fcsh4profile", profileSource, "SH-4 profile"},
			{"ghidra-export.json", exportSource, "Ghidra export"},
			{"program.bin", programSource, "program"},
			{"exporter-script.java", scriptSource, "exporter script"},
			{"sh4-ghidra-join.json", joinSource, "semantic join"},
			{"publisher.ps1", publisherSource, "publisher"},
			{"semantic-validator.exe", semanticValidatorSource, "semantic validator"},
			{"package-validator.exe", packageValidatorSource, "package validator"},
	})
		packaged(result.root, std::get<0>(item), std::get<1>(item), std::get<2>(item));
	if (hasInitialState)
		packaged(result.root, "initial-state.state", initialStateSource,
				"initial state");
	result.packagedValidator = fileIdentity(result.root / "package-validator.exe",
			"package-validator.exe", "package validator");

	const IdentityManifest identity = loadIdentityManifest(result.root / "identity.json");
	require(identity.initialState.available == hasInitialState,
			"identity and package initial-state presence differ");
	require(identity.emulatorExecutable.size == emulatorSource.size
			&& sha256Equal(identity.emulatorExecutable.digest, emulatorSource.digest),
			"emulator bytes differ from identity authority");
	if (hasInitialState)
	{
		require(identity.initialState.size == initialStateSource.size
				&& sha256Equal(identity.initialState.digest, initialStateSource.digest),
				"initial-state bytes differ from identity authority");
		authenticateInitialStateFile(identity, result.root / "initial-state.state");
	}
	const std::uint64_t maximumJoinBytes = number(sourceJob.at("semantic_join"),
			"maximum_bytes", "source job.semantic_join");
	require(maximumJoinBytes >= 256
			&& maximumJoinBytes <= DefaultMaximumSh4GhidraSemanticJoinBytes,
			"semantic join maximum_bytes is invalid");
	const auto summary = validateSh4GhidraSemanticJoin(
			result.root / "sh4-ghidra-join.json",
			result.root / "sh4-profile.fcsh4profile",
			result.root / "identity.json", result.root / "maple-replay.fcmt",
			result.root / "ghidra-export.json", result.root / "program.bin",
			result.root / "exporter-script.java", maximumJoinBytes);
	const std::uint64_t timeout = number(sourceJob.at("limits"),
			"validator_timeout_seconds", "source job.limits");
	require(timeout >= 1 && timeout <= 3600, "validator timeout is invalid");

	for (const Blob& entry : result.lockedEntries)
		equal(fileIdentity(result.root / entry.path, entry.path,
				"locked entry " + entry.path), entry, "locked entry " + entry.path);
	require(inventory(result.root) == expected,
			"package inventory changed during validation");
	const Blob packageManifest = fileIdentity(result.root / "package.json",
			"package.json", "package manifest");
	const Blob semanticJoin = fileIdentity(result.root / "sh4-ghidra-join.json",
			"sh4-ghidra-join.json", "semantic join");
	result.receipt = {{"schema", "flycast-research-sh4-ghidra-package-validation"},
			{"schema_version", 1}, {"package_id", result.packageId},
			{"status", "accepted"}, {"package_manifest", blobJson(packageManifest)},
			{"semantic_join", blobJson(semanticJoin)},
			{"semantic_summary", {{"block_count", summary.blockCount},
					{"edge_count", summary.edgeCount},
					{"edge_occurrences", summary.edgeOccurrences},
					{"authenticated_program_blocks", summary.authenticatedProgramBlocks},
					{"function_owned_blocks", summary.functionOwnedBlocks},
					{"static_executable_unassigned_blocks",
							summary.staticExecutableUnassignedBlocks},
					{"runtime_only_blocks", summary.runtimeOnlyBlocks}}},
			{"validator", blobJson(result.packagedValidator)},
			{"locked_entry_count", result.lockedEntries.size()}};
	if (!issuing)
		require(parseJson(result.root / "package-validation.json", "validation receipt")
				== result.receipt,
				"validation receipt differs from independent revalidation");
	return result;
}

void writeExclusive(const std::filesystem::path& path, const std::string& bytes)
{
#ifdef _WIN32
	HANDLE output = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL, nullptr);
	require(output != INVALID_HANDLE_VALUE,
			"cannot exclusively create validation receipt");
	bool success = true;
	std::size_t position = 0;
	while (position < bytes.size())
	{
		const DWORD count = static_cast<DWORD>(std::min<std::size_t>(
				bytes.size() - position, std::numeric_limits<DWORD>::max()));
		DWORD written = 0;
		if (!WriteFile(output, bytes.data() + position, count, &written, nullptr)
				|| written != count)
		{
			success = false;
			break;
		}
		position += written;
	}
	if (success)
		success = FlushFileBuffers(output) != FALSE;
	if (!CloseHandle(output))
		success = false;
#else
	const int output = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	require(output >= 0, "cannot exclusively create validation receipt");
	bool success = true;
	std::size_t position = 0;
	while (position < bytes.size())
	{
		const ssize_t written = ::write(output, bytes.data() + position,
				bytes.size() - position);
		if (written <= 0)
		{
			success = false;
			break;
		}
		position += static_cast<std::size_t>(written);
	}
	if (success)
		success = ::fsync(output) == 0;
	if (::close(output) != 0)
		success = false;
#endif
	if (!success)
	{
		std::error_code ignored;
		std::filesystem::remove(path, ignored);
		invalid("cannot durably write validation receipt");
	}
}

} // namespace

Sh4GhidraPackageSummary validateSh4GhidraPackageV1ReadOnly(
		const std::filesystem::path& package)
{
	const Validated result = validate(package, false);
	return {result.packageId, result.lockedEntries.size()};
}

Sh4GhidraPackageSummary issueSh4GhidraPackageV1Receipt(
		const std::filesystem::path& package, const std::filesystem::path& receipt,
		const std::filesystem::path& runningValidator)
{
	const Validated result = validate(package, true);
	const auto expectedReceipt = result.root / "package-validation.json";
	require(samePath(std::filesystem::absolute(receipt), expectedReceipt),
			"receipt path is not the fixed package entry");
	const auto validator = canonical(runningValidator, "running validator");
	equal(fileIdentity(validator, validator.u8string(), "running validator"),
			result.packagedValidator, "running package validator");
	writeExclusive(expectedReceipt, result.receipt.dump(2) + "\n");
	return {result.packageId, result.lockedEntries.size()};
}

} // namespace research

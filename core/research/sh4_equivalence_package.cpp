#include "research/sh4_equivalence_package.h"

#include "research/identity_manifest.h"
#include "research/sh4_equivalence.h"
#include "research/sha256.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
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

struct ValidatedPackage
{
	std::filesystem::path root;
	std::string packageId;
	std::filesystem::path acceptedDirectory;
	std::filesystem::path candidateDirectory;
	std::vector<Blob> lockedEntries;
	Blob packagedValidator;
	json expectedReceipt;
};

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid SH-4 equivalence package v1: " + reason);
}

void requireKeys(const json& value, const std::set<std::string>& keys,
		const std::string& field)
{
	if (!value.is_object())
		invalid(field + " must be an object");
	for (const auto& item : value.items())
		if (keys.find(item.key()) == keys.end())
			invalid(field + " has unknown field '" + item.key() + "'");
	for (const std::string& key : keys)
		if (!value.contains(key))
			invalid(field + "." + key + " is missing");
}

std::string stringValue(const json& parent, const char *name,
		const std::string& field, std::size_t maximum = 4096)
{
	if (!parent.contains(name) || !parent.at(name).is_string())
		invalid(field + "." + name + " must be a string");
	const std::string value = parent.at(name).get<std::string>();
	if (value.empty() || value.size() > maximum)
		invalid(field + "." + name + " has an invalid length");
	return value;
}

std::uint64_t unsignedValue(const json& parent, const char *name,
		const std::string& field)
{
	if (!parent.contains(name) || !parent.at(name).is_number_unsigned())
		invalid(field + "." + name + " must be an unsigned integer");
	return parent.at(name).get<std::uint64_t>();
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

bool safeEntryPath(const std::string& value)
{
	if (value.empty() || value.size() > 4096 || value.front() == '/'
			|| value.front() == '\\' || value.find('\\') != std::string::npos)
		return false;
	const std::filesystem::path path = std::filesystem::u8path(value);
	if (path.is_absolute() || path.has_root_path())
		return false;
	for (const std::filesystem::path& component : path)
	{
		const std::string text = component.u8string();
		if (text.empty() || text == "." || text == ".."
				|| !std::all_of(text.begin(), text.end(), [](unsigned char character) {
					return std::isalnum(character) || character == '.'
							|| character == '_' || character == '-';
				}))
			return false;
	}
	return true;
}

std::filesystem::path canonicalPath(const std::filesystem::path& value,
		const std::string& field)
{
	std::error_code error;
	const std::filesystem::path absolute = std::filesystem::absolute(value, error);
	if (error)
		invalid(field + " cannot be made absolute");
	const std::filesystem::path result = std::filesystem::weakly_canonical(absolute,
			error);
	if (error || !result.is_absolute())
		invalid(field + " cannot be canonicalized");
	return result;
}

bool samePath(const std::filesystem::path& left, const std::filesystem::path& right)
{
	std::error_code leftError;
	std::error_code rightError;
	auto leftText = std::filesystem::weakly_canonical(left, leftError).native();
	auto rightText = std::filesystem::weakly_canonical(right, rightError).native();
	if (leftError || rightError)
		return false;
#ifdef _WIN32
	if (leftText.size() > INT_MAX || rightText.size() > INT_MAX)
		return false;
	return CompareStringOrdinal(leftText.data(), static_cast<int>(leftText.size()),
			rightText.data(), static_cast<int>(rightText.size()), TRUE) == CSTR_EQUAL;
#else
	return leftText == rightText;
#endif
}

std::uint64_t fileSize(const std::filesystem::path& path, const std::string& field)
{
	std::error_code error;
	const std::filesystem::file_status status = std::filesystem::symlink_status(path,
			error);
	if (error || !std::filesystem::is_regular_file(status)
			|| std::filesystem::is_symlink(status))
		invalid(field + " is missing, linked, or non-regular");
	const std::uintmax_t raw = std::filesystem::file_size(path, error);
	if (error || raw > std::numeric_limits<std::uint64_t>::max())
		invalid(field + " size is unavailable");
	return static_cast<std::uint64_t>(raw);
}

Blob fileIdentity(const std::filesystem::path& path, const std::string& pathText,
		const std::string& field)
{
	const std::uint64_t size = fileSize(path, field);
	return {pathText, size, hashFileExact(path, size)};
}

json blobJson(const Blob& blob)
{
	return {{"path", blob.path}, {"size", blob.size},
			{"sha256", sha256ToHex(blob.digest)}};
}

void requireBlob(const Blob& actual, const Blob& expected, const std::string& field)
{
	if (actual.size != expected.size || !sha256Equal(actual.digest, expected.digest))
		invalid(field + " bytes differ from their declaration");
}

json parseJson(const std::filesystem::path& path, const std::string& field)
{
	const std::vector<std::uint8_t> bytes = readFileExact(path,
			MaxSh4EquivalencePackageJsonBytes);
	if (bytes.empty())
		invalid(field + " is empty");
	std::vector<std::set<std::string>> keys;
	auto callback = [&keys, &field](int depth, json::parse_event_t event, json& parsed) {
		if (depth < 0 || depth > 64)
			invalid(field + " JSON depth is invalid");
		const std::size_t level = static_cast<std::size_t>(depth);
		if (event == json::parse_event_t::object_start)
		{
			if (keys.size() <= level)
				keys.resize(level + 1);
			keys[level].clear();
		}
		else if (event == json::parse_event_t::key)
		{
			if (level == 0 || keys.size() < level || !parsed.is_string())
				invalid(field + " parser key depth is invalid");
			if (!keys[level - 1].insert(parsed.get<std::string>()).second)
				invalid(field + " contains a duplicate object key");
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

Blob parseBlob(const json& value, const std::string& field,
		bool requireAbsolutePath)
{
	requireKeys(value, {"path", "sha256", "size"}, field);
	Blob result;
	result.path = stringValue(value, "path", field);
	if (requireAbsolutePath && !std::filesystem::u8path(result.path).is_absolute())
		invalid(field + ".path must be absolute");
	result.size = unsignedValue(value, "size", field);
	if (result.size == 0)
		invalid(field + ".size must be positive");
	const std::string digest = stringValue(value, "sha256", field, 64);
	if (!sha256FromHex(digest, result.digest) || sha256ToHex(result.digest) != digest)
		invalid(field + ".sha256 must be lowercase SHA-256");
	return result;
}

void verifyLockedEntry(const std::filesystem::path& root, const Blob& blob)
{
	requireBlob(fileIdentity(root / std::filesystem::u8path(blob.path), blob.path,
			"locked entry " + blob.path), blob, "locked entry " + blob.path);
}

std::set<std::string> enumeratePackage(const std::filesystem::path& root)
{
	std::error_code error;
	std::set<std::string> files;
	for (std::filesystem::recursive_directory_iterator iterator(root, error), end;
			iterator != end; iterator.increment(error))
	{
		if (error)
			invalid("cannot enumerate the package directory");
		const std::filesystem::file_status status = iterator->symlink_status(error);
		if (error || std::filesystem::is_symlink(status))
			invalid("package contains a linked entry");
		const std::filesystem::path relative = iterator->path().lexically_relative(root);
		const std::string text = relative.generic_u8string();
		if (std::filesystem::is_directory(status))
		{
			if (text != "manifests")
				invalid("package contains an unexpected directory: " + text);
			continue;
		}
		if (!std::filesystem::is_regular_file(status) || !safeEntryPath(text)
				|| !files.insert(text).second)
			invalid("package contains an invalid entry: " + text);
	}
	if (error)
		invalid("cannot enumerate the package directory");
	return files;
}

void requirePackageEntries(const std::filesystem::path& root,
		const std::vector<Blob>& lockedEntries, bool receiptRequired)
{
	std::set<std::string> expected {"package.json"};
	if (receiptRequired)
		expected.insert("package-validation.json");
	for (const Blob& blob : lockedEntries)
		expected.insert(blob.path);
	if (enumeratePackage(root) != expected)
		invalid("package entries do not exactly match package.json");
}

void setBlobPath(json& root, const json::json_pointer& pointer,
		const std::string& path)
{
	if (!root.contains(pointer) || !root.at(pointer).is_object()
			|| !root.at(pointer).contains("path"))
		invalid("source job is missing a required blob mapping");
	root.at(pointer).at("path") = path;
}

void requireLocalizedJob(const std::filesystem::path& root)
{
	json expected = parseJson(root / "source-job.json", "source-job");
	const json localized = parseJson(root / "job.json", "localized job");
	setBlobPath(expected, json::json_pointer("/interpreter/identity"),
			"interpreter-identity.json");
	setBlobPath(expected, json::json_pointer("/interpreter/trace"), "interpreter.fcso");
	setBlobPath(expected, json::json_pointer("/dynarec/identity"), "dynarec-identity.json");
	setBlobPath(expected, json::json_pointer("/dynarec/trace"), "dynarec.fcso");
	setBlobPath(expected, json::json_pointer("/emulator"), "emulator.bin");
	setBlobPath(expected, json::json_pointer("/replay"), "maple-replay.fcmt");
	setBlobPath(expected, json::json_pointer("/manifest_set"), "manifest-set.json");
	setBlobPath(expected, json::json_pointer("/comparator/executable"), "comparator.exe");
	if (!expected.contains("manifests") || !expected.at("manifests").is_array())
		invalid("source job manifests are missing");
	for (std::size_t index = 0; index < expected.at("manifests").size(); ++index)
	{
		const json& manifest = expected.at("manifests").at(index);
		const std::string name = stringValue(manifest, "name",
				"source-job.manifests[" + std::to_string(index) + "]", 128);
		if (!safeEntryPath(name) || name.find('/') != std::string::npos)
			invalid("source job manifest name is not a safe package entry");
		setBlobPath(expected, json::json_pointer("/manifests/"
				+ std::to_string(index) + "/source"), "manifests/" + name);
	}
	if (localized != expected)
		invalid("localized job differs from source-job outside fixed blob paths");
}

ValidatedPackage validatePackage(const std::filesystem::path& package,
		bool issuingReceipt)
{
	ValidatedPackage result;
	result.root = canonicalPath(package, "package");
	std::error_code error;
	const std::filesystem::file_status rootStatus = std::filesystem::symlink_status(
			result.root, error);
	if (error || !std::filesystem::is_directory(rootStatus)
			|| std::filesystem::is_symlink(rootStatus))
		invalid("package is missing, linked, or not a directory");

	const json packageManifest = parseJson(result.root / "package.json", "package");
	requireKeys(packageManifest,
			{"locked_entries", "package_id", "schema", "schema_version"}, "package");
	if (packageManifest.at("schema")
				!= "flycast-research-sh4-equivalence-package"
			|| !packageManifest.at("schema_version").is_number_unsigned()
			|| packageManifest.at("schema_version").get<std::uint64_t>() != 1)
		invalid("unsupported package schema");
	result.packageId = stringValue(packageManifest, "package_id", "package", 36);
	if (!uuid(result.packageId))
		invalid("package.package_id must be a lowercase UUID");
	if (!packageManifest.at("locked_entries").is_array()
			|| packageManifest.at("locked_entries").empty()
			|| packageManifest.at("locked_entries").size() > 128)
		invalid("package.locked_entries count is outside [1, 128]");
	std::string previous;
	for (std::size_t index = 0;
			index < packageManifest.at("locked_entries").size(); ++index)
	{
		Blob blob = parseBlob(packageManifest.at("locked_entries").at(index),
				"package.locked_entries[" + std::to_string(index) + "]", false);
		if (!safeEntryPath(blob.path) || blob.path == "package.json"
				|| blob.path == "package-validation.json"
				|| (!previous.empty() && blob.path <= previous))
			invalid("package.locked_entries paths are unsafe or not uniquely ordered");
		previous = blob.path;
		result.lockedEntries.push_back(std::move(blob));
	}
	requirePackageEntries(result.root, result.lockedEntries, !issuingReceipt);
	for (const Blob& blob : result.lockedEntries)
		verifyLockedEntry(result.root, blob);

	const std::set<std::string> mandatory {
			"comparator.exe", "dynarec-identity.json", "dynarec.fcso",
			"emulator.bin", "equivalence-report.json", "interpreter-identity.json",
			"interpreter.fcso", "job.json", "manifest-set.json",
			"maple-replay.fcmt", "package-validator.exe", "publication-job.json",
			"publisher.ps1", "source-job.json",
	};
	std::set<std::string> lockedNames;
	for (const Blob& blob : result.lockedEntries)
		lockedNames.insert(blob.path);
	for (const std::string& name : mandatory)
		if (lockedNames.count(name) != 1)
			invalid("package is missing mandatory locked entry " + name);

	const json publicationJob = parseJson(result.root / "publication-job.json",
			"publication-job");
	requireKeys(publicationJob, {"equivalence_job", "limits", "metadata", "output",
			"package_id", "publisher", "schema", "schema_version", "validator"},
			"publication-job");
	if (publicationJob.at("schema")
				!= "flycast-research-sh4-equivalence-package-job"
			|| !publicationJob.at("schema_version").is_number_unsigned()
			|| publicationJob.at("schema_version").get<std::uint64_t>() != 1)
		invalid("unsupported publication-job schema");
	if (stringValue(publicationJob, "package_id", "publication-job", 36)
			!= result.packageId)
		invalid("publication-job package_id differs from package.json");
	const json& output = publicationJob.at("output");
	requireKeys(output, {"accepted_directory"}, "publication-job.output");
	result.acceptedDirectory = canonicalPath(std::filesystem::u8path(stringValue(output,
			"accepted_directory", "publication-job.output")),
			"publication-job.output.accepted_directory");
	result.candidateDirectory = result.acceptedDirectory.parent_path()
			/ (".flycast-research-sh4-equivalence-candidate-" + result.packageId);
	if (issuingReceipt)
	{
		if (!samePath(result.root, result.candidateDirectory))
			invalid("receipt may only be issued inside the declared private candidate");
		if (std::filesystem::exists(result.acceptedDirectory))
			invalid("accepted directory appeared before receipt issuance");
	}
	else if (!samePath(result.root, result.acceptedDirectory))
		invalid("published package is not at its declared accepted directory");

	const Blob sourceJob = parseBlob(publicationJob.at("equivalence_job"),
			"publication-job.equivalence_job", true);
	const json& publisherObject = publicationJob.at("publisher");
	requireKeys(publisherObject, {"script"}, "publication-job.publisher");
	const Blob publisher = parseBlob(publisherObject.at("script"),
			"publication-job.publisher.script", true);
	const json& validatorObject = publicationJob.at("validator");
	requireKeys(validatorObject, {"executable"}, "publication-job.validator");
	const Blob validator = parseBlob(validatorObject.at("executable"),
			"publication-job.validator.executable", true);
	const json& limits = publicationJob.at("limits");
	requireKeys(limits, {"validator_timeout_seconds"}, "publication-job.limits");
	const std::uint64_t timeout = unsignedValue(limits, "validator_timeout_seconds",
			"publication-job.limits");
	if (timeout == 0 || timeout > 3600)
		invalid("publication-job validator timeout is outside [1, 3600]");
	if (!publicationJob.at("metadata").is_object()
			|| publicationJob.at("metadata").size() > 64)
		invalid("publication-job.metadata must be a bounded object");
	requireBlob(fileIdentity(result.root / "source-job.json", "source-job.json",
			"source-job"), sourceJob, "source-job");
	requireBlob(fileIdentity(result.root / "publisher.ps1", "publisher.ps1",
			"publisher"), publisher, "publisher");
	result.packagedValidator = fileIdentity(result.root / "package-validator.exe",
			"package-validator.exe", "package validator");
	requireBlob(result.packagedValidator, validator, "package validator");

	requireLocalizedJob(result.root);
	const Sh4EquivalenceReportSummary report = validateSh4EquivalenceReport(
			result.root / "job.json", result.root / "equivalence-report.json",
			result.root / "comparator.exe");
	if (!report.equivalent)
		invalid("equivalence report is divergent");

	// Reauthenticate the full locked set after independent report recomputation.
	for (const Blob& blob : result.lockedEntries)
		verifyLockedEntry(result.root, blob);
	requirePackageEntries(result.root, result.lockedEntries, !issuingReceipt);
	const Blob packageIdentity = fileIdentity(result.root / "package.json",
			"package.json", "package manifest");
	const Blob reportIdentity = fileIdentity(result.root / "equivalence-report.json",
			"equivalence-report.json", "equivalence report");
	result.expectedReceipt = {
			{"schema", "flycast-research-sh4-equivalence-package-validation"},
			{"schema_version", 1},
			{"package_id", result.packageId},
			{"status", "accepted"},
			{"package_manifest", blobJson(packageIdentity)},
			{"report", blobJson(reportIdentity)},
			{"validator", blobJson(result.packagedValidator)},
			{"locked_entry_count", result.lockedEntries.size()},
	};
	if (!issuingReceipt)
	{
		const json receipt = parseJson(result.root / "package-validation.json",
				"package-validation");
		if (receipt != result.expectedReceipt)
			invalid("validation receipt differs from independent revalidation");
	}
	return result;
}

void writeExclusive(const std::filesystem::path& path, const std::string& bytes)
{
#ifdef _WIN32
	HANDLE output = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL, nullptr);
	if (output == INVALID_HANDLE_VALUE)
		invalid("cannot exclusively create the validation receipt");
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
	if (output < 0)
		invalid("cannot exclusively create the validation receipt");
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
		invalid("cannot durably write the validation receipt");
	}
}

} // namespace

Sh4EquivalencePackageSummary validateSh4EquivalencePackageV1ReadOnly(
		const std::filesystem::path& package)
{
	const ValidatedPackage result = validatePackage(package, false);
	return {result.packageId, result.lockedEntries.size()};
}

Sh4EquivalencePackageSummary issueSh4EquivalencePackageV1Receipt(
		const std::filesystem::path& package,
		const std::filesystem::path& receipt,
		const std::filesystem::path& runningValidator)
{
	const ValidatedPackage result = validatePackage(package, true);
	const std::filesystem::path expectedReceipt = result.root
			/ "package-validation.json";
	if (!samePath(std::filesystem::absolute(receipt), expectedReceipt))
		invalid("receipt path is not the fixed package-validation.json entry");
	const std::filesystem::path validatorPath = canonicalPath(runningValidator,
			"running validator");
	const Blob actualValidator = fileIdentity(validatorPath,
			validatorPath.u8string(), "running validator");
	requireBlob(actualValidator, result.packagedValidator,
			"running package validator");
	writeExclusive(expectedReceipt, result.expectedReceipt.dump(2) + "\n");
	return {result.packageId, result.lockedEntries.size()};
}

} // namespace research

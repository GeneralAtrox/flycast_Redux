#include "research/capture_package_v2.h"

#include "research/capture_v1_validation.h"
#include "research/ghidra_export.h"
#include "research/identity_manifest.h"
#include "research/memory_ranges_artifact.h"
#include "research/memory_ranges_manifest.h"
#include "research/sh4_events_artifact.h"
#include "research/sh4_events_manifest.h"
#include "research/sha256.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
#endif

namespace research
{
namespace
{

using json = nlohmann::json;

struct Blob
{
	std::string pathText;
	std::uint64_t size = 0;
	std::string sha256;
};

struct Artifact
{
	std::string kind;
	Blob artifactSource;
	Blob manifestSource;
	std::uint64_t maximumBytes = 0;
	std::string artifactName;
	std::string manifestName;
};

struct ValidatedPackage
{
	std::string packageId;
	std::filesystem::path acceptedDirectory;
	std::filesystem::path candidateDirectory;
	Blob validator;
	json expectedReceipt;
	std::size_t artifactCount = 0;
};

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid research capture package v2: " + reason);
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
		std::size_t maximum = 4096)
{
	if (!parent.contains(name) || !parent.at(name).is_string())
		invalid(field + "." + name + " must be a string");
	const std::string value = parent.at(name).get<std::string>();
	if (value.empty() || value.size() > maximum)
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

bool lowerSha256(const std::string& value)
{
	return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char character) {
		return (character >= '0' && character <= '9')
				|| (character >= 'a' && character <= 'f');
	});
}

bool safeEntryName(const std::string& value)
{
	if (value.empty() || value.size() > 128 || value == "." || value == "..")
		return false;
	return std::all_of(value.begin(), value.end(), [](char character) {
		return (character >= 'a' && character <= 'z')
				|| (character >= 'A' && character <= 'Z')
				|| (character >= '0' && character <= '9') || character == '.'
				|| character == '_' || character == '-';
	});
}

bool packageId(const std::string& value)
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

std::filesystem::path pathFromUtf8(const std::string& value)
{
	return std::filesystem::u8path(value);
}

std::string pathToUtf8(const std::filesystem::path& value)
{
	const auto text = value.u8string();
	return std::string(text.begin(), text.end());
}

std::filesystem::path canonicalAbsolute(const std::filesystem::path& value,
		const std::string& field)
{
	if (!value.is_absolute())
		invalid(field + " must be absolute");
	std::error_code error;
	const std::filesystem::path result = std::filesystem::weakly_canonical(value, error);
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

bool sameOrDescendantPath(const std::filesystem::path& parent,
		const std::filesystem::path& value)
{
	std::filesystem::path current = value;
	while (!current.empty())
	{
		if (samePath(parent, current))
			return true;
		const std::filesystem::path next = current.parent_path();
		if (next == current)
			break;
		current = next;
	}
	return false;
}

Blob parseBlob(const json& value, const std::string& field, bool absolute)
{
	requireAllowedKeys(value, {"path", "size", "sha256"}, field);
	Blob blob;
	blob.pathText = requiredString(value, "path", field);
	blob.size = requiredUnsigned(value, "size", field);
	blob.sha256 = requiredString(value, "sha256", field, 64);
	if (!lowerSha256(blob.sha256))
		invalid(field + ".sha256 is not lowercase SHA-256");
	if (blob.size == 0)
		invalid(field + ".size must be positive");
	if (absolute && !pathFromUtf8(blob.pathText).is_absolute())
		invalid(field + ".path must be absolute");
	if (!absolute && !safeEntryName(blob.pathText))
		invalid(field + ".path must be a safe package entry name");
	return blob;
}

json blobJson(const Blob& blob)
{
	return {{"path", blob.pathText}, {"size", blob.size}, {"sha256", blob.sha256}};
}

Blob fileIdentity(const std::filesystem::path& path, const std::string& pathText,
		std::uint64_t maximumBytes = 0)
{
	std::error_code error;
	const std::filesystem::file_status status = std::filesystem::symlink_status(path, error);
	if (error || !std::filesystem::is_regular_file(status)
			|| std::filesystem::is_symlink(status))
		invalid("file is missing, linked, or non-regular: " + pathText);
	const std::uintmax_t rawSize = std::filesystem::file_size(path, error);
	if (error || rawSize > std::numeric_limits<std::uint64_t>::max()
			|| (maximumBytes != 0 && rawSize > maximumBytes))
		invalid("file size is unavailable or exceeds its bound: " + pathText);
	const std::uint64_t size = static_cast<std::uint64_t>(rawSize);
	return Blob {pathText, size, sha256ToHex(hashFileExact(path, size))};
}

void requireBlob(const Blob& actual, const Blob& declared, const std::string& field)
{
	if (actual.size != declared.size || actual.sha256 != declared.sha256)
		invalid(field + " bytes do not match their declaration");
}

void verifyExternalBlob(const Blob& blob, const std::string& field)
{
	const std::filesystem::path path = canonicalAbsolute(pathFromUtf8(blob.pathText),
			field + ".path");
	requireBlob(fileIdentity(path, blob.pathText, blob.size), blob, field);
}

json parseRejectingDuplicateKeys(const std::vector<std::uint8_t>& bytes,
		const std::string& field)
{
	std::vector<std::set<std::string>> keys;
	auto callback = [&keys, &field](int depth, json::parse_event_t event, json& parsed) {
		if (depth < 0)
			invalid(field + " parser depth is invalid");
		if (depth > 64)
			invalid(field + " JSON depth exceeds 64");
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
				invalid(field + " contains duplicate key '" + parsed.get<std::string>() + "'");
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

json parseJsonFile(const std::filesystem::path& path, const std::string& field)
{
	const std::vector<std::uint8_t> bytes = readFileExact(path,
			MaxCapturePackageV2JsonBytes);
	if (bytes.empty())
		invalid(field + " is empty");
	return parseRejectingDuplicateKeys(bytes, field);
}

void requireExactEntries(const std::filesystem::path& package,
		const std::set<std::string>& expected)
{
	std::error_code error;
	std::set<std::string> actual;
	for (const auto& entry : std::filesystem::directory_iterator(package, error))
	{
		if (error)
			invalid("cannot enumerate package directory");
		const std::string name = pathToUtf8(entry.path().filename());
		const std::filesystem::file_status status = entry.symlink_status(error);
		if (error || !std::filesystem::is_regular_file(status)
				|| std::filesystem::is_symlink(status))
			invalid("package entry is linked or non-regular: " + name);
		if (!actual.insert(name).second)
			invalid("duplicate package entry: " + name);
	}
	if (error || actual != expected)
		invalid("package entries do not exactly match the v2 contract");
}

std::vector<Blob> parseBaseEntries(const json& base)
{
	const json& entries = requiredArray(base, "entries", "job.base_capture_v1");
	if (entries.size() != 5)
		invalid("job.base_capture_v1.entries must contain exactly five v1 entries");
	std::vector<Blob> result;
	std::string previous;
	for (std::size_t index = 0; index < entries.size(); ++index)
	{
		Blob blob = parseBlob(entries.at(index), "job.base_capture_v1.entries["
				+ std::to_string(index) + "]", false);
		if (index != 0 && blob.pathText <= previous)
			invalid("job.base_capture_v1.entries are not uniquely ordered by path");
		previous = blob.pathText;
		result.push_back(std::move(blob));
	}
	std::set<std::string> names;
	for (const Blob& blob : result)
		names.insert(blob.pathText);
	if (names.count("job.json") != 1 || names.count("identity.json") != 1
			|| names.count("transcript.json") != 1
			|| names.count("capture-validation.json") != 1)
		invalid("job.base_capture_v1.entries do not identify the mandatory v1 files");
	return result;
}

std::vector<Artifact> parseArtifacts(const json& root)
{
	const json& values = requiredArray(root, "artifacts", "job");
	if (values.empty() || values.size() > 2)
		invalid("job.artifacts count is outside [1, 2]");
	std::vector<Artifact> result;
	std::string previousKind;
	for (std::size_t index = 0; index < values.size(); ++index)
	{
		const json& value = values.at(index);
		const std::string field = "job.artifacts[" + std::to_string(index) + "]";
		requireAllowedKeys(value, {"kind", "artifact", "manifest", "maximum_bytes"},
				field);
		Artifact artifact;
		artifact.kind = requiredString(value, "kind", field, 64);
		if (index != 0 && artifact.kind <= previousKind)
			invalid("job.artifacts are not uniquely ordered by kind");
		previousKind = artifact.kind;
		artifact.artifactSource = parseBlob(value.at("artifact"), field + ".artifact", true);
		artifact.manifestSource = parseBlob(value.at("manifest"), field + ".manifest", true);
		artifact.maximumBytes = requiredUnsigned(value, "maximum_bytes", field);
		if (artifact.kind == "memory-ranges-v1")
		{
			artifact.artifactName = "memory-ranges.fcmr";
			artifact.manifestName = "memory-ranges-manifest.json";
			if (artifact.maximumBytes == 0
					|| artifact.maximumBytes > DefaultMaximumMemoryRangesArtifactBytes)
				invalid(field + ".maximum_bytes exceeds the memory-ranges v1 bound");
		}
		else if (artifact.kind == "sh4-events-v1")
		{
			artifact.artifactName = "sh4-events.fcsh4";
			artifact.manifestName = "sh4-events-manifest.json";
			if (artifact.maximumBytes == 0
					|| artifact.maximumBytes > DefaultMaximumSh4EventsArtifactBytes)
				invalid(field + ".maximum_bytes exceeds the SH-4 events v1 bound");
		}
		else
			invalid(field + ".kind is unsupported");
		if (artifact.artifactSource.size > artifact.maximumBytes)
			invalid(field + ".artifact exceeds maximum_bytes");
		result.push_back(std::move(artifact));
	}
	return result;
}

ValidatedPackage validatePackage(const std::filesystem::path& package, bool issuingReceipt)
{
	std::error_code error;
	if (!std::filesystem::is_directory(package, error) || error)
		invalid("package directory does not exist");
	const std::filesystem::path jobPath = package / "job.json";
	const json job = parseJsonFile(jobPath, "job");
	requireAllowedKeys(job, {"schema", "schema_version", "package_id", "output",
			"base_capture_v1", "identity", "static_analysis", "artifacts", "publisher",
			"validator", "limits", "metadata"}, "job");
	if (requiredString(job, "schema", "job")
				!= "flycast-research-capture-package-job"
			|| requiredUnsigned(job, "schema_version", "job") != 2)
		invalid("unsupported job schema");
	ValidatedPackage result;
	result.packageId = requiredString(job, "package_id", "job", 36);
	if (!packageId(result.packageId))
		invalid("job.package_id is not a lowercase UUID");
	const json& output = requiredObject(job, "output", "job");
	requireAllowedKeys(output, {"accepted_directory"}, "job.output");
	result.acceptedDirectory = canonicalAbsolute(pathFromUtf8(requiredString(output,
			"accepted_directory", "job.output")), "job.output.accepted_directory");
	result.candidateDirectory = result.acceptedDirectory.parent_path()
			/ (".flycast-research-package-v2-candidate-" + result.packageId);
	if (issuingReceipt)
	{
		if (!samePath(package, result.candidateDirectory))
			invalid("receipt may only be issued in the declared private candidate");
		if (std::filesystem::exists(result.acceptedDirectory))
			invalid("accepted directory appeared before receipt issuance");
	}
	else if (!samePath(package, result.acceptedDirectory))
		invalid("published package is not at its declared accepted directory");

	const json& base = requiredObject(job, "base_capture_v1", "job");
	requireAllowedKeys(base, {"accepted_directory", "entries"}, "job.base_capture_v1");
	const std::filesystem::path baseDirectory = canonicalAbsolute(pathFromUtf8(requiredString(
			base, "accepted_directory", "job.base_capture_v1")),
			"job.base_capture_v1.accepted_directory");
	if (sameOrDescendantPath(baseDirectory, result.acceptedDirectory))
		invalid("v2 accepted directory may not be inside the immutable v1 base package");
	const std::vector<Blob> baseEntries = parseBaseEntries(base);
	validateCaptureV1PackageReadOnly(baseDirectory);
	for (const Blob& blob : baseEntries)
		requireBlob(fileIdentity(baseDirectory / pathFromUtf8(blob.pathText), blob.pathText,
				blob.size), blob, "base capture entry " + blob.pathText);

	const Blob identitySource = parseBlob(job.at("identity"), "job.identity", true);
	verifyExternalBlob(identitySource, "job.identity");
	const Blob packagedIdentity = fileIdentity(package / "identity.json", "identity.json",
			MaxCapturePackageV2JsonBytes);
	requireBlob(packagedIdentity, Blob {"identity.json", identitySource.size,
			identitySource.sha256}, "packaged identity");
	const auto baseIdentity = std::find_if(baseEntries.begin(), baseEntries.end(),
			[](const Blob& blob) { return blob.pathText == "identity.json"; });
	if (baseIdentity == baseEntries.end() || baseIdentity->size != identitySource.size
			|| baseIdentity->sha256 != identitySource.sha256)
		invalid("v2 identity is not the exact v1 base-capture identity");
	(void)parseJsonFile(package / "identity.json", "identity");
	const IdentityManifest identity = loadIdentityManifest(package / "identity.json");

	const json& staticAnalysis = requiredObject(job, "static_analysis", "job");
	requireAllowedKeys(staticAnalysis, {"export", "program", "exporter_script"},
			"job.static_analysis");
	const Blob exportSource = parseBlob(staticAnalysis.at("export"),
			"job.static_analysis.export", true);
	const Blob programSource = parseBlob(staticAnalysis.at("program"),
			"job.static_analysis.program", true);
	const Blob scriptSource = parseBlob(staticAnalysis.at("exporter_script"),
			"job.static_analysis.exporter_script", true);
	verifyExternalBlob(exportSource, "job.static_analysis.export");
	verifyExternalBlob(programSource, "job.static_analysis.program");
	verifyExternalBlob(scriptSource, "job.static_analysis.exporter_script");
	const Blob packagedExport = fileIdentity(package / "static-analysis.json",
			"static-analysis.json", MaxGhidraExportBytes);
	requireBlob(packagedExport, Blob {"static-analysis.json", exportSource.size,
			exportSource.sha256}, "packaged static analysis");
	const GhidraExport ghidra = loadGhidraExport(package / "static-analysis.json");
	requireGhidraExportIdentity(ghidra, identity, pathFromUtf8(programSource.pathText),
			pathFromUtf8(scriptSource.pathText));

	const std::vector<Artifact> artifacts = parseArtifacts(job);
	result.artifactCount = artifacts.size();
	std::set<std::string> expectedEntries {"job.json", "identity.json",
			"static-analysis.json"};
	if (!issuingReceipt)
		expectedEntries.insert("capture-package-validation.json");
	json receiptArtifacts = json::array();
	for (const Artifact& artifact : artifacts)
	{
		verifyExternalBlob(artifact.artifactSource, "job artifact " + artifact.kind);
		verifyExternalBlob(artifact.manifestSource, "job manifest " + artifact.kind);
		expectedEntries.insert(artifact.artifactName);
		expectedEntries.insert(artifact.manifestName);
		const Blob packagedArtifact = fileIdentity(package / artifact.artifactName,
				artifact.artifactName, artifact.maximumBytes);
		const Blob packagedManifest = fileIdentity(package / artifact.manifestName,
				artifact.manifestName, MaxCapturePackageV2JsonBytes);
		requireBlob(packagedArtifact, Blob {artifact.artifactName,
				artifact.artifactSource.size, artifact.artifactSource.sha256},
				"packaged artifact " + artifact.kind);
		requireBlob(packagedManifest, Blob {artifact.manifestName,
				artifact.manifestSource.size, artifact.manifestSource.sha256},
				"packaged manifest " + artifact.kind);
		if (artifact.kind == "memory-ranges-v1")
		{
			const MemoryRangesManifest manifest = loadMemoryRangesManifest(
					package / artifact.manifestName);
			validateProductionMemoryRangesArtifactFile(package / artifact.artifactName,
					identity, manifest, artifact.maximumBytes);
			requireGhidraExportMemoryRangesJoin(ghidra, manifest);
		}
		else
		{
			const Sh4EventsManifest manifest = loadSh4EventsManifest(
					package / artifact.manifestName);
			validateProductionSh4EventsArtifactFile(package / artifact.artifactName,
					identity, manifest, artifact.maximumBytes);
			requireGhidraExportSh4Join(ghidra, manifest);
		}
		receiptArtifacts.push_back({
			{"kind", artifact.kind},
			{"artifact", blobJson(packagedArtifact)},
			{"manifest", blobJson(packagedManifest)},
		});
	}

	const json& publisherObject = requiredObject(job, "publisher", "job");
	requireAllowedKeys(publisherObject, {"script"}, "job.publisher");
	const Blob publisher = parseBlob(publisherObject.at("script"),
			"job.publisher.script", true);
	verifyExternalBlob(publisher, "job.publisher.script");
	const json& validatorObject = requiredObject(job, "validator", "job");
	requireAllowedKeys(validatorObject, {"executable"}, "job.validator");
	result.validator = parseBlob(validatorObject.at("executable"),
			"job.validator.executable", true);
	verifyExternalBlob(result.validator, "job.validator.executable");
	const json& limits = requiredObject(job, "limits", "job");
	requireAllowedKeys(limits, {"validator_timeout_seconds"}, "job.limits");
	const std::uint64_t timeout = requiredUnsigned(limits, "validator_timeout_seconds",
			"job.limits");
	if (timeout == 0 || timeout > 3600)
		invalid("job.limits.validator_timeout_seconds is outside [1, 3600]");
	const json& metadata = requiredObject(job, "metadata", "job");
	if (metadata.size() > 64)
		invalid("job.metadata has too many fields");

	requireExactEntries(package, expectedEntries);
	const Blob packagedJob = fileIdentity(jobPath, "job.json", MaxCapturePackageV2JsonBytes);
	json receiptBaseEntries = json::array();
	for (const Blob& blob : baseEntries)
		receiptBaseEntries.push_back(blobJson(blob));
	result.expectedReceipt = {
		{"schema", "flycast-research-capture-package-validation"},
		{"schema_version", 2},
		{"package_id", result.packageId},
		{"job", blobJson(packagedJob)},
		{"identity", blobJson(packagedIdentity)},
		{"static_analysis", blobJson(packagedExport)},
		{"base_capture_v1", {
			{"accepted_directory", pathToUtf8(baseDirectory)},
			{"entries", receiptBaseEntries},
		}},
		{"artifacts", receiptArtifacts},
		{"publisher", blobJson(publisher)},
		{"validator", blobJson(result.validator)},
	};
	if (!issuingReceipt)
	{
		const json receipt = parseJsonFile(package / "capture-package-validation.json",
				"capture-package-validation");
		if (receipt != result.expectedReceipt)
			invalid("validation receipt does not match the revalidated package");
	}
	return result;
}

void writeReceipt(const std::filesystem::path& receipt, const json& value)
{
	if (std::filesystem::exists(receipt))
		invalid("validation receipt already exists");
	std::filesystem::path temporary = receipt;
	temporary += ".candidate";
	if (std::filesystem::exists(temporary))
		invalid("validation receipt temporary path already exists");
	{
		std::ofstream output(temporary, std::ios::binary | std::ios::out | std::ios::trunc);
		if (!output)
			invalid("cannot create validation receipt temporary file");
		const std::string bytes = value.dump(2) + "\n";
		output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		output.flush();
		if (!output)
			invalid("cannot write validation receipt temporary file");
	}
	std::error_code error;
	std::filesystem::rename(temporary, receipt, error);
	if (error)
	{
		std::filesystem::remove(temporary);
		invalid("cannot atomically publish validation receipt: " + error.message());
	}
}

} // namespace

CapturePackageV2Summary validateCapturePackageV2ReadOnly(
		const std::filesystem::path& package)
{
	const ValidatedPackage result = validatePackage(
			canonicalAbsolute(package, "package"), false);
	return {result.packageId, result.artifactCount};
}

CapturePackageV2Summary issueCapturePackageV2Receipt(
		const std::filesystem::path& package, const std::filesystem::path& receipt,
		const std::filesystem::path& runningValidator)
{
	const std::filesystem::path canonicalPackage = canonicalAbsolute(package, "package");
	const ValidatedPackage result = validatePackage(canonicalPackage, true);
	const std::filesystem::path expectedReceipt = canonicalPackage
			/ "capture-package-validation.json";
	if (!samePath(std::filesystem::absolute(receipt), expectedReceipt))
		invalid("receipt path is not the fixed package validation entry");
	const std::filesystem::path validatorPath = canonicalAbsolute(runningValidator,
			"running validator");
	const Blob actualValidator = fileIdentity(validatorPath, pathToUtf8(validatorPath),
			result.validator.size);
	requireBlob(actualValidator, result.validator, "running v2 validator");
	writeReceipt(expectedReceipt, result.expectedReceipt);
	return {result.packageId, result.artifactCount};
}

} // namespace research

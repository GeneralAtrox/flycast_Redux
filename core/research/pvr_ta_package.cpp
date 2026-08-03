#include "research/pvr_ta_package.h"

#include "research/identity_manifest.h"
#include "research/pvr_ta_artifact.h"
#include "research/pvr_ta_manifest.h"
#include "research/sh4_equivalence_package.h"
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
	Blob packagedValidator;
	json expectedReceipt;
};

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid PowerVR TA package v1: " + reason);
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

std::string stringValue(const json& parent, const char *name,
		const std::string& field, std::size_t maximum = 4096)
{
	if (!parent.at(name).is_string())
		invalid(field + "." + name + " must be a string");
	const std::string result = parent.at(name).get<std::string>();
	if (result.empty() || result.size() > maximum)
		invalid(field + "." + name + " has an invalid length");
	return result;
}

std::uint64_t unsignedValue(const json& parent, const char *name,
		const std::string& field)
{
	if (!parent.at(name).is_number_unsigned())
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
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error || size > std::numeric_limits<std::uint64_t>::max())
		invalid(field + " size is unavailable");
	return static_cast<std::uint64_t>(size);
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
			MaxPvrTaPackageJsonBytes);
	if (bytes.empty())
		invalid(field + " is empty");
	try
	{
		std::vector<std::set<std::string>> keys;
		auto callback = [&keys, &field](int depth, json::parse_event_t event,
				json& parsed) {
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
				if (level == 0 || keys.size() < level || !parsed.is_string()
						|| !keys[level - 1].insert(parsed.get<std::string>()).second)
					invalid(field + " contains a duplicate key");
			}
			return true;
		};
		return json::parse(bytes.begin(), bytes.end(), callback);
	}
	catch (const json::exception& exception)
	{
		invalid(field + " JSON parse error: " + exception.what());
	}
}

Blob parseBlob(const json& value, const std::string& field)
{
	requireKeys(value, {"path", "sha256", "size"}, field);
	Blob result;
	result.path = stringValue(value, "path", field);
	if (!std::filesystem::u8path(result.path).is_absolute())
		invalid(field + ".path must be absolute");
	result.size = unsignedValue(value, "size", field);
	if (result.size == 0)
		invalid(field + ".size must be positive");
	const std::string digest = stringValue(value, "sha256", field, 64);
	if (!sha256FromHex(digest, result.digest) || sha256ToHex(result.digest) != digest)
		invalid(field + ".sha256 must be lowercase SHA-256");
	return result;
}

Blob verifySourceBlob(const json& value, const std::string& field)
{
	const Blob declared = parseBlob(value, field);
	requireBlob(fileIdentity(std::filesystem::u8path(declared.path), declared.path,
			field), declared, field);
	return declared;
}

void requirePackagedCopy(const std::filesystem::path& root,
		const std::string& name, const Blob& source, const std::string& field)
{
	requireBlob(fileIdentity(root / name, name, field), source, field);
}

std::set<std::string> enumeratePackage(const std::filesystem::path& root)
{
	std::error_code error;
	std::set<std::string> files;
	for (std::filesystem::directory_iterator iterator(root, error), end;
			iterator != end; iterator.increment(error))
	{
		if (error)
			invalid("cannot enumerate package");
		const auto status = iterator->symlink_status(error);
		if (error || !std::filesystem::is_regular_file(status)
				|| std::filesystem::is_symlink(status))
			invalid("package contains a linked or non-regular entry");
		files.insert(iterator->path().filename().u8string());
	}
	if (error)
		invalid("cannot enumerate package");
	return files;
}

std::vector<Blob> packageInventory(const std::filesystem::path& root)
{
	std::vector<Blob> result;
	for (const std::string& name : enumeratePackage(root))
		if (name != "package-validation.json")
			result.push_back(fileIdentity(root / name, name, "package entry " + name));
	return result;
}

ValidatedPackage validatePackage(const std::filesystem::path& package,
		bool issuingReceipt)
{
	ValidatedPackage result;
	result.root = canonicalPath(package, "package");
	std::error_code error;
	const auto status = std::filesystem::symlink_status(result.root, error);
	if (error || !std::filesystem::is_directory(status)
			|| std::filesystem::is_symlink(status))
		invalid("package is missing, linked, or not a directory");

	std::set<std::string> expected {
		"artifact-validator.exe", "identity.json", "job.json",
		"maple-replay.fcmt", "package-validator.exe", "publisher.ps1",
		"pvr-ta-manifest.json", "pvr-ta.fcpvr", "static-analysis.bin",
	};
	const bool hasPackagedInitialState =
			std::filesystem::exists(result.root / "initial-state.state");
	if (hasPackagedInitialState)
		expected.insert("initial-state.state");
	std::set<std::string> expectedWithReceipt = expected;
	if (!issuingReceipt)
		expectedWithReceipt.insert("package-validation.json");
	if (enumeratePackage(result.root) != expectedWithReceipt)
		invalid("package entries do not match the fixed v1 inventory");

	const json job = parseJson(result.root / "job.json", "job");
	std::set<std::string> jobKeys {"artifact", "artifact_validator", "base_equivalence_package",
			"identity", "limits", "maple_replay", "metadata", "output",
			"package_id", "package_validator", "publisher", "pvr_manifest",
			"schema", "schema_version", "static_analysis"};
	const bool hasDeclaredInitialState = job.contains("initial_state");
	if (hasDeclaredInitialState)
		jobKeys.insert("initial_state");
	requireKeys(job, jobKeys, "job");
	if (hasDeclaredInitialState != hasPackagedInitialState)
		invalid("job and package initial-state presence differ");
	if (job.at("schema") != "flycast-research-pvr-ta-package-job"
			|| !job.at("schema_version").is_number_unsigned()
			|| job.at("schema_version").get<std::uint64_t>() != 1)
		invalid("unsupported job schema");
	result.packageId = stringValue(job, "package_id", "job", 36);
	if (!uuid(result.packageId))
		invalid("job.package_id must be a lowercase UUID");

	requireKeys(job.at("output"), {"accepted_directory"}, "job.output");
	result.acceptedDirectory = canonicalPath(std::filesystem::u8path(stringValue(
			job.at("output"), "accepted_directory", "job.output")),
			"job.output.accepted_directory");
	result.candidateDirectory = result.acceptedDirectory.parent_path()
			/ (".flycast-research-pvr-ta-candidate-" + result.packageId);
	if (issuingReceipt)
	{
		if (!samePath(result.root, result.candidateDirectory)
				|| std::filesystem::exists(result.acceptedDirectory))
			invalid("receipt may only be issued in the private candidate");
	}
	else if (!samePath(result.root, result.acceptedDirectory))
		invalid("published package is not at its declared accepted path");

	requireKeys(job.at("base_equivalence_package"),
			{"accepted_directory", "backend"}, "job.base_equivalence_package");
	const std::filesystem::path base = canonicalPath(std::filesystem::u8path(
			stringValue(job.at("base_equivalence_package"), "accepted_directory",
					"job.base_equivalence_package")), "base equivalence package");
	const std::string backendName = stringValue(job.at("base_equivalence_package"),
			"backend", "job.base_equivalence_package", 16);
	if (backendName != "interpreter" && backendName != "dynarec")
		invalid("base backend is unsupported");
	validateSh4EquivalencePackageV1ReadOnly(base);

	const Blob identitySource = verifySourceBlob(job.at("identity"), "job.identity");
	const Blob replaySource = verifySourceBlob(job.at("maple_replay"),
			"job.maple_replay");
	const Blob manifestSource = verifySourceBlob(job.at("pvr_manifest"),
			"job.pvr_manifest");
	Blob initialStateSource;
	if (hasDeclaredInitialState)
	{
		initialStateSource = verifySourceBlob(job.at("initial_state"),
				"job.initial_state");
		requirePackagedCopy(result.root, "initial-state.state", initialStateSource,
				"initial state");
	}
	requireKeys(job.at("artifact"), {"candidate", "maximum_bytes",
			"maximum_events"}, "job.artifact");
	const Blob artifactSource = verifySourceBlob(job.at("artifact").at("candidate"),
			"job.artifact.candidate");
	const std::uint64_t maximumBytes = unsignedValue(job.at("artifact"),
			"maximum_bytes", "job.artifact");
	const std::uint64_t maximumEvents = unsignedValue(job.at("artifact"),
			"maximum_events", "job.artifact");
	requirePackagedCopy(result.root, "identity.json", identitySource, "identity");
	requirePackagedCopy(result.root, "maple-replay.fcmt", replaySource, "replay");
	requirePackagedCopy(result.root, "pvr-ta-manifest.json", manifestSource,
			"PVR manifest");
	requirePackagedCopy(result.root, "pvr-ta.fcpvr", artifactSource, "PVR artifact");
	const Blob baseIdentity = fileIdentity(base / (backendName + "-identity.json"),
			"base identity", "base identity");
	const Blob baseReplay = fileIdentity(base / "maple-replay.fcmt", "base replay",
			"base replay");
	requireBlob(baseIdentity, identitySource, "selected base identity");
	requireBlob(baseReplay, replaySource, "selected base replay");

	const IdentityManifest identity = loadIdentityManifest(result.root / "identity.json");
	requireSh4EquivalenceIdentityV2(identity);
	if (identity.initialState.available != hasDeclaredInitialState)
		invalid("identity and job initial-state presence differ");
	if (hasDeclaredInitialState)
	{
		if (identity.initialState.size != initialStateSource.size
				|| !sha256Equal(identity.initialState.digest,
						initialStateSource.digest))
			invalid("initial-state bytes differ from the selected identity");
		authenticateInitialStateFile(identity, result.root / "initial-state.state");
		requireBlob(fileIdentity(base / "initial-state.state", "base initial state",
				"base initial state"), initialStateSource,
				"selected base initial state");
	}
	const Sh4ObservationBackend backend = backendName == "dynarec"
			? Sh4ObservationBackend::Dynarec : Sh4ObservationBackend::Interpreter;
	if ((identity.runtimeConfiguration.cpuBackend == "dynarec")
			!= (backend == Sh4ObservationBackend::Dynarec))
		invalid("identity backend differs from selected base backend");
	const PvrTaManifest manifest = loadPvrTaManifest(
			result.root / "pvr-ta-manifest.json");
	requirePvrTaManifestIdentity(manifest, identity);
	if (manifest.maximumBytes != maximumBytes
			|| manifest.maximumEvents != maximumEvents)
		invalid("job artifact limits differ from manifest");

	requireKeys(job.at("static_analysis"), {"export", "program"},
			"job.static_analysis");
	const Blob exportSource = verifySourceBlob(job.at("static_analysis").at("export"),
			"job.static_analysis.export");
	const Blob programSource = verifySourceBlob(job.at("static_analysis").at("program"),
			"job.static_analysis.program");
	requirePackagedCopy(result.root, "static-analysis.bin", exportSource,
			"static analysis");
	if (!sha256Equal(exportSource.digest, identity.staticAnalysisExportDigest)
			|| !sha256Equal(programSource.digest,
					identity.staticAnalysisProgramDigest)
			|| !sha256Equal(programSource.digest, identity.bootExecutableDigest))
		invalid("static-analysis bytes differ from the selected identity");

	PvrTaArtifactBinding binding;
	binding.backend = backend;
	binding.identityDigest = identity.digest;
	binding.replayDigest = replaySource.digest;
	binding.manifestDigest = manifest.digest;
	const PvrTaArtifactSummary artifact = validatePvrTaArtifactFile(
			result.root / "pvr-ta.fcpvr", binding, maximumBytes, maximumEvents);
	if (artifact.typeCounts[4] != manifest.renderDoneCount)
		invalid("artifact render-done count differs from manifest");

	requireKeys(job.at("publisher"), {"script"}, "job.publisher");
	requireKeys(job.at("artifact_validator"), {"executable"},
			"job.artifact_validator");
	requireKeys(job.at("package_validator"), {"executable"},
			"job.package_validator");
	const Blob publisher = verifySourceBlob(job.at("publisher").at("script"),
			"job.publisher.script");
	const Blob artifactValidator = verifySourceBlob(
			job.at("artifact_validator").at("executable"),
			"job.artifact_validator.executable");
	const Blob packageValidator = verifySourceBlob(
			job.at("package_validator").at("executable"),
			"job.package_validator.executable");
	requirePackagedCopy(result.root, "publisher.ps1", publisher, "publisher");
	requirePackagedCopy(result.root, "artifact-validator.exe", artifactValidator,
			"artifact validator");
	requirePackagedCopy(result.root, "package-validator.exe", packageValidator,
			"package validator");
	result.packagedValidator = fileIdentity(result.root / "package-validator.exe",
			"package-validator.exe", "package validator");
	requireKeys(job.at("limits"), {"validator_timeout_seconds"}, "job.limits");
	const std::uint64_t timeout = unsignedValue(job.at("limits"),
			"validator_timeout_seconds", "job.limits");
	if (timeout == 0 || timeout > 3600 || !job.at("metadata").is_object()
			|| job.at("metadata").size() > 64)
		invalid("job limits or metadata are invalid");

	const std::vector<Blob> inventory = packageInventory(result.root);
	json entries = json::array();
	for (const Blob& entry : inventory)
		entries.push_back(blobJson(entry));
	result.expectedReceipt = {
		{"schema", "flycast-research-pvr-ta-package-validation"},
		{"schema_version", 1},
		{"package_id", result.packageId},
		{"status", "accepted"},
		{"entries", entries},
		{"validator", blobJson(result.packagedValidator)},
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
		invalid("cannot exclusively create validation receipt");
	DWORD written = 0;
	bool success = bytes.size() <= (std::numeric_limits<DWORD>::max)()
			&& WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()),
					&written, nullptr)
			&& written == static_cast<DWORD>(bytes.size())
			&& FlushFileBuffers(output);
	if (!CloseHandle(output))
		success = false;
#else
	const int output = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (output < 0)
		invalid("cannot exclusively create validation receipt");
	const ssize_t written = ::write(output, bytes.data(), bytes.size());
	const bool success = written == static_cast<ssize_t>(bytes.size())
			&& ::fsync(output) == 0 && ::close(output) == 0;
#endif
	if (!success)
	{
		std::error_code ignored;
		std::filesystem::remove(path, ignored);
		invalid("cannot durably write validation receipt");
	}
}

} // namespace

PvrTaPackageSummary validatePvrTaPackageV1ReadOnly(
		const std::filesystem::path& package)
{
	const ValidatedPackage result = validatePackage(package, false);
	return {result.packageId, packageInventory(result.root).size()};
}

PvrTaPackageSummary issuePvrTaPackageV1Receipt(
		const std::filesystem::path& package,
		const std::filesystem::path& receipt,
		const std::filesystem::path& runningValidator)
{
	const ValidatedPackage result = validatePackage(package, true);
	const std::filesystem::path expectedReceipt = result.root
			/ "package-validation.json";
	if (!samePath(std::filesystem::absolute(receipt), expectedReceipt))
		invalid("receipt path is not the fixed package-validation.json entry");
	const Blob running = fileIdentity(canonicalPath(runningValidator,
			"running validator"), "running validator", "running validator");
	requireBlob(running, result.packagedValidator, "running package validator");
	writeExclusive(expectedReceipt, result.expectedReceipt.dump(2) + "\n");
	return {result.packageId, packageInventory(result.root).size()};
}

} // namespace research

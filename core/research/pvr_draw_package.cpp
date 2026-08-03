#include "research/pvr_draw_package.h"

#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/pvr_draw_artifact.h"
#include "research/pvr_presentation_artifact.h"
#include "research/pvr_ta_manifest.h"
#include "research/pvr_ta_package.h"
#include "research/sha256.h"
#include "json.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
	std::string packageId;
	std::filesystem::path accepted;
	Blob packagedValidator;
	json receipt;
};

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid PowerVR draw package v1: " + reason);
}

void require(bool condition, const std::string& reason)
{
	if (!condition) invalid(reason);
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

std::string text(const json& value, const char* name, const std::string& field,
		std::size_t maximum = 4096)
{
	require(value.at(name).is_string(), field + "." + name + " must be a string");
	const std::string result = value.at(name).get<std::string>();
	require(!result.empty() && result.size() <= maximum,
			field + "." + name + " length is invalid");
	return result;
}

std::uint64_t number(const json& value, const char* name,
		const std::string& field)
{
	require(value.at(name).is_number_unsigned(),
			field + "." + name + " must be unsigned");
	return value.at(name).get<std::uint64_t>();
}

bool uuid(const std::string& value)
{
	if (value.size() != 36) return false;
	for (std::size_t i = 0; i < value.size(); ++i)
	{
		if (i == 8 || i == 13 || i == 18 || i == 23)
		{
			if (value[i] != '-') return false;
		}
		else if (!((value[i] >= '0' && value[i] <= '9')
				|| (value[i] >= 'a' && value[i] <= 'f'))) return false;
	}
	return true;
}

std::filesystem::path canonical(const std::filesystem::path& path,
		const std::string& field)
{
	std::error_code error;
	auto result = std::filesystem::weakly_canonical(
			std::filesystem::absolute(path, error), error);
	require(!error && result.is_absolute(), field + " cannot be canonicalized");
	return result;
}

bool samePath(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
{
	std::error_code error1, error2;
	auto left = std::filesystem::weakly_canonical(lhs, error1).native();
	auto right = std::filesystem::weakly_canonical(rhs, error2).native();
	if (error1 || error2) return false;
#ifdef _WIN32
	return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
			right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
#else
	return left == right;
#endif
}

Blob identity(const std::filesystem::path& path, const std::string& pathText,
		const std::string& field)
{
	std::error_code error;
	const auto status = std::filesystem::symlink_status(path, error);
	require(!error && std::filesystem::is_regular_file(status)
			&& !std::filesystem::is_symlink(status),
			field + " is missing, linked, or non-regular");
	const auto size = std::filesystem::file_size(path, error);
	require(!error && size != 0 && size <= std::numeric_limits<std::uint64_t>::max(),
			field + " size is invalid");
	return {pathText, static_cast<std::uint64_t>(size),
			hashFileExact(path, static_cast<std::uint64_t>(size))};
}

void equal(const Blob& actual, const Blob& expected, const std::string& field)
{
	require(actual.size == expected.size
			&& sha256Equal(actual.digest, expected.digest),
			field + " bytes differ");
}

json blobJson(const Blob& value)
{
	return {{"path", value.path}, {"size", value.size},
			{"sha256", sha256ToHex(value.digest)}};
}

json parseJson(const std::filesystem::path& path, const std::string& field)
{
	const auto bytes = readFileExact(path, MaxPvrDrawPackageJsonBytes);
	require(!bytes.empty(), field + " is empty");
	try { return json::parse(bytes.begin(), bytes.end()); }
	catch (const json::exception& exception)
	{
		invalid(field + " JSON parse error: " + exception.what());
	}
}

Blob declaredBlob(const json& value, const std::string& field)
{
	keys(value, {"path", "size", "sha256"}, field);
	Blob result;
	result.path = text(value, "path", field);
	require(std::filesystem::u8path(result.path).is_absolute(),
			field + ".path must be absolute");
	result.size = number(value, "size", field);
	require(result.size != 0, field + ".size must be positive");
	const std::string digest = text(value, "sha256", field, 64);
	require(sha256FromHex(digest, result.digest)
			&& sha256ToHex(result.digest) == digest,
			field + ".sha256 must be lowercase SHA-256");
	equal(identity(std::filesystem::u8path(result.path), result.path, field),
			result, field);
	return result;
}

void packaged(const std::filesystem::path& root, const std::string& name,
		const Blob& source, const std::string& field)
{
	equal(identity(root / name, name, field), source, field);
}

std::set<std::string> inventory(const std::filesystem::path& root)
{
	std::error_code error;
	std::set<std::string> result;
	for (std::filesystem::directory_iterator it(root, error), end;
			it != end; it.increment(error))
	{
		require(!error, "cannot enumerate package");
		const auto status = it->symlink_status(error);
		require(!error && std::filesystem::is_regular_file(status)
				&& !std::filesystem::is_symlink(status),
				"package contains a linked or non-regular entry");
		result.insert(it->path().filename().u8string());
	}
	require(!error, "cannot enumerate package");
	return result;
}

std::vector<Blob> receiptInventory(const std::filesystem::path& root)
{
	std::vector<Blob> result;
	for (const std::string& name : inventory(root))
		if (name != "package-validation.json")
			result.push_back(identity(root / name, name, "package entry"));
	return result;
}

Validated validate(const std::filesystem::path& package, bool issuing)
{
	Validated result;
	result.root = canonical(package, "package");
	std::error_code error;
	const auto status = std::filesystem::symlink_status(result.root, error);
	require(!error && std::filesystem::is_directory(status)
			&& !std::filesystem::is_symlink(status), "package is invalid");
	std::set<std::string> expected {
		"draw-validator.exe", "identity.json", "job.json", "maple-replay.fcmt",
		"package-validator.exe", "publisher.ps1", "pvr-draw.fcpvrd",
		"pvr-presentation.fcpvrp", "pvr-ta-manifest.json", "pvr-ta.fcpvr",
	};
	const bool hasPackagedInitialState =
			std::filesystem::exists(result.root / "initial-state.state");
	if (hasPackagedInitialState)
		expected.insert("initial-state.state");
	if (!issuing) expected.insert("package-validation.json");
	require(inventory(result.root) == expected,
			"package entries do not match the fixed v1 inventory");

	const json job = parseJson(result.root / "job.json", "job");
	std::set<std::string> jobKeys {"schema", "schema_version", "package_id", "output",
			"base_pvr_ta_package", "identity", "maple_replay", "pvr_manifest",
			"ta_artifact", "presentation_artifact", "draw_artifact", "publisher",
			"draw_validator", "package_validator", "limits", "metadata"};
	const bool hasDeclaredInitialState = job.contains("initial_state");
	if (hasDeclaredInitialState)
		jobKeys.insert("initial_state");
	keys(job, jobKeys, "job");
	if (hasDeclaredInitialState != hasPackagedInitialState)
		invalid("job and package initial-state presence differ");
	require(job.at("schema") == "flycast-research-pvr-draw-package-job"
			&& job.at("schema_version") == 1, "unsupported job schema");
	result.packageId = text(job, "package_id", "job", 36);
	require(uuid(result.packageId), "package_id is not a lowercase UUID");
	keys(job.at("output"), {"accepted_directory"}, "job.output");
	result.accepted = canonical(std::filesystem::u8path(text(job.at("output"),
			"accepted_directory", "job.output")), "accepted directory");
	const auto candidate = result.accepted.parent_path()
			/ (".flycast-research-pvr-draw-candidate-" + result.packageId);
	if (issuing)
		require(samePath(result.root, candidate)
				&& !std::filesystem::exists(result.accepted),
				"receipt may only be issued in the private candidate");
	else
		require(samePath(result.root, result.accepted),
				"published package is not at its accepted path");

	keys(job.at("base_pvr_ta_package"), {"accepted_directory"},
			"job.base_pvr_ta_package");
	const auto base = canonical(std::filesystem::u8path(text(
			job.at("base_pvr_ta_package"), "accepted_directory",
			"job.base_pvr_ta_package")), "base PVR TA package");
	validatePvrTaPackageV1ReadOnly(base);
	const Blob identitySource = declaredBlob(job.at("identity"), "job.identity");
	const Blob replaySource = declaredBlob(job.at("maple_replay"), "job.maple_replay");
	const Blob manifestSource = declaredBlob(job.at("pvr_manifest"), "job.pvr_manifest");
	const Blob taSource = declaredBlob(job.at("ta_artifact"), "job.ta_artifact");
	Blob initialStateSource;
	if (hasDeclaredInitialState)
	{
		initialStateSource = declaredBlob(job.at("initial_state"),
				"job.initial_state");
		packaged(result.root, "initial-state.state", initialStateSource,
				"initial state");
	}
	keys(job.at("presentation_artifact"), {"candidate", "maximum_bytes"},
			"job.presentation_artifact");
	keys(job.at("draw_artifact"), {"candidate", "maximum_bytes", "maximum_events"},
			"job.draw_artifact");
	const Blob presentationSource = declaredBlob(
			job.at("presentation_artifact").at("candidate"),
			"job.presentation_artifact.candidate");
	const Blob drawSource = declaredBlob(job.at("draw_artifact").at("candidate"),
			"job.draw_artifact.candidate");
	const std::uint64_t presentationMax = number(job.at("presentation_artifact"),
			"maximum_bytes", "job.presentation_artifact");
	const std::uint64_t drawMax = number(job.at("draw_artifact"), "maximum_bytes",
			"job.draw_artifact");
	const std::uint64_t drawEvents = number(job.at("draw_artifact"),
			"maximum_events", "job.draw_artifact");
	for (const auto& item : std::array<std::tuple<const char*, const Blob*, const char*>, 6> {{
		{"identity.json", &identitySource, "identity"},
		{"maple-replay.fcmt", &replaySource, "replay"},
		{"pvr-ta-manifest.json", &manifestSource, "manifest"},
		{"pvr-ta.fcpvr", &taSource, "TA artifact"},
		{"pvr-presentation.fcpvrp", &presentationSource, "presentation artifact"},
		{"pvr-draw.fcpvrd", &drawSource, "draw artifact"},
	}})
		packaged(result.root, std::get<0>(item), *std::get<1>(item), std::get<2>(item));
	for (const char* name : {"identity.json", "maple-replay.fcmt",
			"pvr-ta-manifest.json", "pvr-ta.fcpvr"})
		equal(identity(result.root / name, name, "packaged base entry"),
				identity(base / name, name, "base entry"), "base PVR TA entry");

	const IdentityManifest identityManifest = loadIdentityManifest(result.root / "identity.json");
	requireSh4EquivalenceIdentityV2(identityManifest);
	if (identityManifest.initialState.available != hasDeclaredInitialState)
		invalid("identity and job initial-state presence differ");
	if (hasDeclaredInitialState)
	{
		require(identityManifest.initialState.size == initialStateSource.size
				&& sha256Equal(identityManifest.initialState.digest,
						initialStateSource.digest),
				"initial-state bytes differ from the selected identity");
		authenticateInitialStateFile(identityManifest,
				result.root / "initial-state.state");
		equal(identity(base / "initial-state.state", "base initial state",
				"base initial state"), initialStateSource,
				"base PVR TA initial state");
	}
	require(identityManifest.runtimeConfiguration.pvrDrawConfiguration.available,
			"identity has no PowerVR draw configuration");
	validateProductionMapleTraceFile(result.root / "maple-replay.fcmt",
			identityManifest.mapleReplayIdentityDigest, replaySource.size);
	const PvrTaManifest manifest = loadPvrTaManifest(result.root / "pvr-ta-manifest.json");
	requirePvrTaManifestIdentity(manifest, identityManifest);
	const auto backend = identityManifest.runtimeConfiguration.cpuBackend == "dynarec"
			? Sh4ObservationBackend::Dynarec : Sh4ObservationBackend::Interpreter;
	PvrTaArtifactBinding taBinding {backend, identityManifest.digest,
			replaySource.digest, manifest.digest};
	const auto ta = validatePvrTaArtifactFile(result.root / "pvr-ta.fcpvr", taBinding,
			manifest.maximumBytes, manifest.maximumEvents);
	require(ta.typeCounts[4] == manifest.renderDoneCount,
			"TA render-done count differs from manifest");
	PvrPresentationArtifactBinding presentationBinding {backend,
			identityManifest.digest, replaySource.digest};
	const auto presentation = validatePvrPresentationArtifactFile(
			result.root / "pvr-presentation.fcpvrp", presentationBinding,
			presentationMax, DefaultMaximumPvrPresentationArtifactEvents);
	require(presentation.completeVerticalSlice, "presentation vertical slice is incomplete");
	PvrDrawArtifactBinding drawBinding;
	drawBinding.backend = backend;
	drawBinding.identityDigest = identityManifest.digest;
	drawBinding.replayDigest = replaySource.digest;
	drawBinding.taArtifactDigest = taSource.digest;
	drawBinding.presentationArtifactDigest = presentationSource.digest;
	drawBinding.rendererConfigurationDigest = pvrDrawConfigurationDigest(
			identityManifest.runtimeConfiguration.pvrDrawConfiguration);
	validatePvrDrawArtifactAgainstTaFile(result.root / "pvr-draw.fcpvrd", drawBinding,
			result.root / "pvr-ta.fcpvr", taBinding, drawMax, drawEvents,
			manifest.maximumBytes, manifest.maximumEvents);

	keys(job.at("publisher"), {"script"}, "job.publisher");
	keys(job.at("draw_validator"), {"executable"}, "job.draw_validator");
	keys(job.at("package_validator"), {"executable"}, "job.package_validator");
	const Blob publisher = declaredBlob(job.at("publisher").at("script"),
			"job.publisher.script");
	const Blob drawValidator = declaredBlob(job.at("draw_validator").at("executable"),
			"job.draw_validator.executable");
	const Blob packageValidator = declaredBlob(
			job.at("package_validator").at("executable"),
			"job.package_validator.executable");
	packaged(result.root, "publisher.ps1", publisher, "publisher");
	packaged(result.root, "draw-validator.exe", drawValidator, "draw validator");
	packaged(result.root, "package-validator.exe", packageValidator, "package validator");
	result.packagedValidator = identity(result.root / "package-validator.exe",
			"package-validator.exe", "package validator");
	keys(job.at("limits"), {"validator_timeout_seconds"}, "job.limits");
	const auto timeout = number(job.at("limits"), "validator_timeout_seconds", "job.limits");
	require(timeout >= 1 && timeout <= 3600 && job.at("metadata").is_object()
			&& job.at("metadata").size() <= 64, "job limits or metadata are invalid");
	json entries = json::array();
	for (const Blob& entry : receiptInventory(result.root)) entries.push_back(blobJson(entry));
	result.receipt = {{"schema", "flycast-research-pvr-draw-package-validation"},
		{"schema_version", 1}, {"package_id", result.packageId},
		{"status", "accepted"}, {"entries", entries},
		{"validator", blobJson(result.packagedValidator)}};
	if (!issuing)
		require(parseJson(result.root / "package-validation.json", "receipt")
				== result.receipt, "validation receipt differs from revalidation");
	return result;
}

void exclusiveWrite(const std::filesystem::path& path, const std::string& bytes)
{
#ifdef _WIN32
	HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL, nullptr);
	require(handle != INVALID_HANDLE_VALUE, "cannot exclusively create receipt");
	DWORD written = 0;
	bool ok = bytes.size() <= (std::numeric_limits<DWORD>::max)()
			&& WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()),
					&written, nullptr) && written == bytes.size()
			&& FlushFileBuffers(handle) && CloseHandle(handle);
	if (!ok) { CloseHandle(handle); std::filesystem::remove(path); invalid("cannot write receipt"); }
#else
	std::ofstream output(path, std::ios::binary | std::ios::out);
	require(output.good(), "cannot create receipt");
	output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
	output.flush();
	require(output.good(), "cannot write receipt");
#endif
}

} // namespace

PvrDrawPackageSummary validatePvrDrawPackageV1ReadOnly(
		const std::filesystem::path& package)
{
	const Validated result = validate(package, false);
	return {result.packageId, receiptInventory(result.root).size()};
}

PvrDrawPackageSummary issuePvrDrawPackageV1Receipt(
		const std::filesystem::path& package, const std::filesystem::path& receipt,
		const std::filesystem::path& runningValidator)
{
	const Validated result = validate(package, true);
	const auto expectedReceipt = result.root / "package-validation.json";
	require(samePath(std::filesystem::absolute(receipt), expectedReceipt),
			"receipt path is not the fixed package entry");
	equal(identity(canonical(runningValidator, "running validator"),
			"running validator", "running validator"), result.packagedValidator,
			"running package validator");
	exclusiveWrite(expectedReceipt, result.receipt.dump(2) + "\n");
	return {result.packageId, receiptInventory(result.root).size()};
}

} // namespace research

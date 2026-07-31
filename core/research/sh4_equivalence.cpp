#include "research/sh4_equivalence.h"

#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/sha256.h"

#include "json.hpp"

#include <algorithm>
#include <cctype>
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
#else
#include <cerrno>
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
	std::filesystem::path path;
	std::string pathText;
	std::uint64_t size = 0;
	Sha256Digest digest {};
};

struct BackendInput
{
	Blob identity;
	Blob trace;
};

struct ManifestInput
{
	std::string kind;
	std::string name;
	Blob source;
};

struct Job
{
	std::string jobId;
	BackendInput interpreter;
	BackendInput dynarec;
	Blob emulator;
	Blob replay;
	Blob manifestSet;
	Blob comparator;
	std::vector<ManifestInput> manifests;
	std::uint64_t maximumReplayBytes = 0;
	std::uint64_t maximumTraceBytes = 0;
	std::uint64_t maximumEvents = 0;
	std::vector<std::uint8_t> bytes;
	Sha256Digest digest {};
};

struct IdentityDetails
{
	IdentityManifest manifest;
	std::string gitCommit;
	std::uint64_t emulatorSize = 0;
	Sha256Digest emulatorDigest {};
	Sha256Digest configurationDigest {};
	json normalizedConfiguration;
};

struct Evaluation
{
	Sh4EquivalenceReportSummary summary;
	std::string reportBytes;
};

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid SH-4 equivalence job/report v1: " + reason);
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

const json& object(const json& parent, const char *name, const std::string& field)
{
	if (!parent.contains(name) || !parent.at(name).is_object())
		invalid(field + "." + name + " must be an object");
	return parent.at(name);
}

std::string stringValue(const json& parent, const char *name,
		const std::string& field, std::size_t maximum = 4096)
{
	if (!parent.contains(name) || !parent.at(name).is_string())
		invalid(field + "." + name + " must be a string");
	const std::string result = parent.at(name).get<std::string>();
	if (result.empty() || result.size() > maximum)
		invalid(field + "." + name + " has an invalid length");
	return result;
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

std::uint64_t fileSize(const std::filesystem::path& path)
{
	std::error_code error;
	const std::uintmax_t raw = std::filesystem::file_size(path, error);
	if (error || raw > std::numeric_limits<std::uint64_t>::max())
		invalid("cannot determine file size: " + path.string());
	return static_cast<std::uint64_t>(raw);
}

bool safeRelativePath(const std::filesystem::path& path)
{
	if (path.empty() || path.is_absolute() || path.has_root_path())
		return false;
	for (const std::filesystem::path& component : path)
		if (component == "." || component == ".." || component.empty())
			return false;
	return true;
}

Blob parseBlob(const json& value, const std::string& field,
		const std::filesystem::path& jobDirectory)
{
	requireKeys(value, {"path", "sha256", "size"}, field);
	Blob result;
	result.pathText = stringValue(value, "path", field);
	const std::filesystem::path declared = std::filesystem::u8path(result.pathText);
	if (declared.is_absolute())
		result.path = declared;
	else
	{
		if (!safeRelativePath(declared))
			invalid(field + ".path must be absolute or a safe relative path");
		result.path = jobDirectory / declared;
	}
	result.size = unsignedValue(value, "size", field);
	if (result.size == 0)
		invalid(field + ".size must be positive");
	const std::string digest = stringValue(value, "sha256", field, 64);
	if (!sha256FromHex(digest, result.digest) || sha256ToHex(result.digest) != digest)
		invalid(field + ".sha256 must be lowercase SHA-256");
	return result;
}

void authenticate(const Blob& blob, const std::string& field)
{
	if (fileSize(blob.path) != blob.size)
		invalid(field + " size differs from its declaration");
	if (!sha256Equal(hashFileExact(blob.path, blob.size), blob.digest))
		invalid(field + " digest differs from its declaration");
}

BackendInput parseBackend(const json& value, const std::string& field,
		const std::filesystem::path& jobDirectory)
{
	requireKeys(value, {"identity", "trace"}, field);
	return BackendInput {parseBlob(value.at("identity"), field + ".identity",
			jobDirectory), parseBlob(value.at("trace"), field + ".trace",
			jobDirectory)};
}

bool lowercaseToken(const std::string& value)
{
	return !value.empty() && std::all_of(value.begin(), value.end(),
			[](unsigned char character) {
				return std::islower(character) || std::isdigit(character)
						|| character == '-';
			});
}

bool safeEntryName(const std::string& value)
{
	return !value.empty() && value != "." && value != ".."
			&& std::all_of(value.begin(), value.end(), [](unsigned char character) {
				return std::isalnum(character) || character == '.'
						|| character == '_' || character == '-';
			});
}

Job loadJob(const std::filesystem::path& path)
{
	Job job;
	job.bytes = readFileExact(path, MaxSh4EquivalenceJsonBytes);
	if (job.bytes.empty())
		invalid("job is empty");
	job.digest = sha256(job.bytes.data(), job.bytes.size());
	const std::filesystem::path jobDirectory = path.parent_path();
	json root;
	try
	{
		root = json::parse(job.bytes.begin(), job.bytes.end());
	}
	catch (const json::exception& exception)
	{
		invalid(std::string("job JSON parse error: ") + exception.what());
	}
	requireKeys(root, {"comparator", "dynarec", "emulator", "interpreter",
			"job_id", "limits", "manifest_set", "manifests", "metadata", "replay",
			"schema", "schema_version"}, "job");
	if (root.at("schema") != "flycast-research-sh4-equivalence-job"
			|| !root.at("schema_version").is_number_unsigned()
			|| root.at("schema_version").get<std::uint64_t>() != 1)
		invalid("unsupported job schema");
	job.jobId = stringValue(root, "job_id", "job", 36);
	if (!uuid(job.jobId))
		invalid("job.job_id must be a lowercase UUID");
	job.interpreter = parseBackend(root.at("interpreter"), "job.interpreter",
			jobDirectory);
	job.dynarec = parseBackend(root.at("dynarec"), "job.dynarec", jobDirectory);
	job.emulator = parseBlob(root.at("emulator"), "job.emulator", jobDirectory);
	job.replay = parseBlob(root.at("replay"), "job.replay", jobDirectory);
	job.manifestSet = parseBlob(root.at("manifest_set"), "job.manifest_set",
			jobDirectory);
	const json& comparator = object(root, "comparator", "job");
	requireKeys(comparator, {"executable"}, "job.comparator");
	job.comparator = parseBlob(comparator.at("executable"),
			"job.comparator.executable", jobDirectory);
	if (!root.at("manifests").is_array() || root.at("manifests").empty()
			|| root.at("manifests").size() > 64)
		invalid("job.manifests count is outside [1, 64]");
	std::string previousManifestKey;
	for (std::size_t index = 0; index < root.at("manifests").size(); ++index)
	{
		const json& entry = root.at("manifests").at(index);
		const std::string field = "job.manifests[" + std::to_string(index) + "]";
		requireKeys(entry, {"kind", "name", "source"}, field);
		ManifestInput manifest;
		manifest.kind = stringValue(entry, "kind", field, 64);
		manifest.name = stringValue(entry, "name", field, 128);
		if (!lowercaseToken(manifest.kind) || !safeEntryName(manifest.name))
			invalid(field + " kind or name is invalid");
		const std::string key = manifest.kind + "\n" + manifest.name;
		if (!previousManifestKey.empty() && key <= previousManifestKey)
			invalid("job.manifests must be uniquely ordered by kind and name");
		previousManifestKey = key;
		manifest.source = parseBlob(entry.at("source"), field + ".source",
				jobDirectory);
		job.manifests.push_back(std::move(manifest));
	}
	const json& limits = object(root, "limits", "job");
	requireKeys(limits, {"maximum_events", "maximum_replay_bytes",
			"maximum_trace_bytes"}, "job.limits");
	job.maximumReplayBytes = unsignedValue(limits, "maximum_replay_bytes",
			"job.limits");
	job.maximumTraceBytes = unsignedValue(limits, "maximum_trace_bytes",
			"job.limits");
	job.maximumEvents = unsignedValue(limits, "maximum_events", "job.limits");
	if (job.maximumReplayBytes < MapleTraceHeaderSize
			|| job.maximumReplayBytes > DefaultMaximumMapleTraceBytes)
		invalid("job replay byte limit is outside the implementation bound");
	if (job.maximumTraceBytes < Sh4ObservationTraceHeaderSize
			|| job.maximumTraceBytes > DefaultMaximumSh4ObservationTraceBytes)
		invalid("job trace byte limit is outside the implementation bound");
	if (job.maximumEvents == 0
			|| job.maximumEvents > DefaultMaximumSh4ObservationTraceEvents)
		invalid("job event limit is outside the implementation bound");
	if (!root.at("metadata").is_object())
		invalid("job.metadata must be an object");
	return job;
}

void requireAllDistinct(const std::vector<std::pair<const char *, const Blob *>>& blobs)
{
	for (std::size_t left = 0; left < blobs.size(); ++left)
		for (std::size_t right = left + 1; right < blobs.size(); ++right)
			if (pathsAlias(blobs[left].second->path, blobs[right].second->path))
				invalid(std::string(blobs[left].first) + " aliases " + blobs[right].first);
}

IdentityDetails loadIdentityDetails(const Blob& blob, const char *backend)
{
	IdentityDetails result;
	result.manifest = loadIdentityManifest(blob.path);
	requireSh4EquivalenceIdentityV2(result.manifest);
	if (result.manifest.runtimeConfiguration.cpuBackend != backend)
		invalid(std::string(backend) + " identity declares a different backend");
	json root;
	try
	{
		root = json::parse(result.manifest.bytes.begin(), result.manifest.bytes.end());
	}
	catch (const json::exception& exception)
	{
		invalid(std::string("identity JSON parse error: ") + exception.what());
	}
	const json& emulator = root.at("emulator");
	result.gitCommit = emulator.at("git_commit").get<std::string>();
	result.emulatorSize = emulator.at("executable").at("size").get<std::uint64_t>();
	if (!sha256FromHex(emulator.at("executable").at("sha256").get<std::string>(),
			result.emulatorDigest))
		invalid("identity emulator digest is invalid");
	const json& configuration = root.at("configuration");
	if (!sha256FromHex(configuration.at("sha256").get<std::string>(),
			result.configurationDigest))
		invalid("identity configuration digest is invalid");
	result.normalizedConfiguration = configuration.at("values");
	result.normalizedConfiguration.erase("cpu_backend");
	result.normalizedConfiguration.erase("dynarec_observation");
	return result;
}

std::size_t validateManifestSet(const Blob& blob,
		const std::vector<ManifestInput>& manifests,
		const std::vector<std::filesystem::path>& reservedPaths)
{
	const std::vector<std::uint8_t> bytes = readFileExact(blob.path, blob.size);
	json root;
	try
	{
		root = json::parse(bytes.begin(), bytes.end());
	}
	catch (const json::exception& exception)
	{
		invalid(std::string("manifest-set JSON parse error: ") + exception.what());
	}
	requireKeys(root, {"manifests", "schema", "schema_version"}, "manifest_set");
	if (root.at("schema") != "flycast-research-sh4-equivalence-manifest-set"
			|| !root.at("schema_version").is_number_unsigned()
			|| root.at("schema_version").get<std::uint64_t>() != 1)
		invalid("unsupported manifest-set schema");
	if (!root.at("manifests").is_array() || root.at("manifests").empty()
			|| root.at("manifests").size() > 64)
		invalid("manifest_set.manifests count is outside [1, 64]");
	if (root.at("manifests").size() != manifests.size())
		invalid("job and manifest-set manifest counts differ");
	std::string previousKey;
	std::vector<std::filesystem::path> paths;
	for (std::size_t index = 0; index < root.at("manifests").size(); ++index)
	{
		const json& entry = root.at("manifests").at(index);
		const std::string field = "manifest_set.manifests[" + std::to_string(index) + "]";
		requireKeys(entry, {"kind", "name", "sha256", "size"}, field);
		const std::string kind = stringValue(entry, "kind", field, 64);
		const std::string name = stringValue(entry, "name", field, 128);
		if (!lowercaseToken(kind))
			invalid(field + ".kind must be a lowercase token");
		if (!safeEntryName(name))
			invalid(field + ".name must be a safe entry name");
		const std::string key = kind + "\n" + name;
		if (!previousKey.empty() && key <= previousKey)
			invalid("manifest_set.manifests must be uniquely ordered by kind and name");
		previousKey = key;
		const ManifestInput& declared = manifests.at(index);
		if (kind != declared.kind || name != declared.name
				|| unsignedValue(entry, "size", field) != declared.source.size)
			invalid(field + " differs from the job manifest mapping");
		Sha256Digest inventoryDigest {};
		const std::string inventoryHex = stringValue(entry, "sha256", field, 64);
		if (!sha256FromHex(inventoryHex, inventoryDigest)
				|| sha256ToHex(inventoryDigest) != inventoryHex
				|| !sha256Equal(inventoryDigest, declared.source.digest))
			invalid(field + ".sha256 differs from the job manifest mapping");
		const Blob& source = declared.source;
		authenticate(source, field + ".source");
		for (const std::filesystem::path& reserved : reservedPaths)
			if (pathsAlias(reserved, source.path))
				invalid(field + ".source aliases a non-manifest job input");
		for (const std::filesystem::path& prior : paths)
			if (pathsAlias(prior, source.path))
				invalid("manifest-set sources alias");
		paths.push_back(source.path);
	}
	return root.at("manifests").size();
}

json blobJson(const Blob& blob)
{
	return {{"path", blob.pathText}, {"size", blob.size},
			{"sha256", sha256ToHex(blob.digest)}};
}

json traceJson(const Blob& blob, const Sh4ObservationTraceSummary& summary)
{
	return {{"file", blobJson(blob)}, {"event_count", summary.eventCount},
			{"payload_bytes", summary.payloadBytes},
			{"payload_sha256", sha256ToHex(summary.payloadDigest)},
			{"start_tick", summary.startTick}, {"end_tick", summary.endTick},
			{"dropped_events", summary.droppedEvents}};
}

Evaluation evaluate(const std::filesystem::path& jobPath,
		const std::filesystem::path& runningComparator)
{
	Job job = loadJob(jobPath);
	const std::vector<std::pair<const char *, const Blob *>> inputBlobs {
			{"interpreter identity", &job.interpreter.identity},
			{"interpreter trace", &job.interpreter.trace},
			{"dynarec identity", &job.dynarec.identity},
			{"dynarec trace", &job.dynarec.trace}, {"emulator", &job.emulator},
			{"replay", &job.replay}, {"manifest set", &job.manifestSet},
			{"comparator", &job.comparator},
	};
	requireAllDistinct(inputBlobs);
	for (const auto& item : inputBlobs)
		authenticate(*item.second, item.first);
	for (const ManifestInput& manifest : job.manifests)
		authenticate(manifest.source, "job manifest source");
	const Blob running {runningComparator, runningComparator.u8string(),
			fileSize(runningComparator), hashFileExact(runningComparator,
					job.comparator.size)};
	if (running.size != job.comparator.size
			|| !sha256Equal(running.digest, job.comparator.digest))
		invalid("running comparator does not match job.comparator.executable");
	std::vector<std::filesystem::path> reservedPaths {jobPath, runningComparator};
	for (const auto& item : inputBlobs)
		reservedPaths.push_back(item.second->path);

	const IdentityDetails interpreter = loadIdentityDetails(
			job.interpreter.identity, "interpreter");
	const IdentityDetails dynarec = loadIdentityDetails(job.dynarec.identity, "dynarec");
	if (sha256Equal(interpreter.manifest.digest, dynarec.manifest.digest))
		invalid("backend-specific identities must be distinct");
	if (interpreter.gitCommit != dynarec.gitCommit
			|| interpreter.emulatorSize != dynarec.emulatorSize
			|| !sha256Equal(interpreter.emulatorDigest, dynarec.emulatorDigest))
		invalid("backend identities do not bind the same emulator build");
	if (job.emulator.size != interpreter.emulatorSize
			|| !sha256Equal(job.emulator.digest, interpreter.emulatorDigest))
		invalid("job emulator bytes do not match the backend identities");
	if (interpreter.normalizedConfiguration != dynarec.normalizedConfiguration)
		invalid("backend configurations differ outside backend observation settings");
	if (!sha256Equal(interpreter.manifest.mapleReplayIdentityDigest,
			dynarec.manifest.mapleReplayIdentityDigest))
		invalid("backend identities bind different Maple replay identities");
	validateProductionMapleTraceFile(job.replay.path,
			interpreter.manifest.mapleReplayIdentityDigest, job.maximumReplayBytes);
	const std::size_t manifestCount = validateManifestSet(job.manifestSet,
			job.manifests, reservedPaths);

	Sh4ObservationEquivalenceContract contract;
	contract.interpreterIdentityDigest = interpreter.manifest.digest;
	contract.dynarecIdentityDigest = dynarec.manifest.digest;
	contract.replayDigest = job.replay.digest;
	contract.manifestSetDigest = job.manifestSet.digest;
	contract.maximumTraceBytes = job.maximumTraceBytes;
	contract.maximumEvents = job.maximumEvents;
	const Sh4ObservationComparison comparison = compareSh4ObservationTraces(
			job.interpreter.trace.path, job.dynarec.trace.path, contract);

	json divergence = nullptr;
	if (comparison.firstDivergence.has_value())
	{
		const Sh4ObservationDivergence& value = *comparison.firstDivergence;
		divergence = {{"ordinal", value.ordinal}, {"field", value.field},
				{"interpreter_value", value.interpreterValue},
				{"dynarec_value", value.dynarecValue}};
	}
	json report {
		{"schema", "flycast-research-sh4-equivalence-report"},
		{"schema_version", 1},
		{"job_id", job.jobId},
		{"status", comparison.equivalent ? "equivalent" : "divergent"},
		{"job", {{"size", job.bytes.size()},
				{"sha256", sha256ToHex(job.digest)}}},
		{"emulator", {{"git_commit", interpreter.gitCommit},
				{"file", blobJson(job.emulator)}}},
		{"replay", {{"file", blobJson(job.replay)},
				{"maple_identity_sha256", sha256ToHex(
						interpreter.manifest.mapleReplayIdentityDigest)}}},
		{"manifest_set", {{"file", blobJson(job.manifestSet)},
				{"manifest_count", manifestCount}}},
		{"interpreter", {
				{"identity", blobJson(job.interpreter.identity)},
				{"identity_sha256", sha256ToHex(interpreter.manifest.digest)},
				{"configuration_sha256", sha256ToHex(
						interpreter.configurationDigest)},
				{"trace", traceJson(job.interpreter.trace, comparison.interpreter)}}},
		{"dynarec", {
				{"identity", blobJson(job.dynarec.identity)},
				{"identity_sha256", sha256ToHex(dynarec.manifest.digest)},
				{"configuration_sha256", sha256ToHex(dynarec.configurationDigest)},
				{"trace", traceJson(job.dynarec.trace, comparison.dynarec)}}},
		{"comparator", {{"executable", blobJson(job.comparator)}}},
		{"matched_event_count", comparison.matchedEventCount},
		{"first_divergence", std::move(divergence)},
	};
	Evaluation result;
	result.summary.jobId = job.jobId;
	result.summary.equivalent = comparison.equivalent;
	result.summary.matchedEventCount = comparison.matchedEventCount;
	result.summary.firstDivergence = comparison.firstDivergence;
	result.reportBytes = report.dump(2) + "\n";

	// No authenticated input may change between admission and report issue.
	const std::vector<std::uint8_t> jobAfter = readFileExact(jobPath,
			MaxSh4EquivalenceJsonBytes);
	if (jobAfter != job.bytes)
		invalid("job changed during comparison");
	for (const auto& item : inputBlobs)
		authenticate(*item.second, item.first);
	if (validateManifestSet(job.manifestSet, job.manifests, reservedPaths)
			!= manifestCount)
		invalid("manifest set changed during comparison");
	if (fileSize(runningComparator) != running.size
			|| !sha256Equal(hashFileExact(runningComparator, running.size),
					running.digest))
		invalid("running comparator changed during comparison");
	return result;
}

void writeExclusive(const std::filesystem::path& path, const std::string& bytes)
{
#ifdef _WIN32
	HANDLE output = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
			CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (output == INVALID_HANDLE_VALUE)
		invalid(GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS
				? "report destination already exists"
				: "cannot create report destination");
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
		invalid(errno == EEXIST ? "report destination already exists"
				: "cannot create report destination");
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
		invalid("cannot write report destination");
	}
}

} // namespace

Sh4EquivalenceReportSummary issueSh4EquivalenceReport(
		const std::filesystem::path& jobPath,
		const std::filesystem::path& reportPath,
		const std::filesystem::path& runningComparator)
{
	if (pathsAlias(jobPath, reportPath))
		invalid("job and report paths alias");
	const Evaluation result = evaluate(jobPath, runningComparator);
	writeExclusive(reportPath, result.reportBytes);
	return result.summary;
}

Sh4EquivalenceReportSummary validateSh4EquivalenceReport(
		const std::filesystem::path& jobPath,
		const std::filesystem::path& reportPath,
		const std::filesystem::path& runningComparator)
{
	if (pathsAlias(jobPath, reportPath))
		invalid("job and report paths alias");
	const Evaluation result = evaluate(jobPath, runningComparator);
	const std::vector<std::uint8_t> actual = readFileExact(reportPath,
			MaxSh4EquivalenceJsonBytes);
	if (actual.size() != result.reportBytes.size()
			|| !std::equal(actual.begin(), actual.end(), result.reportBytes.begin()))
		invalid("report bytes differ from independent recomputation");
	return result.summary;
}

} // namespace research

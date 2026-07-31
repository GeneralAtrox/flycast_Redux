#include "research/identity_manifest.h"

#include "json.hpp"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>

namespace research
{
namespace
{

using json = nlohmann::json;

[[noreturn]] void invalid(const std::string& reason)
{
	throw std::runtime_error("invalid research identity manifest: " + reason);
}

const json& requiredObject(const json& parent, const char *name)
{
	if (!parent.contains(name) || !parent.at(name).is_object())
		invalid(std::string("missing object '") + name + "'");
	return parent.at(name);
}

const json& requiredArray(const json& parent, const char *name)
{
	if (!parent.contains(name) || !parent.at(name).is_array())
		invalid(std::string("missing array '") + name + "'");
	return parent.at(name);
}

void validateSha256(const json& value, const std::string& field)
{
	if (!value.is_string())
		invalid(field + " must be a lowercase SHA-256 string");
	Sha256Digest digest {};
	if (!sha256FromHex(value.get<std::string>(), digest))
		invalid(field + " must be a lowercase SHA-256 string");
}

void validateBlob(const json& value, const std::string& field)
{
	if (!value.is_object())
		invalid(field + " must be an object");
	if (!value.contains("size") || !value.at("size").is_number_unsigned())
		invalid(field + ".size must be an unsigned integer");
	if (!value.contains("sha256"))
		invalid(field + ".sha256 is missing");
	validateSha256(value.at("sha256"), field + ".sha256");
	if (value.contains("path") && !value.at("path").is_string())
		invalid(field + ".path must be a string");
}

const json& requiredConfigurationValue(const json& values, const char *name)
{
	if (!values.contains(name))
		invalid(std::string("configuration.values.") + name + " is missing");
	return values.at(name);
}

struct ValidatedIdentity
{
	std::uint32_t schemaVersion = 0;
	IdentityRuntimeConfiguration runtimeConfiguration;
	std::string mediaKind;
	std::size_t mediaTrackCount = 0;
	Sha256Digest bootExecutableDigest {};
	bool hasStaticAnalysis = false;
	Sha256Digest staticAnalysisProgramDigest {};
	Sha256Digest staticAnalysisExportDigest {};
	std::uint32_t staticAnalysisImageBase = 0;
	bool hasHookManifestDigest = false;
	Sha256Digest hookManifestDigest {};
	bool hasMapleReplayIdentityDigest = false;
	Sha256Digest mapleReplayIdentityDigest {};
};

std::uint32_t parseImageBase(const std::string& value)
{
	if (value.size() != 10 || value[0] != '0' || value[1] != 'x')
		invalid("static_analysis.image_base must use 0x00000000 form");
	std::uint32_t result = 0;
	for (std::size_t index = 2; index < value.size(); ++index)
	{
		const char character = value[index];
		unsigned digit = 0;
		if (character >= '0' && character <= '9')
			digit = static_cast<unsigned>(character - '0');
		else if (character >= 'a' && character <= 'f')
			digit = static_cast<unsigned>(character - 'a' + 10);
		else if (character >= 'A' && character <= 'F')
			digit = static_cast<unsigned>(character - 'A' + 10);
		else
			invalid("static_analysis.image_base must use 0x00000000 form");
		result = (result << 4) | digit;
	}
	return result;
}

ValidatedIdentity validateIdentityJson(const json& root)
{
	if (!root.is_object())
		invalid("root must be an object");
	const std::set<std::string> allowedTopLevel {
		"schema",
		"schema_version",
		"media",
		"firmware",
		"persistent_devices",
		"emulator",
		"configuration",
		"static_analysis",
		"equivalence",
	};
	for (const auto& item : root.items())
		if (allowedTopLevel.find(item.key()) == allowedTopLevel.end())
			invalid("unknown top-level field '" + item.key() + "'");

	if (!root.contains("schema") || root.at("schema") != "flycast-research-identity")
		invalid("unsupported schema");
	if (!root.contains("schema_version") || !root.at("schema_version").is_number_unsigned())
		invalid("unsupported schema_version");
	const std::uint64_t schemaVersionValue = root.at("schema_version").get<std::uint64_t>();
	if (schemaVersionValue != 1 && schemaVersionValue != 2)
		invalid("unsupported schema_version");
	const std::uint32_t schemaVersion = static_cast<std::uint32_t>(schemaVersionValue);
	if (schemaVersion == 1 && root.contains("equivalence"))
		invalid("identity v1 does not permit equivalence metadata");

	const json& media = requiredObject(root, "media");
	if (!media.contains("kind") || !media.at("kind").is_string())
		invalid("media.kind is missing");
	const std::set<std::string> mediaKinds {
		"gdi", "cue", "chd", "cdi", "elf", "bios", "arcade",
	};
	const std::string mediaKind = media.at("kind").get<std::string>();
	if (mediaKinds.find(mediaKind) == mediaKinds.end())
		invalid("media.kind is unsupported");
	validateBlob(media.contains("source") ? media.at("source") : json(), "media.source");
	validateBlob(media.contains("ip_bin") ? media.at("ip_bin") : json(), "media.ip_bin");
	validateBlob(media.contains("boot_executable") ? media.at("boot_executable") : json(),
			"media.boot_executable");
	Sha256Digest bootExecutableDigest {};
	if (!sha256FromHex(media.at("boot_executable").at("sha256").get<std::string>(),
			bootExecutableDigest))
		invalid("media.boot_executable.sha256 must be a lowercase SHA-256 string");
	if (!media.at("boot_executable").contains("name")
			|| !media.at("boot_executable").at("name").is_string()
			|| media.at("boot_executable").at("name").get<std::string>().empty())
		invalid("media.boot_executable.name is missing");
	if (media.contains("tracks"))
	{
		if (!media.at("tracks").is_array())
			invalid("media.tracks must be an array");
		std::uint64_t previousTrack = 0;
		for (std::size_t i = 0; i < media.at("tracks").size(); ++i)
		{
			const json& track = media.at("tracks").at(i);
			const std::string field = "media.tracks[" + std::to_string(i) + "]";
			validateBlob(track, field);
			for (const char *number : {"track", "start_fad", "sector_size", "offset"})
				if (!track.contains(number) || !track.at(number).is_number_unsigned())
					invalid(field + "." + number + " must be an unsigned integer");
			const std::uint64_t trackNumber = track.at("track").get<std::uint64_t>();
			if (trackNumber == 0 || trackNumber > 99 || trackNumber <= previousTrack)
				invalid(field + ".track must be strictly increasing in [1, 99]");
			previousTrack = trackNumber;
		}
	}

	const json& firmware = requiredObject(root, "firmware");
	if (!firmware.contains("mode") || !firmware.at("mode").is_string())
		invalid("firmware.mode is missing");
	const std::string firmwareMode = firmware.at("mode").get<std::string>();
	if (firmwareMode != "real" && firmwareMode != "hle")
		invalid("firmware.mode must be 'real' or 'hle'");
	validateBlob(firmware.contains("flash_initial") ? firmware.at("flash_initial") : json(),
			"firmware.flash_initial");
	if (firmwareMode == "real")
	{
		if (!firmware.contains("bios") || firmware.at("bios").is_null())
			invalid("real firmware requires firmware.bios");
		validateBlob(firmware.at("bios"), "firmware.bios");
	}
	else if (!firmware.contains("hle_identity") || !firmware.at("hle_identity").is_string()
			|| firmware.at("hle_identity").get<std::string>().empty())
	{
		invalid("HLE firmware requires firmware.hle_identity");
	}

	const json& devices = requiredArray(root, "persistent_devices");
	for (std::size_t i = 0; i < devices.size(); ++i)
	{
		const json& device = devices.at(i);
		const std::string field = "persistent_devices[" + std::to_string(i) + "]";
		validateBlob(device, field);
		if (!device.contains("kind") || !device.at("kind").is_string()
				|| device.at("kind").get<std::string>().empty())
			invalid(field + ".kind is missing");
		for (const char *number : {"bus", "port"})
			if (!device.contains(number) || !device.at(number).is_number_unsigned())
				invalid(field + "." + number + " must be an unsigned integer");
		if (device.at("bus").get<std::uint64_t>() > 3
				|| device.at("port").get<std::uint64_t>() > 5)
			invalid(field + " bus/port is out of range");
	}

	const json& emulator = requiredObject(root, "emulator");
	if (!emulator.contains("git_commit") || !emulator.at("git_commit").is_string())
		invalid("emulator.git_commit is missing");
	const std::string commit = emulator.at("git_commit").get<std::string>();
	if (commit.size() != 40
			|| !std::all_of(commit.begin(), commit.end(), [](char c) {
				return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
			}))
		invalid("emulator.git_commit must be 40 lowercase hexadecimal characters");
	validateBlob(emulator.contains("executable") ? emulator.at("executable") : json(),
			"emulator.executable");

	const json& configuration = requiredObject(root, "configuration");
	if (!configuration.contains("values") || !configuration.at("values").is_object())
		invalid("configuration.values must be an object");
	const json& values = configuration.at("values");
	if (schemaVersion == 1 && values.contains("dynarec_observation"))
		invalid("identity v1 does not permit configuration.values.dynarec_observation");
	IdentityRuntimeConfiguration runtimeConfiguration;
	const json& cpuBackend = requiredConfigurationValue(values, "cpu_backend");
	if (!cpuBackend.is_string())
		invalid("configuration.values.cpu_backend must be a string");
	runtimeConfiguration.cpuBackend = cpuBackend.get<std::string>();
	if (schemaVersion == 1 && runtimeConfiguration.cpuBackend != "interpreter")
		invalid("identity v1 configuration.values.cpu_backend must be 'interpreter'");
	if (schemaVersion == 2 && runtimeConfiguration.cpuBackend != "interpreter"
			&& runtimeConfiguration.cpuBackend != "dynarec")
		invalid("identity v2 configuration.values.cpu_backend is unsupported");
	if (schemaVersion == 2)
	{
		const json& observation = requiredConfigurationValue(values,
				"dynarec_observation");
		if (!observation.is_boolean())
			invalid("configuration.values.dynarec_observation must be boolean");
		runtimeConfiguration.dynarecObservation = observation.get<bool>();
		if (runtimeConfiguration.dynarecObservation
				!= (runtimeConfiguration.cpuBackend == "dynarec"))
			invalid("identity v2 dynarec_observation must match cpu_backend");
		const json& rtcSeed = requiredConfigurationValue(values,
				"dreamcast_rtc_seed");
		if (!rtcSeed.is_number_unsigned()
				|| rtcSeed.get<std::uint64_t>()
						> std::numeric_limits<std::uint32_t>::max())
			invalid("identity v2 dreamcast_rtc_seed is outside [0, 4294967295]");
		runtimeConfiguration.dreamcastRtcSeed =
				static_cast<std::uint32_t>(rtcSeed.get<std::uint64_t>());
	}
	for (const auto& [name, destination] : {
			std::pair<const char *, bool *>("threaded_rendering",
					&runtimeConfiguration.threadedRendering),
			std::pair<const char *, bool *>("autoload_state",
					&runtimeConfiguration.autoLoadState),
			std::pair<const char *, bool *>("autosave_state",
					&runtimeConfiguration.autoSaveState),
			std::pair<const char *, bool *>("ggpo", &runtimeConfiguration.ggpo),
	})
	{
		const json& value = requiredConfigurationValue(values, name);
		if (!value.is_boolean() || value.get<bool>())
			invalid(std::string("configuration.values.") + name + " must be false");
		*destination = false;
	}
	if (values.contains("maple_dma_checkpoint"))
	{
		const json& checkpoint = values.at("maple_dma_checkpoint");
		if (!checkpoint.is_number_unsigned()
				|| checkpoint.get<std::uint64_t>() == 0
				|| checkpoint.get<std::uint64_t>() > MaximumMapleDmaCheckpoint)
			invalid("configuration.values.maple_dma_checkpoint is outside [1, 10000000]");
		runtimeConfiguration.mapleDmaCheckpoint = checkpoint.get<std::uint64_t>();
	}
	if (values.contains("sh4_observation_start_dma"))
	{
		const json& startDma = values.at("sh4_observation_start_dma");
		if (!startDma.is_number_unsigned()
				|| startDma.get<std::uint64_t>() == 0
				|| startDma.get<std::uint64_t>() > MaximumMapleDmaCheckpoint)
			invalid("configuration.values.sh4_observation_start_dma is outside [1, 10000000]");
		runtimeConfiguration.sh4ObservationStartDma =
				startDma.get<std::uint64_t>();
	}
	if (!configuration.contains("sha256"))
		invalid("configuration.sha256 is missing");
	validateSha256(configuration.at("sha256"), "configuration.sha256");
	const std::string canonicalConfiguration = configuration.at("values").dump();
	const Sha256Digest computedConfiguration = sha256(canonicalConfiguration.data(),
			canonicalConfiguration.size());
	Sha256Digest declaredConfiguration {};
	if (!sha256FromHex(configuration.at("sha256").get<std::string>(), declaredConfiguration)
			|| !sha256Equal(computedConfiguration, declaredConfiguration))
		invalid("configuration.sha256 does not match canonical configuration.values bytes");

	bool hasStaticAnalysis = false;
	Sha256Digest staticAnalysisProgramDigest {};
	Sha256Digest staticAnalysisExportDigest {};
	std::uint32_t staticAnalysisImageBase = 0;
	bool hasHookManifestDigest = false;
	Sha256Digest hookManifestDigest {};
	if (root.contains("static_analysis"))
	{
		hasStaticAnalysis = true;
		const json& staticAnalysis = requiredObject(root, "static_analysis");
		for (const char *field : {"program_sha256", "export_sha256"})
		{
			if (!staticAnalysis.contains(field))
				invalid(std::string("static_analysis.") + field + " is missing");
			validateSha256(staticAnalysis.at(field), std::string("static_analysis.") + field);
		}
		if (staticAnalysis.contains("hook_manifest_sha256"))
			validateSha256(staticAnalysis.at("hook_manifest_sha256"),
					"static_analysis.hook_manifest_sha256");
		if (!staticAnalysis.contains("image_base") || !staticAnalysis.at("image_base").is_string())
			invalid("static_analysis.image_base is missing");
		staticAnalysisImageBase = parseImageBase(
				staticAnalysis.at("image_base").get<std::string>());
		if (!sha256FromHex(staticAnalysis.at("program_sha256").get<std::string>(),
				staticAnalysisProgramDigest)
				|| !sha256FromHex(staticAnalysis.at("export_sha256").get<std::string>(),
						staticAnalysisExportDigest))
			invalid("static_analysis digest conversion failed");
		if (staticAnalysis.contains("hook_manifest_sha256"))
		{
			hasHookManifestDigest = true;
			if (!sha256FromHex(staticAnalysis.at("hook_manifest_sha256").get<std::string>(),
					hookManifestDigest))
				invalid("static_analysis.hook_manifest_sha256 conversion failed");
		}
	}

	bool hasMapleReplayIdentityDigest = false;
	Sha256Digest mapleReplayIdentityDigest {};
	if (schemaVersion == 2)
	{
		const json& equivalence = requiredObject(root, "equivalence");
		if (equivalence.size() != 1
				|| !equivalence.contains("maple_replay_identity_sha256"))
			invalid("identity v2 equivalence metadata is incomplete or unknown");
		validateSha256(equivalence.at("maple_replay_identity_sha256"),
				"equivalence.maple_replay_identity_sha256");
		if (!sha256FromHex(equivalence.at("maple_replay_identity_sha256")
					.get<std::string>(), mapleReplayIdentityDigest))
			invalid("equivalence.maple_replay_identity_sha256 conversion failed");
		hasMapleReplayIdentityDigest = true;
	}
	ValidatedIdentity result;
	result.schemaVersion = schemaVersion;
	result.runtimeConfiguration = runtimeConfiguration;
	result.mediaKind = mediaKind;
	result.mediaTrackCount = media.contains("tracks") ? media.at("tracks").size() : 0;
	result.bootExecutableDigest = bootExecutableDigest;
	result.hasStaticAnalysis = hasStaticAnalysis;
	result.staticAnalysisProgramDigest = staticAnalysisProgramDigest;
	result.staticAnalysisExportDigest = staticAnalysisExportDigest;
	result.staticAnalysisImageBase = staticAnalysisImageBase;
	result.hasHookManifestDigest = hasHookManifestDigest;
	result.hookManifestDigest = hookManifestDigest;
	result.hasMapleReplayIdentityDigest = hasMapleReplayIdentityDigest;
	result.mapleReplayIdentityDigest = mapleReplayIdentityDigest;
	return result;
}

std::uint64_t stableFileSize(const std::filesystem::path& path)
{
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);
	if (error)
		throw std::runtime_error("cannot stat file '" + path.string() + "': " + error.message());
	if (size > std::numeric_limits<std::uint64_t>::max())
		throw std::runtime_error("file is too large: " + path.string());
	return static_cast<std::uint64_t>(size);
}

} // namespace

std::vector<std::uint8_t> readFileExact(const std::filesystem::path& path,
		std::uint64_t maximumBytes)
{
	const std::uint64_t sizeBefore = stableFileSize(path);
	if (sizeBefore > maximumBytes || sizeBefore > std::numeric_limits<std::size_t>::max())
		throw std::runtime_error("file exceeds size limit: " + path.string());

	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open file for reading: " + path.string());
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(sizeBefore));
	if (!bytes.empty())
	{
		input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
		if (input.gcount() != static_cast<std::streamsize>(bytes.size()) || !input)
			throw std::runtime_error("file shrank or read failed: " + path.string());
	}
	char extra = 0;
	input.read(&extra, 1);
	if (input.gcount() != 0)
		throw std::runtime_error("file grew while reading: " + path.string());
	const std::uint64_t sizeAfter = stableFileSize(path);
	if (sizeBefore != sizeAfter)
		throw std::runtime_error("file size changed while reading: " + path.string());
	return bytes;
}

Sha256Digest hashFileExact(const std::filesystem::path& path, std::uint64_t maximumBytes)
{
	const std::uint64_t sizeBefore = stableFileSize(path);
	if (sizeBefore > maximumBytes)
		throw std::runtime_error("file exceeds size limit: " + path.string());

	std::ifstream input(path, std::ios::binary);
	if (!input)
		throw std::runtime_error("cannot open file for reading: " + path.string());
	constexpr std::size_t HashWindowBytes = 64 * 1024;
	std::vector<std::uint8_t> window(HashWindowBytes);
	Sha256 hasher;
	std::uint64_t remaining = sizeBefore;
	while (remaining != 0)
	{
		const std::size_t count = static_cast<std::size_t>(
				std::min<std::uint64_t>(remaining, window.size()));
		input.read(reinterpret_cast<char *>(window.data()), static_cast<std::streamsize>(count));
		if (input.gcount() != static_cast<std::streamsize>(count) || !input)
			throw std::runtime_error("file shrank or read failed: " + path.string());
		hasher.update(window.data(), count);
		remaining -= count;
	}
	char extra = 0;
	input.read(&extra, 1);
	if (input.gcount() != 0)
		throw std::runtime_error("file grew while reading: " + path.string());
	const std::uint64_t sizeAfter = stableFileSize(path);
	if (sizeBefore != sizeAfter)
		throw std::runtime_error("file size changed while reading: " + path.string());
	return hasher.finalize();
}

IdentityManifest loadIdentityManifest(const std::filesystem::path& path)
{
	IdentityManifest manifest;
	manifest.path = path;
	manifest.bytes = readFileExact(path, MaxIdentityManifestBytes);
	if (manifest.bytes.empty())
		invalid("file is empty");
	try
	{
		const json root = json::parse(manifest.bytes.begin(), manifest.bytes.end());
		const ValidatedIdentity validated = validateIdentityJson(root);
		manifest.schemaVersion = validated.schemaVersion;
		manifest.runtimeConfiguration = validated.runtimeConfiguration;
		manifest.mediaKind = validated.mediaKind;
		manifest.mediaTrackCount = validated.mediaTrackCount;
		manifest.bootExecutableDigest = validated.bootExecutableDigest;
		manifest.hasStaticAnalysis = validated.hasStaticAnalysis;
		manifest.staticAnalysisProgramDigest = validated.staticAnalysisProgramDigest;
		manifest.staticAnalysisExportDigest = validated.staticAnalysisExportDigest;
		manifest.staticAnalysisImageBase = validated.staticAnalysisImageBase;
		manifest.hasHookManifestDigest = validated.hasHookManifestDigest;
		manifest.hookManifestDigest = validated.hookManifestDigest;
		manifest.hasMapleReplayIdentityDigest = validated.hasMapleReplayIdentityDigest;
		manifest.mapleReplayIdentityDigest = validated.mapleReplayIdentityDigest;
	}
	catch (const nlohmann::json::exception& exception)
	{
		invalid(std::string("JSON parse/type error: ") + exception.what());
	}
	manifest.digest = sha256(manifest.bytes.data(), manifest.bytes.size());
	return manifest;
}

void requireCaptureV1Identity(const IdentityManifest& manifest)
{
	if (manifest.schemaVersion != 1)
		invalid("capture-v1 requires identity schema_version 1");
	if (manifest.runtimeConfiguration.dynarecObservation)
		invalid("capture-v1 does not permit dynarec observation");
	if (manifest.mediaKind != "gdi")
		invalid("capture-v1 requires media.kind 'gdi'");
	if (manifest.mediaTrackCount == 0)
		invalid("capture-v1 requires at least one media track");
}

void requireSh4EquivalenceIdentityV2(const IdentityManifest& manifest)
{
	if (manifest.schemaVersion != 2)
		invalid("SH-4 equivalence requires identity schema_version 2");
	if (!manifest.hasMapleReplayIdentityDigest)
		invalid("SH-4 equivalence identity is missing Maple replay provenance");
	if (manifest.runtimeConfiguration.cpuBackend != "interpreter"
			&& manifest.runtimeConfiguration.cpuBackend != "dynarec")
		invalid("SH-4 equivalence identity CPU backend is unsupported");
	if (manifest.runtimeConfiguration.dynarecObservation
			!= (manifest.runtimeConfiguration.cpuBackend == "dynarec"))
		invalid("SH-4 equivalence identity dynarec observation mismatch");
}

bool pathsAlias(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
{
	std::error_code lhsError;
	std::error_code rhsError;
	const std::filesystem::path lhsCanonical = std::filesystem::weakly_canonical(lhs, lhsError);
	const std::filesystem::path rhsCanonical = std::filesystem::weakly_canonical(rhs, rhsError);
	if (lhsError || rhsError)
		throw std::runtime_error("cannot canonicalize research artifact paths");
#ifdef _WIN32
	std::wstring lhsText = lhsCanonical.native();
	std::wstring rhsText = rhsCanonical.native();
	std::transform(lhsText.begin(), lhsText.end(), lhsText.begin(), ::towlower);
	std::transform(rhsText.begin(), rhsText.end(), rhsText.begin(), ::towlower);
	return lhsText == rhsText;
#else
	return lhsCanonical == rhsCanonical;
#endif
}

} // namespace research

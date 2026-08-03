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
	Sha256Digest configurationDigest {};
	std::string mediaKind;
	std::size_t mediaTrackCount = 0;
	std::filesystem::path mediaSourcePath;
	std::uint64_t mediaSourceSize = 0;
	Sha256Digest mediaSourceDigest {};
	std::vector<MediaTrackIdentity> mediaTracks;
	std::vector<PersistentDeviceIdentity> persistentDevices;
	EmulatorExecutableIdentity emulatorExecutable;
	Sha256Digest bootExecutableDigest {};
	bool hasStaticAnalysis = false;
	Sha256Digest staticAnalysisProgramDigest {};
	Sha256Digest staticAnalysisExportDigest {};
	std::uint32_t staticAnalysisImageBase = 0;
	bool hasHookManifestDigest = false;
	Sha256Digest hookManifestDigest {};
	bool hasMapleReplayIdentityDigest = false;
	Sha256Digest mapleReplayIdentityDigest {};
	InitialStateIdentity initialState;
	FirmwareIdentity firmware;
};

BlobIdentity parseBlobIdentity(const json& blob, const std::string& field,
		bool requirePath)
{
	validateBlob(blob, field);
	BlobIdentity result;
	result.available = true;
	result.size = blob.at("size").get<std::uint64_t>();
	if (blob.contains("path") && !blob.at("path").get<std::string>().empty())
		result.path = std::filesystem::u8path(blob.at("path").get<std::string>());
	if (requirePath && result.path.empty())
		invalid(field + ".path is missing");
	if (!sha256FromHex(blob.at("sha256").get<std::string>(), result.digest))
		invalid(field + ".sha256 conversion failed");
	return result;
}

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
		"initial_state",
	};
	for (const auto& item : root.items())
		if (allowedTopLevel.find(item.key()) == allowedTopLevel.end())
			invalid("unknown top-level field '" + item.key() + "'");

	if (!root.contains("schema") || root.at("schema") != "flycast-research-identity")
		invalid("unsupported schema");
	if (!root.contains("schema_version") || !root.at("schema_version").is_number_unsigned())
		invalid("unsupported schema_version");
	const std::uint64_t schemaVersionValue = root.at("schema_version").get<std::uint64_t>();
	if (schemaVersionValue != 1 && schemaVersionValue != 2
			&& schemaVersionValue != 3)
		invalid("unsupported schema_version");
	const std::uint32_t schemaVersion = static_cast<std::uint32_t>(schemaVersionValue);
	if (schemaVersion == 1 && root.contains("equivalence"))
		invalid("identity v1 does not permit equivalence metadata");
	if (schemaVersion == 1 && root.contains("initial_state"))
		invalid("identity v1 does not permit initial_state");
	if (schemaVersion == 3 && root.contains("equivalence"))
		invalid("identity v3 does not permit equivalence metadata");
	if (schemaVersion == 3 && !root.contains("initial_state"))
		invalid("identity v3 requires initial_state");

	InitialStateIdentity initialState;
	if (root.contains("initial_state"))
	{
		const json& state = requiredObject(root, "initial_state");
		const std::set<std::string> required {"kind", "slot", "blob"};
		if (state.size() != required.size())
			invalid("initial_state is incomplete or unknown");
		for (const std::string& name : required)
			if (!state.contains(name))
				invalid("initial_state." + name + " is missing");
		if (!state.at("kind").is_string()
				|| state.at("kind").get<std::string>() != "savestate")
			invalid("initial_state.kind must be 'savestate'");
		if (!state.at("slot").is_number_unsigned()
				|| state.at("slot").get<std::uint64_t>() > 9)
			invalid("initial_state.slot is outside [0, 9]");
		validateBlob(state.at("blob"), "initial_state.blob");
		const json& blob = state.at("blob");
		if (!blob.contains("path") || !blob.at("path").is_string()
				|| blob.at("path").get<std::string>().empty())
			invalid("initial_state.blob.path is missing");
		if (blob.at("size").get<std::uint64_t>() == 0
				|| blob.at("size").get<std::uint64_t>() > MaxInitialStateBytes)
			invalid("initial_state.blob.size is outside the supported range");
		initialState.available = true;
		initialState.path = std::filesystem::u8path(
				blob.at("path").get<std::string>());
		initialState.size = blob.at("size").get<std::uint64_t>();
		initialState.slot = static_cast<std::uint32_t>(
				state.at("slot").get<std::uint64_t>());
		if (!sha256FromHex(blob.at("sha256").get<std::string>(),
				initialState.digest))
			invalid("initial_state.blob.sha256 conversion failed");
	}

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
	const json& mediaSource = media.at("source");
	const std::filesystem::path mediaSourcePath = mediaSource.contains("path")
			&& !mediaSource.at("path").get<std::string>().empty()
			? std::filesystem::u8path(mediaSource.at("path").get<std::string>())
			: std::filesystem::path {};
	const std::uint64_t mediaSourceSize = mediaSource.at("size").get<std::uint64_t>();
	Sha256Digest mediaSourceDigest {};
	if (!sha256FromHex(mediaSource.at("sha256").get<std::string>(),
			mediaSourceDigest))
		invalid("media.source.sha256 conversion failed");
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
	std::vector<MediaTrackIdentity> mediaTracks;
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
			MediaTrackIdentity parsed;
			if (track.contains("path") && !track.at("path").get<std::string>().empty())
				parsed.path = std::filesystem::u8path(track.at("path").get<std::string>());
			parsed.size = track.at("size").get<std::uint64_t>();
			if (!sha256FromHex(track.at("sha256").get<std::string>(), parsed.digest))
				invalid(field + ".sha256 conversion failed");
			parsed.track = static_cast<std::uint32_t>(trackNumber);
			parsed.startFad = static_cast<std::uint32_t>(
					track.at("start_fad").get<std::uint64_t>());
			parsed.sectorSize = static_cast<std::uint32_t>(
					track.at("sector_size").get<std::uint64_t>());
			parsed.offset = track.at("offset").get<std::uint64_t>();
			mediaTracks.push_back(std::move(parsed));
		}
	}

	const json& firmware = requiredObject(root, "firmware");
	if (!firmware.contains("mode") || !firmware.at("mode").is_string())
		invalid("firmware.mode is missing");
	const std::string firmwareMode = firmware.at("mode").get<std::string>();
	if (firmwareMode != "real" && firmwareMode != "hle")
		invalid("firmware.mode must be 'real' or 'hle'");
	FirmwareIdentity firmwareIdentity;
	firmwareIdentity.mode = firmwareMode == "real" ? FirmwareMode::Real
			: FirmwareMode::Hle;
	firmwareIdentity.initialFlash = parseBlobIdentity(
			firmware.contains("flash_initial") ? firmware.at("flash_initial") : json(),
			"firmware.flash_initial", firmwareMode == "real");
	if (firmwareMode == "real")
	{
		if (!firmware.contains("bios") || firmware.at("bios").is_null())
			invalid("real firmware requires firmware.bios");
		firmwareIdentity.bios = parseBlobIdentity(firmware.at("bios"),
				"firmware.bios", true);
	}
	else if (!firmware.contains("hle_identity") || !firmware.at("hle_identity").is_string()
			|| firmware.at("hle_identity").get<std::string>().empty())
	{
		invalid("HLE firmware requires firmware.hle_identity");
	}
	else
	{
		firmwareIdentity.hleIdentity = firmware.at("hle_identity").get<std::string>();
	}

	const json& devices = requiredArray(root, "persistent_devices");
	std::vector<PersistentDeviceIdentity> persistentDevices;
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
		PersistentDeviceIdentity parsed;
		parsed.kind = device.at("kind").get<std::string>();
		parsed.bus = static_cast<std::uint32_t>(
				device.at("bus").get<std::uint64_t>());
		parsed.port = static_cast<std::uint32_t>(
				device.at("port").get<std::uint64_t>());
		parsed.blob = parseBlobIdentity(device, field, false);
		persistentDevices.push_back(std::move(parsed));
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
	const json& executable = emulator.at("executable");
	EmulatorExecutableIdentity emulatorExecutable;
	if (executable.contains("path")
			&& !executable.at("path").get<std::string>().empty())
		emulatorExecutable.path = std::filesystem::u8path(
				executable.at("path").get<std::string>());
	emulatorExecutable.size = executable.at("size").get<std::uint64_t>();
	if (!sha256FromHex(executable.at("sha256").get<std::string>(),
			emulatorExecutable.digest))
		invalid("emulator.executable.sha256 conversion failed");

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
		invalid("Maple record identity configuration.values.cpu_backend must be 'interpreter'");
	if ((schemaVersion == 2 || schemaVersion == 3)
			&& runtimeConfiguration.cpuBackend != "interpreter"
			&& runtimeConfiguration.cpuBackend != "dynarec")
		invalid("identity configuration.values.cpu_backend is unsupported");
	if (schemaVersion == 2 || schemaVersion == 3)
	{
		const json& observation = requiredConfigurationValue(values,
				"dynarec_observation");
		if (!observation.is_boolean())
			invalid("configuration.values.dynarec_observation must be boolean");
		runtimeConfiguration.dynarecObservation = observation.get<bool>();
		if (runtimeConfiguration.cpuBackend == "interpreter"
				&& runtimeConfiguration.dynarecObservation)
			invalid("interpreter execution cannot enable dynarec_observation");
		const json& rtcSeed = requiredConfigurationValue(values,
				"dreamcast_rtc_seed");
		if (!rtcSeed.is_number_unsigned()
				|| rtcSeed.get<std::uint64_t>()
						> std::numeric_limits<std::uint32_t>::max())
			invalid("dreamcast_rtc_seed is outside [0, 4294967295]");
		runtimeConfiguration.dreamcastRtcSeed =
				static_cast<std::uint32_t>(rtcSeed.get<std::uint64_t>());
	}
	if (values.contains("dynarec_profile"))
	{
		if (!values.at("dynarec_profile").is_boolean())
			invalid("configuration.values.dynarec_profile must be boolean");
		runtimeConfiguration.dynarecProfile =
				values.at("dynarec_profile").get<bool>();
		if (runtimeConfiguration.dynarecProfile
				&& (runtimeConfiguration.cpuBackend != "dynarec"
						|| runtimeConfiguration.dynarecObservation))
			invalid("dynarec_profile requires normal dynarec execution without instruction markers");
	}
	if (schemaVersion == 3 && runtimeConfiguration.cpuBackend == "dynarec"
			&& !runtimeConfiguration.dynarecProfile)
		invalid("identity v3 dynarec recording requires dynarec_profile");
	for (const auto& [name, destination] : {
			std::pair<const char *, bool *>("threaded_rendering",
					&runtimeConfiguration.threadedRendering),
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
	const json& autoLoadState = requiredConfigurationValue(values,
			"autoload_state");
	if (!autoLoadState.is_boolean()
			|| autoLoadState.get<bool>() != initialState.available)
		invalid("configuration.values.autoload_state must match initial_state availability");
	runtimeConfiguration.autoLoadState = autoLoadState.get<bool>();
	if (initialState.available)
	{
		const json& slot = requiredConfigurationValue(values, "savestate_slot");
		if (!slot.is_number_unsigned()
				|| slot.get<std::uint64_t>() != initialState.slot)
			invalid("configuration.values.savestate_slot must match initial_state.slot");
		runtimeConfiguration.savestateSlot = initialState.slot;
	}
	else if (values.contains("savestate_slot"))
	{
		invalid("configuration.values.savestate_slot requires initial_state");
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
	if (schemaVersion == 3 && runtimeConfiguration.mapleDmaCheckpoint == 0)
		invalid("identity v3 requires configuration.values.maple_dma_checkpoint");
	if (schemaVersion == 3 && (values.contains("sh4_observation_start_dma")
			|| values.contains("pvr_ta_start_dma")
			|| values.contains("pvr_draw_configuration")
			|| values.contains("aica_configuration")))
		invalid("identity v3 only permits state-started Maple recording settings");
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
	if (values.contains("pvr_ta_start_dma"))
	{
		const json& startDma = values.at("pvr_ta_start_dma");
		if (!startDma.is_number_unsigned()
				|| startDma.get<std::uint64_t>() == 0
				|| startDma.get<std::uint64_t>() > MaximumMapleDmaCheckpoint)
			invalid("configuration.values.pvr_ta_start_dma is outside [1, 10000000]");
		runtimeConfiguration.pvrTaStartDma = startDma.get<std::uint64_t>();
	}
	if (values.contains("pvr_draw_configuration"))
	{
		if (schemaVersion != 2)
			invalid("only identity v2 permits configuration.values.pvr_draw_configuration");
		const json& draw = values.at("pvr_draw_configuration");
		if (!draw.is_object())
			invalid("configuration.values.pvr_draw_configuration must be an object");
		const std::set<std::string> required {
			"renderer", "per_strip_sorting", "translucent_polygon_depth_mask",
			"modifier_volumes", "render_resolution", "emulate_framebuffer",
			"fix_upscale_bleeding_edge",
		};
		if (draw.size() != required.size())
			invalid("configuration.values.pvr_draw_configuration is incomplete or unknown");
		for (const std::string& name : required)
			if (!draw.contains(name))
				invalid("configuration.values.pvr_draw_configuration." + name + " is missing");
		PvrDrawConfiguration& parsed = runtimeConfiguration.pvrDrawConfiguration;
		if (!draw.at("renderer").is_string()
				|| draw.at("renderer").get<std::string>() != "directx11")
			invalid("configuration.values.pvr_draw_configuration.renderer must be 'directx11'");
		parsed.renderer = draw.at("renderer").get<std::string>();
		for (const auto& [name, destination] : {
			std::pair<const char *, bool *>("per_strip_sorting", &parsed.perStripSorting),
			std::pair<const char *, bool *>("translucent_polygon_depth_mask",
					&parsed.translucentPolygonDepthMask),
			std::pair<const char *, bool *>("modifier_volumes", &parsed.modifierVolumes),
			std::pair<const char *, bool *>("emulate_framebuffer", &parsed.emulateFramebuffer),
			std::pair<const char *, bool *>("fix_upscale_bleeding_edge",
					&parsed.fixUpscaleBleedingEdge),
		})
		{
			if (!draw.at(name).is_boolean())
				invalid(std::string("configuration.values.pvr_draw_configuration.")
						+ name + " must be boolean");
			*destination = draw.at(name).get<bool>();
		}
		if (!draw.at("render_resolution").is_number_unsigned()
				|| draw.at("render_resolution").get<std::uint64_t>() == 0
				|| draw.at("render_resolution").get<std::uint64_t>() > 16384)
			invalid("configuration.values.pvr_draw_configuration.render_resolution is outside [1, 16384]");
		parsed.renderResolution = static_cast<std::uint32_t>(
				draw.at("render_resolution").get<std::uint64_t>());
		parsed.available = true;
	}
	if (values.contains("aica_configuration"))
	{
		if (schemaVersion != 2)
			invalid("only identity v2 permits configuration.values.aica_configuration");
		const json& aica = values.at("aica_configuration");
		const std::set<std::string> required {"dsp_enabled", "vmu_sound",
				"sample_rate", "sample_format", "output_stage"};
		if (!aica.is_object() || aica.size() != required.size())
			invalid("configuration.values.aica_configuration is incomplete or unknown");
		for (const std::string& name : required)
			if (!aica.contains(name))
				invalid("configuration.values.aica_configuration." + name + " is missing");
		AicaConfiguration& parsed = runtimeConfiguration.aicaConfiguration;
		if (!aica.at("dsp_enabled").is_boolean()
				|| !aica.at("vmu_sound").is_boolean())
			invalid("configuration.values.aica_configuration boolean field is invalid");
		parsed.dspEnabled = aica.at("dsp_enabled").get<bool>();
		parsed.vmuSound = aica.at("vmu_sound").get<bool>();
		if (parsed.vmuSound)
			invalid("configuration.values.aica_configuration.vmu_sound must be false");
		if (!aica.at("sample_rate").is_number_unsigned()
				|| aica.at("sample_rate").get<std::uint64_t>() != 44100)
			invalid("configuration.values.aica_configuration.sample_rate must be 44100");
		parsed.sampleRate = 44100;
		if (!aica.at("sample_format").is_string()
				|| aica.at("sample_format").get<std::string>() != "signed-pcm16-le-stereo")
			invalid("configuration.values.aica_configuration.sample_format is unsupported");
		parsed.sampleFormat = aica.at("sample_format").get<std::string>();
		if (!aica.at("output_stage").is_string()
				|| aica.at("output_stage").get<std::string>() != "pre-backend-pre-user-volume")
			invalid("configuration.values.aica_configuration.output_stage is unsupported");
		parsed.outputStage = aica.at("output_stage").get<std::string>();
		parsed.available = true;
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
	result.configurationDigest = declaredConfiguration;
	result.mediaKind = mediaKind;
	result.mediaTrackCount = media.contains("tracks") ? media.at("tracks").size() : 0;
	result.mediaSourcePath = mediaSourcePath;
	result.mediaSourceSize = mediaSourceSize;
	result.mediaSourceDigest = mediaSourceDigest;
	result.mediaTracks = std::move(mediaTracks);
	result.persistentDevices = std::move(persistentDevices);
	result.emulatorExecutable = std::move(emulatorExecutable);
	result.bootExecutableDigest = bootExecutableDigest;
	result.hasStaticAnalysis = hasStaticAnalysis;
	result.staticAnalysisProgramDigest = staticAnalysisProgramDigest;
	result.staticAnalysisExportDigest = staticAnalysisExportDigest;
	result.staticAnalysisImageBase = staticAnalysisImageBase;
	result.hasHookManifestDigest = hasHookManifestDigest;
	result.hookManifestDigest = hookManifestDigest;
	result.hasMapleReplayIdentityDigest = hasMapleReplayIdentityDigest;
	result.mapleReplayIdentityDigest = mapleReplayIdentityDigest;
	result.initialState = initialState;
	result.firmware = std::move(firmwareIdentity);
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
		manifest.firmware = validated.firmware;
		manifest.configurationDigest = validated.configurationDigest;
		manifest.mediaKind = validated.mediaKind;
		manifest.mediaTrackCount = validated.mediaTrackCount;
		manifest.mediaSourcePath = validated.mediaSourcePath;
		manifest.mediaSourceSize = validated.mediaSourceSize;
		manifest.mediaSourceDigest = validated.mediaSourceDigest;
		manifest.mediaTracks = validated.mediaTracks;
		manifest.persistentDevices = validated.persistentDevices;
		manifest.emulatorExecutable = validated.emulatorExecutable;
		manifest.bootExecutableDigest = validated.bootExecutableDigest;
		manifest.hasStaticAnalysis = validated.hasStaticAnalysis;
		manifest.staticAnalysisProgramDigest = validated.staticAnalysisProgramDigest;
		manifest.staticAnalysisExportDigest = validated.staticAnalysisExportDigest;
		manifest.staticAnalysisImageBase = validated.staticAnalysisImageBase;
		manifest.hasHookManifestDigest = validated.hasHookManifestDigest;
		manifest.hookManifestDigest = validated.hookManifestDigest;
		manifest.hasMapleReplayIdentityDigest = validated.hasMapleReplayIdentityDigest;
		manifest.mapleReplayIdentityDigest = validated.mapleReplayIdentityDigest;
		manifest.initialState = validated.initialState;
	}
	catch (const nlohmann::json::exception& exception)
	{
		invalid(std::string("JSON parse/type error: ") + exception.what());
	}
	manifest.digest = sha256(manifest.bytes.data(), manifest.bytes.size());
	return manifest;
}

Sha256Digest pvrDrawConfigurationDigest(const PvrDrawConfiguration& configuration)
{
	if (!configuration.available)
		invalid("PowerVR draw configuration is unavailable");
	const std::string canonical =
			"renderer=" + configuration.renderer
			+ ";per_strip_sorting=" + std::to_string(configuration.perStripSorting)
			+ ";translucent_polygon_depth_mask="
			+ std::to_string(configuration.translucentPolygonDepthMask)
			+ ";modifier_volumes=" + std::to_string(configuration.modifierVolumes)
			+ ";render_resolution=" + std::to_string(configuration.renderResolution)
			+ ";emulate_framebuffer=" + std::to_string(configuration.emulateFramebuffer)
			+ ";fix_upscale_bleeding_edge="
			+ std::to_string(configuration.fixUpscaleBleedingEdge);
	return sha256(canonical.data(), canonical.size());
}

Sha256Digest aicaConfigurationDigest(const AicaConfiguration& configuration)
{
	if (!configuration.available)
		throw std::invalid_argument("AICA configuration is unavailable");
	const json canonical = {
		{"dsp_enabled", configuration.dspEnabled},
		{"output_stage", configuration.outputStage},
		{"sample_format", configuration.sampleFormat},
		{"sample_rate", configuration.sampleRate},
		{"vmu_sound", configuration.vmuSound},
	};
	const std::string bytes = canonical.dump();
	return sha256(bytes.data(), bytes.size());
}

void authenticateInitialStateFile(const IdentityManifest& manifest,
		const std::filesystem::path& loadedPath)
{
	if (!manifest.initialState.available)
		return;
	const std::vector<std::uint8_t> bytes = readFileExact(loadedPath,
			MaxInitialStateBytes);
	if (bytes.size() != manifest.initialState.size
			|| !sha256Equal(sha256(bytes.data(), bytes.size()),
					manifest.initialState.digest))
		throw std::runtime_error(
				"initial state loaded by Flycast differs from research identity");
}

namespace
{
void authenticateBlobFile(const BlobIdentity& blob, const char *description)
{
	if (!blob.available || blob.path.empty())
		throw std::runtime_error(std::string(description) + " has no file authority");
	const auto bytes = readFileExact(blob.path, blob.size);
	if (bytes.size() != blob.size
			|| !sha256Equal(sha256(bytes.data(), bytes.size()), blob.digest))
		throw std::runtime_error(std::string(description)
				+ " differs from research identity");
}
} // namespace

void authenticateFirmwareFiles(const IdentityManifest& manifest)
{
	if (manifest.firmware.mode == FirmwareMode::Real)
	{
		authenticateBlobFile(manifest.firmware.bios, "BIOS file");
		authenticateBlobFile(manifest.firmware.initialFlash, "initial flash file");
	}
}

void authenticateLoadedDreamcastFirmware(const IdentityManifest& manifest,
		bool useReios, const std::uint8_t* loadedBios, std::size_t loadedBiosBytes)
{
	const bool expectsReal = manifest.firmware.mode == FirmwareMode::Real;
	if (expectsReal == useReios)
		throw std::runtime_error("running firmware mode differs from research identity");
	if (!expectsReal)
		return;
	if (!manifest.firmware.bios.available
			|| manifest.firmware.bios.size != DreamcastBiosBytes
			|| loadedBios == nullptr || loadedBiosBytes != DreamcastBiosBytes)
		throw std::runtime_error("loaded Dreamcast BIOS has no valid research authority");
	if (!sha256Equal(sha256(loadedBios, loadedBiosBytes),
			manifest.firmware.bios.digest))
		throw std::runtime_error("loaded Dreamcast BIOS differs from research identity");
}

void authenticateLoadedDreamcastFlash(const IdentityManifest& manifest,
		const std::uint8_t* loadedFlash, std::size_t loadedFlashBytes)
{
	if (!manifest.firmware.initialFlash.available
			|| manifest.firmware.initialFlash.size != DreamcastFlashBytes
			|| loadedFlash == nullptr || loadedFlashBytes != DreamcastFlashBytes)
		throw std::runtime_error("loaded Dreamcast flash has no valid research authority");
	if (!sha256Equal(sha256(loadedFlash, loadedFlashBytes),
			manifest.firmware.initialFlash.digest))
		throw std::runtime_error("loaded Dreamcast flash differs from research identity");
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

void requireMapleRecordIdentityV3(const IdentityManifest& manifest)
{
	if (manifest.schemaVersion != 3)
		invalid("state-started Maple recording requires identity schema_version 3");
	if (!manifest.initialState.available)
		invalid("identity v3 requires an authenticated initial state");
	const bool interpreter = manifest.runtimeConfiguration.cpuBackend == "interpreter"
			&& !manifest.runtimeConfiguration.dynarecObservation
			&& !manifest.runtimeConfiguration.dynarecProfile;
	const bool profiledDynarec = manifest.runtimeConfiguration.cpuBackend == "dynarec"
			&& !manifest.runtimeConfiguration.dynarecObservation
			&& manifest.runtimeConfiguration.dynarecProfile;
	if (!interpreter && !profiledDynarec)
		invalid("identity v3 Maple recording requires interpreter or production-profile dynarec execution");
	if (manifest.runtimeConfiguration.mapleDmaCheckpoint == 0)
		invalid("identity v3 Maple recording requires a DMA checkpoint");
	if (manifest.mediaKind != "gdi" || manifest.mediaTrackCount == 0)
		invalid("identity v3 Maple recording requires GDI media with tracks");
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

void requireSh4DynarecProfileIdentityV2(const IdentityManifest& manifest)
{
	if (manifest.schemaVersion != 2)
		invalid("SH-4 dynarec profiling requires identity schema_version 2");
	if (!manifest.hasMapleReplayIdentityDigest)
		invalid("SH-4 dynarec profile identity is missing Maple replay provenance");
	if (manifest.runtimeConfiguration.cpuBackend != "dynarec"
			|| manifest.runtimeConfiguration.dynarecObservation
			|| !manifest.runtimeConfiguration.dynarecProfile)
		invalid("SH-4 dynarec profile identity does not select normal profiled dynarec execution");
}

void requireSh4DynarecProfileRecordIdentityV3(const IdentityManifest& manifest)
{
	requireMapleRecordIdentityV3(manifest);
	if (manifest.runtimeConfiguration.cpuBackend != "dynarec"
			|| manifest.runtimeConfiguration.dynarecObservation
			|| !manifest.runtimeConfiguration.dynarecProfile)
		invalid("SH-4 dynarec profile record identity does not select normal profiled dynarec execution");
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

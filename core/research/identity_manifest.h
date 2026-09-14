#pragma once

#include "research/sha256.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace research
{

constexpr std::size_t MaxIdentityManifestBytes = 16 * 1024 * 1024;
constexpr std::uint64_t MaxInitialStateBytes = 128 * 1024 * 1024;
constexpr std::uint64_t DreamcastBiosBytes = 2 * 1024 * 1024;
constexpr std::uint64_t DreamcastFlashBytes = 128 * 1024;
constexpr std::uint64_t MaximumMapleDmaCheckpoint = 10'000'000;

struct InitialStateIdentity
{
	bool available = false;
	std::filesystem::path path;
	std::uint64_t size = 0;
	Sha256Digest digest {};
	std::uint32_t slot = 0;
};

struct BlobIdentity
{
	bool available = false;
	std::filesystem::path path;
	std::uint64_t size = 0;
	Sha256Digest digest {};
};

enum class FirmwareMode : std::uint8_t
{
	Hle = 1,
	Real = 2,
};

struct FirmwareIdentity
{
	FirmwareMode mode = FirmwareMode::Hle;
	BlobIdentity bios;
	BlobIdentity initialFlash;
	std::string hleIdentity;
};

struct PvrDrawConfiguration
{
	bool available = false;
	std::string renderer;
	bool perStripSorting = false;
	bool translucentPolygonDepthMask = false;
	bool modifierVolumes = true;
	std::uint32_t renderResolution = 480;
	bool emulateFramebuffer = false;
	bool fixUpscaleBleedingEdge = true;
};

struct AicaConfiguration
{
	bool available = false;
	bool dspEnabled = false;
	bool vmuSound = false;
	std::uint32_t sampleRate = 44100;
	std::string sampleFormat;
	std::string outputStage;
};

struct MediaTrackIdentity
{
	std::filesystem::path path;
	std::uint64_t size = 0;
	Sha256Digest digest {};
	std::uint32_t track = 0;
	std::uint32_t startFad = 0;
	std::uint32_t sectorSize = 0;
	std::uint64_t offset = 0;
};

struct PersistentDeviceIdentity
{
	std::string kind;
	std::uint32_t bus = 0;
	std::uint32_t port = 0;
	BlobIdentity blob;
};

struct EmulatorExecutableIdentity
{
	std::filesystem::path path;
	std::uint64_t size = 0;
	Sha256Digest digest {};
};

struct IdentityRuntimeConfiguration
{
	std::string cpuBackend;
	bool dynarecObservation = false;
	bool dynarecProfile = false;
	bool dynarecReplayDiagnostic = false;
	bool threadedRendering = true;
	bool autoLoadState = true;
	bool autoSaveState = true;
	bool ggpo = true;
	std::uint32_t savestateSlot = 0;
	std::uint64_t mapleDmaCheckpoint = 0;
	std::uint32_t sh4PcCheckpoint = 0;
	std::uint32_t sh4PcCheckpointU32Address = 0;
	std::uint32_t sh4PcCheckpointU32Value = 0;
	std::uint64_t sh4ObservationStartDma = 0;
	std::uint64_t pvrTaStartDma = 0;
	std::uint32_t dreamcastRtcSeed = 0;
	PvrDrawConfiguration pvrDrawConfiguration;
	AicaConfiguration aicaConfiguration;
};

struct IdentityManifest
{
	std::uint32_t schemaVersion = 0;
	std::filesystem::path path;
	std::vector<std::uint8_t> bytes;
	Sha256Digest digest {};
	Sha256Digest configurationDigest {};
	InitialStateIdentity initialState;
	IdentityRuntimeConfiguration runtimeConfiguration;
	FirmwareIdentity firmware;
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
};

IdentityManifest loadIdentityManifest(const std::filesystem::path& path);
void requireCaptureV1Identity(const IdentityManifest& manifest);
void requireMapleRecordIdentityV3(const IdentityManifest& manifest);
void requireSh4EquivalenceIdentityV2(const IdentityManifest& manifest);
void requireSh4DynarecProfileIdentityV2(const IdentityManifest& manifest);
void requireSh4DynarecReplayDiagnosticIdentityV2(
		const IdentityManifest& manifest);
void requireSh4DynarecProfileRecordIdentityV3(const IdentityManifest& manifest);
Sha256Digest pvrDrawConfigurationDigest(const PvrDrawConfiguration& configuration);
Sha256Digest aicaConfigurationDigest(const AicaConfiguration& configuration);
Sha256Digest hashFileExact(const std::filesystem::path& path, std::uint64_t maximumBytes);
std::vector<std::uint8_t> readFileExact(const std::filesystem::path& path,
		std::uint64_t maximumBytes);
bool pathsAlias(const std::filesystem::path& lhs, const std::filesystem::path& rhs);
void authenticateInitialStateFile(const IdentityManifest& manifest,
		const std::filesystem::path& loadedPath);
void authenticateFirmwareFiles(const IdentityManifest& manifest);
void authenticateLoadedDreamcastFirmware(const IdentityManifest& manifest,
		bool useReios, const std::uint8_t* loadedBios, std::size_t loadedBiosBytes);
void authenticateLoadedDreamcastFlash(const IdentityManifest& manifest,
		const std::uint8_t* loadedFlash, std::size_t loadedFlashBytes);

} // namespace research

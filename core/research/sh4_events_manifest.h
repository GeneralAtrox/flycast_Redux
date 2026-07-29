#pragma once

#include "research/identity_manifest.h"
#include "research/sha256.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace research
{

constexpr std::size_t MaxSh4EventsManifestBytes = 1024 * 1024;
constexpr std::size_t MaxSh4HookCount = 64;
constexpr std::size_t MaxSh4WatchRangeCount = 64;
constexpr std::size_t MaxSh4SnapshotCount = 128;
constexpr std::uint32_t MaxSh4SnapshotLength = 4096;
constexpr std::uint64_t MaxSh4EventCount = 10'000'000;
constexpr std::uint64_t MaxSh4SnapshotBytesPerEvent = 1024 * 1024;
constexpr std::uint64_t MaxSh4TotalSnapshotBytes = 1024ull * 1024 * 1024;
constexpr std::uint32_t MaxSh4OpenInvocations = 1024;

enum class Sh4SnapshotPhase : std::uint8_t
{
	Call = 1,
	Return = 2,
};

enum class Sh4SnapshotSourceKind : std::uint8_t
{
	Absolute = 1,
	RegisterRelative = 2,
};

enum Sh4WatchAccess : std::uint8_t
{
	Sh4WatchRead = 1u << 0,
	Sh4WatchWrite = 1u << 1,
};

struct Sh4EventsBindings
{
	Sha256Digest executableDigest {};
	std::string staticAnalysisId;
	Sha256Digest staticAnalysisDigest {};
	std::string hookManifestId;
	Sha256Digest hookManifestDigest {};
};

struct Sh4SnapshotDefinition
{
	std::string id;
	Sh4SnapshotPhase phase = Sh4SnapshotPhase::Call;
	Sh4SnapshotSourceKind sourceKind = Sh4SnapshotSourceKind::Absolute;
	std::uint32_t absoluteAddress = 0;
	std::uint8_t registerIndex = 0;
	std::int32_t offset = 0;
	std::uint32_t length = 0;
	bool required = true;
};

struct Sh4HookDefinition
{
	std::string id;
	std::uint32_t entryPc = 0;
	std::uint32_t endPcExclusive = 0;
	std::vector<Sh4SnapshotDefinition> snapshots;
};

struct Sh4WatchRangeDefinition
{
	std::string id;
	std::uint32_t address = 0;
	std::uint32_t length = 0;
	std::uint8_t access = 0;
};

struct Sh4EventsManifest
{
	std::filesystem::path path;
	std::vector<std::uint8_t> bytes;
	Sha256Digest digest {};
	std::string id;
	Sh4EventsBindings bindings;
	std::vector<Sh4HookDefinition> hooks;
	std::vector<Sh4WatchRangeDefinition> watchRanges;
	std::uint64_t maximumEvents = 0;
	std::uint64_t maximumSnapshotBytesPerEvent = 0;
	std::uint64_t maximumTotalSnapshotBytes = 0;
	std::uint32_t maximumOpenInvocations = 0;
	std::uint64_t minimumCallEvents = 0;
	std::uint64_t minimumWatchEvents = 0;
};

Sh4EventsManifest loadSh4EventsManifest(const std::filesystem::path& path);
void requireSh4EventsIdentity(const Sh4EventsManifest& manifest,
		const IdentityManifest& identity);

} // namespace research

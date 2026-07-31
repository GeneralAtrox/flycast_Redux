#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "research/sh4_observation_trace.h"
#include "research/sha256.h"

#include "json.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using json = nlohmann::json;

void writeText(const std::filesystem::path& path, const std::string& text)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output)
		throw std::runtime_error("cannot create fixture file: " + path.string());
	output.write(text.data(), static_cast<std::streamsize>(text.size()));
	if (!output)
		throw std::runtime_error("cannot write fixture file: " + path.string());
}

std::string digest(const std::string& text)
{
	return research::sha256ToHex(research::sha256(text.data(), text.size()));
}

json blob(const std::filesystem::path& path)
{
	const std::filesystem::path absolute = std::filesystem::absolute(path);
	const std::uint64_t size = std::filesystem::file_size(absolute);
	return {{"path", absolute.generic_u8string()}, {"size", size},
			{"sha256", research::sha256ToHex(
					research::hashFileExact(absolute, size))}};
}

json identity(const char *backend, const json& emulator,
		const std::string& mapleIdentity)
{
	json values {
			{"cpu_backend", backend},
			{"dynarec_observation", std::string(backend) == "dynarec"},
			{"dreamcast_rtc_seed", 0x90000000u},
			{"threaded_rendering", false}, {"autoload_state", false},
			{"autosave_state", false}, {"ggpo", false},
	};
	const json descriptiveBlob {
			{"path", "descriptive-only.bin"}, {"size", 0},
			{"sha256", std::string(64, '0')},
	};
	json boot = descriptiveBlob;
	boot["name"] = "fixture.elf";
	return {
			{"schema", "flycast-research-identity"}, {"schema_version", 2},
			{"media", {{"kind", "elf"}, {"source", descriptiveBlob},
					{"ip_bin", descriptiveBlob}, {"boot_executable", boot}}},
			{"firmware", {{"mode", "hle"}, {"hle_identity", "fixture-hle"},
					{"flash_initial", descriptiveBlob}}},
			{"persistent_devices", json::array()},
			{"emulator", {{"git_commit", std::string(40, 'a')},
					{"executable", emulator}}},
			{"configuration", {{"values", values},
					{"sha256", digest(values.dump())}}},
			{"equivalence", {{"maple_replay_identity_sha256", mapleIdentity}}},
	};
}

std::vector<std::uint8_t> words(std::initializer_list<std::uint32_t> values)
{
	std::vector<std::uint8_t> bytes;
	for (const std::uint32_t value : values)
	{
		bytes.push_back(static_cast<std::uint8_t>(value));
		bytes.push_back(static_cast<std::uint8_t>(value >> 8));
		bytes.push_back(static_cast<std::uint8_t>(value >> 16));
		bytes.push_back(static_cast<std::uint8_t>(value >> 24));
	}
	return bytes;
}

void writeReplay(const std::filesystem::path& path,
		const research::Sha256Digest& identityDigest)
{
	research::MapleTraceWriter writer(path, identityDigest);
	research::MapleDmaBeginEvent begin;
	begin.tick = 100;
	begin.descriptorAddress = 0x0c001000;
	begin.mden = 1;
	begin.mdst = 1;
	begin.mmsel = 1;
	begin.trigger = research::MapleDmaTrigger::Software;
	writer.beginDma(begin);
	research::MapleTransactionEvent transaction;
	transaction.dmaOrdinal = 0;
	transaction.tick = 100;
	transaction.descriptorAddress = 0x0c001000;
	transaction.destinationAddress = 0x0c002000;
	transaction.descriptorHeader1 = 0x80000001;
	transaction.descriptorHeader2 = 0x0c002000;
	transaction.deviceType = 0;
	transaction.bus = 0;
	transaction.port = 5;
	transaction.command = 0x09;
	transaction.flags = research::MapleTransactionDevicePresent;
	transaction.request = words({0x01002009, 0x01000000});
	transaction.response = words({0x02002008, 0x01000000, 0xffff0000});
	writer.writeTransaction(transaction);
	research::MapleDmaScheduleEvent schedule;
	schedule.dmaOrdinal = 0;
	schedule.tick = 100;
	schedule.inputWireBytes = 11;
	schedule.outputWireBytes = 15;
	schedule.scheduledCycles = 1000;
	schedule.responseCount = 1;
	writer.scheduleDma(schedule);
	research::MapleDmaCommitEvent commit;
	commit.dmaOrdinal = 0;
	commit.tick = 1100;
	commit.callbackCycles = 1000;
	commit.responseCount = 1;
	commit.flags = research::MapleCommitInterruptRaised;
	writer.commitDma(commit);
	writer.finalize();
}

research::Sh4Observation instruction(research::Sh4ObservationBackend backend,
		research::Sh4ObservationType type, std::uint64_t tick,
		std::uint32_t registerZero)
{
	research::Sh4Observation event;
	event.backend = backend;
	event.type = type;
	event.tick = tick;
	event.instructionPc = 0x8c010000;
	event.nextPc = 0x8c010002;
	event.opcode = 0x0009;
	event.availableFields = research::Sh4Observation::HasNextPc
			| research::Sh4Observation::HasRegisters;
	event.registers.r[0] = registerZero;
	return event;
}

void writeTrace(const std::filesystem::path& path,
		research::Sh4ObservationBackend backend,
		const research::Sha256Digest& identityDigest,
		const research::Sha256Digest& replayDigest,
		const research::Sha256Digest& manifestSetDigest,
		std::uint32_t registerZero)
{
	research::Sh4ObservationTraceBinding binding;
	binding.backend = backend;
	binding.identityDigest = identityDigest;
	binding.replayDigest = replayDigest;
	binding.manifestSetDigest = manifestSetDigest;
	research::Sh4ObservationTraceWriter writer(path, binding);
	writer.write(instruction(backend,
			research::Sh4ObservationType::InstructionBegin, 10, registerZero));
	writer.write(instruction(backend,
			research::Sh4ObservationType::InstructionEnd, 11, registerZero));
	writer.finalize();
}

std::string argumentValue(int argc, char *argv[], const std::string& name)
{
	for (int index = 1; index + 1 < argc; ++index)
		if (argv[index] == name)
			return argv[index + 1];
	throw std::runtime_error("missing argument " + name);
}

bool hasArgument(int argc, char *argv[], const std::string& name)
{
	for (int index = 1; index < argc; ++index)
		if (argv[index] == name)
			return true;
	return false;
}

} // namespace

int main(int argc, char *argv[])
{
	try
	{
		const std::filesystem::path root = argumentValue(argc, argv, "--root");
		const std::filesystem::path comparator = argumentValue(argc, argv,
				"--comparator");
		const std::filesystem::path publisher = argumentValue(argc, argv,
				"--publisher");
		const std::filesystem::path validator = argumentValue(argc, argv,
				"--validator");
		const std::filesystem::path accepted = argumentValue(argc, argv,
				"--accepted");
		const std::filesystem::path publicationJob = argumentValue(argc, argv,
				"--publication-job");
		const std::string packageId = argumentValue(argc, argv, "--package-id");
		const bool divergent = hasArgument(argc, argv, "--divergent");
		std::filesystem::create_directories(root);

		const std::filesystem::path emulator = root / "emulator.bin";
		const std::filesystem::path manifest = root / "hooks.json";
		const std::filesystem::path manifestSet = root / "manifest-set.json";
		const std::filesystem::path replay = root / "replay.fcmt";
		const std::filesystem::path interpreterIdentity = root
				/ "interpreter-identity.json";
		const std::filesystem::path dynarecIdentity = root / "dynarec-identity.json";
		const std::filesystem::path interpreterTrace = root / "interpreter.fcso";
		const std::filesystem::path dynarecTrace = root / "dynarec.fcso";
		const std::filesystem::path sourceJob = root / "source-job.json";
		writeText(emulator, "fixture emulator bytes");
		writeText(manifest, "{\"hooks\":[]}");
		writeText(manifestSet, json {
				{"schema", "flycast-research-sh4-equivalence-manifest-set"},
				{"schema_version", 1},
				{"manifests", json::array({{{"kind", "hook-manifest"},
						{"name", "hooks.json"},
						{"size", std::filesystem::file_size(manifest)},
						{"sha256", research::sha256ToHex(research::hashFileExact(
								manifest, std::filesystem::file_size(manifest)))}}})},
		}.dump());
		const research::Sha256Digest mapleIdentity = research::sha256(
				"maple-v1-identity", 17);
		writeReplay(replay, mapleIdentity);
		writeText(interpreterIdentity, identity("interpreter", blob(emulator),
				research::sha256ToHex(mapleIdentity)).dump());
		writeText(dynarecIdentity, identity("dynarec", blob(emulator),
				research::sha256ToHex(mapleIdentity)).dump());
		const research::Sha256Digest replayDigest = research::hashFileExact(replay,
				std::filesystem::file_size(replay));
		const research::Sha256Digest setDigest = research::hashFileExact(manifestSet,
				std::filesystem::file_size(manifestSet));
		writeTrace(interpreterTrace, research::Sh4ObservationBackend::Interpreter,
				research::hashFileExact(interpreterIdentity,
						std::filesystem::file_size(interpreterIdentity)), replayDigest,
				setDigest, 7);
		writeTrace(dynarecTrace, research::Sh4ObservationBackend::Dynarec,
				research::hashFileExact(dynarecIdentity,
						std::filesystem::file_size(dynarecIdentity)), replayDigest,
				setDigest, divergent ? 8 : 7);
		const json equivalenceJob {
				{"schema", "flycast-research-sh4-equivalence-job"},
				{"schema_version", 1},
				{"job_id", "12345678-1234-1234-1234-123456789abc"},
				{"interpreter", {{"identity", blob(interpreterIdentity)},
						{"trace", blob(interpreterTrace)}}},
				{"dynarec", {{"identity", blob(dynarecIdentity)},
						{"trace", blob(dynarecTrace)}}},
				{"emulator", blob(emulator)}, {"replay", blob(replay)},
				{"manifest_set", blob(manifestSet)},
				{"manifests", json::array({{{"kind", "hook-manifest"},
						{"name", "hooks.json"}, {"source", blob(manifest)}}})},
				{"comparator", {{"executable", blob(comparator)}}},
				{"limits", {{"maximum_replay_bytes", 1024 * 1024},
						{"maximum_trace_bytes", 1024 * 1024},
						{"maximum_events", 1000}}},
				{"metadata", json::object()},
		};
		writeText(sourceJob, equivalenceJob.dump(2) + "\n");
		const json packageJob {
				{"schema", "flycast-research-sh4-equivalence-package-job"},
				{"schema_version", 1}, {"package_id", packageId},
				{"output", {{"accepted_directory",
						std::filesystem::absolute(accepted).generic_u8string()}}},
				{"equivalence_job", blob(sourceJob)},
				{"publisher", {{"script", blob(publisher)}}},
				{"validator", {{"executable", blob(validator)}}},
				{"limits", {{"validator_timeout_seconds", 30}}},
				{"metadata", json::object()},
		};
		writeText(publicationJob, packageJob.dump(2) + "\n");
		std::cout << std::filesystem::absolute(publicationJob).string() << '\n';
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
}

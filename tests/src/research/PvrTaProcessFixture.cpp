#include "research/identity_manifest.h"
#include "research/pvr_ta_artifact.h"
#include "research/pvr_ta_manifest.h"
#include "research/sha256.h"

#include "json.hpp"

#include <algorithm>
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

void writeText(const std::filesystem::path& path, const std::string& text)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	if (!output)
		throw std::runtime_error("cannot create fixture file: " + path.string());
	output.write(text.data(), static_cast<std::streamsize>(text.size()));
	if (!output)
		throw std::runtime_error("cannot write fixture file: " + path.string());
}

json blob(const std::filesystem::path& path)
{
	const std::filesystem::path absolute = std::filesystem::absolute(path);
	const std::uint64_t size = std::filesystem::file_size(absolute);
	return {{"path", absolute.generic_u8string()}, {"size", size},
			{"sha256", research::sha256ToHex(
					research::hashFileExact(absolute, size))}};
}

research::Sh4InstructionOwnerToken owner(
		research::Sh4ObservationBackend backend, std::uint64_t generation,
		std::uint64_t tick)
{
	research::Sh4InstructionOwnerToken result;
	result.valid = true;
	result.backend = backend;
	result.generation = generation;
	result.tick = tick;
	result.pc = 0x8c010100;
	result.pr = 0x8c020000;
	result.opcode = 0x2102;
	return result;
}

void writeArtifact(const std::filesystem::path& path,
		const research::PvrTaArtifactBinding& binding, bool incomplete)
{
	using Type = research::PvrTaObservationType;
	research::PvrTaArtifactWriter writer(path, binding, 1024 * 1024, 100);
	research::PvrTaObservation init;
	init.type = Type::ListInit;
	init.emissionOrdinal = 40;
	init.tick = 101;
	init.initiator = owner(binding.backend, 7, 100);
	init.contextAddress = 0x00100000;
	init.contextGeneration = 12;
	writer.write(init);
	if (incomplete)
	{
		writer.abandon();
		return;
	}

	research::PvrTaObservation block;
	block.type = Type::AcceptedBlock;
	block.emissionOrdinal = 41;
	block.tick = 102;
	block.initiator = init.initiator;
	block.contextAddress = init.contextAddress;
	block.contextGeneration = init.contextGeneration;
	block.contextBlockOrdinal = 0;
	block.renderPass = 0;
	block.listTypeBefore = 7;
	block.listTypeAfter = 0;
	block.parserStateBefore = 0;
	block.parserStateAfter = 1;
	block.source = research::PvrTaInputSource::StoreQueue;
	block.sourceAddress = 0xe0000020;
	block.taAddress = 0x10000020;
	for (std::size_t index = 0; index < block.block.size(); ++index)
		block.block[index] = static_cast<std::uint8_t>(index);
	writer.write(block);

	research::PvrTaObservation start;
	start.type = Type::StartRender;
	start.emissionOrdinal = 42;
	start.tick = 103;
	start.initiator = owner(binding.backend, 8, 103);
	start.renderGeneration = 3;
	start.renderContextAvailable = true;
	start.selectedContexts.push_back({init.contextAddress,
			init.contextGeneration, true});
	start.regionBase = 0x00200000;
	start.fpuParamCfg = 0;
	const std::vector<research::PvrTaVramRead> reads {
		{0x00200010, 0}, {0x00200000, 0x80000000},
		{0x00200000, 0x80000000}, {0x00200000, 0x80000000},
		{0x00200004, 0x00300000}, {0x00300000, 0x00100000},
	};
	start.renderSelectionReadCount = reads.size();
	std::copy(reads.begin(), reads.end(), start.renderSelectionReads.begin());
	writer.write(start);

	research::PvrTaObservation done;
	done.type = Type::RenderDone;
	done.emissionOrdinal = 43;
	done.tick = 200;
	done.renderGeneration = start.renderGeneration;
	writer.write(done);
	writer.finalize();
}

} // namespace

int main(int argc, char *argv[])
{
	try
	{
		const std::filesystem::path root = argumentValue(argc, argv, "--root");
		const std::filesystem::path base = argumentValue(argc, argv, "--base");
		const std::string backendName = argumentValue(argc, argv, "--backend");
		const auto backend = backendName == "dynarec"
				? research::Sh4ObservationBackend::Dynarec
				: research::Sh4ObservationBackend::Interpreter;
		if (backendName != "interpreter" && backendName != "dynarec")
			throw std::runtime_error("unsupported backend");
		const std::filesystem::path identity = base
				/ (backendName + "-identity.json");
		const std::filesystem::path replay = base / "maple-replay.fcmt";
		const std::filesystem::path staticAnalysis = argumentValue(argc, argv,
				"--static-analysis");
		const std::filesystem::path program = argumentValue(argc, argv, "--program");
		const std::filesystem::path publisher = argumentValue(argc, argv, "--publisher");
		const std::filesystem::path artifactValidator = argumentValue(argc, argv,
				"--artifact-validator");
		const std::filesystem::path packageValidator = argumentValue(argc, argv,
				"--package-validator");
		const std::filesystem::path accepted = argumentValue(argc, argv, "--accepted");
		const std::filesystem::path publicationJob = argumentValue(argc, argv,
				"--publication-job");
		const std::string packageId = argumentValue(argc, argv, "--package-id");
		std::filesystem::create_directories(root);

		const research::IdentityManifest identityManifest =
				research::loadIdentityManifest(identity);
		const std::filesystem::path manifest = root / "pvr-ta-manifest.json";
		writeText(manifest, json {
			{"schema", "flycast-research-pvr-ta-capture-manifest"},
			{"schema_version", 1}, {"manifest_id", "package-fixture"},
			{"capture", {{"start_dma", 0}, {"render_done_count", 1}}},
			{"limits", {{"maximum_bytes", 1024 * 1024},
					{"maximum_events", 100}}},
			{"static_analysis", {
					{"export_sha256", research::sha256ToHex(
							identityManifest.staticAnalysisExportDigest)},
					{"executable_sha256", research::sha256ToHex(
							identityManifest.bootExecutableDigest)}}},
		}.dump(2) + "\n");
		const research::PvrTaManifest captureManifest =
				research::loadPvrTaManifest(manifest);
		research::PvrTaArtifactBinding binding;
		binding.backend = backend;
		binding.identityDigest = identityManifest.digest;
		binding.replayDigest = research::hashFileExact(replay,
				std::filesystem::file_size(replay));
		binding.manifestDigest = captureManifest.digest;
		const std::filesystem::path artifact = root / "pvr-ta.fcpvr";
		writeArtifact(artifact, binding, hasArgument(argc, argv, "--incomplete"));

		const json job {
			{"schema", "flycast-research-pvr-ta-package-job"},
			{"schema_version", 1}, {"package_id", packageId},
			{"output", {{"accepted_directory",
					std::filesystem::absolute(accepted).generic_u8string()}}},
			{"base_equivalence_package", {
					{"accepted_directory", std::filesystem::absolute(base).generic_u8string()},
					{"backend", backendName}}},
			{"identity", blob(identity)}, {"maple_replay", blob(replay)},
			{"pvr_manifest", blob(manifest)},
			{"artifact", {{"candidate", blob(artifact)},
					{"maximum_bytes", 1024 * 1024}, {"maximum_events", 100}}},
			{"static_analysis", {{"export", blob(staticAnalysis)},
					{"program", blob(program)}}},
			{"publisher", {{"script", blob(publisher)}}},
			{"artifact_validator", {{"executable", blob(artifactValidator)}}},
			{"package_validator", {{"executable", blob(packageValidator)}}},
			{"limits", {{"validator_timeout_seconds", 30}}},
			{"metadata", json::object()},
		};
		writeText(publicationJob, job.dump(2) + "\n");
		std::cout << std::filesystem::absolute(publicationJob).string() << '\n';
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << exception.what() << '\n';
		return 1;
	}
}

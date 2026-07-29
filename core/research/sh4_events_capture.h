#pragma once

#include "research/sh4_events_artifact.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace research
{

using Sh4GuestMemoryReader =
		std::function<const std::uint8_t *(std::uint32_t address, std::uint32_t length)>;

struct Sh4InstructionState
{
	std::uint32_t pc = 0;
	std::uint32_t nextPc = 0;
	std::uint16_t opcode = 0;
	std::uint64_t tick = 0;
	Sh4RegisterSnapshot registers;
};

class Sh4EventsCapture
{
public:
	Sh4EventsCapture(const std::filesystem::path& outputPath,
			const IdentityManifest& identity, const Sh4EventsManifest& manifest,
			std::uint64_t maximumBytes = DefaultMaximumSh4EventsArtifactBytes);
	~Sh4EventsCapture();

	Sh4EventsCapture(const Sh4EventsCapture&) = delete;
	Sh4EventsCapture& operator=(const Sh4EventsCapture&) = delete;

	void beginInstruction(const Sh4InstructionState& state,
			const Sh4GuestMemoryReader& reader);
	void endInstruction(const Sh4InstructionState& state,
			const Sh4GuestMemoryReader& reader);
	void abortInstruction() noexcept;
	void observeMemoryAccess(std::uint32_t address, std::uint8_t width,
			Sh4MemoryAccessKind kind, std::uint64_t value);
	void observeException(std::uint32_t exceptionPc, std::uint32_t vectorPc,
			std::uint32_t exceptionCode, std::uint64_t tick,
			const Sh4RegisterSnapshot& registers);
	Sh4EventsArtifactSummary finish();
	void abandon() noexcept;

	bool instructionActive() const { return !instructionFrames.empty(); }

private:
	struct InstructionFrame
	{
		std::uint32_t pc = 0;
		std::uint16_t opcode = 0;
		std::uint64_t tick = 0;
	};

	struct Invocation
	{
		std::uint64_t id = 0;
		std::uint32_t hookIndex = 0;
		std::uint32_t returnPc = 0;
	};

	std::vector<Sh4SnapshotView> collectSnapshots(const Sh4HookDefinition& hook,
			Sh4SnapshotPhase phase, const Sh4RegisterSnapshot& registers,
			const Sh4GuestMemoryReader& reader) const;
	bool pcInHook(std::uint32_t pc) const;

	Sh4EventsManifest manifest;
	std::unique_ptr<Sh4EventsArtifactWriter> writer;
	std::vector<InstructionFrame> instructionFrames;
	std::vector<Invocation> invocations;
	std::uint64_t nextInvocationId = 0;
	bool exceptionUnwinding = false;
	bool finished = false;
};

} // namespace research

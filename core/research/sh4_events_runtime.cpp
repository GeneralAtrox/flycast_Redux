#include "research/sh4_events_runtime.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"
#include "research/identity_manifest.h"
#include "research/sh4_events_capture.h"
#include "research/sh4_events_manifest.h"
#include "types.h"

#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace research
{
namespace
{

bool configured = false;
std::unique_ptr<Sh4EventsCapture> session;

std::filesystem::path researchPath(const std::string& value)
{
#ifdef __cpp_char8_t
	std::u8string utf8(value.size(), u8'\0');
	std::memcpy(utf8.data(), value.data(), value.size());
	return std::filesystem::path(utf8);
#else
	return std::filesystem::u8path(value);
#endif
}

void applyDeterministicOverrides()
{
	config::DynarecEnabled.override(false);
	config::ThreadedRendering.override(false);
	config::AutoLoadState.override(false);
	config::AutoSaveState.override(false);
	config::GGPOEnable.override(false);
}

void verifyRuntimeConfiguration(const IdentityManifest& identity)
{
	const IdentityRuntimeConfiguration& expected = identity.runtimeConfiguration;
	if (expected.cpuBackend != "interpreter" || config::DynarecEnabled.get())
		throw FlycastException("SH-4 events identity/runtime CPU backend mismatch");
	if (expected.threadedRendering != config::ThreadedRendering.get())
		throw FlycastException("SH-4 events identity/runtime threaded-rendering mismatch");
	if (expected.autoLoadState != config::AutoLoadState.get())
		throw FlycastException("SH-4 events identity/runtime auto-load-state mismatch");
	if (expected.autoSaveState != config::AutoSaveState.get())
		throw FlycastException("SH-4 events identity/runtime auto-save-state mismatch");
	if (expected.ggpo != config::GGPOEnable.get())
		throw FlycastException("SH-4 events identity/runtime GGPO mismatch");
}

void requireDistinctPaths(const std::vector<std::pair<const char *, std::filesystem::path>>& paths)
{
	for (std::size_t lhs = 0; lhs < paths.size(); ++lhs)
		for (std::size_t rhs = lhs + 1; rhs < paths.size(); ++rhs)
			if (pathsAlias(paths[lhs].second, paths[rhs].second))
				throw FlycastException(std::string("research paths alias: ") + paths[lhs].first
						+ " and " + paths[rhs].first);
}

Sh4RegisterSnapshot snapshotRegisters(const Sh4Context& context)
{
	Sh4RegisterSnapshot snapshot;
	for (std::size_t index = 0; index < snapshot.r.size(); ++index)
		snapshot.r[index] = context.r[index];
	snapshot.pr = context.pr;
	snapshot.gbr = context.gbr;
	snapshot.vbr = context.vbr;
	snapshot.mach = context.mac.h;
	snapshot.macl = context.mac.l;
	snapshot.sr = context.sr.getFull();
	snapshot.fpul = context.fpul;
	snapshot.fpscr = context.fpscr.full;
	return snapshot;
}

Sh4GuestMemoryReader guestMemoryReader()
{
	return [](std::uint32_t address, std::uint32_t length) -> const std::uint8_t * {
		return GetMemPtr(address, length);
	};
}

} // namespace

void configureSh4EventsRuntime()
{
	abortSh4EventsRuntime();
	const bool hasManifest = !config::ResearchSh4EventsManifestPath.get().empty();
	const bool hasOutput = !config::ResearchSh4EventsRecordPath.get().empty();
	if (!hasManifest && !hasOutput)
		return;
	if (!hasManifest || !hasOutput)
		throw FlycastException(
				"SH-4 events capture requires both research.Sh4EventsManifest and Sh4EventsRecord");
	if (config::ResearchIdentityManifestPath.get().empty())
		throw FlycastException("SH-4 events capture requires research.IdentityManifest");
	if (config::ResearchSh4EventsMaxBytes.get() < Sh4EventsArtifactHeaderSize)
		throw FlycastException("research.Sh4EventsMaxBytes is smaller than the artifact header");
	for (const char *key : {"IdentityManifest", "Sh4EventsManifest", "Sh4EventsRecord"})
		if (!config::isTransient("research", key))
			throw FlycastException("SH-4 events research paths must be supplied as transient options");
	applyDeterministicOverrides();
	configured = true;
}

void startSh4EventsRuntime()
{
	if (!configured)
		return;
	if (session != nullptr)
		throw FlycastException("SH-4 events runtime is already active");
	applyDeterministicOverrides();

	const std::filesystem::path identityPath = researchPath(
			config::ResearchIdentityManifestPath.get());
	const std::filesystem::path manifestPath = researchPath(
			config::ResearchSh4EventsManifestPath.get());
	const std::filesystem::path outputPath = researchPath(
			config::ResearchSh4EventsRecordPath.get());
	std::vector<std::pair<const char *, std::filesystem::path>> paths {
		{"identity", identityPath},
		{"SH-4 events manifest", manifestPath},
		{"SH-4 events output", outputPath},
	};
	if (!config::ResearchMapleRecordPath.get().empty())
		paths.emplace_back("Maple record", researchPath(config::ResearchMapleRecordPath.get()));
	if (!config::ResearchMapleReplayPath.get().empty())
		paths.emplace_back("Maple replay", researchPath(config::ResearchMapleReplayPath.get()));
	if (!config::ResearchMemoryRangesManifestPath.get().empty())
		paths.emplace_back("memory-ranges manifest",
				researchPath(config::ResearchMemoryRangesManifestPath.get()));
	if (!config::ResearchMemoryRangesRecordPath.get().empty())
		paths.emplace_back("memory-ranges output",
				researchPath(config::ResearchMemoryRangesRecordPath.get()));
	requireDistinctPaths(paths);

	const IdentityManifest identity = loadIdentityManifest(identityPath);
	const Sh4EventsManifest manifest = loadSh4EventsManifest(manifestPath);
	verifyRuntimeConfiguration(identity);
	session = std::make_unique<Sh4EventsCapture>(outputPath, identity, manifest,
			static_cast<std::uint64_t>(config::ResearchSh4EventsMaxBytes.get()));
	NOTICE_LOG(SH4, "Armed SH-4 events manifest %s (%zu hooks, %zu watch ranges) to %s",
			manifest.id.c_str(), manifest.hooks.size(), manifest.watchRanges.size(),
			outputPath.string().c_str());
}

void stopSh4EventsRuntime(bool clean)
{
	if (session == nullptr)
	{
		configured = false;
		return;
	}
	std::unique_ptr<Sh4EventsCapture> finishing = std::move(session);
	configured = false;
	if (!clean)
	{
		finishing->abandon();
		return;
	}
	try
	{
		const Sh4EventsArtifactSummary summary = finishing->finish();
		NOTICE_LOG(SH4,
				"SH-4 events artifact complete: %llu calls, %llu returns, %llu watches, %llu exceptions",
				static_cast<unsigned long long>(summary.callCount),
				static_cast<unsigned long long>(summary.returnCount),
				static_cast<unsigned long long>(summary.watchReadCount
						+ summary.watchWriteCount),
				static_cast<unsigned long long>(summary.exceptionCount));
	}
	catch (...)
	{
		finishing->abandon();
		throw;
	}
}

void abortSh4EventsRuntime() noexcept
{
	configured = false;
	if (session != nullptr)
	{
		session->abandon();
		session.reset();
	}
}

void sh4EventsInstructionBegin(std::uint32_t pc, std::uint16_t opcode,
		std::uint64_t tick, const Sh4Context& context)
{
	if (session == nullptr)
		return;
	Sh4InstructionState state;
	state.pc = pc;
	state.nextPc = context.pc;
	state.opcode = opcode;
	state.tick = tick;
	state.registers = snapshotRegisters(context);
	session->beginInstruction(state, guestMemoryReader());
}

void sh4EventsInstructionEnd(std::uint32_t pc, std::uint16_t opcode,
		std::uint64_t tick, const Sh4Context& context)
{
	if (session == nullptr)
		return;
	Sh4InstructionState state;
	state.pc = pc;
	state.nextPc = context.pc;
	state.opcode = opcode;
	state.tick = tick;
	state.registers = snapshotRegisters(context);
	session->endInstruction(state, guestMemoryReader());
}

void sh4EventsInstructionAbort() noexcept
{
	if (session != nullptr)
		session->abortInstruction();
}

void sh4EventsMemoryAccess(std::uint32_t address, std::uint8_t width,
		Sh4MemoryAccessKind kind, std::uint64_t value)
{
	if (session != nullptr)
		session->observeMemoryAccess(address, width, kind, value);
}

void sh4EventsException(std::uint32_t exceptionPc, std::uint32_t vectorPc,
		std::uint32_t exceptionCode, std::uint64_t tick, const Sh4Context& context)
{
	if (session != nullptr)
		session->observeException(exceptionPc, vectorPc, exceptionCode, tick,
				snapshotRegisters(context));
}

bool sh4EventsRuntimeActive()
{
	return session != nullptr;
}

} // namespace research

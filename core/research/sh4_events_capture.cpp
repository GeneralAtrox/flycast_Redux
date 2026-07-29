#include "research/sh4_events_capture.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace research
{
namespace
{

bool decodeCall(const Sh4InstructionState& state, Sh4CallKind& kind,
		std::uint32_t& targetPc)
{
	const std::uint16_t opcode = state.opcode;
	if ((opcode & 0xf000u) == 0xb000u)
	{
		kind = Sh4CallKind::Bsr;
		const std::int32_t displacement = static_cast<std::int16_t>(
				static_cast<std::uint16_t>((opcode & 0x0fffu) << 4)) >> 4;
		targetPc = state.pc + 4u + static_cast<std::uint32_t>(displacement * 2);
		return true;
	}
	if ((opcode & 0xf0ffu) == 0x0003u)
	{
		kind = Sh4CallKind::Bsrf;
		const std::uint32_t registerIndex = (opcode >> 8) & 0x0fu;
		targetPc = state.pc + 4u + state.registers.r[registerIndex];
		return true;
	}
	if ((opcode & 0xf0ffu) == 0x400bu)
	{
		kind = Sh4CallKind::Jsr;
		const std::uint32_t registerIndex = (opcode >> 8) & 0x0fu;
		targetPc = state.registers.r[registerIndex];
		return true;
	}
	return false;
}

bool deriveSnapshotAddress(const Sh4SnapshotDefinition& definition,
		const Sh4RegisterSnapshot& registers, std::uint32_t& address)
{
	std::int64_t candidate = definition.absoluteAddress;
	if (definition.sourceKind == Sh4SnapshotSourceKind::RegisterRelative)
		candidate = static_cast<std::int64_t>(registers.r[definition.registerIndex])
				+ definition.offset;
	if (candidate < 0 || candidate > std::numeric_limits<std::uint32_t>::max())
		return false;
	const std::uint64_t end = static_cast<std::uint64_t>(candidate) + definition.length;
	if (end > (std::uint64_t {1} << 32))
		return false;
	address = static_cast<std::uint32_t>(candidate);
	return true;
}

bool overlaps(std::uint32_t firstAddress, std::uint32_t firstLength,
		std::uint32_t secondAddress, std::uint32_t secondLength)
{
	const std::uint64_t firstEnd = static_cast<std::uint64_t>(firstAddress) + firstLength;
	const std::uint64_t secondEnd = static_cast<std::uint64_t>(secondAddress) + secondLength;
	return firstAddress < secondEnd && secondAddress < firstEnd;
}

} // namespace

Sh4EventsCapture::Sh4EventsCapture(const std::filesystem::path& outputPath,
		const IdentityManifest& identity, const Sh4EventsManifest& manifest,
		std::uint64_t maximumBytes)
	: manifest(manifest)
{
	requireSh4EventsIdentity(this->manifest, identity);
	writer = std::make_unique<Sh4EventsArtifactWriter>(outputPath, identity.digest,
			this->manifest, maximumBytes);
}

Sh4EventsCapture::~Sh4EventsCapture()
{
	if (!finished)
		abandon();
}

std::vector<Sh4SnapshotView> Sh4EventsCapture::collectSnapshots(
		const Sh4HookDefinition& hook, Sh4SnapshotPhase phase,
		const Sh4RegisterSnapshot& registers, const Sh4GuestMemoryReader& reader) const
{
	if (!reader)
		throw std::invalid_argument("SH-4 events guest-memory reader is empty");
	std::vector<Sh4SnapshotView> views;
	for (std::size_t index = 0; index < hook.snapshots.size(); ++index)
	{
		const Sh4SnapshotDefinition& definition = hook.snapshots[index];
		if (definition.phase != phase)
			continue;
		Sh4SnapshotView view;
		view.definitionIndex = static_cast<std::uint32_t>(index);
		view.declaredLength = definition.length;
		view.required = definition.required;
		const bool addressValid = deriveSnapshotAddress(definition, registers, view.address);
		if (addressValid)
		{
			view.data = reader(view.address, definition.length);
			if (view.data != nullptr)
				view.size = definition.length;
		}
		if (view.data == nullptr && definition.required)
			throw std::runtime_error("required SH-4 snapshot is not backed by contiguous guest RAM: "
					+ definition.id);
		views.push_back(view);
	}
	return views;
}

void Sh4EventsCapture::beginInstruction(const Sh4InstructionState& state,
		const Sh4GuestMemoryReader& reader)
{
	if (finished)
		throw std::logic_error("SH-4 events capture is already finished");
	InstructionFrame frame;
	frame.pc = state.pc;
	frame.opcode = state.opcode;
	frame.tick = state.tick;
	instructionFrames.push_back(frame);

	Sh4CallKind kind = Sh4CallKind::Bsr;
	std::uint32_t targetPc = 0;
	if (!decodeCall(state, kind, targetPc))
		return;
	for (std::size_t hookIndex = 0; hookIndex < manifest.hooks.size(); ++hookIndex)
	{
		const Sh4HookDefinition& hook = manifest.hooks[hookIndex];
		if (targetPc != hook.entryPc)
			continue;
		if (invocations.size() >= manifest.maximumOpenInvocations)
			throw std::runtime_error("SH-4 open-invocation limit exceeded");
		Sh4CallEvent event;
		event.tick = state.tick;
		event.invocationId = nextInvocationId++;
		event.hookIndex = static_cast<std::uint32_t>(hookIndex);
		event.kind = kind;
		event.opcode = state.opcode;
		event.callPc = state.pc;
		event.targetPc = targetPc;
		event.returnPc = state.pc + 4u;
		event.delaySlotPc = state.pc + 2u;
		event.delaySlotDepth = static_cast<std::uint32_t>(instructionFrames.size() - 1);
		event.registers = state.registers;
		event.snapshots = collectSnapshots(hook, Sh4SnapshotPhase::Call,
				state.registers, reader);
		writer->writeCall(event);
		invocations.push_back(Invocation {event.invocationId, event.hookIndex,
				event.returnPc});
		writer->observeOpenInvocations(invocations.size());
		return;
	}
}

void Sh4EventsCapture::endInstruction(const Sh4InstructionState& state,
		const Sh4GuestMemoryReader& reader)
{
	if (finished || instructionFrames.empty())
		throw std::logic_error("SH-4 instruction end has no matching begin");
	const InstructionFrame frame = instructionFrames.back();
	if (frame.pc != state.pc || frame.opcode != state.opcode)
		throw std::logic_error("SH-4 instruction end does not match the active instruction");

	if (state.opcode == 0x000bu && !invocations.empty())
	{
		const Invocation& invocation = invocations.back();
		const Sh4HookDefinition& hook = manifest.hooks.at(invocation.hookIndex);
		if (state.pc >= hook.entryPc && state.pc < hook.endPcExclusive
				&& state.nextPc == invocation.returnPc)
		{
			Sh4ReturnEvent event;
			event.instructionTick = frame.tick;
			event.completionTick = state.tick;
			event.invocationId = invocation.id;
			event.hookIndex = invocation.hookIndex;
			event.opcode = state.opcode;
			event.instructionPc = state.pc;
			event.resumedPc = state.nextPc;
			event.expectedReturnPc = invocation.returnPc;
			event.delaySlotDepth = static_cast<std::uint32_t>(instructionFrames.size() - 1);
			event.registers = state.registers;
			event.snapshots = collectSnapshots(hook, Sh4SnapshotPhase::Return,
					state.registers, reader);
			writer->writeReturn(event);
			invocations.pop_back();
		}
	}
	instructionFrames.pop_back();
}

void Sh4EventsCapture::abortInstruction() noexcept
{
	if (!instructionFrames.empty())
		instructionFrames.pop_back();
	if (instructionFrames.empty())
		exceptionUnwinding = false;
}

void Sh4EventsCapture::observeMemoryAccess(std::uint32_t address, std::uint8_t width,
		Sh4MemoryAccessKind kind, std::uint64_t value)
{
	if (finished || instructionFrames.empty())
		return;
	if (width != 1 && width != 2 && width != 4 && width != 8)
		throw std::invalid_argument("SH-4 watch access width is unsupported");
	const std::uint8_t requiredAccess = kind == Sh4MemoryAccessKind::Read
			? Sh4WatchRead : Sh4WatchWrite;
	for (std::size_t index = 0; index < manifest.watchRanges.size(); ++index)
	{
		const Sh4WatchRangeDefinition& range = manifest.watchRanges[index];
		if ((range.access & requiredAccess) == 0
				|| !overlaps(address, width, range.address, range.length))
			continue;
		const InstructionFrame& frame = instructionFrames.back();
		Sh4WatchEvent event;
		event.tick = frame.tick;
		event.watchIndex = static_cast<std::uint32_t>(index);
		event.instructionPc = frame.pc;
		event.address = address;
		event.width = width;
		event.kind = kind;
		event.delaySlotDepth = static_cast<std::uint16_t>(instructionFrames.size() - 1);
		event.value = value;
		writer->writeWatch(event);
		return;
	}
}

void Sh4EventsCapture::observeException(std::uint32_t exceptionPc,
		std::uint32_t vectorPc, std::uint32_t exceptionCode, std::uint64_t tick,
		const Sh4RegisterSnapshot& registers)
{
	if (finished || instructionFrames.empty() || exceptionUnwinding)
		return;
	const InstructionFrame& frame = instructionFrames.back();
	if (!pcInHook(frame.pc) && invocations.empty())
		return;
	exceptionUnwinding = true;
	Sh4ExceptionEvent event;
	event.tick = tick;
	event.instructionPc = frame.pc;
	event.exceptionPc = exceptionPc;
	event.vectorPc = vectorPc;
	event.exceptionCode = exceptionCode;
	event.delaySlotDepth = static_cast<std::uint16_t>(instructionFrames.size() - 1);
	if (event.delaySlotDepth == 1)
	{
		event.exceptionPc -= 2;
		if (event.exceptionCode == 0x800u)
			event.exceptionCode = 0x820u;
		else if (event.exceptionCode == 0x180u)
			event.exceptionCode = 0x1a0u;
	}
	event.registers = registers;
	writer->writeException(event);
}

bool Sh4EventsCapture::pcInHook(std::uint32_t pc) const
{
	for (const Sh4HookDefinition& hook : manifest.hooks)
		if (pc >= hook.entryPc && pc < hook.endPcExclusive)
			return true;
	return false;
}

Sh4EventsArtifactSummary Sh4EventsCapture::finish()
{
	if (finished)
		throw std::logic_error("SH-4 events capture is already finished");
	if (!instructionFrames.empty())
		throw std::logic_error("cannot finalize SH-4 events with an active instruction");
	if (!invocations.empty())
		throw std::logic_error("cannot finalize SH-4 events with open invocations");
	const Sh4EventsArtifactSummary& observed = writer->getSummary();
	if (observed.callCount < manifest.minimumCallEvents)
		throw std::logic_error("SH-4 call-event minimum was not reached");
	if (observed.watchReadCount + observed.watchWriteCount
			< manifest.minimumWatchEvents)
		throw std::logic_error("SH-4 watch-event minimum was not reached");
	Sh4EventsArtifactSummary summary = writer->finalize();
	finished = true;
	return summary;
}

void Sh4EventsCapture::abandon() noexcept
{
	if (finished)
		return;
	finished = true;
	instructionFrames.clear();
	invocations.clear();
	if (writer != nullptr)
		writer->abandon();
}

} // namespace research

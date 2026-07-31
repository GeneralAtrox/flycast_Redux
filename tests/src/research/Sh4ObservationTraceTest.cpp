#include "research/sh4_observation_compare.h"
#include "research/sh4_observation_trace.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

class TemporaryDirectory
{
public:
	TemporaryDirectory()
	{
		static std::atomic<unsigned> sequence {0};
		path = std::filesystem::temp_directory_path()
				/ ("flycast-sh4-observation-trace-test-"
						+ std::to_string(sequence.fetch_add(1)));
		std::error_code error;
		std::filesystem::remove_all(path, error);
		std::filesystem::create_directories(path);
	}

	~TemporaryDirectory()
	{
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	std::filesystem::path file(const char *name) const { return path / name; }

private:
	std::filesystem::path path;
};

research::Sha256Digest digest(const std::string& text)
{
	return research::sha256(text.data(), text.size());
}

research::Sh4ObservationTraceBinding binding(
		research::Sh4ObservationBackend backend)
{
	research::Sh4ObservationTraceBinding value;
	value.backend = backend;
	value.identityDigest = digest(backend == research::Sh4ObservationBackend::Interpreter
			? "interpreter-identity" : "dynarec-identity");
	value.replayDigest = digest("common-maple-replay");
	value.manifestSetDigest = digest("common-manifest-set");
	return value;
}

research::Sh4ObservationEquivalenceContract contract()
{
	research::Sh4ObservationEquivalenceContract value;
	value.interpreterIdentityDigest = binding(
			research::Sh4ObservationBackend::Interpreter).identityDigest;
	value.dynarecIdentityDigest = binding(
			research::Sh4ObservationBackend::Dynarec).identityDigest;
	value.replayDigest = binding(
			research::Sh4ObservationBackend::Interpreter).replayDigest;
	value.manifestSetDigest = binding(
			research::Sh4ObservationBackend::Interpreter).manifestSetDigest;
	return value;
}

research::Sh4Observation instruction(research::Sh4ObservationBackend backend,
		research::Sh4ObservationType type, std::uint64_t tick,
		std::uint32_t pc, std::uint16_t opcode, std::uint16_t depth = 0)
{
	research::Sh4Observation event;
	event.backend = backend;
	event.type = type;
	event.tick = tick;
	event.instructionPc = pc;
	event.nextPc = pc + 2;
	event.opcode = opcode;
	event.delaySlotDepth = depth;
	event.availableFields = research::Sh4Observation::HasNextPc
			| research::Sh4Observation::HasRegisters;
	event.registers.r[0] = 0x10203040;
	event.registers.r[1] = 0x8c020000;
	event.registers.pr = 0x8c030000;
	event.registers.sr = 0x40000001;
	return event;
}

std::vector<research::Sh4Observation> semanticStream(
		research::Sh4ObservationBackend backend)
{
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> events;

	auto callBegin = instruction(backend, Type::InstructionBegin, 100,
			0x8c010000, 0x410b);
	events.push_back(callBegin);
	auto call = callBegin;
	call.type = Type::Call;
	call.callKind = research::Sh4CallKind::Jsr;
	call.targetPc = call.registers.r[1];
	call.returnPc = call.instructionPc + 4;
	call.delaySlotPc = call.instructionPc + 2;
	events.push_back(call);

	auto delayBegin = instruction(backend, Type::InstructionBegin, 101,
			0x8c010002, 0x0009, 1);
	events.push_back(delayBegin);
	research::Sh4Observation read;
	read.backend = backend;
	read.type = Type::MemoryRead;
	read.tick = 101;
	read.instructionPc = delayBegin.instructionPc;
	read.opcode = delayBegin.opcode;
	read.delaySlotDepth = 1;
	read.memoryAddress = 0x8c100000;
	read.memoryWidth = 8;
	read.memoryValue = 0x8877665544332211ull;
	events.push_back(read);
	auto delayEnd = instruction(backend, Type::InstructionEnd, 102,
			delayBegin.instructionPc, delayBegin.opcode, 1);
	delayEnd.nextPc = 0x8c020000;
	events.push_back(delayEnd);
	auto callEnd = instruction(backend, Type::InstructionEnd, 103,
			callBegin.instructionPc, callBegin.opcode);
	callEnd.nextPc = 0x8c020000;
	events.push_back(callEnd);

	auto returnBegin = instruction(backend, Type::InstructionBegin, 110,
			0x8c020100, 0x000b);
	returnBegin.registers.pr = 0x8c010004;
	events.push_back(returnBegin);
	auto returnDelayBegin = instruction(backend, Type::InstructionBegin, 111,
			returnBegin.instructionPc + 2u, 0x0009, 1);
	events.push_back(returnDelayBegin);
	auto returnDelayEnd = instruction(backend, Type::InstructionEnd, 112,
			returnDelayBegin.instructionPc, returnDelayBegin.opcode, 1);
	returnDelayEnd.nextPc = returnBegin.registers.pr;
	events.push_back(returnDelayEnd);
	auto returnEnd = instruction(backend, Type::InstructionEnd, 113,
			returnBegin.instructionPc, returnBegin.opcode);
	returnEnd.nextPc = returnBegin.registers.pr;
	returnEnd.registers.pr = returnBegin.registers.pr;
	auto returned = returnEnd;
	returned.type = Type::Return;
	returned.targetPc = returnEnd.nextPc;
	returned.returnPc = returnBegin.registers.pr;
	returned.delaySlotPc = returnBegin.instructionPc + 2u;
	events.push_back(returned);
	events.push_back(returnEnd);

	auto exceptionBegin = instruction(backend, Type::InstructionBegin, 120,
			0x8c020200, 0x0009);
	events.push_back(exceptionBegin);
	auto exception = instruction(backend, Type::Exception, 121,
			exceptionBegin.instructionPc, exceptionBegin.opcode);
	exception.exceptionPc = exceptionBegin.instructionPc;
	exception.vectorPc = 0x8c000100;
	exception.exceptionCode = 0x160;
	events.push_back(exception);
	research::Sh4Observation abort;
	abort.backend = backend;
	abort.type = Type::InstructionAbort;
	abort.tick = 121;
	abort.instructionPc = exceptionBegin.instructionPc;
	abort.opcode = exceptionBegin.opcode;
	events.push_back(abort);
	return events;
}

std::vector<research::Sh4Observation> nestedDelaySlotExceptionStream(
		research::Sh4ObservationBackend backend)
{
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> events;
	const std::uint32_t branchPc = 0x8c040000;

	auto branchBegin = instruction(backend, Type::InstructionBegin, 200,
			branchPc, 0xb000);
	events.push_back(branchBegin);
	auto call = branchBegin;
	call.type = Type::Call;
	call.callKind = research::Sh4CallKind::Bsr;
	call.targetPc = branchPc + 4u;
	call.returnPc = branchPc + 4u;
	call.delaySlotPc = branchPc + 2u;
	events.push_back(call);

	auto delayBegin = instruction(backend, Type::InstructionBegin, 201,
			branchPc + 2u, 0xf00c, 1);
	events.push_back(delayBegin);
	auto exception = instruction(backend, Type::Exception, 201,
			delayBegin.instructionPc, delayBegin.opcode, 1);
	exception.exceptionPc = branchPc;
	exception.vectorPc = 0x8c000100;
	exception.exceptionCode = 0x820;
	events.push_back(exception);

	research::Sh4Observation delayAbort;
	delayAbort.backend = backend;
	delayAbort.type = Type::InstructionAbort;
	delayAbort.tick = 201;
	delayAbort.instructionPc = delayBegin.instructionPc;
	delayAbort.opcode = delayBegin.opcode;
	delayAbort.delaySlotDepth = 1;
	events.push_back(delayAbort);
	auto branchAbort = delayAbort;
	branchAbort.instructionPc = branchBegin.instructionPc;
	branchAbort.opcode = branchBegin.opcode;
	branchAbort.delaySlotDepth = 0;
	events.push_back(branchAbort);
	return events;
}

std::vector<research::Sh4Observation> unownedInterruptStream(
		research::Sh4ObservationBackend backend)
{
	auto interrupt = instruction(backend,
			research::Sh4ObservationType::Exception, 300,
			0x8c050000, 0);
	interrupt.exceptionPc = interrupt.instructionPc;
	interrupt.vectorPc = 0x8c000600;
	interrupt.exceptionCode = 0x320;
	return {interrupt};
}

void writeTrace(const std::filesystem::path& path,
		const research::Sh4ObservationTraceBinding& traceBinding,
		const std::vector<research::Sh4Observation>& events)
{
	research::Sh4ObservationTraceWriter writer(path, traceBinding);
	for (const auto& event : events)
		writer.write(event);
	writer.finalize();
}

std::uint32_t testCrc32(const std::vector<std::uint8_t>& bytes,
		std::size_t size)
{
	std::uint32_t crc = 0xffffffffu;
	for (std::size_t index = 0; index < size; ++index)
	{
		crc ^= bytes[index];
		for (unsigned bit = 0; bit < 8; ++bit)
			crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

void setU32(std::vector<std::uint8_t>& bytes, std::size_t offset,
		std::uint32_t value)
{
	for (unsigned index = 0; index < 4; ++index)
		bytes[offset + index] = static_cast<std::uint8_t>(value >> (index * 8));
}

void rewriteHeaderWithDrop(const std::filesystem::path& path)
{
	std::vector<std::uint8_t> header(research::Sh4ObservationTraceHeaderSize);
	std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
	file.read(reinterpret_cast<char *>(header.data()),
			static_cast<std::streamsize>(header.size()));
	ASSERT_EQ(static_cast<std::streamsize>(header.size()), file.gcount());
	header[48] = 1;
	for (std::size_t index = 49; index < 56; ++index)
		header[index] = 0;
	setU32(header, research::Sh4ObservationTraceHeaderSize - 4,
			testCrc32(header, research::Sh4ObservationTraceHeaderSize - 4));
	file.clear();
	file.seekp(0);
	file.write(reinterpret_cast<const char *>(header.data()),
			static_cast<std::streamsize>(header.size()));
	file.close();
}

void flipByte(const std::filesystem::path& path, std::uint64_t offset)
{
	std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
	file.seekg(static_cast<std::streamoff>(offset));
	char byte = 0;
	file.read(&byte, 1);
	file.clear();
	file.seekp(static_cast<std::streamoff>(offset));
	byte ^= 1;
	file.write(&byte, 1);
	file.close();
}

} // namespace

TEST(ResearchSh4ObservationTrace, EquivalentStreamsValidateAndCompare)
{
	TemporaryDirectory temporary;
	const auto interpreterPath = temporary.file("interpreter.fcso");
	const auto dynarecPath = temporary.file("dynarec.fcso");
	const auto interpreterBinding = binding(
			research::Sh4ObservationBackend::Interpreter);
	const auto dynarecBinding = binding(research::Sh4ObservationBackend::Dynarec);
	const auto interpreterEvents = semanticStream(interpreterBinding.backend);
	const auto dynarecEvents = semanticStream(dynarecBinding.backend);
	writeTrace(interpreterPath, interpreterBinding, interpreterEvents);
	writeTrace(dynarecPath, dynarecBinding, dynarecEvents);

	const auto interpreterSummary = research::validateSh4ObservationTraceFile(
			interpreterPath, interpreterBinding);
	const auto dynarecSummary = research::validateSh4ObservationTraceFile(
			dynarecPath, dynarecBinding);
	EXPECT_EQ(interpreterEvents.size(), interpreterSummary.eventCount);
	EXPECT_EQ(dynarecEvents.size(), dynarecSummary.eventCount);

	const auto comparison = research::compareSh4ObservationTraces(
			interpreterPath, dynarecPath, contract());
	EXPECT_TRUE(comparison.equivalent);
	EXPECT_FALSE(comparison.firstDivergence.has_value());
	EXPECT_EQ(interpreterEvents.size(), comparison.matchedEventCount);
}

TEST(ResearchSh4ObservationTrace,
		NestedDelaySlotExceptionAbortsInnermostThenOwner)
{
	TemporaryDirectory temporary;
	const auto interpreterPath = temporary.file("interpreter.fcso");
	const auto dynarecPath = temporary.file("dynarec.fcso");
	const auto interpreterEvents = nestedDelaySlotExceptionStream(
			research::Sh4ObservationBackend::Interpreter);
	const auto dynarecEvents = nestedDelaySlotExceptionStream(
			research::Sh4ObservationBackend::Dynarec);
	writeTrace(interpreterPath,
			binding(research::Sh4ObservationBackend::Interpreter), interpreterEvents);
	writeTrace(dynarecPath, binding(research::Sh4ObservationBackend::Dynarec),
			dynarecEvents);

	EXPECT_EQ(interpreterEvents.size(),
			research::validateSh4ObservationTraceFile(interpreterPath,
					binding(research::Sh4ObservationBackend::Interpreter)).eventCount);
	EXPECT_EQ(dynarecEvents.size(),
			research::validateSh4ObservationTraceFile(dynarecPath,
					binding(research::Sh4ObservationBackend::Dynarec)).eventCount);
	const auto comparison = research::compareSh4ObservationTraces(
			interpreterPath, dynarecPath, contract());
	EXPECT_TRUE(comparison.equivalent);
	EXPECT_EQ(interpreterEvents.size(), comparison.matchedEventCount);
}

TEST(ResearchSh4ObservationTrace, SchedulerInterruptNeedsNoInstructionOwner)
{
	TemporaryDirectory temporary;
	const auto interpreterPath = temporary.file("interpreter.fcso");
	const auto dynarecPath = temporary.file("dynarec.fcso");
	const auto interpreterEvents = unownedInterruptStream(
			research::Sh4ObservationBackend::Interpreter);
	const auto dynarecEvents = unownedInterruptStream(
			research::Sh4ObservationBackend::Dynarec);
	writeTrace(interpreterPath,
			binding(research::Sh4ObservationBackend::Interpreter), interpreterEvents);
	writeTrace(dynarecPath, binding(research::Sh4ObservationBackend::Dynarec),
			dynarecEvents);

	EXPECT_NO_THROW(research::validateSh4ObservationTraceFile(interpreterPath,
			binding(research::Sh4ObservationBackend::Interpreter)));
	EXPECT_NO_THROW(research::validateSh4ObservationTraceFile(dynarecPath,
			binding(research::Sh4ObservationBackend::Dynarec)));
	EXPECT_TRUE(research::compareSh4ObservationTraces(interpreterPath,
			dynarecPath, contract()).equivalent);
}

TEST(ResearchSh4ObservationTrace, ReportsFirstSemanticDivergence)
{
	TemporaryDirectory temporary;
	const auto interpreterPath = temporary.file("interpreter.fcso");
	const auto dynarecPath = temporary.file("dynarec.fcso");
	auto interpreterEvents = semanticStream(
			research::Sh4ObservationBackend::Interpreter);
	auto dynarecEvents = semanticStream(research::Sh4ObservationBackend::Dynarec);
	dynarecEvents[0].registers.r[0] ^= 1;
	dynarecEvents[1].registers.r[0] ^= 1;
	writeTrace(interpreterPath,
			binding(research::Sh4ObservationBackend::Interpreter), interpreterEvents);
	writeTrace(dynarecPath, binding(research::Sh4ObservationBackend::Dynarec),
			dynarecEvents);

	const auto comparison = research::compareSh4ObservationTraces(
			interpreterPath, dynarecPath, contract());
	ASSERT_FALSE(comparison.equivalent);
	ASSERT_TRUE(comparison.firstDivergence.has_value());
	EXPECT_EQ(0u, comparison.firstDivergence->ordinal);
	EXPECT_EQ("registers.r[0]", comparison.firstDivergence->field);
	EXPECT_EQ(0u, comparison.matchedEventCount);
}

TEST(ResearchSh4ObservationTrace, ReportsFirstMissingEventAfterMatchingPrefix)
{
	TemporaryDirectory temporary;
	const auto interpreterPath = temporary.file("interpreter.fcso");
	const auto dynarecPath = temporary.file("dynarec.fcso");
	auto interpreterEvents = semanticStream(
			research::Sh4ObservationBackend::Interpreter);
	auto dynarecEvents = semanticStream(research::Sh4ObservationBackend::Dynarec);
	dynarecEvents.resize(dynarecEvents.size() - 3);
	writeTrace(interpreterPath,
			binding(research::Sh4ObservationBackend::Interpreter), interpreterEvents);
	writeTrace(dynarecPath, binding(research::Sh4ObservationBackend::Dynarec),
			dynarecEvents);

	const auto comparison = research::compareSh4ObservationTraces(
			interpreterPath, dynarecPath, contract());
	ASSERT_FALSE(comparison.equivalent);
	ASSERT_TRUE(comparison.firstDivergence.has_value());
	EXPECT_EQ(dynarecEvents.size(), comparison.firstDivergence->ordinal);
	EXPECT_EQ("event-presence", comparison.firstDivergence->field);
	EXPECT_EQ(dynarecEvents.size(), comparison.matchedEventCount);
}

TEST(ResearchSh4ObservationTrace, RejectsMismatchedBindings)
{
	TemporaryDirectory temporary;
	const auto interpreterPath = temporary.file("interpreter.fcso");
	const auto dynarecPath = temporary.file("dynarec.fcso");
	writeTrace(interpreterPath,
			binding(research::Sh4ObservationBackend::Interpreter),
			semanticStream(research::Sh4ObservationBackend::Interpreter));
	auto wrongReplayBinding = binding(research::Sh4ObservationBackend::Dynarec);
	wrongReplayBinding.replayDigest = digest("different-replay");
	writeTrace(dynarecPath, wrongReplayBinding,
			semanticStream(research::Sh4ObservationBackend::Dynarec));
	EXPECT_THROW(research::compareSh4ObservationTraces(interpreterPath,
			dynarecPath, contract()), std::runtime_error);
}

TEST(ResearchSh4ObservationTrace, IncompleteAndCorruptFilesFailClosed)
{
	TemporaryDirectory temporary;
	const auto incompletePath = temporary.file("incomplete.fcso");
	const auto interpreterBinding = binding(
			research::Sh4ObservationBackend::Interpreter);
	{
		research::Sh4ObservationTraceWriter writer(incompletePath,
				interpreterBinding);
		writer.write(semanticStream(interpreterBinding.backend).front());
	}
	EXPECT_THROW(research::validateSh4ObservationTraceFile(incompletePath,
			interpreterBinding), std::runtime_error);

	const auto corruptPath = temporary.file("corrupt.fcso");
	writeTrace(corruptPath, interpreterBinding,
			semanticStream(interpreterBinding.backend));
	flipByte(corruptPath, research::Sh4ObservationTraceHeaderSize + 24);
	EXPECT_THROW(research::validateSh4ObservationTraceFile(corruptPath,
			interpreterBinding), std::runtime_error);
}

TEST(ResearchSh4ObservationTrace, IndependentComparatorRejectsMalformedInputs)
{
	TemporaryDirectory temporary;
	const auto dynarecPath = temporary.file("dynarec.fcso");
	writeTrace(dynarecPath, binding(research::Sh4ObservationBackend::Dynarec),
			semanticStream(research::Sh4ObservationBackend::Dynarec));
	const auto interpreterBinding = binding(
			research::Sh4ObservationBackend::Interpreter);
	const auto interpreterEvents = semanticStream(interpreterBinding.backend);

	const auto incompletePath = temporary.file("incomplete.fcso");
	{
		research::Sh4ObservationTraceWriter writer(incompletePath,
				interpreterBinding);
		writer.write(interpreterEvents.front());
	}
	EXPECT_THROW(research::compareSh4ObservationTraces(incompletePath,
			dynarecPath, contract()), std::runtime_error);

	const auto corruptPath = temporary.file("corrupt.fcso");
	writeTrace(corruptPath, interpreterBinding, interpreterEvents);
	flipByte(corruptPath, research::Sh4ObservationTraceHeaderSize + 24);
	EXPECT_THROW(research::compareSh4ObservationTraces(corruptPath,
			dynarecPath, contract()), std::runtime_error);

	const auto trailingPath = temporary.file("trailing.fcso");
	writeTrace(trailingPath, interpreterBinding, interpreterEvents);
	{
		std::ofstream output(trailingPath, std::ios::binary | std::ios::app);
		output.put('\0');
	}
	EXPECT_THROW(research::compareSh4ObservationTraces(trailingPath,
			dynarecPath, contract()), std::runtime_error);

	const auto droppedPath = temporary.file("dropped.fcso");
	writeTrace(droppedPath, interpreterBinding, interpreterEvents);
	rewriteHeaderWithDrop(droppedPath);
	EXPECT_THROW(research::compareSh4ObservationTraces(droppedPath,
			dynarecPath, contract()), std::runtime_error);
}

TEST(ResearchSh4ObservationTrace, IndependentComparatorRejectsInvalidCallGrammar)
{
	TemporaryDirectory temporary;
	const auto interpreterPath = temporary.file("interpreter.fcso");
	const auto dynarecPath = temporary.file("dynarec.fcso");
	for (const auto backend : {research::Sh4ObservationBackend::Interpreter,
			research::Sh4ObservationBackend::Dynarec})
	{
		std::vector<research::Sh4Observation> events;
		events.push_back(instruction(backend,
				research::Sh4ObservationType::InstructionBegin, 10,
				0x8c010000, 0xb000));
		events.push_back(instruction(backend,
				research::Sh4ObservationType::InstructionEnd, 12,
				0x8c010000, 0xb000));
		writeTrace(backend == research::Sh4ObservationBackend::Interpreter
				? interpreterPath : dynarecPath, binding(backend), events);
	}
	EXPECT_THROW(research::compareSh4ObservationTraces(interpreterPath,
			dynarecPath, contract()), std::runtime_error);
}

TEST(ResearchSh4ObservationTrace, IndependentComparatorRejectsRtsTargetNotPr)
{
	TemporaryDirectory temporary;
	const auto interpreterPath = temporary.file("interpreter.fcso");
	const auto dynarecPath = temporary.file("dynarec.fcso");
	for (const auto backend : {research::Sh4ObservationBackend::Interpreter,
			research::Sh4ObservationBackend::Dynarec})
	{
		auto events = semanticStream(backend);
		events[9].nextPc = 0x8c0bad00;
		events[9].targetPc = 0x8c0bad00;
		events[10].nextPc = 0x8c0bad00;
		writeTrace(backend == research::Sh4ObservationBackend::Interpreter
				? interpreterPath : dynarecPath, binding(backend), events);
	}
	EXPECT_THROW(research::compareSh4ObservationTraces(interpreterPath,
			dynarecPath, contract()), std::runtime_error);
}

TEST(ResearchSh4ObservationTrace, IndependentComparatorRejectsMissingDelaySlot)
{
	TemporaryDirectory temporary;
	const auto interpreterPath = temporary.file("interpreter.fcso");
	const auto dynarecPath = temporary.file("dynarec.fcso");
	for (const auto backend : {research::Sh4ObservationBackend::Interpreter,
			research::Sh4ObservationBackend::Dynarec})
	{
		std::vector<research::Sh4Observation> events;
		auto begin = instruction(backend,
				research::Sh4ObservationType::InstructionBegin, 10,
				0x8c010000, 0xb000);
		events.push_back(begin);
		auto call = begin;
		call.type = research::Sh4ObservationType::Call;
		call.callKind = research::Sh4CallKind::Bsr;
		call.targetPc = begin.instructionPc + 4u;
		call.returnPc = begin.instructionPc + 4u;
		call.delaySlotPc = begin.instructionPc + 2u;
		events.push_back(call);
		events.push_back(instruction(backend,
				research::Sh4ObservationType::InstructionEnd, 12,
				begin.instructionPc, begin.opcode));
		writeTrace(backend == research::Sh4ObservationBackend::Interpreter
				? interpreterPath : dynarecPath, binding(backend), events);
	}
	EXPECT_THROW(research::compareSh4ObservationTraces(interpreterPath,
			dynarecPath, contract()), std::runtime_error);
}

TEST(ResearchSh4ObservationTrace, IndependentComparatorRejectsUnjustifiedAbort)
{
	TemporaryDirectory temporary;
	const auto interpreterPath = temporary.file("interpreter.fcso");
	const auto dynarecPath = temporary.file("dynarec.fcso");
	for (const auto backend : {research::Sh4ObservationBackend::Interpreter,
			research::Sh4ObservationBackend::Dynarec})
	{
		std::vector<research::Sh4Observation> events;
		auto begin = instruction(backend,
				research::Sh4ObservationType::InstructionBegin, 10,
				0x8c010000, 0x0009);
		events.push_back(begin);
		research::Sh4Observation abort;
		abort.backend = backend;
		abort.type = research::Sh4ObservationType::InstructionAbort;
		abort.tick = begin.tick;
		abort.instructionPc = begin.instructionPc;
		abort.opcode = begin.opcode;
		events.push_back(abort);
		writeTrace(backend == research::Sh4ObservationBackend::Interpreter
				? interpreterPath : dynarecPath, binding(backend), events);
	}
	EXPECT_THROW(research::compareSh4ObservationTraces(interpreterPath,
			dynarecPath, contract()), std::runtime_error);
}

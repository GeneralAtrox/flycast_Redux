#pragma once

#include "research/sh4_observation.h"
#include "research/sh4_observation_runtime.h"

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace sh4_dynarec_test
{

constexpr std::uint32_t StartPc = 0xac000000;
constexpr std::uint32_t DataAddress = 0x8c001000;
constexpr std::uint32_t LoopEndPc = StartPc + 0x1c;
constexpr std::uint32_t MapleStorePc = StartPc + 0x20;
constexpr std::uint32_t InterruptVectorBase = 0x8c002000;
constexpr std::uint32_t InterruptHandlerPc = InterruptVectorBase + 0x600;
constexpr std::uint32_t InterruptMapleStorePc = InterruptHandlerPc + 0x2000;
constexpr std::size_t LoopCount = 8;

struct MapleDmaTickSample
{
	std::uint64_t rawTick = 0;
	std::uint64_t executionTick = 0;
	std::uint32_t ownerPc = 0;
	std::uint16_t ownerOpcode = 0;
	bool ownerValid = false;
	bool captured = false;
};

// Installs the maple DMA tick observer for the lifetime of the guard and
// records the first sample delivered into the referenced MapleDmaTickSample.
class MapleDmaTickObserverGuard
{
public:
	explicit MapleDmaTickObserverGuard(MapleDmaTickSample& sample);
	~MapleDmaTickObserverGuard();
};

void writeProgram(std::initializer_list<std::uint16_t> opcodes);

void configureMmu(bool fullMmu);

// Enables/disables precise dynarec execution timing capture for the given
// backend for the lifetime of the guard, and resets the timing state on
// construction and destruction.
class NativeCaptureTimingGuard
{
public:
	explicit NativeCaptureTimingGuard(
			research::Sh4ObservationBackend backend, bool precise = true);
	~NativeCaptureTimingGuard();

private:
	research::Sh4ObservationBackend backend;
	bool precise;
};

std::vector<research::Sh4Observation> runBackend(
		research::Sh4ObservationBackend backend, bool fullMmu,
		bool precise = true);

std::vector<research::Sh4Observation> runFaultBackend(
		research::Sh4ObservationBackend backend, bool delaySlot);

std::vector<research::Sh4Observation> runInterruptBackend(
		research::Sh4ObservationBackend backend, bool precise = true);

std::vector<research::Sh4Observation> runRteInterruptBackend(
		research::Sh4ObservationBackend backend, bool precise);

std::pair<std::vector<research::Sh4Observation>, MapleDmaTickSample>
runMapleMmioBackend(research::Sh4ObservationBackend backend);

std::pair<std::vector<research::Sh4Observation>, MapleDmaTickSample>
runInterruptMapleMmioBackend(research::Sh4ObservationBackend backend);

std::vector<research::Sh4Observation> runSelfModifyingBackend(
		research::Sh4ObservationBackend backend);

std::vector<research::Sh4Observation> runConditionalDelayBackend(
		research::Sh4ObservationBackend backend, bool taken);

std::vector<research::Sh4Observation> runFallbackMemoryBackend(
		research::Sh4ObservationBackend backend);

void expectSameSemanticEvent(const research::Sh4Observation& interpreter,
		const research::Sh4Observation& dynarec, std::size_t index);

void expectSameSemanticStream(
		const std::vector<research::Sh4Observation>& interpreter,
		const std::vector<research::Sh4Observation>& dynarec);

} // namespace sh4_dynarec_test

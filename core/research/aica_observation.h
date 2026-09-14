#pragma once

#include "research/sh4_observation_runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace research
{

constexpr std::uint32_t AicaObservationSchemaVersion = 1;

enum class AicaObservationType : std::uint8_t
{
	RegisterWrite = 1,
	RamWrite = 2,
	G2DmaBegin = 3,
	G2DmaTransfer = 4,
	G2DmaComplete = 5,
	KeyOn = 6,
	KeyOff = 7,
	SampleFrame = 8,
	Reset = 9,
	KeyBatchComplete = 10,
	CddaSector = 11,
	SampleSuppressed = 12,
	KeyBatchBegin = 13,
};

enum class AicaSampleSuppression : std::uint8_t
{
	None = 0,
	Muted = 1,
	FastForward = 2,
};

enum class AicaWriter : std::uint8_t
{
	Unknown = 0,
	Sh4Direct = 1,
	Sh4G2Dma = 2,
	Arm7 = 3,
	AicaInternalDma = 4,
	Mixer = 5,
	ReiosHle = 6,
};

struct AicaOwnerToken
{
	AicaWriter writer = AicaWriter::Unknown;
	Sh4InstructionOwnerToken sh4;
	// The current ARM7 interpreter/recompiler does not expose a common exact
	// instruction boundary. Keep that absence explicit rather than deriving a
	// misleading PC from the architecturally visible R15 value.
	bool arm7PcAvailable = false;
	std::uint32_t arm7Pc = 0;
};

struct AicaObservation
{
	std::uint32_t schemaVersion = AicaObservationSchemaVersion;
	std::uint64_t emissionOrdinal = 0;
	AicaObservationType type = AicaObservationType::RegisterWrite;
	std::uint64_t tick = 0;
	AicaOwnerToken owner;

	std::uint32_t address = 0;
	std::uint8_t width = 0;
	std::uint32_t value = 0;
	std::vector<std::uint8_t> bytes;

	std::uint64_t dmaGeneration = 0;
	std::uint32_t sourceAddress = 0;
	std::uint32_t destinationAddress = 0;
	std::uint32_t transferLength = 0;
	bool aicaRamIsDestination = false;

	std::uint8_t channel = 0;
	std::array<std::uint8_t, 0x80> channelRegisters {};
	std::uint64_t keyOnMask = 0;
	std::uint64_t keyOffMask = 0;
	// Number of AICA sample frames completed before this key transition/batch.
	// This is the authoritative cut used to schedule the transition on replay.
	std::uint64_t sampleCutOrdinal = 0;

	std::uint64_t cddaGeneration = 0;
	std::uint32_t cddaFad = 0;
	std::uint32_t cddaStatus = 0;
	std::uint32_t cddaRepeats = 0;
	bool cddaReadSuccessful = false;
	std::uint16_t cddaFrameIndex = 0;
	AicaSampleSuppression suppression = AicaSampleSuppression::None;

	std::uint64_t sampleOrdinal = 0;
	std::uint64_t activeChannelMask = 0;
	std::int32_t dryLeft = 0;
	std::int32_t dryRight = 0;
	std::int32_t cddaInputLeft = 0;
	std::int32_t cddaInputRight = 0;
	std::int32_t cddaContributionLeft = 0;
	std::int32_t cddaContributionRight = 0;
	bool dspEnabled = false;
	std::int32_t dspContributionLeft = 0;
	std::int32_t dspContributionRight = 0;
	std::array<std::int32_t, 16> dspInputs {};
	std::array<std::int16_t, 16> dspEffectOutputs {};
	std::int16_t finalLeft = 0;
	std::int16_t finalRight = 0;
};

enum class AicaCheckpointPhase : std::uint32_t
{
	Unknown = 0,
	PreKeyBatch = 1,
	PostSample = 2,
};

struct AicaChannelCheckpoint
{
	std::uint32_t sampleAddress = 0;
	std::uint32_t currentAddress = 0;
	std::uint32_t step = 0;
	std::int32_t sample0 = 0;
	std::int32_t sample1 = 0;
	bool looped = false;
	std::int32_t adpcmLastQuant = 0;
	std::int32_t adpcmLoopQuant = 0;
	std::int32_t adpcmLoopSample = 0;
	bool adpcmInLoop = false;
	std::uint32_t noiseState = 0;
	std::int32_t aegValue = 0;
	std::uint32_t aegState = 0;
	std::uint32_t fegValue = 0;
	std::uint32_t fegState = 0;
	std::int32_t fegPrevious1 = 0;
	std::int32_t fegPrevious2 = 0;
	std::int32_t fegFraction = 0;
	std::uint32_t lfoCounter = 0;
	std::uint8_t lfoState = 0;
	bool enabled = false;
};

struct AicaCheckpoint
{
	std::uint64_t tick = 0;
	std::uint64_t nextSampleOrdinal = 0;
	AicaCheckpointPhase phase = AicaCheckpointPhase::Unknown;
	std::uint64_t activeChannelMask = 0;
	std::array<std::uint8_t, 0x8000> registers {};
	std::vector<std::uint8_t> ram;
	std::array<AicaChannelCheckpoint, 64> channels {};
	std::array<std::int32_t, 128> dspTemp {};
	std::array<std::int32_t, 32> dspMems {};
	std::array<std::int32_t, 16> dspMixs {};
	std::uint32_t dspRingBufferPointer = 0;
	std::uint32_t dspRingBufferLength = 0;
	std::uint32_t dspMemoryDecodeCounter = 0;
	std::array<std::uint8_t, 2352> cddaSector {};
	std::uint32_t cddaIndex = 0;
	std::uint64_t cddaGeneration = 0;
	bool cddaSourceAvailable = false;
	std::uint32_t cddaFad = 0;
	std::uint32_t cddaStatus = 0;
	std::uint32_t cddaRepeats = 0;
	bool cddaReadSuccessful = false;
	std::uint64_t cddaControlGeneration = 0;
};

using AicaObservationSubscription = std::uint64_t;
using AicaObservationCallback = std::function<void(const AicaObservation&)>;

class AicaWriterScope
{
public:
	explicit AicaWriterScope(AicaWriter writer) noexcept;
	~AicaWriterScope();
	AicaWriterScope(const AicaWriterScope&) = delete;
	AicaWriterScope& operator=(const AicaWriterScope&) = delete;
private:
	AicaWriter previous = AicaWriter::Unknown;
};

#ifdef LIBRETRO
inline AicaObservationSubscription subscribeAicaObservations(
		AicaObservationCallback) { return 0; }
inline AicaObservationSubscription subscribeAicaEvidenceObservations(
		AicaObservationCallback) { return 0; }
inline bool unsubscribeAicaObservations(AicaObservationSubscription) noexcept { return false; }
inline bool aicaObservationBusActive() noexcept { return false; }
inline std::uint64_t aicaObservationDroppedCount() noexcept { return 0; }
inline AicaWriterScope::AicaWriterScope(AicaWriter) noexcept {}
inline AicaWriterScope::~AicaWriterScope() = default;
inline void observeAicaRegisterWrite(AicaWriter, std::uint32_t, std::uint8_t,
		std::uint32_t, std::uint64_t) noexcept {}
inline void observeAicaRamWrite(AicaWriter, std::uint32_t, const std::uint8_t*,
		std::size_t, std::uint64_t) noexcept {}
inline void observeAicaRamWriteValue(AicaWriter, std::uint32_t, std::uint32_t,
		std::uint8_t, std::uint64_t) noexcept {}
inline std::uint64_t observeAicaG2DmaBegin(std::uint32_t, std::uint32_t,
		std::uint32_t, bool, std::uint64_t) noexcept { return 0; }
inline void observeAicaG2DmaTransfer(std::uint64_t, const std::uint8_t*,
		std::size_t, std::uint64_t) noexcept {}
inline void observeAicaG2DmaComplete(std::uint64_t) noexcept {}
inline void observeAicaKeyTransition(bool, std::uint8_t, const std::uint8_t*,
		const std::uint8_t*, std::size_t, std::uint64_t) noexcept {}
inline void observeAicaKeyBatchBegin(std::uint64_t) noexcept {}
inline void observeAicaKeyBatchComplete(std::uint64_t, std::uint64_t,
		std::uint64_t) noexcept {}
inline std::uint64_t observeAicaCddaSector(std::uint32_t, std::uint32_t,
		std::uint32_t, bool, const std::uint8_t*, std::size_t,
		std::uint64_t) noexcept { return 0; }
inline void observeAicaSampleSuppressed(AicaSampleSuppression,
		std::uint64_t) noexcept {}
inline void observeAicaSampleFrame(std::uint64_t, std::int32_t, std::int32_t,
		std::int32_t, std::int32_t, std::int32_t, std::int32_t, bool,
		std::int32_t, std::int32_t, const std::int32_t*, const std::int16_t*,
		std::int16_t, std::int16_t,
		std::uint64_t, std::uint16_t, std::uint64_t) noexcept {}
inline bool aicaObservationDmaActive() noexcept { return false; }
inline std::uint64_t aicaObservationNextSampleOrdinal() noexcept { return 0; }
inline void restoreAicaCddaGeneration(std::uint64_t) noexcept {}
inline void resetAicaObservation(std::uint64_t) noexcept {}
#else
AicaObservationSubscription subscribeAicaObservations(
		AicaObservationCallback callback);
AicaObservationSubscription subscribeAicaEvidenceObservations(
		AicaObservationCallback callback);
bool unsubscribeAicaObservations(AicaObservationSubscription subscription) noexcept;
bool aicaObservationBusActive() noexcept;
std::uint64_t aicaObservationDroppedCount() noexcept;
void observeAicaRegisterWrite(AicaWriter writer, std::uint32_t address,
		std::uint8_t width, std::uint32_t value, std::uint64_t tick) noexcept;
void observeAicaRamWrite(AicaWriter writer, std::uint32_t address,
		const std::uint8_t* bytes, std::size_t byteCount,
		std::uint64_t tick) noexcept;
void observeAicaRamWriteValue(AicaWriter writer, std::uint32_t address,
		std::uint32_t value, std::uint8_t width, std::uint64_t tick) noexcept;
std::uint64_t observeAicaG2DmaBegin(std::uint32_t sourceAddress,
		std::uint32_t destinationAddress, std::uint32_t length,
		bool aicaRamIsDestination, std::uint64_t tick) noexcept;
void observeAicaG2DmaTransfer(std::uint64_t generation,
		const std::uint8_t* bytes, std::size_t byteCount,
		std::uint64_t tick) noexcept;
void observeAicaG2DmaComplete(std::uint64_t tick) noexcept;
void observeAicaKeyTransition(bool keyOn, std::uint8_t channel,
		const std::uint8_t* channelRegisters, const std::uint8_t* aicaRam,
		std::size_t aicaRamSize, std::uint64_t tick) noexcept;
void observeAicaKeyBatchBegin(std::uint64_t tick) noexcept;
void observeAicaKeyBatchComplete(std::uint64_t keyOnMask,
		std::uint64_t keyOffMask, std::uint64_t tick) noexcept;
std::uint64_t observeAicaCddaSector(std::uint32_t fad,
		std::uint32_t status, std::uint32_t repeats, bool readSuccessful,
		const std::uint8_t* bytes, std::size_t byteCount,
		std::uint64_t tick) noexcept;
void observeAicaSampleSuppressed(AicaSampleSuppression suppression,
		std::uint64_t tick) noexcept;
void observeAicaSampleFrame(std::uint64_t activeChannelMask,
		std::int32_t dryLeft, std::int32_t dryRight,
		std::int32_t cddaInputLeft, std::int32_t cddaInputRight,
		std::int32_t cddaContributionLeft, std::int32_t cddaContributionRight,
		bool dspEnabled, std::int32_t dspContributionLeft,
		std::int32_t dspContributionRight, const std::int32_t* dspInputs,
		const std::int16_t* dspEffectOutputs, std::int16_t finalLeft,
		std::int16_t finalRight, std::uint64_t cddaGeneration,
		std::uint16_t cddaFrameIndex, std::uint64_t tick) noexcept;
bool aicaObservationDmaActive() noexcept;
std::uint64_t aicaObservationNextSampleOrdinal() noexcept;
void restoreAicaCddaGeneration(std::uint64_t generation) noexcept;
void resetAicaObservation(std::uint64_t tick) noexcept;
#endif

} // namespace research

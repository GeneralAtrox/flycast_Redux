#include "research/aica_observation.h"
#include "research/aica_observation_internal.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace research
{

using namespace detail_aica;

namespace
{

AicaOwnerToken currentOwner(AicaWriter writer) noexcept
{
	AicaOwnerToken owner;
	owner.writer = writer;
	if (writer == AicaWriter::Sh4Direct || writer == AicaWriter::Sh4G2Dma)
		owner.sh4 = sh4ObservationCurrentInstructionOwner();
	return owner;
}

} // namespace

AicaWriterScope::AicaWriterScope(AicaWriter writer) noexcept
	: previous(currentWriter)
{
	currentWriter = writer;
}

AicaWriterScope::~AicaWriterScope()
{
	currentWriter = previous;
}

void observeAicaRegisterWrite(AicaWriter writer, std::uint32_t address,
		std::uint8_t width, std::uint32_t value, std::uint64_t tick) noexcept
{
	if (!aicaObservationBusActive())
		return;
	AicaObservation observation;
	observation.type = AicaObservationType::RegisterWrite;
	observation.tick = tick;
	observation.owner = currentOwner(writer);
	if (observation.owner.sh4.valid)
		observation.tick = std::max(observation.tick, observation.owner.sh4.tick);
	observation.address = address & 0x7fff;
	observation.width = width;
	observation.value = value;
	publish(std::move(observation));
}

void observeAicaRamWrite(AicaWriter writer, std::uint32_t address,
		const std::uint8_t* bytes, std::size_t byteCount,
		std::uint64_t tick) noexcept
{
	if (!aicaObservationBusActive())
		return;
	try
	{
		AicaObservation observation;
		observation.type = AicaObservationType::RamWrite;
		observation.tick = tick;
		observation.owner = currentOwner(writer);
		if (observation.owner.sh4.valid)
			observation.tick = std::max(observation.tick, observation.owner.sh4.tick);
		observation.address = address;
		observation.bytes.assign(bytes, bytes + byteCount);
		publish(std::move(observation));
	}
	catch (...) { dropped(); }
}

void observeAicaRamWriteValue(AicaWriter writer, std::uint32_t address,
		std::uint32_t value, std::uint8_t width, std::uint64_t tick) noexcept
{
	if (!aicaObservationBusActive())
		return;
	std::array<std::uint8_t, 4> bytes {};
	for (std::uint8_t index = 0; index < width && index < bytes.size(); ++index)
		bytes[index] = static_cast<std::uint8_t>(value >> (index * 8));
	observeAicaRamWrite(writer, address, bytes.data(),
			std::min<std::size_t>(width, bytes.size()), tick);
}

std::uint64_t observeAicaG2DmaBegin(std::uint32_t sourceAddress,
		std::uint32_t destinationAddress, std::uint32_t length,
		bool aicaRamIsDestination, std::uint64_t tick) noexcept
{
	if (!aicaObservationBusActive())
		return 0;
	AicaObservation observation;
	observation.type = AicaObservationType::G2DmaBegin;
	observation.tick = tick;
	observation.owner = currentOwner(AicaWriter::Sh4G2Dma);
	if (observation.owner.sh4.valid)
		observation.tick = std::max(observation.tick, observation.owner.sh4.tick);
	observation.sourceAddress = sourceAddress;
	observation.destinationAddress = destinationAddress;
	observation.transferLength = length;
	observation.aicaRamIsDestination = aicaRamIsDestination;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (dmaState.active) { dropped(); return 0; }
		dmaState.active = true;
		dmaState.generation = nextDmaGeneration.fetch_add(1,
				std::memory_order_relaxed);
		dmaState.owner = observation.owner;
		dmaState.sourceAddress = sourceAddress;
		dmaState.destinationAddress = destinationAddress;
		dmaState.transferLength = length;
		dmaState.aicaRamIsDestination = aicaRamIsDestination;
		observation.dmaGeneration = dmaState.generation;
	}
	const std::uint64_t generation = observation.dmaGeneration;
	if (!publish(std::move(observation)))
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		dmaState = {};
		return 0;
	}
	return generation;
}

void observeAicaG2DmaTransfer(std::uint64_t generation,
		const std::uint8_t* bytes, std::size_t byteCount,
		std::uint64_t tick) noexcept
{
	if (!aicaObservationBusActive() || generation == 0)
		return;
	try
	{
		AicaObservation observation;
		observation.type = AicaObservationType::G2DmaTransfer;
		observation.tick = tick;
		observation.dmaGeneration = generation;
		observation.bytes.assign(bytes, bytes + byteCount);
		{
			const std::lock_guard<std::mutex> lock(stateMutex);
			if (!dmaState.active || dmaState.generation != generation)
			{
				dropped();
				return;
			}
			observation.owner = dmaState.owner;
			observation.sourceAddress = dmaState.sourceAddress;
			observation.destinationAddress = dmaState.destinationAddress;
			observation.transferLength = dmaState.transferLength;
			observation.aicaRamIsDestination = dmaState.aicaRamIsDestination;
		}
		publish(std::move(observation));
	}
	catch (...) { dropped(); }
}

void observeAicaG2DmaComplete(std::uint64_t tick) noexcept
{
	if (!aicaObservationBusActive())
		return;
	AicaObservation observation;
	observation.type = AicaObservationType::G2DmaComplete;
	observation.tick = tick;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		if (!dmaState.active) { dropped(); return; }
		observation.dmaGeneration = dmaState.generation;
		observation.owner = dmaState.owner;
		observation.sourceAddress = dmaState.sourceAddress;
		observation.destinationAddress = dmaState.destinationAddress;
		observation.transferLength = dmaState.transferLength;
		observation.aicaRamIsDestination = dmaState.aicaRamIsDestination;
		dmaState = {};
	}
	publish(std::move(observation));
}

void observeAicaKeyTransition(bool keyOn, std::uint8_t channel,
		const std::uint8_t* channelRegisters, const std::uint8_t* aicaRam,
		std::size_t aicaRamSize, std::uint64_t tick) noexcept
{
	if (!aicaObservationBusActive())
		return;
	try
	{
		AicaObservation observation;
		observation.type = keyOn ? AicaObservationType::KeyOn
				: AicaObservationType::KeyOff;
		observation.tick = tick;
		observation.owner = currentOwner(currentWriter);
		if (observation.owner.sh4.valid)
			observation.tick = std::max(observation.tick, observation.owner.sh4.tick);
		observation.channel = channel;
		observation.sampleCutOrdinal = nextSampleOrdinal.load(std::memory_order_acquire);
		std::copy(channelRegisters,
				channelRegisters + observation.channelRegisters.size(),
				observation.channelRegisters.begin());
		if (keyOn)
		{
			if (aicaRam == nullptr || aicaRamSize == 0
					|| (aicaRamSize & (aicaRamSize - 1)) != 0)
			{
				dropped();
				return;
			}
			const auto& r = observation.channelRegisters;
			const std::uint16_t word0 = std::uint16_t(r[0])
					| (std::uint16_t(r[1]) << 8);
			if (((word0 >> 10) & 1) == 0)
			{
				const std::uint32_t format = (word0 >> 7) & 3;
				std::uint32_t address = (std::uint32_t(word0 & 0x7f) << 16)
						| r[4] | (std::uint32_t(r[5]) << 8);
				if (format == 0) address &= ~1u;
				const std::uint32_t lsa = r[8] | (std::uint32_t(r[9]) << 8);
				const std::uint32_t lea = r[12] | (std::uint32_t(r[13]) << 8);
				const std::uint32_t samples = std::max(lsa, lea);
				const std::uint64_t length = format == 0
						? std::uint64_t(samples) * 2
						: format == 1 ? samples : (std::uint64_t(samples) + 1) / 2;
				if (length == 0 || length > aicaRamSize)
				{
					dropped();
					return;
				}
				observation.bytes.resize(static_cast<std::size_t>(length));
				for (std::size_t index = 0; index < observation.bytes.size(); ++index)
					observation.bytes[index] = aicaRam[(std::uint64_t(address) + index)
							& (aicaRamSize - 1)];
			}
		}
		publish(std::move(observation));
	}
	catch (...) { dropped(); }
}

void observeAicaKeyBatchBegin(std::uint64_t tick) noexcept
{
	if (!aicaObservationBusActive())
		return;
	AicaObservation observation;
	observation.type = AicaObservationType::KeyBatchBegin;
	observation.tick = tick;
	observation.owner = currentOwner(currentWriter);
	if (observation.owner.sh4.valid)
		observation.tick = std::max(observation.tick, observation.owner.sh4.tick);
	observation.sampleCutOrdinal = nextSampleOrdinal.load(std::memory_order_acquire);
	publish(std::move(observation));
}

void observeAicaKeyBatchComplete(std::uint64_t keyOnMask,
		std::uint64_t keyOffMask, std::uint64_t tick) noexcept
{
	if (!aicaObservationBusActive())
		return;
	AicaObservation observation;
	observation.type = AicaObservationType::KeyBatchComplete;
	observation.tick = tick;
	observation.owner = currentOwner(currentWriter);
	if (observation.owner.sh4.valid)
		observation.tick = std::max(observation.tick, observation.owner.sh4.tick);
	observation.keyOnMask = keyOnMask;
	observation.keyOffMask = keyOffMask;
	observation.sampleCutOrdinal = nextSampleOrdinal.load(std::memory_order_acquire);
	publish(std::move(observation));
}

std::uint64_t observeAicaCddaSector(std::uint32_t fad,
		std::uint32_t status, std::uint32_t repeats, bool readSuccessful,
		const std::uint8_t* bytes, std::size_t byteCount,
		std::uint64_t tick) noexcept
{
	if (!aicaObservationBusActive())
		return 0;
	try
	{
		AicaObservation observation;
		observation.type = AicaObservationType::CddaSector;
		observation.tick = tick;
		observation.owner.writer = AicaWriter::Mixer;
		observation.cddaGeneration = nextCddaGeneration.fetch_add(1,
				std::memory_order_relaxed);
		observation.cddaFad = fad;
		observation.cddaStatus = status;
		observation.cddaRepeats = repeats;
		observation.cddaReadSuccessful = readSuccessful;
		observation.bytes.assign(bytes, bytes + byteCount);
		const auto generation = observation.cddaGeneration;
		return publish(std::move(observation)) ? generation : 0;
	}
	catch (...) { dropped(); return 0; }
}

void observeAicaSampleSuppressed(AicaSampleSuppression suppression,
		std::uint64_t tick) noexcept
{
	if (!aicaObservationBusActive())
		return;
	AicaObservation observation;
	observation.type = AicaObservationType::SampleSuppressed;
	observation.tick = tick;
	observation.owner.writer = AicaWriter::Mixer;
	observation.suppression = suppression;
	publish(std::move(observation));
}

void observeAicaSampleFrame(std::uint64_t activeChannelMask,
		std::int32_t dryLeft, std::int32_t dryRight,
		std::int32_t cddaInputLeft, std::int32_t cddaInputRight,
		std::int32_t cddaContributionLeft, std::int32_t cddaContributionRight,
		bool dspEnabled, std::int32_t dspContributionLeft,
		std::int32_t dspContributionRight, const std::int32_t* dspInputs,
		const std::int16_t* dspEffectOutputs, std::int16_t finalLeft,
		std::int16_t finalRight, std::uint64_t cddaGeneration,
		std::uint16_t cddaFrameIndex, std::uint64_t tick) noexcept
{
	if (!aicaObservationBusActive())
		return;
	AicaObservation observation;
	observation.type = AicaObservationType::SampleFrame;
	observation.tick = tick;
	observation.owner.writer = AicaWriter::Mixer;
	observation.sampleOrdinal = nextSampleOrdinal.fetch_add(1,
			std::memory_order_relaxed);
	observation.activeChannelMask = activeChannelMask;
	observation.dryLeft = dryLeft;
	observation.dryRight = dryRight;
	observation.cddaInputLeft = cddaInputLeft;
	observation.cddaInputRight = cddaInputRight;
	observation.cddaContributionLeft = cddaContributionLeft;
	observation.cddaContributionRight = cddaContributionRight;
	observation.dspEnabled = dspEnabled;
	observation.dspContributionLeft = dspContributionLeft;
	observation.dspContributionRight = dspContributionRight;
	std::copy(dspInputs, dspInputs + observation.dspInputs.size(),
			observation.dspInputs.begin());
	std::copy(dspEffectOutputs,
			dspEffectOutputs + observation.dspEffectOutputs.size(),
			observation.dspEffectOutputs.begin());
	observation.finalLeft = finalLeft;
	observation.finalRight = finalRight;
	observation.cddaGeneration = cddaGeneration;
	observation.cddaFrameIndex = cddaFrameIndex;
	publish(std::move(observation));
}

void resetAicaObservation(std::uint64_t tick) noexcept
{
	if (!aicaObservationBusActive())
		return;
	AicaObservation observation;
	observation.type = AicaObservationType::Reset;
	observation.tick = tick;
	{
		const std::lock_guard<std::mutex> lock(stateMutex);
		observation.dmaGeneration = dmaState.generation;
		dmaState = {};
	}
	nextSampleOrdinal.store(0, std::memory_order_release);
	publish(std::move(observation));
}

} // namespace research

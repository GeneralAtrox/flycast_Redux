#include "research/aica_observation.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace research
{
namespace
{

struct Subscription
{
	AicaObservationSubscription id = 0;
	AicaObservationCallback callback;
	std::atomic<bool> active {true};
	bool evidence = false;
};

struct DmaState
{
	bool active = false;
	std::uint64_t generation = 0;
	AicaOwnerToken owner;
	std::uint32_t sourceAddress = 0;
	std::uint32_t destinationAddress = 0;
	std::uint32_t transferLength = 0;
	bool aicaRamIsDestination = false;
};

std::recursive_mutex dispatchMutex;
std::mutex subscriptionMutex;
std::mutex stateMutex;
std::vector<std::shared_ptr<Subscription>> subscriptions;
std::atomic<bool> activeSubscription {false};
std::atomic<bool> activeEvidenceSubscription {false};
std::atomic<std::uint64_t> nextSubscription {1};
std::atomic<std::uint64_t> nextEmissionOrdinal {0};
std::atomic<std::uint64_t> nextDmaGeneration {1};
std::atomic<std::uint64_t> nextCddaGeneration {1};
std::atomic<std::uint64_t> nextSampleOrdinal {0};
std::atomic<std::uint64_t> droppedCount {0};
DmaState dmaState;
thread_local bool publishing = false;
thread_local AicaWriter currentWriter = AicaWriter::Unknown;

void reserveCddaGeneration(std::uint64_t generation) noexcept
{
	std::uint64_t expected = nextCddaGeneration.load(std::memory_order_relaxed);
	while (expected <= generation &&
			!nextCddaGeneration.compare_exchange_weak(expected, generation + 1,
					std::memory_order_relaxed)) {}
}

void dropped() noexcept
{
	droppedCount.fetch_add(1, std::memory_order_relaxed);
}

AicaOwnerToken currentOwner(AicaWriter writer) noexcept
{
	AicaOwnerToken owner;
	owner.writer = writer;
	if (writer == AicaWriter::Sh4Direct || writer == AicaWriter::Sh4G2Dma)
		owner.sh4 = sh4ObservationCurrentInstructionOwner();
	return owner;
}

bool publish(AicaObservation observation) noexcept
{
	try
	{
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		std::vector<std::shared_ptr<Subscription>> targets;
		{
			const std::lock_guard<std::mutex> lock(subscriptionMutex);
			targets = subscriptions;
		}
		if (targets.empty())
			return false;
		if (publishing)
		{
			dropped();
			return false;
		}
		publishing = true;
		struct Reset { ~Reset() { publishing = false; } } reset;
		observation.emissionOrdinal = nextEmissionOrdinal.fetch_add(1,
				std::memory_order_relaxed);
		bool delivered = false;
		for (const auto& target : targets)
		{
			if (!target->active.load(std::memory_order_acquire))
				continue;
			try { target->callback(observation); }
			catch (...) { dropped(); continue; }
			delivered = true;
		}
		return delivered;
	}
	catch (...) { dropped(); return false; }
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

AicaObservationSubscription subscribe(
		AicaObservationCallback callback, bool evidence)
{
	if (!callback)
		throw std::invalid_argument("AICA observation callback is empty");
	const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
	const std::lock_guard<std::mutex> lock(subscriptionMutex);
	if (evidence && !subscriptions.empty())
		throw std::logic_error("AICA evidence observation requires exclusive ownership");
	if (!evidence && activeEvidenceSubscription.load(std::memory_order_acquire))
		throw std::logic_error("AICA evidence observation owns the bus exclusively");
	auto next = std::make_shared<Subscription>();
	next->id = nextSubscription.fetch_add(1, std::memory_order_relaxed);
	if (next->id == 0)
		throw std::overflow_error("AICA observation subscription id overflow");
	next->callback = std::move(callback);
	next->evidence = evidence;
	if (subscriptions.empty()) {
		const std::lock_guard<std::mutex> stateLock(stateMutex);
		dmaState = {};
	}
	subscriptions.push_back(next);
	retainSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
	retainSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
	activeSubscription.store(true, std::memory_order_release);
	if (evidence)
		activeEvidenceSubscription.store(true, std::memory_order_release);
	return next->id;
}

AicaObservationSubscription subscribeAicaObservations(
		AicaObservationCallback callback)
{
	return subscribe(std::move(callback), false);
}

AicaObservationSubscription subscribeAicaEvidenceObservations(
		AicaObservationCallback callback)
{
	return subscribe(std::move(callback), true);
}

bool unsubscribeAicaObservations(AicaObservationSubscription id) noexcept
{
	try
	{
		const std::lock_guard<std::recursive_mutex> dispatchLock(dispatchMutex);
		const std::lock_guard<std::mutex> lock(subscriptionMutex);
		const auto found = std::find_if(subscriptions.begin(), subscriptions.end(),
				[id](const std::shared_ptr<Subscription>& entry) {
					return entry->id == id;
				});
		if (found == subscriptions.end())
			return false;
		(*found)->active.store(false, std::memory_order_release);
		if ((*found)->evidence)
			activeEvidenceSubscription.store(false, std::memory_order_release);
		subscriptions.erase(found);
		activeSubscription.store(!subscriptions.empty(), std::memory_order_release);
		if (subscriptions.empty()) {
			const std::lock_guard<std::mutex> stateLock(stateMutex);
			dmaState = {};
		}
		releaseSh4InstructionOwnership(Sh4ObservationBackend::Interpreter);
		releaseSh4InstructionOwnership(Sh4ObservationBackend::Dynarec);
		return true;
	}
	catch (...) { dropped(); return false; }
}

bool aicaObservationBusActive() noexcept
{
	return activeSubscription.load(std::memory_order_acquire);
}

std::uint64_t aicaObservationDroppedCount() noexcept
{
	return droppedCount.load(std::memory_order_acquire);
}

bool aicaObservationDmaActive() noexcept
{
	const std::lock_guard<std::mutex> lock(stateMutex);
	return dmaState.active;
}

void restoreAicaCddaGeneration(std::uint64_t generation) noexcept
{
	reserveCddaGeneration(generation);
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
		const std::uint8_t* channelRegisters, std::uint64_t tick) noexcept
{
	if (!aicaObservationBusActive())
		return;
	AicaObservation observation;
	observation.type = keyOn ? AicaObservationType::KeyOn
			: AicaObservationType::KeyOff;
	observation.tick = tick;
	observation.owner = currentOwner(currentWriter);
	if (observation.owner.sh4.valid)
		observation.tick = std::max(observation.tick, observation.owner.sh4.tick);
	observation.channel = channel;
	std::copy(channelRegisters, channelRegisters + observation.channelRegisters.size(),
			observation.channelRegisters.begin());
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
		std::int32_t dspContributionRight, std::int16_t finalLeft,
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

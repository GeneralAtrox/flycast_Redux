#include "research/sh4_lua_subscriptions.h"
#include "research/sh4_observation_trace.h"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{

class NativeSubscription
{
public:
	explicit NativeSubscription(research::Sh4ObservationSubscription token = 0)
		: token(token)
	{
	}

	~NativeSubscription()
	{
		if (token != 0)
			research::unsubscribeSh4Observations(token);
	}

	NativeSubscription(const NativeSubscription&) = delete;
	NativeSubscription& operator=(const NativeSubscription&) = delete;

private:
	research::Sh4ObservationSubscription token;
};

class TemporaryDirectory
{
public:
	TemporaryDirectory()
	{
		static std::atomic<unsigned> sequence {0};
		path = std::filesystem::temp_directory_path()
				/ ("flycast-sh4-lua-subscription-test-"
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

research::Sh4Observation instruction(std::uint32_t pc)
{
	research::Sh4Observation observation;
	observation.backend = research::Sh4ObservationBackend::Interpreter;
	observation.type = research::Sh4ObservationType::InstructionEnd;
	observation.tick = pc;
	observation.instructionPc = pc;
	observation.opcode = 0x0009;
	return observation;
}

research::Sh4Observation memory(research::Sh4ObservationType type,
		std::uint32_t address, std::uint8_t width, std::uint64_t value)
{
	research::Sh4Observation observation = instruction(0x8c010000);
	observation.type = type;
	observation.memoryAddress = address;
	observation.memoryWidth = width;
	observation.memoryValue = value;
	return observation;
}

research::Sh4ObservationFilter interpreterInstructions()
{
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(
			research::Sh4ObservationBackend::Interpreter);
	filter.typeMask = research::sh4ObservationTypeBit(
			research::Sh4ObservationType::InstructionEnd);
	return filter;
}

std::vector<std::uint8_t> recordInstructionPair(
		const std::filesystem::path& path, bool withDiscoverySubscriber)
{
	research::Sh4ObservationTraceBinding binding;
	binding.backend = research::Sh4ObservationBackend::Interpreter;
	research::Sh4ObservationTraceWriter writer(path, binding);
	{
		research::Sh4ObservationFilter recorderFilter;
		recorderFilter.backendMask = research::sh4ObservationBackendBit(
				research::Sh4ObservationBackend::Interpreter);
		NativeSubscription recorder(research::subscribeSh4Observations(
				recorderFilter,
				[&writer](const research::Sh4Observation& observation) {
					writer.write(observation);
				}));
		std::unique_ptr<research::Sh4LuaSubscriptionQueue> discovery;
		if (withDiscoverySubscriber)
		{
			discovery = std::make_unique<research::Sh4LuaSubscriptionQueue>();
			discovery->subscribe(interpreterInstructions(),
					[](research::Sh4LuaSubscriptionQueue::Token,
							const research::Sh4Observation&) {});
		}

		research::Sh4Observation begin = instruction(0x8c010000);
		begin.type = research::Sh4ObservationType::InstructionBegin;
		begin.availableFields = research::Sh4Observation::HasNextPc
				| research::Sh4Observation::HasRegisters;
		begin.nextPc = 0x8c010002;
		research::publishSh4Observation(begin);
	research::Sh4Observation end = begin;
	end.type = research::Sh4ObservationType::InstructionEnd;
	end.tick = begin.tick + 1;
		research::publishSh4Observation(end);
		if (discovery != nullptr)
			EXPECT_EQ(1u, discovery->pendingCount());
	}
	writer.finalize();
	std::ifstream input(path, std::ios::binary);
	return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>());
}

TEST(ResearchSh4LuaSubscriptions, NoSubscriberLeavesCanonicalFastPathInactive)
{
	ASSERT_EQ(0u, research::sh4ObservationSubscriberCount());
	research::Sh4LuaSubscriptionQueue queue;
	EXPECT_EQ(0u, queue.subscriptionCount());
	EXPECT_EQ(0u, queue.pendingCount());
	EXPECT_FALSE(research::publishSh4Observation(instruction(0x8c010000)));
	EXPECT_FALSE(research::sh4ObservationBusActive());
}

TEST(ResearchSh4LuaSubscriptions, FiltersBeforeQueueAndPreservesCanonicalCopy)
{
	std::vector<research::Sh4Observation> canonical;
	NativeSubscription recorder(research::subscribeSh4Observations(
			research::Sh4ObservationFilter {},
			[&canonical](const research::Sh4Observation& observation) {
				canonical.push_back(observation);
			}));

	research::Sh4LuaSubscriptionQueue queue;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(
			research::Sh4ObservationBackend::Interpreter);
	filter.typeMask = research::sh4ObservationTypeBit(
			research::Sh4ObservationType::MemoryWrite);
	filter.hasMemoryRange = true;
	filter.memoryStart = 0x2000;
	filter.memoryEndExclusive = 0x2010;
	std::vector<research::Sh4Observation> delivered;
	queue.subscribe(filter,
			[&delivered](research::Sh4LuaSubscriptionQueue::Token,
					const research::Sh4Observation& observation) {
				delivered.push_back(observation);
			});

	research::publishSh4Observation(memory(
			research::Sh4ObservationType::MemoryRead, 0x2000, 4, 1));
	research::publishSh4Observation(memory(
			research::Sh4ObservationType::MemoryWrite, 0x3000, 4, 2));
	research::Sh4Observation source = memory(
			research::Sh4ObservationType::MemoryWrite, 0x1ffe, 4,
			0x1122334455667788ull);
	research::publishSh4Observation(source);
	source.memoryValue = 0;

	ASSERT_EQ(1u, queue.pendingCount());
	ASSERT_EQ(1u, queue.drain());
	ASSERT_EQ(1u, delivered.size());
	ASSERT_EQ(3u, canonical.size());
	EXPECT_EQ(canonical.back().emissionOrdinal, delivered[0].emissionOrdinal);
	EXPECT_EQ(0x1122334455667788ull, delivered[0].memoryValue);
}

TEST(ResearchSh4LuaSubscriptions, CombinesMemoryAndInstructionPcFiltersBeforeQueue)
{
	research::Sh4LuaSubscriptionQueue queue;
	research::Sh4ObservationFilter filter;
	filter.typeMask = research::sh4ObservationTypeBit(
			research::Sh4ObservationType::MemoryWrite);
	filter.hasInstructionPcRange = true;
	filter.instructionPcStart = 0x8c010100;
	filter.instructionPcEndExclusive = 0x8c010200;
	filter.hasMemoryRange = true;
	filter.memoryStart = 0x2000;
	filter.memoryEndExclusive = 0x2010;
	std::vector<research::Sh4Observation> delivered;
	queue.subscribe(filter,
			[&delivered](research::Sh4LuaSubscriptionQueue::Token,
					const research::Sh4Observation& observation) {
				delivered.push_back(observation);
			});

	research::Sh4Observation wrongPc = memory(
			research::Sh4ObservationType::MemoryWrite, 0x2000, 4, 1);
	wrongPc.instructionPc = 0x8c010000;
	research::publishSh4Observation(wrongPc);
	research::Sh4Observation wrongAddress = memory(
			research::Sh4ObservationType::MemoryWrite, 0x3000, 4, 2);
	wrongAddress.instructionPc = 0x8c010100;
	research::publishSh4Observation(wrongAddress);
	research::Sh4Observation match = memory(
			research::Sh4ObservationType::MemoryWrite, 0x200e, 4, 3);
	match.instructionPc = 0x8c0101fe;
	research::publishSh4Observation(match);

	ASSERT_EQ(1u, queue.pendingCount());
	ASSERT_EQ(1u, queue.drain());
	ASSERT_EQ(1u, delivered.size());
	EXPECT_EQ(0x8c0101feu, delivered[0].instructionPc);
	EXPECT_EQ(0x200eu, delivered[0].memoryAddress);
}

TEST(ResearchSh4LuaSubscriptions,
		DiscoverySubscriberCannotChangeNativeTraceBytes)
{
	TemporaryDirectory directory;
	const std::vector<std::uint8_t> withoutDiscovery = recordInstructionPair(
			directory.file("without-discovery.fcso"), false);
	const std::vector<std::uint8_t> withDiscovery = recordInstructionPair(
			directory.file("with-discovery.fcso"), true);
	EXPECT_EQ(withoutDiscovery, withDiscovery);
}

TEST(ResearchSh4LuaSubscriptions, OverflowDropsNewestAndReportsStablePrefix)
{
	research::Sh4LuaSubscriptionQueue queue;
	std::vector<std::uint32_t> deliveredPcs;
	const auto token = queue.subscribe(interpreterInstructions(),
			[&deliveredPcs](research::Sh4LuaSubscriptionQueue::Token,
					const research::Sh4Observation& observation) {
				deliveredPcs.push_back(observation.instructionPc);
			}, 2);
	for (std::uint32_t index = 0; index < 4; ++index)
		research::publishSh4Observation(instruction(0x8c010000 + index * 2));

	const auto before = queue.stats(token);
	ASSERT_TRUE(before.has_value());
	EXPECT_EQ(2u, before->capacity);
	EXPECT_EQ(2u, before->queued);
	EXPECT_EQ(2u, before->dropped);
	EXPECT_EQ(2u, queue.drain());
	EXPECT_EQ((std::vector<std::uint32_t> {0x8c010000, 0x8c010002}), deliveredPcs);
	const auto after = queue.stats(token);
	ASSERT_TRUE(after.has_value());
	EXPECT_EQ(0u, after->queued);
	EXPECT_EQ(2u, after->delivered);
}

TEST(ResearchSh4LuaSubscriptions, CallbackCanUnsubscribeAndCannotReenterDrain)
{
	research::Sh4LuaSubscriptionQueue queue;
	research::Sh4LuaSubscriptionQueue::Token token = 0;
	std::size_t callbackCount = 0;
	std::size_t nestedDeliveries = 99;
	token = queue.subscribe(interpreterInstructions(),
			[&](research::Sh4LuaSubscriptionQueue::Token callbackToken,
					const research::Sh4Observation&) {
				++callbackCount;
				nestedDeliveries = queue.drain();
				EXPECT_EQ(token, callbackToken);
				EXPECT_TRUE(queue.unsubscribe(callbackToken));
			});
	research::publishSh4Observation(instruction(0x8c010000));
	research::publishSh4Observation(instruction(0x8c010002));

	EXPECT_EQ(1u, queue.drain());
	EXPECT_EQ(1u, callbackCount);
	EXPECT_EQ(0u, nestedDeliveries);
	EXPECT_EQ(0u, queue.pendingCount());
	EXPECT_EQ(0u, queue.subscriptionCount());
	EXPECT_FALSE(queue.stats(token).has_value());
}

TEST(ResearchSh4LuaSubscriptions, CallbackFailureIsIsolatedPerSubscriber)
{
	research::Sh4LuaSubscriptionQueue queue;
	std::size_t laterCalls = 0;
	std::size_t reportedFailures = 0;
	const auto failing = queue.subscribe(interpreterInstructions(),
			[](research::Sh4LuaSubscriptionQueue::Token,
					const research::Sh4Observation&) {
				throw std::runtime_error("fixture callback failure");
			}, research::Sh4LuaSubscriptionQueue::DefaultCapacity,
			[&reportedFailures](research::Sh4LuaSubscriptionQueue::Token,
					std::exception_ptr) {
				++reportedFailures;
			});
	queue.subscribe(interpreterInstructions(),
			[&laterCalls](research::Sh4LuaSubscriptionQueue::Token,
					const research::Sh4Observation&) {
				++laterCalls;
			});
	research::publishSh4Observation(instruction(0x8c010000));

	EXPECT_EQ(2u, queue.drain());
	EXPECT_EQ(1u, laterCalls);
	EXPECT_EQ(1u, reportedFailures);
	const auto stats = queue.stats(failing);
	ASSERT_TRUE(stats.has_value());
	EXPECT_EQ(1u, stats->callbackFailures);
	EXPECT_EQ(1u, stats->delivered);
}

TEST(ResearchSh4LuaSubscriptions, ClearDropsQueuedWorkAndDetachesNativeBus)
{
	ASSERT_EQ(0u, research::sh4ObservationSubscriberCount());
	research::Sh4LuaSubscriptionQueue queue;
	queue.subscribe(interpreterInstructions(),
			[](research::Sh4LuaSubscriptionQueue::Token,
					const research::Sh4Observation&) {});
	research::publishSh4Observation(instruction(0x8c010000));
	ASSERT_EQ(1u, queue.pendingCount());
	queue.clear();
	EXPECT_EQ(0u, queue.pendingCount());
	EXPECT_EQ(0u, queue.subscriptionCount());
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
	EXPECT_EQ(0u, queue.drain());
	EXPECT_FALSE(research::publishSh4Observation(instruction(0x8c010002)));
}

TEST(ResearchSh4LuaSubscriptions, DeliveryRejectsNonOwningThread)
{
	research::Sh4LuaSubscriptionQueue queue;
	queue.subscribe(interpreterInstructions(),
			[](research::Sh4LuaSubscriptionQueue::Token,
					const research::Sh4Observation&) {});
	research::publishSh4Observation(instruction(0x8c010000));
	std::atomic<bool> rejected {false};
	std::thread other([&] {
		try
		{
			queue.drain();
		}
		catch (const std::logic_error&)
		{
			rejected.store(true, std::memory_order_release);
		}
	});
	other.join();
	EXPECT_TRUE(rejected.load(std::memory_order_acquire));
	EXPECT_EQ(1u, queue.drain());
}

TEST(ResearchSh4LuaSubscriptions, ConcurrentShutdownCannotQueueAfterDetach)
{
	ASSERT_EQ(0u, research::sh4ObservationSubscriberCount());
	research::Sh4LuaSubscriptionQueue queue;
	queue.subscribe(interpreterInstructions(),
			[](research::Sh4LuaSubscriptionQueue::Token,
					const research::Sh4Observation&) {});
	std::atomic<bool> run {true};
	std::atomic<std::uint32_t> published {0};
	std::thread producer([&] {
		while (run.load(std::memory_order_acquire))
		{
			research::publishSh4Observation(instruction(
					0x8c010000 + (published.fetch_add(1,
							std::memory_order_relaxed) & 0xffu) * 2));
		}
	});
	while (published.load(std::memory_order_acquire) < 100)
		std::this_thread::yield();
	queue.clear();
	run.store(false, std::memory_order_release);
	producer.join();

	EXPECT_EQ(0u, queue.subscriptionCount());
	EXPECT_EQ(0u, queue.pendingCount());
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
	EXPECT_FALSE(research::publishSh4Observation(instruction(0x8c020000)));
}

TEST(ResearchSh4LuaSubscriptions, SharesTokensBoundsAndOrderWithMaple)
{
	ASSERT_EQ(0u, research::sh4ObservationSubscriberCount());
	ASSERT_EQ(0u, research::mapleObservationSubscriberCount());
	research::Sh4LuaSubscriptionQueue queue;
	std::vector<std::string> delivered;
	const auto sh4Token = queue.subscribe(interpreterInstructions(),
			[&delivered](research::Sh4LuaSubscriptionQueue::Token,
					const research::Sh4Observation&) {
				delivered.emplace_back("sh4");
			});
	research::MapleObservationFilter mapleFilter;
	mapleFilter.typeMask = research::mapleObservationTypeBit(
			research::MapleObservationType::Response);
	const auto mapleToken = queue.subscribe(mapleFilter,
			[&delivered](research::Sh4LuaSubscriptionQueue::Token,
					const research::MapleObservation&) {
				delivered.emplace_back("maple-response");
			});
	EXPECT_NE(sh4Token, mapleToken);
	EXPECT_EQ(2u, queue.subscriptionCount());

	research::publishSh4Observation(instruction(0x8c010000));
	research::MapleTransactionEvent transaction;
	transaction.tick = 77;
	transaction.deviceType = 1;
	transaction.bus = 0;
	transaction.port = 5;
	transaction.command = 9;
	transaction.flags = research::MapleTransactionDevicePresent;
	transaction.request = {9, 0x20, 0x01, 0};
	transaction.response = {8, 0x01, 0x20, 0};
	research::publishMapleTransactionObservations(
			research::beginMapleObservationDma(), transaction);
	research::publishSh4Observation(instruction(0x8c010002));

	ASSERT_EQ(3u, queue.pendingCount());
	EXPECT_EQ(3u, queue.drain());
	EXPECT_EQ((std::vector<std::string> {"sh4", "maple-response", "sh4"}),
			delivered);
	queue.clear();
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
	EXPECT_EQ(0u, research::mapleObservationSubscriberCount());
}

TEST(ResearchSh4LuaSubscriptions, MapleOverflowDropsNewestAndReportsStablePrefix)
{
	research::Sh4LuaSubscriptionQueue queue;
	std::vector<std::uint8_t> deliveredCodes;
	research::MapleObservationFilter filter;
	filter.typeMask = research::mapleObservationTypeBit(
			research::MapleObservationType::Response);
	const auto token = queue.subscribe(filter,
			[&deliveredCodes](research::Sh4LuaSubscriptionQueue::Token,
					const research::MapleObservation& observation) {
				deliveredCodes.push_back(observation.payload[0]);
			}, 1);
	research::MapleTransactionEvent first;
	first.deviceType = 1;
	first.command = 9;
	first.flags = research::MapleTransactionDevicePresent;
	first.request = {9, 0x20, 0x01, 0};
	first.response = {8, 0x01, 0x20, 0};
	research::publishMapleTransactionObservations(
			research::beginMapleObservationDma(), first);
	research::MapleTransactionEvent second = first;
	second.response[0] = 7;
	research::publishMapleTransactionObservations(
			research::beginMapleObservationDma(), second);

	const auto stats = queue.stats(token);
	ASSERT_TRUE(stats.has_value());
	EXPECT_EQ(1u, stats->queued);
	EXPECT_EQ(1u, stats->dropped);
	EXPECT_EQ(1u, queue.drain());
	EXPECT_EQ((std::vector<std::uint8_t> {8}), deliveredCodes);
}

TEST(ResearchSh4LuaSubscriptions, FiltersAndOrdersPvrGdromCddaAndAicaBuses)
{
	research::Sh4LuaSubscriptionQueue queue;
	std::vector<std::string> delivered;
	research::PvrTaObservationFilter ta;
	ta.typeMask = research::pvrTaObservationTypeBit(
			research::PvrTaObservationType::ListInit);
	queue.subscribe(ta, [&](auto, const research::PvrTaObservation&) {
		delivered.emplace_back("ta");
	});
	research::Sh4LuaSubscriptionQueue::PvrPresentationFilter presentation;
	presentation.typeMask = 1;
	presentation.hasAddressRange = true;
	presentation.addressStart = 0x005f8000;
	presentation.addressEndExclusive = 0x005f8100;
	queue.subscribe(presentation,
			[&](auto, const research::PvrPresentationObservation&) {
				delivered.emplace_back("presentation");
			});
	research::Sh4LuaSubscriptionQueue::PvrDrawFilter draw;
	draw.typeMask = std::uint32_t {1}
			<< (static_cast<unsigned>(research::PvrDrawObservationType::RenderCompleted) - 1u);
	draw.hasRenderGeneration = true;
	draw.renderGeneration = 9;
	queue.subscribe(draw, [&](auto, const research::PvrDrawObservation&) {
		delivered.emplace_back("draw");
	});
	research::Sh4LuaSubscriptionQueue::GdromFilter gdrom;
	gdrom.typeMask = 1;
	queue.subscribe(gdrom, [&](auto, const research::GdromObservation&) {
		delivered.emplace_back("gdrom");
	});
	research::Sh4LuaSubscriptionQueue::CddaFilter cdda;
	cdda.typeMask = std::uint32_t {1}
			<< (static_cast<unsigned>(research::CddaObservationType::ControlApplied) - 1u);
	cdda.hasCommand = true;
	cdda.command = 0x15;
	queue.subscribe(cdda, [&](auto, const research::CddaObservation&) {
		delivered.emplace_back("cdda");
	});
	research::Sh4LuaSubscriptionQueue::AicaFilter aica;
	aica.typeMask = 1;
	aica.hasWriter = true;
	aica.writerMask = 1;
	aica.hasAddressRange = true;
	aica.addressStart = 0x100;
	aica.addressEndExclusive = 0x110;
	queue.subscribe(aica, [&](auto, const research::AicaObservation&) {
		delivered.emplace_back("aica");
	});

	research::observePvrTaListBoundary(false, 0x1000, 0, 1);
	research::observePvrRegisterWrite(0x005f8004, 4, 1, 0, 1,
			research::PvrRegisterWriteDisposition::Stored, 0, 2);
	research::observePvrDrawRenderCompleted(8, true, 3);
	research::observePvrDrawRenderCompleted(9, true, 4);
	const std::uint32_t parameters[4] {45150, 1, 0x0c100000, 0};
	research::observeReiosGdromCommand(41, 0x11, parameters, 5);
	research::observeReiosGdromAbort(41, 6);
	research::resetCddaObservation(6);
	const std::uint32_t playParameters[4] {600, 601, 0, 0};
	research::observeReiosCddaControlAccepted(42, 0x15, playParameters, 7);
	research::CddaDriveState before;
	research::CddaDriveState after;
	after.status = 1;
	after.currentFad = after.startFad = 600;
	after.endFad = 601;
	research::observeReiosCddaControlApplied(42, 0x15, before, after, true, 8);
	research::observeAicaRegisterWrite(research::AicaWriter::Arm7,
			0x104, 2, 1, 9);
	research::observeAicaRegisterWrite(research::AicaWriter::Sh4Direct,
			0x104, 2, 2, 10);

	ASSERT_EQ(6u, queue.pendingCount());
	EXPECT_EQ(6u, queue.drain());
	EXPECT_EQ((std::vector<std::string> {"ta", "presentation", "draw",
			"gdrom", "cdda", "aica"}), delivered);
}

TEST(ResearchSh4LuaSubscriptions, FiltersSuccessfulCddaAndNonzeroMixerContributionNatively)
{
	research::Sh4LuaSubscriptionQueue queue;
	std::vector<std::string> delivered;
	research::Sh4LuaSubscriptionQueue::CddaFilter sector;
	sector.typeMask = std::uint32_t {1}
			<< (static_cast<unsigned>(research::CddaObservationType::Sector) - 1u);
	sector.hasSuccessful = true;
	sector.successful = true;
	queue.subscribe(sector, [&](auto, const research::CddaObservation&) {
		delivered.emplace_back("sector");
	});
	research::Sh4LuaSubscriptionQueue::AicaFilter sample;
	sample.typeMask = std::uint32_t {1}
			<< (static_cast<unsigned>(research::AicaObservationType::SampleFrame) - 1u);
	sample.requireNonzeroCddaContribution = true;
	queue.subscribe(sample, [&](auto, const research::AicaObservation&) {
		delivered.emplace_back("sample");
	});

	const std::uint8_t bytes[4] {0, 0, 1, 0};
	research::CddaDriveState drive;
	research::observeCddaSector(1, 600, drive, drive, false, bytes, sizeof(bytes), 1);
	research::observeCddaSector(2, 601, drive, drive, true, bytes, sizeof(bytes), 2);
	research::observeAicaSampleFrame(0, 0, 0, 0, 0, 0, 0, false,
			0, 0, 0, 0, 2, 0, 3);
	research::observeAicaSampleFrame(0, 0, 0, 1, 0, 1, 0, false,
			0, 0, 1, 0, 2, 1, 4);

	ASSERT_EQ(2u, queue.pendingCount());
	EXPECT_EQ(2u, queue.drain());
	EXPECT_EQ((std::vector<std::string> {"sector", "sample"}), delivered);
}

} // namespace

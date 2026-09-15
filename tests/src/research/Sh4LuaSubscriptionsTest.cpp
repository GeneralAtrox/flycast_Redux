#include "Sh4LuaSubscriptionsTestSupport.h"

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

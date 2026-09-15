#include "Sh4ObservationTestSupport.h"

TEST(ResearchSh4Observation, NoSubscriberFastPathDoesNotDeliver)
{
	ASSERT_EQ(0u, research::sh4ObservationSubscriberCount());
	EXPECT_FALSE(research::sh4ObservationBusActive());
	research::Sh4Observation observation;
	EXPECT_FALSE(research::publishSh4Observation(observation));
	research::Sh4Observation invalid = memoryObservation(
			research::Sh4ObservationType::MemoryRead, 0x8c000000, 3, 0);
	EXPECT_THROW(research::publishSh4Observation(invalid), std::invalid_argument);
}

TEST(ResearchSh4Observation, FiltersMemoryDirectionAndOverlappingRange)
{
	std::vector<research::Sh4Observation> all;
	std::vector<research::Sh4Observation> filtered;
	ObservationSubscription allSubscription(research::subscribeSh4Observations(
			research::Sh4ObservationFilter {},
			[&all](const research::Sh4Observation& observation) {
				all.push_back(observation);
			}));
	research::Sh4ObservationFilter filter;
	filter.typeMask = research::sh4ObservationTypeBit(
			research::Sh4ObservationType::MemoryWrite);
	filter.hasMemoryRange = true;
	filter.memoryStart = 0x8c002000;
	filter.memoryEndExclusive = 0x8c002010;
	ObservationSubscription filteredSubscription(research::subscribeSh4Observations(filter,
			[&filtered](const research::Sh4Observation& observation) {
				filtered.push_back(observation);
			}));

	research::Sh4Observation instruction;
	instruction.type = research::Sh4ObservationType::InstructionBegin;
	EXPECT_TRUE(research::publishSh4Observation(instruction));
	EXPECT_TRUE(research::publishSh4Observation(memoryObservation(
			research::Sh4ObservationType::MemoryWrite, 0x8c001ffc, 4, 1)));
	EXPECT_TRUE(research::publishSh4Observation(memoryObservation(
			research::Sh4ObservationType::MemoryRead, 0x8c002000, 4, 2)));
	EXPECT_TRUE(research::publishSh4Observation(memoryObservation(
			research::Sh4ObservationType::MemoryWrite, 0x8c00200e, 4, 3)));

	ASSERT_EQ(4u, all.size());
	for (std::size_t index = 1; index < all.size(); ++index)
		EXPECT_EQ(all[index - 1].emissionOrdinal + 1, all[index].emissionOrdinal);
	ASSERT_EQ(1u, filtered.size());
	EXPECT_EQ(0x8c00200eu, filtered[0].memoryAddress);
	EXPECT_EQ(3u, filtered[0].memoryValue);
}

TEST(ResearchSh4Observation, FiltersCanonicalInstructionOwnerPc)
{
	std::vector<research::Sh4Observation> filtered;
	research::Sh4ObservationFilter filter;
	filter.hasInstructionPcRange = true;
	filter.instructionPcStart = 0x8c010100;
	filter.instructionPcEndExclusive = 0x8c010200;
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&filtered](const research::Sh4Observation& observation) {
				filtered.push_back(observation);
			}));

	research::Sh4Observation before;
	before.instructionPc = 0x8c0100fe;
	EXPECT_FALSE(research::publishSh4Observation(before));
	research::Sh4Observation first = before;
	first.instructionPc = 0x8c010100;
	EXPECT_TRUE(research::publishSh4Observation(first));
	research::Sh4Observation last = before;
	last.instructionPc = 0x8c0101fe;
	EXPECT_TRUE(research::publishSh4Observation(last));
	research::Sh4Observation after = before;
	after.instructionPc = 0x8c010200;
	EXPECT_FALSE(research::publishSh4Observation(after));

	ASSERT_EQ(2u, filtered.size());
	EXPECT_EQ(0x8c010100u, filtered[0].instructionPc);
	EXPECT_EQ(0x8c0101feu, filtered[1].instructionPc);
}

TEST(ResearchSh4Observation, CallbackCanUnsubscribeItself)
{
	std::size_t calls = 0;
	research::Sh4ObservationSubscription id = 0;
	id = research::subscribeSh4Observations(research::Sh4ObservationFilter {},
			[&](const research::Sh4Observation&) {
				++calls;
				EXPECT_TRUE(research::unsubscribeSh4Observations(id));
			});
	research::Sh4Observation observation;
	EXPECT_TRUE(research::publishSh4Observation(observation));
	EXPECT_EQ(1u, calls);
	EXPECT_FALSE(research::publishSh4Observation(observation));
	EXPECT_FALSE(research::unsubscribeSh4Observations(id));
}

TEST(ResearchSh4Observation, BackendFilterKeepsInterpreterRecorderIndependent)
{
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(
			research::Sh4ObservationBackend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&observed](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	research::Sh4Observation interpreter;
	EXPECT_FALSE(research::publishSh4Observation(interpreter));
	research::Sh4Observation dynarec;
	dynarec.backend = research::Sh4ObservationBackend::Dynarec;
	EXPECT_TRUE(research::publishSh4Observation(dynarec));
	EXPECT_FALSE(research::publishSh4Observation(interpreter));
	EXPECT_TRUE(research::publishSh4Observation(dynarec));
	ASSERT_EQ(2u, observed.size());
	EXPECT_EQ(research::Sh4ObservationBackend::Dynarec, observed[0].backend);
	EXPECT_EQ(observed[0].emissionOrdinal + 1, observed[1].emissionOrdinal);
}

TEST(ResearchSh4Observation, TracksBackendSpecificActivity)
{
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount(
			research::Sh4ObservationBackend::Interpreter));
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount(
			research::Sh4ObservationBackend::Dynarec));
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(
			research::Sh4ObservationBackend::Dynarec);
	{
		ObservationSubscription subscription(research::subscribeSh4Observations(filter,
				[](const research::Sh4Observation&) {}));
		EXPECT_FALSE(research::sh4ObservationBusActive(
				research::Sh4ObservationBackend::Interpreter));
		EXPECT_TRUE(research::sh4ObservationBusActive(
				research::Sh4ObservationBackend::Dynarec));
		EXPECT_EQ(0u, research::sh4ObservationSubscriberCount(
				research::Sh4ObservationBackend::Interpreter));
		EXPECT_EQ(1u, research::sh4ObservationSubscriberCount(
				research::Sh4ObservationBackend::Dynarec));
	}
	EXPECT_FALSE(research::sh4ObservationBusActive(
			research::Sh4ObservationBackend::Dynarec));
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount(
			research::Sh4ObservationBackend::Dynarec));
}

TEST(ResearchSh4Observation, SubscriberFailureDoesNotSkipLaterSubscribers)
{
	std::size_t laterCalls = 0;
	ObservationSubscription failing(research::subscribeSh4Observations(
			research::Sh4ObservationFilter {}, [](const research::Sh4Observation&) {
				throw std::runtime_error("fixture subscriber failure");
			}));
	ObservationSubscription later(research::subscribeSh4Observations(
			research::Sh4ObservationFilter {},
			[&laterCalls](const research::Sh4Observation&) { ++laterCalls; }));
	research::Sh4Observation observation;
	EXPECT_THROW(research::publishSh4Observation(observation), std::runtime_error);
	EXPECT_EQ(1u, laterCalls);
}

TEST(ResearchSh4Observation, RejectsInvalidSubscriptionContracts)
{
	research::Sh4ObservationFilter empty;
	empty.typeMask = 0;
	EXPECT_THROW(research::subscribeSh4Observations(empty,
			[](const research::Sh4Observation&) {}), std::invalid_argument);

	research::Sh4ObservationFilter wrapping;
	wrapping.hasMemoryRange = true;
	wrapping.memoryStart = 0x8c002000;
	wrapping.memoryEndExclusive = 0x8c002000;
	EXPECT_THROW(research::subscribeSh4Observations(wrapping,
			[](const research::Sh4Observation&) {}), std::invalid_argument);

	research::Sh4ObservationFilter emptyPc;
	emptyPc.hasInstructionPcRange = true;
	emptyPc.instructionPcStart = 0x8c010000;
	emptyPc.instructionPcEndExclusive = 0x8c010000;
	EXPECT_THROW(research::subscribeSh4Observations(emptyPc,
			[](const research::Sh4Observation&) {}), std::invalid_argument);

	EXPECT_THROW(research::subscribeSh4Observations(research::Sh4ObservationFilter {}, {}),
			std::invalid_argument);

	research::Sh4ObservationFilter backend;
	backend.backendMask = 0;
	EXPECT_THROW(research::subscribeSh4Observations(backend,
			[](const research::Sh4Observation&) {}), std::invalid_argument);
}

TEST(ResearchSh4Observation, ConcurrentPublishIsDeliveredInOrdinalOrder)
{
	std::vector<std::uint64_t> ordinals;
	ObservationSubscription subscription(research::subscribeSh4Observations(
			research::Sh4ObservationFilter {},
			[&ordinals](const research::Sh4Observation& observation) {
				ordinals.push_back(observation.emissionOrdinal);
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}));
	auto first = std::async(std::launch::async, [] {
		research::Sh4Observation observation;
		research::publishSh4Observation(observation);
	});
	auto second = std::async(std::launch::async, [] {
		research::Sh4Observation observation;
		research::publishSh4Observation(observation);
	});
	first.get();
	second.get();
	ASSERT_EQ(2u, ordinals.size());
	EXPECT_EQ(ordinals[0] + 1, ordinals[1]);
}

TEST(ResearchSh4Observation, CrossThreadUnsubscribeWaitsForInflightCallback)
{
	std::promise<void> enteredPromise;
	auto entered = enteredPromise.get_future();
	std::promise<void> releasePromise;
	auto release = releasePromise.get_future().share();
	const research::Sh4ObservationSubscription subscription =
			research::subscribeSh4Observations(research::Sh4ObservationFilter {},
					[&](const research::Sh4Observation&) {
						enteredPromise.set_value();
						release.wait();
					});
	auto publisher = std::async(std::launch::async, [] {
		research::Sh4Observation observation;
		research::publishSh4Observation(observation);
	});
	ASSERT_EQ(std::future_status::ready, entered.wait_for(std::chrono::seconds(2)));
	auto unsubscribe = std::async(std::launch::async, [subscription] {
		return research::unsubscribeSh4Observations(subscription);
	});
	EXPECT_EQ(std::future_status::timeout,
			unsubscribe.wait_for(std::chrono::milliseconds(20)));
	releasePromise.set_value();
	EXPECT_NO_THROW(publisher.get());
	ASSERT_EQ(std::future_status::ready,
			unsubscribe.wait_for(std::chrono::seconds(2)));
	EXPECT_TRUE(unsubscribe.get());
}

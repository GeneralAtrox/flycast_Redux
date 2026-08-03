#include "research/sh4_observation.h"
#include "research/sh4_observation_runtime.h"
#include "research/sh4_events_runtime.h"
#include "cfg/option.h"
#include "hw/sh4/sh4_cycles.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/dyna/shil.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{

class ObservationSubscription
{
public:
	ObservationSubscription() = default;
	explicit ObservationSubscription(research::Sh4ObservationSubscription id)
		: id(id) {}
	~ObservationSubscription()
	{
		research::unsubscribeSh4Observations(id);
	}

	ObservationSubscription(const ObservationSubscription&) = delete;
	ObservationSubscription& operator=(const ObservationSubscription&) = delete;

	research::Sh4ObservationSubscription get() const { return id; }

private:
	research::Sh4ObservationSubscription id = 0;
};

research::Sh4Observation memoryObservation(research::Sh4ObservationType type,
		std::uint32_t address, std::uint8_t width, std::uint64_t value)
{
	research::Sh4Observation observation;
	observation.type = type;
	observation.instructionPc = 0x8c010000;
	observation.memoryAddress = address;
	observation.memoryWidth = width;
	observation.memoryValue = value;
	return observation;
}

void invokeDynarecMarker(Sh4Context& context, const shil_opcode& marker)
{
	const auto function = research::sh4DynarecObservationMarkerFor(marker.op);
	ASSERT_NE(nullptr, function);
	ASSERT_LE(marker.size, 0xffffu);
	function(&context, marker.rs1._imm,
			marker.rs2._imm | (marker.size << 16), marker.rs3._imm);
}

} // namespace

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

TEST(ResearchSh4Observation, InterpreterPublishesCanonicalCallAndReturnOrdering)
{
	std::vector<research::Sh4Observation> observed;
	ObservationSubscription subscription(research::subscribeSh4Observations(
			research::Sh4ObservationFilter {},
			[&observed](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.pc = 0x8c020002;
	context.r[1] = 0x8c010100;
	context.pr = 0x8c030000;
	research::sh4EventsInstructionBegin(0x8c020000, 0x410b, 100, context);
	context.pc = 0x8c010100;
	research::sh4EventsInstructionEnd(0x8c020000, 0x410b, 104, context);

	context.pc = 0x8c010112;
	context.pr = 0x8c020004;
	research::sh4EventsInstructionBegin(0x8c010110, 0x000b, 120, context);
	context.pc = 0x8c020004;
	research::sh4EventsInstructionEnd(0x8c010110, 0x000b, 124, context);
	context.pc = 0x8c030002;
	research::sh4EventsInstructionBegin(0x8c030000, 0x0009, 130, context);
	research::sh4EventsInstructionAbort();

	ASSERT_EQ(8u, observed.size());
	EXPECT_EQ(research::Sh4ObservationType::InstructionBegin, observed[0].type);
	EXPECT_EQ(research::Sh4ObservationType::Call, observed[1].type);
	EXPECT_EQ(research::Sh4CallKind::Jsr, observed[1].callKind);
	EXPECT_EQ(0x8c010100u, observed[1].targetPc);
	EXPECT_EQ(0x8c020004u, observed[1].returnPc);
	EXPECT_EQ(0x8c020002u, observed[1].delaySlotPc);
	EXPECT_EQ(research::Sh4ObservationType::InstructionEnd, observed[2].type);
	EXPECT_EQ(research::Sh4ObservationType::InstructionBegin, observed[3].type);
	EXPECT_EQ(research::Sh4ObservationType::Return, observed[4].type);
	EXPECT_EQ(0x8c020004u, observed[4].targetPc);
	EXPECT_EQ(0x8c020004u, observed[4].returnPc);
	EXPECT_EQ(0x8c010112u, observed[4].delaySlotPc);
	EXPECT_EQ(research::Sh4ObservationType::InstructionEnd, observed[5].type);
	EXPECT_EQ(research::Sh4Observation::HasNextPc
			| research::Sh4Observation::HasRegisters, observed[6].availableFields);
	EXPECT_EQ(research::Sh4ObservationType::InstructionAbort, observed[7].type);
	EXPECT_EQ(0u, observed[7].availableFields);
	for (std::size_t index = 1; index < observed.size(); ++index)
		EXPECT_EQ(observed[index - 1].emissionOrdinal + 1,
				observed[index].emissionOrdinal);
}

TEST(ResearchSh4Observation, SharedRuntimePublishesDynarecInstructionOwnership)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&observed](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));

	Sh4Context context {};
	context.pc = 0x8c010002;
	context.r[1] = 0x8c020000;
	context.pr = 0x8c030000;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x410b, 100, context);
	context.pc = 0x8c010004;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010002,
			0x0009, 101, context);
	// Interpreter opcode handlers are also used as dynarec fallbacks. Their
	// legacy memory wrapper must inherit the owning dynarec frame.
	research::sh4EventsMemoryAccess(0x8c100000, 4,
			research::Sh4MemoryAccessKind::Read, 0x44332211);
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010002,
			0x0009, 102, context);
	context.pc = 0x8c020000;
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010000,
			0x410b, 103, context);

	ASSERT_EQ(6u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(Type::Call, observed[1].type);
	EXPECT_EQ(Type::InstructionBegin, observed[2].type);
	EXPECT_EQ(1u, observed[2].delaySlotDepth);
	EXPECT_EQ(Type::MemoryRead, observed[3].type);
	EXPECT_EQ(1u, observed[3].delaySlotDepth);
	EXPECT_EQ(Type::InstructionEnd, observed[4].type);
	EXPECT_EQ(Type::InstructionEnd, observed[5].type);
	EXPECT_EQ(Backend::Dynarec, observed[5].backend);
	EXPECT_EQ(0x8c020000u, observed[5].nextPc);
}

TEST(ResearchSh4Observation, InactiveBackendDoesNotCreateInstructionFrame)
{
	using Backend = research::Sh4ObservationBackend;
	ASSERT_FALSE(research::sh4ObservationBusActive(Backend::Dynarec));
	Sh4Context context {};
	context.pc = 0x8c010002;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x0009, 10, context);

	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&observed](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010000,
			0x0009, 11, context);
	EXPECT_TRUE(observed.empty());
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x0009, 12, context);
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010000,
			0x0009, 13, context);
	ASSERT_EQ(2u, observed.size());
	EXPECT_EQ(0u, observed[0].delaySlotDepth);
}

TEST(ResearchSh4Observation, SubscriptionChangeInsideNestedInstructionStaysBalanced)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	std::vector<research::Sh4Observation> firstObserved;
	research::Sh4ObservationSubscription firstSubscription = 0;
	firstSubscription = research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				firstObserved.push_back(observation);
				if (observation.type == Type::InstructionBegin
						&& observation.delaySlotDepth == 0)
					EXPECT_TRUE(research::unsubscribeSh4Observations(firstSubscription));
			});

	Sh4Context context {};
	context.pc = 0x8c010002;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x0009, 10, context);
	ASSERT_EQ(1u, firstObserved.size());

	std::vector<research::Sh4Observation> replacementObserved;
	ObservationSubscription replacement(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				replacementObserved.push_back(observation);
			}));
	context.pc = 0x8c010004;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010002,
			0x0009, 11, context);
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010002,
			0x0009, 12, context);
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010000,
			0x0009, 13, context);
	EXPECT_TRUE(replacementObserved.empty());

	context.pc = 0x8c020002;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c020000,
			0x0009, 20, context);
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c020000,
			0x0009, 21, context);
	ASSERT_EQ(2u, replacementObserved.size());
	EXPECT_EQ(0u, replacementObserved[0].delaySlotDepth);
	EXPECT_EQ(Type::InstructionEnd, replacementObserved[1].type);
}

TEST(ResearchSh4Observation, MismatchedAbortClearsEmissionFrames)
{
	using Backend = research::Sh4ObservationBackend;
	std::vector<research::Sh4Observation> observed;
	ObservationSubscription subscription(research::subscribeSh4Observations(
			research::Sh4ObservationFilter {},
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.pc = 0x8c010002;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x0009, 10, context);
	research::sh4ObservationInstructionAbort(Backend::Interpreter);
	context.pc = 0x8c020002;
	EXPECT_NO_THROW(research::sh4ObservationInstructionBegin(Backend::Interpreter,
			0x8c020000, 0x0009, 20, context));
	EXPECT_NO_THROW(research::sh4ObservationInstructionEnd(Backend::Interpreter,
			0x8c020000, 0x0009, 21, context));
	ASSERT_EQ(3u, observed.size());
	EXPECT_EQ(Backend::Dynarec, observed[0].backend);
	EXPECT_EQ(Backend::Interpreter, observed[1].backend);
	EXPECT_EQ(0u, observed[1].delaySlotDepth);
}

TEST(ResearchSh4Observation, DynarecExceptionOwnsAndClosesItsFrame)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&observed](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.pc = 0x8c010002;
	context.vbr = 0x8c000000;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x0009, 20, context);
	research::sh4ObservationException(Backend::Dynarec, 0x8c010000,
			0x8c000100, 0x160, 21, context);
	research::sh4ObservationInstructionAbort(Backend::Dynarec);
	ASSERT_EQ(3u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(Type::Exception, observed[1].type);
	EXPECT_EQ(Type::InstructionAbort, observed[2].type);
	EXPECT_EQ(0x8c010000u, observed[1].instructionPc);
}

TEST(ResearchSh4Observation, DynarecMarkersReconstructTickAndDynamicNextPc)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));

	Sh4Context context {};
	context.cycle_counter = 100;
	context.jdyn = 0x8c020000;
	context.pr = 0x8c030000;
	shil_opcode begin;
	begin.op = shop_research_begin;
	begin.rs1 = shil_param(0x8c010000);
	begin.rs2 = shil_param(0x000b);
	begin.size = 10;
	invokeDynarecMarker(context, begin);
	shil_opcode end;
	end.op = shop_research_end;
	end.rs1 = shil_param(0x8c010000);
	end.rs2 = shil_param(0x000b);
	end.rs3 = shil_param(0xffffffffu);
	end.size = 0;
	invokeDynarecMarker(context, end);

	ASSERT_EQ(3u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(1338u, observed[0].tick);
	EXPECT_EQ(0x8c010002u, observed[0].nextPc);
	EXPECT_EQ(Type::Return, observed[1].type);
	EXPECT_EQ(0x8c020000u, observed[1].targetPc);
	EXPECT_EQ(0x8c020000u, observed[2].nextPc);
}

TEST(ResearchSh4Observation,
		ResearchDynarecTimingChangesOnlyAtNativeCaptureBoundary)
{
	using Backend = research::Sh4ObservationBackend;
	config::ResearchDynarecObservation.override(true);
	research::sh4DynarecExecutionTimingReset();
	struct Cleanup
	{
		~Cleanup()
		{
			research::sh4ObservationResetPreciseTiming();
			research::sh4DynarecExecutionTimingReset();
			config::ResearchDynarecObservation.override(false);
		}
	} cleanup;
	Sh4Context context {};
	context.cycle_counter = 1000;

#ifdef STRICT_MODE
	Sh4Cycles warmupCycles {1};
#else
	Sh4Cycles warmupCycles {8};
#endif
	const int warmupDebit = warmupCycles.countCycles(0x0009);
	shil_opcode begin {};
	begin.op = shop_research_begin;
	begin.rs1 = shil_param(0x8c010000);
	begin.rs2 = shil_param(0x0009);
	shil_opcode end {};
	end.op = shop_research_end;
	end.rs1 = begin.rs1;
	end.rs2 = begin.rs2;
	end.rs3 = shil_param(0x8c010002);
	invokeDynarecMarker(context, begin);
	invokeDynarecMarker(context, end);
	EXPECT_EQ(1000 - warmupDebit, context.cycle_counter);

	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Cycles preciseCycles {1};
	const int preciseDebit = preciseCycles.countCycles(0x0009);
	begin.rs1 = shil_param(0x8c010002);
	end.rs1 = begin.rs1;
	end.rs3 = shil_param(0x8c010004);
	invokeDynarecMarker(context, begin);
	invokeDynarecMarker(context, end);
	EXPECT_EQ(1000 - 2 * warmupDebit, context.cycle_counter);
	ASSERT_EQ(2u, observed.size());
	EXPECT_EQ(research::Sh4ObservationType::InstructionBegin, observed[0].type);
	EXPECT_EQ(research::Sh4ObservationType::InstructionEnd, observed[1].type);

	research::sh4ObservationSetPreciseTiming(Backend::Dynarec, true);
	begin.rs1 = shil_param(0x8c010004);
	end.rs1 = begin.rs1;
	end.rs3 = shil_param(0x8c010006);
	invokeDynarecMarker(context, begin);
	invokeDynarecMarker(context, end);
	EXPECT_EQ(1000 - 2 * warmupDebit - preciseDebit, context.cycle_counter);

}

TEST(ResearchSh4Observation,
		DynarecBoundaryInterruptUsesTheActiveSemanticClock)
{
	using Backend = research::Sh4ObservationBackend;
	config::ResearchDynarecObservation.override(true);
	research::sh4DynarecExecutionTimingReset();
	struct Cleanup
	{
		~Cleanup()
		{
			research::sh4ObservationResetPreciseTiming();
			research::sh4DynarecExecutionTimingReset();
			config::ResearchDynarecObservation.override(false);
		}
	} cleanup;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	research::sh4ObservationSetPreciseTiming(Backend::Dynarec, true);
	Sh4Context context {};
	context.cycle_counter = 1000;
	context.vbr = 0x8c000000;
	shil_opcode begin {};
	begin.op = shop_research_begin;
	begin.rs1 = shil_param(0x8c010000);
	begin.rs2 = shil_param(0x0009);
	shil_opcode end {};
	end.op = shop_research_end;
	end.rs1 = begin.rs1;
	end.rs2 = begin.rs2;
	end.rs3 = shil_param(0x8c010002);
	invokeDynarecMarker(context, begin);
	invokeDynarecMarker(context, end);
	context.pc = 0x8c020000;
	research::sh4ObservationInterruptRaised(Backend::Dynarec, 0x320,
			observed.back().tick + 500, context);

	ASSERT_EQ(3u, observed.size());
	EXPECT_EQ(research::Sh4ObservationType::Exception, observed[2].type);
	EXPECT_EQ(observed[1].tick, observed[2].tick);
}

TEST(ResearchSh4Observation, DynarecMemoryMarkersPublishOnlyCompletedAccesses)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));

	Sh4Context context {};
	context.pc = 0x8c010002;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x6010, 100, context);
	research::sh4DynarecObservationMemoryBegin(0x8c020000, 1, 0);
	research::sh4DynarecObservationMemoryEnd(0, 0, 0xffffffffffffff80ull);
	research::sh4DynarecObservationMemoryBegin(0x8c020004, 0x100u | 2u,
			0x12345678u);
	research::sh4DynarecObservationMemoryEnd(0, 0, 0);
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010000,
			0x6010, 101, context);

	ASSERT_EQ(4u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(Type::MemoryRead, observed[1].type);
	EXPECT_EQ(0x8c020000u, observed[1].memoryAddress);
	EXPECT_EQ(1u, observed[1].memoryWidth);
	EXPECT_EQ(0x80u, observed[1].memoryValue);
	EXPECT_EQ(Type::MemoryWrite, observed[2].type);
	EXPECT_EQ(0x8c020004u, observed[2].memoryAddress);
	EXPECT_EQ(2u, observed[2].memoryWidth);
	EXPECT_EQ(0x5678u, observed[2].memoryValue);
	EXPECT_EQ(Type::InstructionEnd, observed[3].type);

	// A fault between the markers aborts the owner and never publishes access.
	observed.clear();
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010002,
			0x6120, 102, context);
	research::sh4DynarecObservationMemoryBegin(0xdeadbeef, 4, 0);
	research::sh4ObservationInstructionAbort(Backend::Dynarec);
	ASSERT_EQ(2u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(Type::InstructionAbort, observed[1].type);
}

TEST(ResearchSh4Observation, DynarecFaultDropsPendingMemoryAndRestoresDepth)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));

	Sh4Context context {};
	context.pc = 0x8c010002;
	context.vbr = 0x8c000000;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x6010, 100, context);
	research::sh4DynarecObservationMemoryBegin(0xdeadbeef, 4, 0);
	research::sh4ObservationExceptionRaised(0x8c010000, 0x0e0, context);

	context.pc = 0x8c010004;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010002,
			0x0009, 101, context);
	research::sh4DynarecObservationMemoryBegin(0x8c020000, 4, 0);
	research::sh4DynarecObservationMemoryEnd(0, 0, 0x12345678);
	research::sh4ObservationInstructionEnd(Backend::Dynarec, 0x8c010002,
			0x0009, 102, context);

	ASSERT_EQ(6u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(Type::Exception, observed[1].type);
	EXPECT_EQ(0x8c000100u, observed[1].vectorPc);
	EXPECT_EQ(Type::InstructionAbort, observed[2].type);
	EXPECT_EQ(Type::InstructionBegin, observed[3].type);
	EXPECT_EQ(0u, observed[3].delaySlotDepth);
	EXPECT_EQ(Type::MemoryRead, observed[4].type);
	EXPECT_EQ(0x12345678u, observed[4].memoryValue);
	EXPECT_EQ(Type::InstructionEnd, observed[5].type);
}

TEST(ResearchSh4Observation, ExceptionAbortsEveryNestedOwner)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));

	Sh4Context context {};
	context.pc = 0x8c010002;
	context.vbr = 0x8c000000;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0xa001, 100, context);
	context.pc = 0x8c010004;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010002,
			0x6010, 101, context);
	research::sh4DynarecObservationMemoryBegin(0xdeadbeef, 4, 0);
	research::sh4ObservationExceptionRaised(0x8c010000, 0x1a0, context);

	context.pc = 0x8c010006;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010004,
			0x0009, 102, context);
	research::sh4ObservationInstructionAbort(Backend::Dynarec);

	ASSERT_EQ(7u, observed.size());
	EXPECT_EQ(Type::Exception, observed[2].type);
	EXPECT_EQ(1u, observed[2].delaySlotDepth);
	EXPECT_EQ(Type::InstructionAbort, observed[3].type);
	EXPECT_EQ(1u, observed[3].delaySlotDepth);
	EXPECT_EQ(Type::InstructionAbort, observed[4].type);
	EXPECT_EQ(0u, observed[4].delaySlotDepth);
	EXPECT_EQ(Type::InstructionBegin, observed[5].type);
	EXPECT_EQ(0u, observed[5].delaySlotDepth);
}

TEST(ResearchSh4Observation, InterruptIsAnUnownedBackendSpecificException)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.pc = 0x8c010000;
	context.vbr = 0x8c000000;
	context.r[15] = 0x8cffff00;
	research::sh4ObservationInterruptRaised(Backend::Dynarec, 0x320, 123,
			context);

	ASSERT_EQ(1u, observed.size());
	EXPECT_EQ(Type::Exception, observed[0].type);
	EXPECT_EQ(Backend::Dynarec, observed[0].backend);
	EXPECT_EQ(0u, observed[0].opcode);
	EXPECT_EQ(0u, observed[0].delaySlotDepth);
	EXPECT_EQ(0x8c010000u, observed[0].exceptionPc);
	EXPECT_EQ(0x8c000600u, observed[0].vectorPc);
	EXPECT_EQ(0x320u, observed[0].exceptionCode);
	EXPECT_EQ(123u, observed[0].tick);
}

TEST(ResearchSh4Observation,
		InterruptFailsClosedInsteadOfStealingOpenInstructionOwnership)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.pc = 0x8c010002;
	context.vbr = 0x8c000000;
	research::sh4ObservationInstructionBegin(Backend::Dynarec, 0x8c010000,
			0x0009, 120, context);
	research::sh4ObservationInterruptRaised(Backend::Dynarec, 0x320, 121,
			context);

	ASSERT_EQ(1u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	// The stale internal frame was cleared. A later clean scheduler boundary can
	// publish only the documented unowned interrupt shape.
	context.pc = 0x8c020000;
	research::sh4ObservationInterruptRaised(Backend::Dynarec, 0x360, 122,
			context);
	ASSERT_EQ(2u, observed.size());
	EXPECT_EQ(Type::Exception, observed[1].type);
	EXPECT_EQ(0u, observed[1].opcode);
	EXPECT_EQ(0u, observed[1].delaySlotDepth);
}

TEST(ResearchSh4Observation,
		InterpreterSynchronousInterruptRetainsInstructionOwnership)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Interpreter);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.pc = 0x8c010002;
	context.vbr = 0x8c000000;
	research::sh4ObservationInstructionBegin(Backend::Interpreter, 0x8c010000,
			0x402eu, 120, context);
	context.pc = 0x8c020000;
	research::sh4ObservationInterruptRaised(Backend::Interpreter, 0x320, 121,
			context);
	research::sh4ObservationInstructionEnd(Backend::Interpreter, 0x8c010000,
			0x402eu, 122, context);

	ASSERT_EQ(3u, observed.size());
	EXPECT_EQ(Type::InstructionBegin, observed[0].type);
	EXPECT_EQ(Type::InstructionEnd, observed[1].type);
	EXPECT_EQ(Type::Exception, observed[2].type);
	EXPECT_EQ(0x8c020000u, observed[2].instructionPc);
	EXPECT_EQ(0u, observed[2].opcode);
	EXPECT_EQ(0u, observed[2].delaySlotDepth);
	EXPECT_EQ(0x8c020000u, observed[2].exceptionPc);
	EXPECT_EQ(0x8c000600u, observed[2].vectorPc);
	EXPECT_EQ(122u, observed[2].tick);
	EXPECT_EQ(0x8c020000u, observed[1].nextPc);
}

TEST(ResearchSh4Observation, ConditionalDelayMarkersMatchInterpreterNesting)
{
	using Backend = research::Sh4ObservationBackend;
	using Type = research::Sh4ObservationType;
	std::vector<research::Sh4Observation> observed;
	research::Sh4ObservationFilter filter;
	filter.backendMask = research::sh4ObservationBackendBit(Backend::Dynarec);
	ObservationSubscription subscription(research::subscribeSh4Observations(filter,
			[&](const research::Sh4Observation& observation) {
				observed.push_back(observation);
			}));
	Sh4Context context {};
	context.cycle_counter = 100;

	auto marker = [](shilop type, std::uint32_t pc, std::uint16_t opcode,
			std::uint32_t nextPc = 0xffffffffu) {
		shil_opcode value;
		value.op = type;
		value.size = 0;
		value.rs1 = shil_param(pc);
		value.rs2 = shil_param(opcode);
		value.rs3 = shil_param(nextPc);
		return value;
	};

	// BF/S with T=1 is not taken in the authoritative interpreter: the branch
	// closes first and the following instruction is observed at depth zero.
	context.jdyn = 1;
	context.sr.T = 1;
	shil_opcode branchBegin = marker(shop_research_begin, 0x8c010000, 0x8f01);
	shil_opcode beforeDelay = marker(shop_research_conditional_before_delay,
			0x8c010000, 0x8f01, 0x8c010006);
	shil_opcode delayBegin = marker(shop_research_begin, 0x8c010002, 0x0009);
	shil_opcode delayEnd = marker(shop_research_end, 0x8c010002, 0x0009,
			0x8c010004);
	shil_opcode afterDelay = marker(shop_research_conditional_after_delay,
			0x8c010000, 0x8f01, 0x8c010006);
	invokeDynarecMarker(context, branchBegin);
	invokeDynarecMarker(context, beforeDelay);
	context.sr.T = 0; // The slot may mutate T; ownership must use latched jdyn.
	invokeDynarecMarker(context, delayBegin);
	invokeDynarecMarker(context, delayEnd);
	invokeDynarecMarker(context, afterDelay);
	ASSERT_EQ(4u, observed.size());
	EXPECT_EQ(Type::InstructionEnd, observed[1].type);
	EXPECT_EQ(0u, observed[2].delaySlotDepth);

	// With T=0, BF/S is taken and owns the delay slot as a nested instruction.
	observed.clear();
	context.jdyn = 0;
	context.sr.T = 0;
	invokeDynarecMarker(context, branchBegin);
	invokeDynarecMarker(context, beforeDelay);
	context.sr.T = 1; // Prove the slot cannot change the parent's decision.
	invokeDynarecMarker(context, delayBegin);
	invokeDynarecMarker(context, delayEnd);
	invokeDynarecMarker(context, afterDelay);
	ASSERT_EQ(4u, observed.size());
	EXPECT_EQ(1u, observed[1].delaySlotDepth);
	EXPECT_EQ(1u, observed[2].delaySlotDepth);
	EXPECT_EQ(0x8c010006u, observed[3].nextPc);
}

#include "research/maple_observation.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace
{

class MapleSubscription
{
public:
	explicit MapleSubscription(research::MapleObservationSubscription token = 0)
		: token(token)
	{
	}

	~MapleSubscription()
	{
		if (token != 0)
			research::unsubscribeMapleObservations(token);
	}

	MapleSubscription(const MapleSubscription&) = delete;
	MapleSubscription& operator=(const MapleSubscription&) = delete;

private:
	research::MapleObservationSubscription token;
};

research::MapleTransactionEvent transaction(std::uint8_t bus = 1,
		std::uint8_t port = 2, std::uint8_t command = 9)
{
	research::MapleTransactionEvent event;
	event.tick = 1234;
	event.descriptorAddress = 0x8c001000;
	event.destinationAddress = 0x8c002000;
	event.descriptorHeader1 = static_cast<std::uint32_t>(bus) << 16;
	event.descriptorHeader2 = event.destinationAddress;
	event.deviceType = 1;
	event.bus = bus;
	event.port = port;
	event.command = command;
	event.flags = research::MapleTransactionDevicePresent;
	event.request = {command, 0x20, 0x01, 0x00};
	event.response = {0x08, 0x01, 0x20, 0x00};
	return event;
}

TEST(ResearchMapleObservation, InactiveBusHasNoDmaOwnership)
{
	EXPECT_FALSE(research::mapleObservationBusActive());
	EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(),
			research::beginMapleObservationDma());
}

TEST(ResearchMapleObservation, PublishesOrderedRequestAndResponseCopies)
{
	std::vector<research::MapleObservation> observations;
	MapleSubscription subscription(research::subscribeMapleObservations(
			research::MapleObservationFilter {},
			[&observations](const research::MapleObservation& observation) {
				observations.push_back(observation);
			}));
	const std::uint64_t dmaOrdinal = research::beginMapleObservationDma();
	research::MapleTransactionEvent value = transaction();
	ASSERT_TRUE(research::publishMapleTransactionObservations(dmaOrdinal, value));
	value.request[0] = 0xff;
	value.response[0] = 0xff;

	ASSERT_EQ(2u, observations.size());
	EXPECT_EQ(research::MapleObservationType::Request, observations[0].type);
	EXPECT_EQ(research::MapleObservationType::Response, observations[1].type);
	EXPECT_EQ(observations[0].emissionOrdinal + 1, observations[1].emissionOrdinal);
	EXPECT_EQ(observations[0].transactionOrdinal, observations[1].transactionOrdinal);
	EXPECT_EQ(dmaOrdinal, observations[0].dmaOrdinal);
	EXPECT_EQ(9u, observations[0].payload[0]);
	EXPECT_EQ(8u, observations[1].payload[0]);
	EXPECT_EQ(1234u, observations[1].tick);
}

TEST(ResearchMapleObservation, FiltersTypeTopologyAndInitiatingCommand)
{
	std::vector<research::MapleObservation> observations;
	research::MapleObservationFilter filter;
	filter.typeMask = research::mapleObservationTypeBit(
			research::MapleObservationType::Response);
	filter.busMask = 1u << 1;
	filter.portMask = 1u << 2;
	filter.hasCommand = true;
	filter.command = 9;
	MapleSubscription subscription(research::subscribeMapleObservations(filter,
			[&observations](const research::MapleObservation& observation) {
				observations.push_back(observation);
			}));

	const std::uint64_t firstDma = research::beginMapleObservationDma();
	EXPECT_FALSE(research::publishMapleTransactionObservations(firstDma,
			transaction(0, 2, 9)));
	const std::uint64_t secondDma = research::beginMapleObservationDma();
	EXPECT_FALSE(research::publishMapleTransactionObservations(secondDma,
			transaction(1, 2, 10)));
	const std::uint64_t thirdDma = research::beginMapleObservationDma();
	EXPECT_TRUE(research::publishMapleTransactionObservations(thirdDma,
			transaction(1, 2, 9)));

	ASSERT_EQ(1u, observations.size());
	EXPECT_EQ(research::MapleObservationType::Response, observations[0].type);
	EXPECT_EQ(1u, observations[0].bus);
	EXPECT_EQ(2u, observations[0].port);
	EXPECT_EQ(9u, observations[0].command);
}

TEST(ResearchMapleObservation, RejectsInvalidFiltersAndTransactions)
{
	research::MapleObservationFilter filter;
	filter.typeMask = 0;
	EXPECT_THROW(research::subscribeMapleObservations(filter,
			[](const research::MapleObservation&) {}), std::invalid_argument);
	filter = {};
	filter.busMask = 0;
	EXPECT_THROW(research::subscribeMapleObservations(filter,
			[](const research::MapleObservation&) {}), std::invalid_argument);

	MapleSubscription subscription(research::subscribeMapleObservations(
			research::MapleObservationFilter {},
			[](const research::MapleObservation&) {}));
	research::MapleTransactionEvent invalid = transaction();
	invalid.request.clear();
	EXPECT_FALSE(research::publishMapleTransactionObservations(
			research::beginMapleObservationDma(), invalid));
}

TEST(ResearchMapleObservation, SubscriberFailureCannotBreakPairing)
{
	MapleSubscription failing(research::subscribeMapleObservations(
			research::MapleObservationFilter {},
			[](const research::MapleObservation&) {
				throw std::runtime_error("subscriber failure");
			}));
	std::vector<research::MapleObservationType> delivered;
	MapleSubscription later(research::subscribeMapleObservations(
			research::MapleObservationFilter {},
			[&delivered](const research::MapleObservation& observation) {
				delivered.push_back(observation.type);
			}));
	EXPECT_TRUE(research::publishMapleTransactionObservations(
			research::beginMapleObservationDma(), transaction()));
	EXPECT_EQ((std::vector<research::MapleObservationType> {
			research::MapleObservationType::Request,
			research::MapleObservationType::Response}), delivered);
}

TEST(ResearchMapleObservation, PublishesExplicitlySelectedGuestResponse)
{
	std::vector<std::uint8_t> observedResponse;
	research::MapleObservationFilter filter;
	filter.typeMask = research::mapleObservationTypeBit(
			research::MapleObservationType::Response);
	MapleSubscription subscription(research::subscribeMapleObservations(filter,
			[&observedResponse](const research::MapleObservation& observation) {
				observedResponse = observation.payload;
			}));
	research::MapleTransactionEvent live = transaction();
	live.response = {0x07, 0x01, 0x20, 0x00};
	const std::vector<std::uint8_t> selected {0x08, 0x01, 0x20, 0x00};
	EXPECT_TRUE(research::publishMapleTransactionObservations(
			research::beginMapleObservationDma(), live, selected));
	EXPECT_EQ(selected, observedResponse);
}

} // namespace

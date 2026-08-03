#include "research/gdrom_hardware_observation.h"
#include "hw/sh4/sh4_if.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace
{
class Subscription
{
public:
	explicit Subscription(research::GdromHardwareObservationSubscription id)
		: id(id) {}
	~Subscription() { research::unsubscribeGdromHardwareObservations(id); }
private:
	research::GdromHardwareObservationSubscription id;
};

void instruction(std::uint32_t pc, std::uint16_t opcode, std::uint64_t tick,
		const std::function<void()>& body)
{
	Sh4Context context {};
	context.pc = pc + 2;
	context.pr = 0x8c020000;
	research::sh4ObservationInstructionBegin(
			research::Sh4ObservationBackend::Interpreter,
			pc, opcode, tick, context);
	body();
	research::sh4ObservationInstructionEnd(
			research::Sh4ObservationBackend::Interpreter,
			pc, opcode, tick + 1, context);
}

std::array<std::uint8_t, 12> readPacket(std::uint32_t fad,
		std::uint32_t sectors)
{
	std::array<std::uint8_t, 12> packet {};
	packet[0] = 0x30;
	packet[1] = 0x20; // user data, FAD addressing
	packet[2] = static_cast<std::uint8_t>(fad >> 16);
	packet[3] = static_cast<std::uint8_t>(fad >> 8);
	packet[4] = static_cast<std::uint8_t>(fad);
	packet[8] = static_cast<std::uint8_t>(sectors >> 16);
	packet[9] = static_cast<std::uint8_t>(sectors >> 8);
	packet[10] = static_cast<std::uint8_t>(sectors);
	return packet;
}
} // namespace

TEST(ResearchGdromHardwareObservation, PreservesBothPacketOwnersAndDmaByteStream)
{
	std::vector<research::GdromHardwareObservation> events;
	Subscription subscription(research::subscribeGdromHardwareEvidenceObservations(
			[&](const auto& event) { events.push_back(event); }));
	const auto packet = readPacket(45150, 2);
	instruction(0x8c010100, 0x2100, 10, [&] {
		research::observeGdromHardwareAtaPacket(1, 4096, 0, 10);
	});
	instruction(0x8c010200, 0x2201, 20, [&] {
		research::observeGdromHardwarePacket(packet.data(), 45150, 2,
				2048, true, 20);
	});
	research::observeGdromHardwareBufferFill(45150, 2, 2048, true, 30);
	instruction(0x8c010300, 0x2302, 40, [&] {
		research::observeGdromHardwareDmaBegin(0x0c100000, 4096, 1, 1, 40);
	});
	std::array<std::uint8_t, 4096> bytes {};
	for (std::size_t index = 0; index < bytes.size(); ++index)
		bytes[index] = static_cast<std::uint8_t>(index * 17u);
	research::observeGdromHardwareDmaTransfer(0x0c100000, bytes.data(),
			10240 > bytes.size() ? bytes.size() : 10240, 50);
	research::observeGdromHardwareDmaInterrupt(60);
	research::observeGdromHardwareCommandInterrupt(70);
	research::observeGdromHardwareComplete(70);
	instruction(0x8c010400, 0x2403, 80, [&] {
		research::observeGdromHardwareStatusAcknowledged(0x40, 80);
	});

	ASSERT_EQ(8u, events.size());
	const auto& accepted = events[0];
	EXPECT_EQ(research::GdromHardwareObservationType::PacketAccepted,
			accepted.type);
	EXPECT_EQ(0x8c010100u, accepted.ataOwner.pc);
	EXPECT_EQ(0x8c010200u, accepted.packetOwner.pc);
	EXPECT_NE(accepted.ataOwner.generation, accepted.packetOwner.generation);
	EXPECT_EQ(packet, accepted.packet);
	EXPECT_EQ(research::GdromHardwareDelivery::Dma, accepted.delivery);
	EXPECT_EQ(0u, events[3].streamOffset);
	EXPECT_EQ(bytes.size(), events[3].transferredBytes);
	EXPECT_EQ(accepted.commandGeneration, events.back().commandGeneration);
	EXPECT_EQ(bytes.size(), events.back().transferredBytes);
	EXPECT_EQ(research::GdromHardwareObservationType::StatusAcknowledged,
			events.back().type);
	EXPECT_EQ(0x8c010400u, events.back().packetOwner.pc);
}

TEST(ResearchGdromHardwareObservation, LoadStateInvalidatesRestoredInFlightCommand)
{
	std::vector<research::GdromHardwareObservation> events;
	Subscription subscription(research::subscribeGdromHardwareEvidenceObservations(
			[&](const auto& event) { events.push_back(event); }));
	const auto packet = readPacket(45150, 1);
	instruction(0x8c010100, 0x2100, 10, [&] {
		research::observeGdromHardwareAtaPacket(1, 2048, 0, 10);
	});
	instruction(0x8c010200, 0x2201, 20, [&] {
		research::observeGdromHardwarePacket(packet.data(), 45150, 1,
				2048, true, 20);
	});
	research::loadStateGdromHardwareObservation(30);
	std::array<std::uint8_t, 32> bytes {};
	const auto before = research::gdromHardwareObservationDroppedCount();
	research::observeGdromHardwareDmaTransfer(0x0c100000, bytes.data(),
			bytes.size(), 40);
	EXPECT_GT(research::gdromHardwareObservationDroppedCount(), before);
	ASSERT_EQ(2u, events.size());
	EXPECT_EQ(research::GdromHardwareObservationType::LoadState,
			events.back().type);
	EXPECT_NE(0u, events.back().commandGeneration);
}

TEST(ResearchGdromHardwareObservation,
		IgnoresPioTrafficOutsideTrackedReadCommands)
{
	std::vector<research::GdromHardwareObservation> events;
	Subscription subscription(research::subscribeGdromHardwareEvidenceObservations(
			[&](const auto& event) { events.push_back(event); }));
	const auto before = research::gdromHardwareObservationDroppedCount();
	research::observeGdromHardwarePioReady(0, 0, 0, true, 10);
	instruction(0x8c010100, 0x6101, 20, [&] {
		research::observeGdromHardwarePioWord(0x1234, 20);
	});
	EXPECT_TRUE(events.empty());
	EXPECT_EQ(before, research::gdromHardwareObservationDroppedCount());
}

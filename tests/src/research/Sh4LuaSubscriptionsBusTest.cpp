#include "Sh4LuaSubscriptionsTestSupport.h"

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
	research::Sh4LuaSubscriptionQueue::PvrTaFilter ta;
	ta.observation.typeMask = research::pvrTaObservationTypeBit(
			research::PvrTaObservationType::ListInit);
	queue.subscribe(ta, [&](auto,
			const research::Sh4LuaSubscriptionQueue::PvrTaLuaObservation&) {
		delivered.emplace_back("ta");
	});
	research::Sh4LuaSubscriptionQueue::PvrPresentationFilter presentation;
	presentation.typeMask = 1;
	presentation.hasAddressRange = true;
	presentation.addressStart = 0x005f8000;
	presentation.addressEndExclusive = 0x005f8100;
	queue.subscribe(presentation,
			[&](auto,
					const research::Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation&) {
				delivered.emplace_back("presentation");
			});
	research::observePvrTaListBoundary(false, 0x1000, 0, 1);
	const std::uint32_t selectedContexts[] {0x1000};
	const bool contextAvailability[] {true};
	research::PvrTaRenderSelectionTranscript transcript;
	transcript.initialized = true;
	transcript.regionBase = 0x00200000;
	transcript.record(0x00200010, 0);
	transcript.record(0x00200000, 0x80000000);
	transcript.record(0x00200004, 0x00300000);
	transcript.record(0x00300000, 0x1000);
	const std::uint64_t observedRenderGeneration =
			research::observePvrTaStartRender(selectedContexts,
					contextAvailability, 1, &transcript, 2);
	research::Sh4LuaSubscriptionQueue::PvrDrawFilter draw;
	draw.typeMask = std::uint32_t {1}
			<< (static_cast<unsigned>(research::PvrDrawObservationType::RenderCompleted) - 1u);
	draw.hasRenderGeneration = true;
	draw.renderGeneration = observedRenderGeneration;
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

	research::observePvrRegisterWrite(0x005f8004, 4, 1, 0, 1,
			research::PvrRegisterWriteDisposition::Stored, 0, 2);
	research::observePvrDrawRenderCompleted(observedRenderGeneration + 1, true, 3);
	research::observePvrDrawRenderCompleted(observedRenderGeneration, true, 4);
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
	const std::array<std::int32_t, 16> dspInputs {};
	const std::array<std::int16_t, 16> dspOutputs {};
	research::CddaDriveState drive;
	research::observeCddaSector(1, 600, drive, drive, false, bytes, sizeof(bytes), 1);
	research::observeCddaSector(2, 601, drive, drive, true, bytes, sizeof(bytes), 2);
	research::observeAicaSampleFrame(0, 0, 0, 0, 0, 0, 0, false,
			0, 0, dspInputs.data(), dspOutputs.data(), 0, 0, 2, 0, 3);
	research::observeAicaSampleFrame(0, 0, 0, 1, 0, 1, 0, false,
			0, 0, dspInputs.data(), dspOutputs.data(), 1, 0, 2, 1, 4);

	ASSERT_EQ(2u, queue.pendingCount());
	EXPECT_EQ(2u, queue.drain());
	EXPECT_EQ((std::vector<std::string> {"sector", "sample"}), delivered);
}

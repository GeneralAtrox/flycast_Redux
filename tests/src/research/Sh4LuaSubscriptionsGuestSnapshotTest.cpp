#include "Sh4LuaSubscriptionsTestSupport.h"

TEST(ResearchSh4LuaSubscriptions, CompactsFramebufferPayloadToExactDigest)
{
	research::Sh4LuaSubscriptionQueue queue;
	research::PvrPresentationObservation delivered;
	bool called = false;
	research::Sh4LuaSubscriptionQueue::PvrPresentationFilter filter;
	filter.typeMask = std::uint32_t {1}
			<< (static_cast<unsigned>(
					research::PvrPresentationObservationType::FramebufferCaptured) - 1u);
	filter.framebufferDigestOnly = true;
	queue.subscribe(filter,
			[&](auto,
					const research::Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation& queued) {
				delivered = queued.observation;
				called = true;
			});

	research::PvrFramebufferConfig config;
	const std::array<std::uint8_t, 8> pixels {1, 2, 3, 4, 5, 6, 7, 8};
	ASSERT_NE(0u, research::observePvrFramebufferCaptured(
			research::PvrFramebufferKind::PresentedRgb24, 0, config,
			2, 1, 8, pixels.data(), pixels.size(), 123));
	ASSERT_EQ(1u, queue.drain());
	ASSERT_TRUE(called);
	EXPECT_TRUE(delivered.framebufferDigestAvailable);
	EXPECT_TRUE(delivered.bytes.empty());
	EXPECT_TRUE(research::sha256Equal(delivered.framebufferDigest,
			research::sha256(pixels.data(), pixels.size())));
}

TEST(ResearchSh4LuaSubscriptions, CompactsSampledTextureToExistingExactDigest)
{
	research::Sh4LuaSubscriptionQueue queue;
	research::PvrDrawObservation delivered;
	bool called = false;
	research::Sh4LuaSubscriptionQueue::PvrDrawFilter filter;
	filter.typeMask = std::uint32_t {1}
			<< (static_cast<unsigned>(
					research::PvrDrawObservationType::PrimitiveDecoded) - 1u);
	filter.sampledTextureDigestOnly = true;
	queue.subscribe(filter,
			[&](auto, const research::PvrDrawObservation& observation) {
				delivered = observation;
				called = true;
			});

	research::PvrDrawObservation observation;
	observation.type = research::PvrDrawObservationType::PrimitiveDecoded;
	research::observePvrTaListBoundary(false, 0x2000, 0, 1);
	const std::uint32_t selectedContexts[] {0x2000};
	const bool contextAvailability[] {true};
	research::PvrTaRenderSelectionTranscript transcript;
	transcript.initialized = true;
	transcript.regionBase = 0x00210000;
	transcript.record(0x00210010, 0);
	transcript.record(0x00210000, 0x80000000);
	transcript.record(0x00210004, 0x00310000);
	transcript.record(0x00310000, 0x2000);
	observation.renderGeneration = research::observePvrTaStartRender(
			selectedContexts, contextAvailability, 1, &transcript, 2);
	observation.primitiveGeneration = research::allocatePvrPrimitiveGeneration();
	observation.count = 1;
	observation.sampledTexture.available = true;
	observation.sampledTexture.sourceBytes = {1, 2, 3, 4};
	observation.sampledTexture.sourceDigest = research::sha256(
			observation.sampledTexture.sourceBytes.data(),
			observation.sampledTexture.sourceBytes.size());
	research::observePvrPrimitiveDecoded(observation);
	ASSERT_EQ(1u, queue.drain());
	ASSERT_TRUE(called);
	EXPECT_TRUE(delivered.sampledTexture.sourceBytes.empty());
	EXPECT_TRUE(research::sha256Equal(delivered.sampledTexture.sourceDigest,
			observation.sampledTexture.sourceDigest));
}

TEST(ResearchSh4LuaSubscriptions, SnapshotsGuestU32BeforeLuaQueueDelivery)
{
	research_test::clearGuestRam();
	constexpr std::uint32_t address = 0x8c001000;
	ASSERT_TRUE(research_test::writeGuestRam(address, {0x78, 0x56, 0x34, 0x12}));

	research::Sh4LuaSubscriptionQueue queue;
	research::Sh4LuaSubscriptionQueue::PvrTaLuaObservation delivered;
	research::Sh4LuaSubscriptionQueue::PvrTaFilter filter;
	filter.observation.typeMask = research::pvrTaObservationTypeBit(
			research::PvrTaObservationType::AcceptedBlock);
	filter.guestU32Addresses = {address};
	queue.subscribe(filter,
			[&](auto,
					const research::Sh4LuaSubscriptionQueue::PvrTaLuaObservation& observation) {
				delivered = observation;
			});

	research::observePvrTaListBoundary(false, 0x2100, 0, 1);
	const std::array<std::uint8_t, 32> block {};
	research::observePvrTaAcceptedBlock(research::PvrTaInputSource::StoreQueue,
			0xe0000000, 0xe0000000, block.data(), 0x2100, 0,
			0, 0, 0, 0, 1000);
	ASSERT_TRUE(research_test::writeGuestRam(address, {0xef, 0xbe, 0xad, 0xde}));

	ASSERT_EQ(1u, queue.drain());
	EXPECT_EQ(1000u, delivered.guestSnapshotTick);
	ASSERT_EQ(1u, delivered.guestU32Snapshot.size());
	EXPECT_EQ(address, delivered.guestU32Snapshot[0].address);
	EXPECT_TRUE(delivered.guestU32Snapshot[0].available);
	EXPECT_EQ(0x12345678u, delivered.guestU32Snapshot[0].value);
}

TEST(ResearchSh4LuaSubscriptions, RejectsUnsafeGuestU32SnapshotFilters)
{
	research::Sh4LuaSubscriptionQueue queue;
	research::Sh4LuaSubscriptionQueue::PvrTaFilter filter;
	filter.observation.typeMask = research::pvrTaObservationTypeBit(
			research::PvrTaObservationType::AcceptedBlock);
	filter.guestU32Addresses = {0x8c001001};
	EXPECT_THROW(queue.subscribe(filter, [](auto, const auto&) {}),
			std::invalid_argument);
	filter.guestU32Addresses.assign(
			research::Sh4LuaSubscriptionQueue::MaximumGuestU32Snapshot + 1,
			0x8c001000);
	EXPECT_THROW(queue.subscribe(filter, [](auto, const auto&) {}),
			std::invalid_argument);
	filter.guestU32Addresses = {0x8c001000};
	filter.observation.typeMask = research::pvrTaObservationTypeBit(
			research::PvrTaObservationType::StartRender);
	EXPECT_THROW(queue.subscribe(filter, [](auto, const auto&) {}),
			std::invalid_argument);
}

TEST(ResearchSh4LuaSubscriptions,
		SnapshotsGuestU32AtPvrRegisterWriteBeforeLuaQueueDelivery)
{
	research_test::clearGuestRam();
	constexpr std::uint32_t address = 0x8c001000;
	ASSERT_TRUE(research_test::writeGuestRam(address, {0x78, 0x56, 0x34, 0x12}));

	research::Sh4LuaSubscriptionQueue queue;
	research::Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation delivered;
	research::Sh4LuaSubscriptionQueue::PvrPresentationFilter filter;
	filter.typeMask = std::uint32_t {1}
			<< (static_cast<unsigned>(
					research::PvrPresentationObservationType::RegisterWrite) - 1u);
	filter.hasAddressRange = true;
	filter.addressStart = 0x005f8148;
	filter.addressEndExclusive = 0x005f814c;
	filter.guestU32Addresses = {address};
	queue.subscribe(filter,
			[&](auto,
					const research::Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation& observation) {
				delivered = observation;
			});

	research::observePvrRegisterWrite(0x005f8148, 4, 0x00654000, 0,
			0x00654000, research::PvrRegisterWriteDisposition::Stored, 0, 1000);
	ASSERT_TRUE(research_test::writeGuestRam(address, {0xef, 0xbe, 0xad, 0xde}));

	ASSERT_EQ(1u, queue.drain());
	EXPECT_EQ(1000u, delivered.guestSnapshotTick);
	ASSERT_EQ(1u, delivered.guestU32Snapshot.size());
	EXPECT_EQ(address, delivered.guestU32Snapshot[0].address);
	EXPECT_TRUE(delivered.guestU32Snapshot[0].available);
	EXPECT_EQ(0x12345678u, delivered.guestU32Snapshot[0].value);
	EXPECT_EQ(0x005f8148u, delivered.observation.registerPhysicalAddress);
	EXPECT_EQ(0x00654000u, delivered.observation.effectiveValue);
}

TEST(ResearchSh4LuaSubscriptions,
		SnapshotsGuestU32AtPvrVramWriteBeforeLuaQueueDelivery)
{
	research_test::clearGuestRam();
	constexpr std::uint32_t address = 0x8c001000;
	ASSERT_TRUE(research_test::writeGuestRam(address, {0x78, 0x56, 0x34, 0x12}));

	research::Sh4LuaSubscriptionQueue queue;
	research::Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation delivered;
	research::Sh4LuaSubscriptionQueue::PvrPresentationFilter filter;
	filter.typeMask = std::uint32_t {1}
			<< (static_cast<unsigned>(
					research::PvrPresentationObservationType::VramWrite) - 1u);
	filter.hasAddressRange = true;
	filter.addressStart = 0x00653800;
	filter.addressEndExclusive = 0x00653820;
	filter.guestU32Addresses = {address};
	queue.subscribe(filter,
			[&](auto,
					const research::Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation& observation) {
				delivered = observation;
			});

	const std::array<std::uint8_t, 32> row {};
	research::observePvrVramWrite(research::PvrVramWriteSource::YuvConverter,
			0x00653800, 0x00653800, row.data(), row.size(), 0, 1000);
	ASSERT_TRUE(research_test::writeGuestRam(address, {0xef, 0xbe, 0xad, 0xde}));

	ASSERT_EQ(1u, queue.drain());
	EXPECT_EQ(1000u, delivered.guestSnapshotTick);
	ASSERT_EQ(1u, delivered.guestU32Snapshot.size());
	EXPECT_EQ(address, delivered.guestU32Snapshot[0].address);
	EXPECT_TRUE(delivered.guestU32Snapshot[0].available);
	EXPECT_EQ(0x12345678u, delivered.guestU32Snapshot[0].value);
	EXPECT_EQ(research::PvrVramWriteSource::YuvConverter,
			delivered.observation.vramSource);
	EXPECT_EQ(0x00653800u, delivered.observation.physicalAddress);
	EXPECT_EQ(32u, delivered.observation.bytes.size());
}

TEST(ResearchSh4LuaSubscriptions,
		SnapshotsGuestStackAtPvrVramWriteBeforeLuaQueueDelivery)
{
	research_test::clearGuestRam();
	constexpr std::uint32_t stack = 0x8c001000;
	Sh4cntx.r[15] = stack;
	ASSERT_TRUE(research_test::writeGuestRam(stack + 8,
			{0x78, 0x56, 0x34, 0x12}));

	research::Sh4LuaSubscriptionQueue queue;
	research::Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation delivered;
	research::Sh4LuaSubscriptionQueue::PvrPresentationFilter filter;
	filter.typeMask = std::uint32_t {1}
			<< (static_cast<unsigned>(
					research::PvrPresentationObservationType::VramWrite) - 1u);
	filter.guestR15U32Offsets = {8};
	queue.subscribe(filter,
			[&](auto,
					const research::Sh4LuaSubscriptionQueue::PvrPresentationLuaObservation& observation) {
				delivered = observation;
			});

	const std::array<std::uint8_t, 32> row {};
	research::observePvrVramWrite(research::PvrVramWriteSource::YuvConverter,
			0x00653800, 0x00653800, row.data(), row.size(), 0, 1000);
	ASSERT_TRUE(research_test::writeGuestRam(stack + 8,
			{0xef, 0xbe, 0xad, 0xde}));

	ASSERT_EQ(1u, queue.drain());
	EXPECT_TRUE(delivered.guestR15Available);
	EXPECT_EQ(stack, delivered.guestR15);
	ASSERT_EQ(1u, delivered.guestR15U32Snapshot.size());
	EXPECT_EQ(stack + 8, delivered.guestR15U32Snapshot[0].address);
	EXPECT_TRUE(delivered.guestR15U32Snapshot[0].available);
	EXPECT_EQ(0x12345678u, delivered.guestR15U32Snapshot[0].value);
}

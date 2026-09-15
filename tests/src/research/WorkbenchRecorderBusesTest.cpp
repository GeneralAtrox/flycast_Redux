// One recording that touches every hardware bus the recorder binds, so a broken
// row binder fails here instead of only in the live capture. The SH-4 frame is
// opened by hand: the recorder retains instruction ownership, so every hardware
// row must come back carrying that instruction's PC as its owner.

#include "hw/sh4/sh4_if.h"
#include "research/aica_observation.h"
#include "research/cdda_observation.h"
#include "research/gdrom_hardware_observation.h"
#include "research/gdrom_observation.h"
#include "research/pvr_draw_observation.h"
#include "research/pvr_presentation_observation.h"
#include "research/pvr_ta_observation.h"
#include "research/sh4_observation_runtime.h"
#include "research/workbench/workbench_config.h"
#include "research/workbench/workbench_db.h"
#include "research/workbench/workbench_recorder.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace wb = research::workbench;

namespace
{

constexpr std::uint32_t OwnerPc = 0x8c010100;
constexpr std::uint32_t ContextAddress = 0x00100000;

class WorkbenchRecorderBusesTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		directory = std::filesystem::temp_directory_path()
				/ ("flycast-workbench-buses-" + std::to_string(stamp));
		std::filesystem::create_directories(directory);
		research::resetPvrTaObservation(0);
		research::resetCddaObservation(0);
	}

	void TearDown() override
	{
		std::error_code ignored;
		std::filesystem::remove_all(directory, ignored);
	}

	std::filesystem::path databasePath() const { return directory / "buses.sqlite"; }

	std::filesystem::path directory;
};

wb::RunInfo testRun()
{
	wb::RunInfo run;
	run.gameId = "T-BUSES";
	run.cpuBackend = "interpreter";
	run.flycastVersion = "test";
	return run;
}

// Publishes one representative event on every hardware bus while an SH-4
// instruction frame is open. Returns the render generation it created.
std::uint64_t publishOneEventPerBus()
{
	using namespace research;
	Sh4Context context {};
	context.pc = OwnerPc + 2;
	context.pr = 0x8c020000;
	sh4ObservationInstructionBegin(Sh4ObservationBackend::Interpreter, OwnerPc, 0x2102,
			100, context);
	EXPECT_TRUE(sh4ObservationCurrentInstructionOwner().valid)
			<< "the recorder must retain SH-4 instruction ownership";

	// PVR TA: list init, one accepted block, STARTRENDER over that context.
	observePvrTaListBoundary(false, ContextAddress, 0, 101);
	std::array<std::uint8_t, 32> block {};
	for (std::size_t index = 0; index < block.size(); ++index)
		block[index] = static_cast<std::uint8_t>(index);
	const PvrTaBlockProvenance provenance = observePvrTaAcceptedBlock(
			PvrTaInputSource::StoreQueue, 0xe0000020, 0x10000020, block.data(),
			ContextAddress, 0, 7, 0, 0, 1, 102);
	EXPECT_TRUE(provenance.available);
	const std::uint32_t selected[] {ContextAddress};
	const bool available[] {true};
	PvrTaRenderSelectionTranscript transcript;
	transcript.initialized = true;
	transcript.regionBase = 0x00200000;
	transcript.fpuParamCfg = 0x0007ff00;
	transcript.record(0x05000000, ContextAddress);
	const std::uint64_t renderGeneration = observePvrTaStartRender(selected, available, 1,
			&transcript, 103);
	EXPECT_NE(0u, renderGeneration);

	// PVR draw: a decoded strip whose blocks all belong to this instruction.
	PvrDrawObservation primitive;
	primitive.tick = 104;
	primitive.renderGeneration = renderGeneration;
	primitive.primitiveGeneration = allocatePvrPrimitiveGeneration();
	primitive.contextAddress = ContextAddress;
	primitive.contextGeneration = provenance.contextGeneration;
	primitive.listType = 0;
	primitive.primitiveKind = PvrPrimitiveKind::PolygonStrip;
	primitive.count = 3;
	primitive.vertices.resize(3);
	primitive.vertices[0].xBits = 0x3f800000;
	primitive.vertices[0].baseColor = {1, 2, 3, 4};
	primitive.sampledTexture.available = true;
	primitive.sampledTexture.sourceAddress = 0x00300000;
	primitive.sampledTexture.sourceSize = 0x800;
	primitive.sampledTexture.width = 32;
	primitive.sampledTexture.height = 32;
	primitive.sampledTexture.sourceDigest[0] = 0x5a;
	primitive.parameterBlocks = {provenance};
	primitive.vertexBlocks = {provenance};
	primitive.ownerClass = classifyPvrPrimitiveOwnership(primitive.parameterBlocks,
			primitive.vertexBlocks);
	EXPECT_EQ(PvrPrimitiveOwnerClass::Exact, primitive.ownerClass);
	observePvrPrimitiveDecoded(primitive);

	// PVR presentation: a register write.
	observePvrRegisterWrite(0x005f8050, 0x50, 0x01000003, 0, 0x01000000,
			PvrRegisterWriteDisposition::MaskedAndStored, renderGeneration, 105);

	// GD-ROM, both paths.
	const std::uint32_t gdromParameters[4] {45150, 2, 0x0c100000, 0};
	observeReiosGdromCommand(7, 0x11, gdromParameters, 106);
	observeReiosGdromComplete(107);
	// The hardware bus publishes PacketAccepted only once the 12-byte packet
	// follows the ATA command; both halves carry this instruction as owner.
	observeGdromHardwareAtaPacket(1, 4096, 0, 108);
	std::array<std::uint8_t, 12> packet {};
	packet[0] = 0x30;  // CD_READ
	packet[2] = 0x00; packet[3] = 0xb0; packet[4] = 0x5e;  // FAD 45150
	packet[8] = 0x00; packet[9] = 0x00; packet[10] = 0x02;  // 2 sectors
	observeGdromHardwarePacket(packet.data(), 45150, 2, 2048, true, 108);

	// AICA from the SH-4 (passes the default writer filter) and CD-DA control.
	observeAicaRegisterWrite(AicaWriter::Sh4Direct, 0x123, 2, 0x4567, 109);
	const std::uint32_t cddaParameters[4] {600, 601, 0, 0};
	observeReiosCddaControlAccepted(7, 0x15, cddaParameters, 110);

	sh4ObservationInstructionEnd(Sh4ObservationBackend::Interpreter, OwnerPc, 0x2102, 111,
			context);
	observePvrTaRenderDone(200);
	return renderGeneration;
}

} // namespace

TEST_F(WorkbenchRecorderBusesTest, EveryHardwareBusLandsARowOwnedByTheInstruction)
{
	wb::RecorderConfig config = wb::defaultRecorderConfig();
	config.buses = wb::BusPvrTa | wb::BusPvrDraw | wb::BusPvrPresentation | wb::BusGdrom
			| wb::BusGdromHardware | wb::BusAica | wb::BusCdda;
	wb::WorkbenchRecorder recorder;
	ASSERT_NO_THROW(recorder.start(databasePath(), config, testRun()));

	const std::uint64_t renderGeneration = publishOneEventPerBus();

	ASSERT_NO_THROW(recorder.stop());
	EXPECT_EQ(0u, recorder.status().dropped);
	EXPECT_TRUE(recorder.status().error.empty()) << recorder.status().error;

	wb::Database db(databasePath());
	const std::string owner = std::to_string(OwnerPc);

	// TA: list init, accepted block, start render, render done.
	EXPECT_EQ(4, db.queryInt64("SELECT COUNT(*) FROM pvr_ta_events"));
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM pvr_ta_events WHERE type = 3"
			" AND init_pc = " + owner + " AND source = 1 AND length(block) = 32"));
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM pvr_ta_events WHERE type = 4"
			" AND render_gen = " + std::to_string(renderGeneration)
			+ " AND region_base = 2097152 AND selected_count = 1"));
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM pvr_ta_selected_contexts"
			" WHERE ctx_addr = " + std::to_string(ContextAddress) + " AND available = 1"));
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM pvr_ta_selection_reads"
			" WHERE address = 83886080 AND value = " + std::to_string(ContextAddress)));

	// Draw: the primitive, its vertices, both provenance blocks, the texture.
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM pvr_draw_events WHERE type = 1"
			" AND owner_pc = " + owner + " AND owner_class = 2 AND vertex_count = 3"
			" AND tex_addr = 3145728 AND render_gen = " + std::to_string(renderGeneration)));
	EXPECT_EQ(3, db.queryInt64("SELECT COUNT(*) FROM pvr_draw_vertices"));
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM pvr_draw_vertices"
			" WHERE idx = 0 AND x_bits = 1065353216 AND base_color = 67305985"));
	EXPECT_EQ(2, db.queryInt64("SELECT COUNT(*) FROM pvr_draw_blocks WHERE init_pc = " + owner));
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM textures WHERE address = 3145728"
			" AND width = 32 AND bytes IS NULL"));

	// Presentation: the register write carries the owner and the render.
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM pvr_present_events WHERE type = 1"
			" AND init_pc = " + owner + " AND reg_addr = 80 AND requested = 16777219"
			" AND effective = 16777216 AND render_gen = " + std::to_string(renderGeneration)));

	// GD-ROM: HLE command plus complete, hardware ATA packet.
	EXPECT_EQ(2, db.queryInt64("SELECT COUNT(*) FROM gdrom_events"));
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM gdrom_events WHERE type = 1"
			" AND init_pc = " + owner + " AND request_id = 7 AND command = 17 AND p0 = 45150"));
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM gdrom_hw_events WHERE type = 1"
			" AND ata_pc = " + owner + " AND pkt_pc = " + owner
			+ " AND features = 1 AND byte_count_reg = 4096 AND start_fad = 45150"
			" AND sector_count = 2 AND delivery = 2 AND length(packet) = 12"));

	// AICA and CD-DA.
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM aica_events WHERE type = 1"
			" AND writer = 1 AND sh4_pc = " + owner + " AND address = 291 AND width = 2"
			" AND value = 17767"));
	EXPECT_EQ(1, db.queryInt64("SELECT COUNT(*) FROM cdda_events WHERE type = 1"
			" AND init_pc = " + owner + " AND request_id = 7 AND command = 21 AND p0 = 600"));

	// Nothing was left subscribed.
	EXPECT_EQ(0u, research::sh4ObservationSubscriberCount());
	EXPECT_FALSE(research::pvrTaObservationBusActive());
	EXPECT_FALSE(research::aicaObservationBusActive());
}

TEST_F(WorkbenchRecorderBusesTest, AicaWriterFilterDropsArm7TrafficByDefault)
{
	wb::RecorderConfig config = wb::defaultRecorderConfig();
	config.buses = wb::BusAica;
	wb::WorkbenchRecorder recorder;
	ASSERT_NO_THROW(recorder.start(databasePath(), config, testRun()));
	research::observeAicaRegisterWrite(research::AicaWriter::Arm7, 0x200, 4, 1, 10);
	research::observeAicaRegisterWrite(research::AicaWriter::Sh4Direct, 0x204, 4, 2, 11);
	research::observeAicaRegisterWrite(research::AicaWriter::Sh4G2Dma, 0x208, 4, 3, 12);
	ASSERT_NO_THROW(recorder.stop());

	wb::Database db(databasePath());
	EXPECT_EQ(2, db.queryInt64("SELECT COUNT(*) FROM aica_events"));
	EXPECT_EQ(0, db.queryInt64("SELECT COUNT(*) FROM aica_events WHERE writer = 3"));
	// The drop is a filter decision, not a lost row.
	EXPECT_EQ(0u, recorder.status().dropped);
}

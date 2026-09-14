#include "research/workbench/workbench_rows.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace research::workbench
{

namespace
{

// "?, ?, ..., ?" with `count` placeholders.
std::string placeholders(int count)
{
	std::string list;
	for (int i = 0; i < count; i++)
		list += i == 0 ? "?" : ", ?";
	return list;
}

std::uint32_t packColor(const std::array<std::uint8_t, 4>& bytes)
{
	return static_cast<std::uint32_t>(bytes[0])
			| static_cast<std::uint32_t>(bytes[1]) << 8
			| static_cast<std::uint32_t>(bytes[2]) << 16
			| static_cast<std::uint32_t>(bytes[3]) << 24;
}

// Exact-owner primitives inherit the initiator of their first parameter block;
// everything else records NULL owner columns.
Sh4InstructionOwnerToken primitiveOwner(const PvrDrawObservation& observation)
{
	if (observation.ownerClass == PvrPrimitiveOwnerClass::Exact
			&& !observation.parameterBlocks.empty())
		return observation.parameterBlocks.front().initiator;
	return Sh4InstructionOwnerToken {};
}

constexpr int EventColumnCount = 55;
constexpr int BlockRoleParameter = 0;
constexpr int BlockRoleVertex = 1;

// The row binder holds no database handle, so child rows take the parent id
// from the table itself: this connection is the only writer and `id` is the
// rowid alias, so MAX(id) is the event inserted immediately before (an
// O(log n) rightmost-leaf descent through the primary key, not a scan).
constexpr const char *ParentEventId = "(SELECT MAX(id) FROM pvr_draw_events)";

} // namespace

PvrDrawRows::PvrDrawRows(Database& db, std::int64_t run, const RowOptions& options)
	: run(run), options(options),
	  insert(db.prepare("INSERT INTO pvr_draw_events("
			"run, ordinal, type, tick, render_gen, primitive_gen, raster_gen,"
			" ctx_addr, ctx_gen, render_pass, list_type, kind, owner_class,"
			" pcw, isp, tsp, tcw, tsp1, tcw1, tile_clip, first_index, count,"
			" bounds_available, min_x, min_y, min_z, max_x, max_y, max_z,"
			" vertex_count, backend, draw_pass, indexed, successful,"
			" owner_gen, owner_pc, owner_pr, owner_opcode, owner_backend, owner_depth,"
			" tex_available, tex_addr, tex_size, tex_max_level_addr, tex_max_level_size,"
			" tex_w, tex_h, tex_fmt, tex_palette_first, tex_palette_count,"
			" tex_cache_updates, tex_gpu_palette, tex_custom, tex_digest, tex_palette_digest)"
			" VALUES(" + placeholders(EventColumnCount) + ")")),
	  insertVertex(db.prepare("INSERT INTO pvr_draw_vertices("
			"event_id, idx, x_bits, y_bits, z_bits, u_bits, v_bits, u1_bits, v1_bits,"
			" nx_bits, ny_bits, nz_bits, base_color, offset_color, base_color1, offset_color1)"
			" VALUES(" + std::string(ParentEventId) + ", " + placeholders(15) + ")")),
	  insertBlock(db.prepare("INSERT INTO pvr_draw_blocks("
			"event_id, role, idx, init_gen, init_pc, init_pr, init_opcode, init_backend,"
			" init_depth, ctx_addr, ctx_gen, block_ordinal, render_pass, source,"
			" source_addr, ta_addr, available)"
			" VALUES(" + std::string(ParentEventId) + ", " + placeholders(16) + ")")),
	  insertTexture(db.prepare("INSERT OR IGNORE INTO textures("
			"digest, run, address, size, width, height, format, palette_digest, bytes)"
			" VALUES(" + placeholders(9) + ")")),
	  insertConsumedPrimitive(db.prepare("INSERT INTO pvr_draw_consumed("
			"event_id, primitive_gen) VALUES(" + std::string(ParentEventId) + ", ?)"))
{
}

void PvrDrawRows::createTables(Database& db)
{
	db.exec("CREATE TABLE IF NOT EXISTS pvr_draw_events("
			"id INTEGER PRIMARY KEY, run INTEGER, ordinal INTEGER, type INTEGER,"
			" tick INTEGER, render_gen INTEGER, primitive_gen INTEGER, raster_gen INTEGER,"
			" ctx_addr INTEGER, ctx_gen INTEGER, render_pass INTEGER, list_type INTEGER,"
			" kind INTEGER, owner_class INTEGER,"
			" pcw INTEGER, isp INTEGER, tsp INTEGER, tcw INTEGER, tsp1 INTEGER, tcw1 INTEGER,"
			" tile_clip INTEGER, first_index INTEGER, count INTEGER,"
			" bounds_available INTEGER, min_x REAL, min_y REAL, min_z REAL,"
			" max_x REAL, max_y REAL, max_z REAL,"
			" vertex_count INTEGER, backend INTEGER, draw_pass INTEGER,"
			" indexed INTEGER, successful INTEGER,"
			" owner_gen INTEGER, owner_pc INTEGER, owner_pr INTEGER, owner_opcode INTEGER,"
			" owner_backend INTEGER, owner_depth INTEGER,"
			" tex_available INTEGER, tex_addr INTEGER, tex_size INTEGER,"
			" tex_max_level_addr INTEGER, tex_max_level_size INTEGER,"
			" tex_w INTEGER, tex_h INTEGER, tex_fmt INTEGER,"
			" tex_palette_first INTEGER, tex_palette_count INTEGER, tex_cache_updates INTEGER,"
			" tex_gpu_palette INTEGER, tex_custom INTEGER,"
			" tex_digest BLOB, tex_palette_digest BLOB)");
	db.exec("CREATE TABLE IF NOT EXISTS pvr_draw_vertices("
			"event_id INTEGER, idx INTEGER,"
			" x_bits INTEGER, y_bits INTEGER, z_bits INTEGER,"
			" u_bits INTEGER, v_bits INTEGER, u1_bits INTEGER, v1_bits INTEGER,"
			" nx_bits INTEGER, ny_bits INTEGER, nz_bits INTEGER,"
			" base_color INTEGER, offset_color INTEGER,"
			" base_color1 INTEGER, offset_color1 INTEGER)");
	// role: 0 = parameter block, 1 = vertex block.
	db.exec("CREATE TABLE IF NOT EXISTS pvr_draw_blocks("
			"event_id INTEGER, role INTEGER, idx INTEGER,"
			" init_gen INTEGER, init_pc INTEGER, init_pr INTEGER, init_opcode INTEGER,"
			" init_backend INTEGER, init_depth INTEGER,"
			" ctx_addr INTEGER, ctx_gen INTEGER, block_ordinal INTEGER, render_pass INTEGER,"
			" source INTEGER, source_addr INTEGER, ta_addr INTEGER, available INTEGER)");
	db.exec("CREATE TABLE IF NOT EXISTS pvr_draw_consumed("
			"event_id INTEGER, primitive_gen INTEGER)");
	db.exec("CREATE TABLE IF NOT EXISTS textures("
			"digest BLOB PRIMARY KEY, run INTEGER, address INTEGER, size INTEGER,"
			" width INTEGER, height INTEGER, format INTEGER,"
			" palette_digest BLOB, bytes BLOB)");
}

void PvrDrawRows::createIndexes(Database& db)
{
	db.exec("CREATE INDEX IF NOT EXISTS pvr_draw_events_tick ON pvr_draw_events(tick)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_draw_events_render_gen"
			" ON pvr_draw_events(render_gen)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_draw_events_primitive_gen"
			" ON pvr_draw_events(primitive_gen)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_draw_events_owner_pc"
			" ON pvr_draw_events(owner_pc) WHERE owner_pc IS NOT NULL");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_draw_events_type ON pvr_draw_events(type)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_draw_events_tex_addr"
			" ON pvr_draw_events(tex_addr) WHERE tex_addr IS NOT NULL");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_draw_vertices_event"
			" ON pvr_draw_vertices(event_id)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_draw_blocks_event ON pvr_draw_blocks(event_id)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_draw_blocks_init_pc ON pvr_draw_blocks(init_pc)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_draw_consumed_event"
			" ON pvr_draw_consumed(event_id)");
	db.exec("CREATE INDEX IF NOT EXISTS pvr_draw_consumed_primitive_gen"
			" ON pvr_draw_consumed(primitive_gen)");
}

void PvrDrawRows::write(const PvrDrawObservation& o)
{
	const PvrSampledTexture& tex = o.sampledTexture;
	const PvrPrimitiveBounds& bounds = o.bounds;

	// 1..4: run, ordinal, type, tick
	insert.bindInt(1, run)
			.bindUInt(2, o.emissionOrdinal)
			.bindInt(3, static_cast<int>(o.type))
			.bindUInt(4, o.tick)
			// 5..9: render_gen, primitive_gen, raster_gen, ctx_addr, ctx_gen
			.bindGeneration(5, o.renderGeneration)
			.bindGeneration(6, o.primitiveGeneration)
			.bindGeneration(7, o.rasterGeneration)
			.bindOptionalU32(8, o.contextAddress)
			.bindGeneration(9, o.contextGeneration)
			// 10..13: render_pass, list_type, kind, owner_class
			.bindUInt(10, o.renderPass)
			.bindOptionalU32(11, o.listType)
			.bindInt(12, static_cast<int>(o.primitiveKind))
			.bindInt(13, static_cast<int>(o.ownerClass))
			// 14..22: pcw, isp, tsp, tcw, tsp1, tcw1, tile_clip, first_index, count
			.bindUInt(14, o.pcw)
			.bindUInt(15, o.isp)
			.bindUInt(16, o.tsp)
			.bindUInt(17, o.tcw)
			.bindOptionalU32(18, o.tsp1)
			.bindOptionalU32(19, o.tcw1)
			.bindUInt(20, o.tileClip)
			.bindUInt(21, o.first)
			.bindUInt(22, o.count)
			// 23: bounds_available
			.bindBool(23, bounds.available);
	// 24..29: min_x, min_y, min_z, max_x, max_y, max_z
	if (bounds.available)
	{
		insert.bindDouble(24, bounds.minimumX)
				.bindDouble(25, bounds.minimumY)
				.bindDouble(26, bounds.minimumZ)
				.bindDouble(27, bounds.maximumX)
				.bindDouble(28, bounds.maximumY)
				.bindDouble(29, bounds.maximumZ);
	}
	else
	{
		for (int index = 24; index <= 29; index++)
			insert.bindNull(index);
	}
	// 30..34: vertex_count, backend, draw_pass, indexed, successful
	insert.bindUInt(30, o.vertices.size())
			.bindInt(31, static_cast<int>(o.backend))
			.bindInt(32, static_cast<int>(o.drawPass))
			.bindBool(33, o.indexed)
			.bindBool(34, o.successful);
	// 35..40: owner_gen, owner_pc, owner_pr, owner_opcode, owner_backend, owner_depth
	bindOwnerToken(insert, 35, primitiveOwner(o));
	// 41..53: tex_available .. tex_custom
	insert.bindBool(41, tex.available)
			.bindOptionalU32(42, tex.sourceAddress)
			.bindUInt(43, tex.sourceSize)
			.bindOptionalU32(44, tex.maximumLevelAddress)
			.bindUInt(45, tex.maximumLevelSize)
			.bindUInt(46, tex.width)
			.bindUInt(47, tex.height)
			.bindUInt(48, tex.pixelFormat)
			.bindUInt(49, tex.paletteFirstEntry)
			.bindUInt(50, tex.paletteEntryCount)
			.bindUInt(51, tex.cacheUpdates)
			.bindBool(52, tex.gpuPalette)
			.bindBool(53, tex.customReplacement);
	// 54..55: tex_digest, tex_palette_digest
	if (tex.available)
	{
		insert.bindBlob(54, tex.sourceDigest.data(), tex.sourceDigest.size())
				.bindBlob(55, tex.paletteDigest.data(), tex.paletteDigest.size());
	}
	else
	{
		insert.bindNull(54).bindNull(55);
	}
	insert.execute();

	// 1: primitive_gen
	for (std::uint64_t generation : o.primitiveGenerations)
		insertConsumedPrimitive.bindGeneration(1, generation).execute();

	auto writeBlocks = [&](int role, const std::vector<PvrTaBlockProvenance>& blocks) {
		for (std::size_t i = 0; i < blocks.size(); i++)
		{
			const PvrTaBlockProvenance& block = blocks[i];
			// 1..2: role, idx
			insertBlock.bindInt(1, role).bindUInt(2, i);
			// 3..8: init_gen, init_pc, init_pr, init_opcode, init_backend, init_depth
			bindOwnerToken(insertBlock, 3, block.initiator);
			// 9..16: ctx_addr, ctx_gen, block_ordinal, render_pass, source,
			//        source_addr, ta_addr, available
			insertBlock.bindOptionalU32(9, block.contextAddress)
					.bindGeneration(10, block.contextGeneration)
					.bindUInt(11, block.contextBlockOrdinal)
					.bindUInt(12, block.renderPass)
					.bindInt(13, static_cast<int>(block.source))
					.bindOptionalU32(14, block.sourceAddress)
					.bindOptionalU32(15, block.taAddress)
					.bindBool(16, block.available)
					.execute();
		}
	};
	writeBlocks(BlockRoleParameter, o.parameterBlocks);
	writeBlocks(BlockRoleVertex, o.vertexBlocks);

	if (o.type != PvrDrawObservationType::PrimitiveDecoded)
		return;

	if (options.storeDrawVertices)
	{
		for (std::size_t i = 0; i < o.vertices.size(); i++)
		{
			const PvrDecodedVertex& v = o.vertices[i];
			// 1: idx
			insertVertex.bindUInt(1, i)
					// 2..11: x, y, z, u, v, u1, v1, nx, ny, nz bit patterns
					.bindUInt(2, v.xBits)
					.bindUInt(3, v.yBits)
					.bindUInt(4, v.zBits)
					.bindUInt(5, v.uBits)
					.bindUInt(6, v.vBits)
					.bindUInt(7, v.u1Bits)
					.bindUInt(8, v.v1Bits)
					.bindUInt(9, v.nxBits)
					.bindUInt(10, v.nyBits)
					.bindUInt(11, v.nzBits)
					// 12..15: base_color, offset_color, base_color1, offset_color1
					.bindUInt(12, packColor(v.baseColor))
					.bindUInt(13, packColor(v.offsetColor))
					.bindUInt(14, packColor(v.baseColor1))
					.bindUInt(15, packColor(v.offsetColor1))
					.execute();
		}
	}

	if (tex.available)
	{
		// 1..8: digest, run, address, size, width, height, format, palette_digest
		insertTexture.bindBlob(1, tex.sourceDigest.data(), tex.sourceDigest.size())
				.bindInt(2, run)
				.bindOptionalU32(3, tex.sourceAddress)
				.bindUInt(4, tex.sourceSize)
				.bindUInt(5, tex.width)
				.bindUInt(6, tex.height)
				.bindUInt(7, tex.pixelFormat)
				.bindBlob(8, tex.paletteDigest.data(), tex.paletteDigest.size());
		// 9: bytes
		if (options.storeTextureBytes && !tex.sourceBytes.empty())
			insertTexture.bindBlob(9, tex.sourceBytes.data(), tex.sourceBytes.size());
		else
			insertTexture.bindNull(9);
		insertTexture.execute();
	}
}

} // namespace research::workbench

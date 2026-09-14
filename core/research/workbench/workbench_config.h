#pragma once

// What the workbench recorder captures. Built from launch-time research.*
// options or from a JSON object received over the control socket.

#include "research/maple_observation.h"
#include "research/pvr_ta_observation.h"
#include "research/sh4_observation.h"

#include "json.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace research::workbench
{

enum WorkbenchBus : std::uint32_t
{
	BusSh4 = 1u << 0,
	BusMaple = 1u << 1,
	BusPvrTa = 1u << 2,
	BusPvrDraw = 1u << 3,
	BusPvrPresentation = 1u << 4,
	BusGdrom = 1u << 5,
	BusGdromHardware = 1u << 6,
	BusAica = 1u << 7,
	BusCdda = 1u << 8,
	AllWorkbenchBuses = (1u << 9) - 1,
};

// Row-level knobs for the high-volume payloads.
struct RowOptions
{
	bool storeTextureBytes = false;     // pvr_draw sampled texture source bytes
	bool storeDrawVertices = true;      // pvr_draw decoded vertices
	bool recordVramWrites = false;      // pvr_present VramWrite events at all
	bool storeVramWriteBytes = false;   // ...and their payload bytes
	bool storeFramebufferBytes = false; // pvr_present captured framebuffer pixels
	bool storeSectorBytes = false;      // gdrom / cdda sector payloads
	bool recordSampleFrames = false;    // aica per-sample mixer frames (44.1 kHz)
};

// AICA is written tens of thousands of times per second by the ARM7 sound
// driver. By default only SH-4-originated and DMA traffic is recorded.
struct AicaRecordFilter
{
	std::uint32_t writerMask = 0;   // bit (1 << AicaWriter); 0 = default set
	std::uint32_t typeMask = 0;     // bit (1 << AicaObservationType); 0 = all
};

struct RecorderConfig
{
	std::uint32_t buses = AllWorkbenchBuses;
	Sh4ObservationFilter sh4;
	MapleObservationFilter maple;
	PvrTaObservationFilter pvrTa;
	AicaRecordFilter aica;
	RowOptions rows;
	std::size_t queueCapacity = 1u << 18;
	std::string note;
};

// "sh4,g2dma,arm7,internal,mixer,reios,unknown" or "all".
std::uint32_t parseAicaWriterList(std::string_view list);
std::string aicaWriterListToString(std::uint32_t writers);

// Every bus enabled; SH-4 limited to calls, returns, and exceptions so a
// default capture is useful without producing millions of rows per second.
RecorderConfig defaultRecorderConfig();

// "sh4,maple,pvr-ta,pvr-draw,pvr-present,gdrom,gdrom-hw,aica,cdda" or "all".
std::uint32_t parseBusList(std::string_view list);
// "instruction-begin,instruction,instruction-abort,memory-read,memory-write,
//  exception,call,return" or "all".
std::uint64_t parseSh4TypeList(std::string_view list);
std::string busListToString(std::uint32_t buses);
std::string sh4TypeListToString(std::uint64_t types);

// JSON shape (all keys optional, unknown keys rejected):
// { "buses": "sh4,maple", "sh4": { "types": "call,return", "backend": "any",
//   "pc_start": 0x8c010000, "pc_end": 0x8c01ffff,
//   "mem_start": 0x8c000000, "mem_end": 0x8cffffff },
//   "maple": { "bus": 0, "port": 5, "command": 9 },
//   "aica": { "writers": "sh4,g2dma,reios" },
//   "rows": { "texture_bytes": false, ... }, "queue_capacity": 65536,
//   "note": "..." }
RecorderConfig recorderConfigFromJson(const nlohmann::json& json);
nlohmann::json recorderConfigToJson(const RecorderConfig& config);

} // namespace research::workbench

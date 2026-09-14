#include "research/workbench/workbench_config.h"

#include "research/aica_observation.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>
#include <vector>

namespace research::workbench
{
namespace
{

struct NamedBit
{
	const char *name;
	std::uint64_t bit;
};

const NamedBit BusNames[] = {
	{"sh4", BusSh4}, {"maple", BusMaple}, {"pvr-ta", BusPvrTa},
	{"pvr-draw", BusPvrDraw}, {"pvr-present", BusPvrPresentation},
	{"gdrom", BusGdrom}, {"gdrom-hw", BusGdromHardware}, {"aica", BusAica},
	{"cdda", BusCdda},
};

const NamedBit Sh4TypeNames[] = {
	{"instruction-begin", sh4ObservationTypeBit(Sh4ObservationType::InstructionBegin)},
	{"instruction", sh4ObservationTypeBit(Sh4ObservationType::InstructionEnd)},
	{"instruction-abort", sh4ObservationTypeBit(Sh4ObservationType::InstructionAbort)},
	{"memory-read", sh4ObservationTypeBit(Sh4ObservationType::MemoryRead)},
	{"memory-write", sh4ObservationTypeBit(Sh4ObservationType::MemoryWrite)},
	{"exception", sh4ObservationTypeBit(Sh4ObservationType::Exception)},
	{"call", sh4ObservationTypeBit(Sh4ObservationType::Call)},
	{"return", sh4ObservationTypeBit(Sh4ObservationType::Return)},
};

const NamedBit AicaWriterNames[] = {
	{"unknown", 1u << static_cast<unsigned>(AicaWriter::Unknown)},
	{"sh4", 1u << static_cast<unsigned>(AicaWriter::Sh4Direct)},
	{"g2dma", 1u << static_cast<unsigned>(AicaWriter::Sh4G2Dma)},
	{"arm7", 1u << static_cast<unsigned>(AicaWriter::Arm7)},
	{"internal", 1u << static_cast<unsigned>(AicaWriter::AicaInternalDma)},
	{"mixer", 1u << static_cast<unsigned>(AicaWriter::Mixer)},
	{"reios", 1u << static_cast<unsigned>(AicaWriter::ReiosHle)},
};
constexpr std::uint32_t AllAicaWriters = 0x7f;

std::vector<std::string> splitList(std::string_view list)
{
	std::vector<std::string> items;
	std::string current;
	for (const char c : list)
	{
		if (c == ',' || c == ';' || c == ' ')
		{
			if (!current.empty())
				items.push_back(current);
			current.clear();
			continue;
		}
		current.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	}
	if (!current.empty())
		items.push_back(current);
	return items;
}

template<std::size_t N>
std::uint64_t parseBits(std::string_view list, const NamedBit (&names)[N],
		std::uint64_t all, const char *what)
{
	std::uint64_t bits = 0;
	for (const std::string& item : splitList(list))
	{
		if (item == "all")
		{
			bits |= all;
			continue;
		}
		if (item == "none")
			continue;
		const auto found = std::find_if(std::begin(names), std::end(names),
				[&item](const NamedBit& named) { return item == named.name; });
		if (found == std::end(names))
			throw std::invalid_argument(std::string("unknown ") + what + ": " + item);
		bits |= found->bit;
	}
	return bits;
}

template<std::size_t N>
std::string bitsToString(std::uint64_t bits, const NamedBit (&names)[N])
{
	std::string text;
	for (const NamedBit& named : names)
	{
		if ((bits & named.bit) == 0)
			continue;
		if (!text.empty())
			text.push_back(',');
		text += named.name;
	}
	return text.empty() ? "none" : text;
}

std::uint64_t jsonUnsigned(const nlohmann::json& value, const char *key,
		std::uint64_t maximum)
{
	std::uint64_t result;
	if (value.is_number_unsigned())
		result = value.get<std::uint64_t>();
	else if (value.is_number_integer() && value.get<std::int64_t>() >= 0)
		result = static_cast<std::uint64_t>(value.get<std::int64_t>());
	else if (value.is_string())
		result = std::stoull(value.get<std::string>(), nullptr, 0);
	else
		throw std::invalid_argument(std::string(key) + " must be a non-negative integer");
	if (result > maximum)
		throw std::invalid_argument(std::string(key) + " is out of range");
	return result;
}

void rejectUnknownKeys(const nlohmann::json& object, std::initializer_list<const char *> allowed,
		const char *where)
{
	for (auto it = object.begin(); it != object.end(); ++it)
	{
		const bool known = std::any_of(allowed.begin(), allowed.end(),
				[&it](const char *key) { return it.key() == key; });
		if (!known)
			throw std::invalid_argument(std::string("unknown key '") + it.key() + "' in " + where);
	}
}

void parseSh4(const nlohmann::json& json, Sh4ObservationFilter& filter)
{
	rejectUnknownKeys(json, {"types", "backend", "pc_start", "pc_end", "mem_start", "mem_end"}, "sh4");
	if (json.contains("types"))
		filter.typeMask = parseSh4TypeList(json.at("types").get<std::string>());
	if (json.contains("backend"))
	{
		const std::string backend = json.at("backend").get<std::string>();
		if (backend == "any")
			filter.backendMask = AllSh4ObservationBackends;
		else if (backend == "interpreter")
			filter.backendMask = sh4ObservationBackendBit(Sh4ObservationBackend::Interpreter);
		else if (backend == "dynarec")
			filter.backendMask = sh4ObservationBackendBit(Sh4ObservationBackend::Dynarec);
		else
			throw std::invalid_argument("sh4.backend must be any, interpreter, or dynarec");
	}
	if (json.contains("pc_start") != json.contains("pc_end"))
		throw std::invalid_argument("sh4.pc_start and sh4.pc_end must be given together");
	if (json.contains("pc_start"))
	{
		filter.hasInstructionPcRange = true;
		filter.instructionPcStart = static_cast<std::uint32_t>(
				jsonUnsigned(json.at("pc_start"), "sh4.pc_start", 0xffffffffu));
		filter.instructionPcEndExclusive = jsonUnsigned(json.at("pc_end"), "sh4.pc_end",
				0xffffffffu) + 1;
	}
	if (json.contains("mem_start") != json.contains("mem_end"))
		throw std::invalid_argument("sh4.mem_start and sh4.mem_end must be given together");
	if (json.contains("mem_start"))
	{
		filter.hasMemoryRange = true;
		filter.memoryStart = static_cast<std::uint32_t>(
				jsonUnsigned(json.at("mem_start"), "sh4.mem_start", 0xffffffffu));
		filter.memoryEndExclusive = jsonUnsigned(json.at("mem_end"), "sh4.mem_end",
				0xffffffffu) + 1;
	}
}

void parseMaple(const nlohmann::json& json, MapleObservationFilter& filter)
{
	rejectUnknownKeys(json, {"bus", "port", "bus_mask", "port_mask", "command"}, "maple");
	if (json.contains("bus"))
		filter.busMask = static_cast<std::uint8_t>(
				1u << jsonUnsigned(json.at("bus"), "maple.bus", 3));
	if (json.contains("port"))
		filter.portMask = static_cast<std::uint8_t>(
				1u << jsonUnsigned(json.at("port"), "maple.port", 5));
	if (json.contains("bus_mask"))
		filter.busMask = static_cast<std::uint8_t>(
				jsonUnsigned(json.at("bus_mask"), "maple.bus_mask", 0x0f));
	if (json.contains("port_mask"))
		filter.portMask = static_cast<std::uint8_t>(
				jsonUnsigned(json.at("port_mask"), "maple.port_mask", 0x3f));
	if (filter.busMask == 0 || filter.portMask == 0)
		throw std::invalid_argument("maple bus/port masks must select at least one bus and port");
	if (json.contains("command"))
	{
		filter.hasCommand = true;
		filter.command = static_cast<std::uint8_t>(
				jsonUnsigned(json.at("command"), "maple.command", 255));
	}
}

void parseAica(const nlohmann::json& json, AicaRecordFilter& filter)
{
	rejectUnknownKeys(json, {"writers"}, "aica");
	if (json.contains("writers"))
		filter.writerMask = parseAicaWriterList(json.at("writers").get<std::string>());
}

void parseRows(const nlohmann::json& json, RowOptions& rows)
{
	rejectUnknownKeys(json, {"texture_bytes", "draw_vertices", "vram_writes",
			"vram_write_bytes", "framebuffer_bytes", "sector_bytes", "sample_frames"}, "rows");
	const auto flag = [&json](const char *key, bool& target) {
		if (json.contains(key))
			target = json.at(key).get<bool>();
	};
	flag("texture_bytes", rows.storeTextureBytes);
	flag("draw_vertices", rows.storeDrawVertices);
	flag("vram_writes", rows.recordVramWrites);
	flag("vram_write_bytes", rows.storeVramWriteBytes);
	flag("framebuffer_bytes", rows.storeFramebufferBytes);
	flag("sector_bytes", rows.storeSectorBytes);
	flag("sample_frames", rows.recordSampleFrames);
}

} // namespace

RecorderConfig defaultRecorderConfig()
{
	RecorderConfig config;
	config.sh4.typeMask = sh4ObservationTypeBit(Sh4ObservationType::Call)
			| sh4ObservationTypeBit(Sh4ObservationType::Return)
			| sh4ObservationTypeBit(Sh4ObservationType::Exception);
	config.aica.writerMask = parseAicaWriterList("sh4,g2dma,internal,reios");
	return config;
}

std::uint32_t parseAicaWriterList(std::string_view list)
{
	return static_cast<std::uint32_t>(parseBits(list, AicaWriterNames, AllAicaWriters, "aica writer"));
}

std::string aicaWriterListToString(std::uint32_t writers)
{
	return bitsToString(writers, AicaWriterNames);
}

std::uint32_t parseBusList(std::string_view list)
{
	return static_cast<std::uint32_t>(parseBits(list, BusNames, AllWorkbenchBuses, "bus"));
}

std::uint64_t parseSh4TypeList(std::string_view list)
{
	return parseBits(list, Sh4TypeNames, AllSh4ObservationTypes, "sh4 event type");
}

std::string busListToString(std::uint32_t buses)
{
	return bitsToString(buses, BusNames);
}

std::string sh4TypeListToString(std::uint64_t types)
{
	return bitsToString(types, Sh4TypeNames);
}

RecorderConfig recorderConfigFromJson(const nlohmann::json& json)
{
	if (!json.is_object())
		throw std::invalid_argument("recorder config must be a JSON object");
	rejectUnknownKeys(json, {"buses", "sh4", "maple", "aica", "rows", "queue_capacity", "note"},
			"recorder config");
	RecorderConfig config = defaultRecorderConfig();
	if (json.contains("buses"))
		config.buses = parseBusList(json.at("buses").get<std::string>());
	if (json.contains("sh4"))
		parseSh4(json.at("sh4"), config.sh4);
	if (json.contains("maple"))
		parseMaple(json.at("maple"), config.maple);
	if (json.contains("aica"))
		parseAica(json.at("aica"), config.aica);
	if (json.contains("rows"))
		parseRows(json.at("rows"), config.rows);
	if (json.contains("queue_capacity"))
		config.queueCapacity = static_cast<std::size_t>(
				jsonUnsigned(json.at("queue_capacity"), "queue_capacity", 1u << 24));
	if (config.queueCapacity < 16)
		throw std::invalid_argument("queue_capacity must be at least 16");
	if (json.contains("note"))
		config.note = json.at("note").get<std::string>();
	return config;
}

nlohmann::json recorderConfigToJson(const RecorderConfig& config)
{
	nlohmann::json sh4 {
		{"types", sh4TypeListToString(config.sh4.typeMask)},
		{"backend", config.sh4.backendMask == AllSh4ObservationBackends ? "any"
				: config.sh4.backendMask == sh4ObservationBackendBit(Sh4ObservationBackend::Dynarec)
						? "dynarec" : "interpreter"},
	};
	if (config.sh4.hasInstructionPcRange)
	{
		sh4["pc_start"] = config.sh4.instructionPcStart;
		sh4["pc_end"] = config.sh4.instructionPcEndExclusive - 1;
	}
	if (config.sh4.hasMemoryRange)
	{
		sh4["mem_start"] = config.sh4.memoryStart;
		sh4["mem_end"] = config.sh4.memoryEndExclusive - 1;
	}
	nlohmann::json maple = nlohmann::json::object();
	if (config.maple.hasCommand)
		maple["command"] = config.maple.command;
	maple["bus_mask"] = config.maple.busMask;
	maple["port_mask"] = config.maple.portMask;
	return nlohmann::json {
		{"buses", busListToString(config.buses)},
		{"sh4", std::move(sh4)},
		{"maple", std::move(maple)},
		{"aica", {{"writers", aicaWriterListToString(config.aica.writerMask)}}},
		{"rows", {
			{"texture_bytes", config.rows.storeTextureBytes},
			{"draw_vertices", config.rows.storeDrawVertices},
			{"vram_writes", config.rows.recordVramWrites},
			{"vram_write_bytes", config.rows.storeVramWriteBytes},
			{"framebuffer_bytes", config.rows.storeFramebufferBytes},
			{"sector_bytes", config.rows.storeSectorBytes},
			{"sample_frames", config.rows.recordSampleFrames},
		}},
		{"queue_capacity", config.queueCapacity},
		{"note", config.note},
	};
}

} // namespace research::workbench

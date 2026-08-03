#include "research/gdrom_artifact.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace research
{
namespace
{

constexpr std::array<std::uint8_t, 8> Magic {'F','C','G','D','R','O','M','1'};
constexpr std::uint32_t EventHeaderSize = 32;

[[noreturn]] void invalid(const std::string& message)
{ throw std::runtime_error("invalid GD-ROM artifact: " + message); }
void require(bool value, const std::string& message) { if (!value) invalid(message); }

class Reader
{
public:
	Reader(const std::uint8_t* data, std::size_t size) : data(data), size(size) {}
	std::uint8_t byte() { need(1); return data[position++]; }
	std::uint32_t u32() { need(4); std::uint32_t v = 0; for (unsigned i=0;i<4;++i) v |= std::uint32_t(data[position++]) << (8*i); return v; }
	std::uint64_t u64() { need(8); std::uint64_t v = 0; for (unsigned i=0;i<8;++i) v |= std::uint64_t(data[position++]) << (8*i); return v; }
	std::vector<std::uint8_t> bytes(std::size_t count) { need(count); std::vector<std::uint8_t> v(data+position,data+position+count); position += count; return v; }
	void skip(std::size_t count) { need(count); position += count; }
	std::size_t tell() const { return position; }
	std::size_t remaining() const { return size - position; }
private:
	void need(std::size_t count) { if (count > size - position) invalid("binary input is truncated"); }
	const std::uint8_t* data; std::size_t size; std::size_t position = 0;
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
	std::uint32_t crc=0xffffffffu;
	for(std::size_t i=0;i<size;++i){ crc^=data[i]; for(unsigned b=0;b<8;++b) crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u))); }
	return ~crc;
}

struct GdiTrack
{
	std::uint32_t number=0, startFad=0, type=0, sectorSize=0;
	std::uint64_t offset=0, sectors=0;
	std::filesystem::path path;
};

std::vector<GdiTrack> authenticateGdi(const IdentityManifest& identity)
{
	require(identity.mediaKind == "gdi" && !identity.mediaTracks.empty(), "identity is not a GDI with tracks");
	require(!identity.mediaSourcePath.empty(), "identity has no GDI descriptor path");
	require(std::filesystem::file_size(identity.mediaSourcePath) == identity.mediaSourceSize,
			"GDI descriptor size differs from identity");
	require(sha256Equal(hashFileExact(identity.mediaSourcePath, identity.mediaSourceSize),
			identity.mediaSourceDigest), "GDI descriptor digest differs from identity");
	std::ifstream input(identity.mediaSourcePath);
	require(static_cast<bool>(input), "cannot open GDI descriptor");
	std::size_t count=0; input >> count;
	require(count == identity.mediaTracks.size(), "GDI track count differs from identity");
	std::string line; std::getline(input,line);
	std::vector<GdiTrack> tracks;
	for(std::size_t i=0;i<count;++i){
		std::getline(input,line); require(!line.empty(), "GDI track line is missing");
		std::istringstream fields(line); std::uint32_t lba=0; std::string file;
		GdiTrack track;
		fields >> track.number >> lba >> track.type >> track.sectorSize >> std::quoted(file) >> track.offset;
		require(!fields.fail(), "GDI track line is malformed");
		fields >> std::ws; require(fields.eof(), "GDI track line has trailing fields");
		track.startFad = lba + 150;
		const MediaTrackIdentity& expected=identity.mediaTracks[i];
		require(!expected.path.empty(), "identity has a track without a path");
		require(track.number==expected.track && track.startFad==expected.startFad
				&& track.sectorSize==expected.sectorSize && track.offset==expected.offset
				&& std::filesystem::path(file).filename()==expected.path.filename(),
				"GDI track mapping differs from identity");
		require(track.type == 0 || track.type == 4, "GDI track type is unsupported");
		require(std::filesystem::file_size(expected.path)==expected.size,
				"track size differs from identity");
		require(sha256Equal(hashFileExact(expected.path, expected.size), expected.digest),
				"track digest differs from identity");
		require(expected.size >= expected.offset && track.sectorSize != 0
				&& (expected.size-expected.offset)%track.sectorSize==0,
				"track length is not an exact sector sequence");
		track.sectors=(expected.size-expected.offset)/track.sectorSize;
		track.path=expected.path; tracks.push_back(std::move(track));
	}
	return tracks;
}

std::vector<std::uint8_t> readUserSectors(const std::vector<GdiTrack>& tracks,
		std::uint32_t fad, std::uint32_t count)
{
	const GdiTrack* selected=nullptr;
	for(const auto& track:tracks)
		if(track.type==4 && track.startFad<=fad && std::uint64_t(fad-track.startFad)<track.sectors) selected=&track;
	require(selected!=nullptr, "read FAD is outside an authenticated data track");
	require(std::uint64_t(fad-selected->startFad)+count<=selected->sectors,
			"read crosses the authenticated track boundary");
	const std::uint64_t userOffset=selected->sectorSize==2352?16:0;
	require((selected->sectorSize==2352 || selected->sectorSize==2048)
			&& userOffset+2048<=selected->sectorSize, "data sector layout is unsupported");
	std::ifstream input(selected->path,std::ios::binary); require(static_cast<bool>(input),"cannot open data track");
	std::vector<std::uint8_t> result(std::size_t(count)*2048);
	for(std::uint32_t i=0;i<count;++i){
		const std::uint64_t sector=std::uint64_t(fad-selected->startFad)+i;
		const std::uint64_t offset=selected->offset+sector*selected->sectorSize+userOffset;
		input.seekg(static_cast<std::streamoff>(offset));
		input.read(reinterpret_cast<char*>(result.data()+std::size_t(i)*2048),2048);
		require(input.gcount()==2048,"data track sector is truncated");
	}
	return result;
}

struct ActiveCommand
{
	bool active=false; std::uint64_t generation=0,nextChunk=0,transferred=0;
	std::uint32_t request=0,nextFad=0,remaining=0,nextDestination=0;
};

} // namespace

GdromArtifactSummary validateGdromArtifactFile(const std::filesystem::path& artifact,
		const IdentityManifest& identity, const Sha256Digest& replayDigest,
		std::uint64_t maximumBytes, std::uint64_t maximumEvents)
{
	const auto file=readFileExact(artifact,maximumBytes);
	require(file.size()>=GdromArtifactHeaderSize,"file is smaller than its header");
	require(std::equal(Magic.begin(),Magic.end(),file.begin()),"magic is invalid");
	Reader header(file.data()+8,GdromArtifactHeaderSize-8);
	require(header.u32()==GdromArtifactSchemaVersion && header.u32()==GdromArtifactHeaderSize,
			"schema/header size is invalid");
	require(header.u32()==0x01020304 && header.u32()==1,"artifact is incomplete");
	GdromArtifactSummary summary;
	const auto backend=header.u32(); header.u32();
	require(backend==1 || backend==2,"backend is invalid"); summary.binding.backend=static_cast<Sh4ObservationBackend>(backend);
	summary.eventCount=header.u64(); summary.payloadBytes=header.u64(); summary.droppedEvents=header.u64();
	summary.startTick=header.u64(); summary.endTick=header.u64();
	for(auto& b:summary.binding.identityDigest)b=header.byte();
	for(auto& b:summary.binding.replayDigest)b=header.byte();
	for(auto& b:summary.payloadDigest)b=header.byte();
	for(auto& count:summary.typeCounts)count=header.u64();
	require(summary.eventCount>0 && summary.eventCount<=maximumEvents && summary.droppedEvents==0,
			"event count or dropped count is invalid");
	require(summary.payloadBytes==file.size()-GdromArtifactHeaderSize,"payload size is inconsistent");
	require(sha256Equal(summary.binding.identityDigest,identity.digest)
			&& sha256Equal(summary.binding.replayDigest,replayDigest),"artifact binding is incorrect");
	require(sha256Equal(summary.payloadDigest,sha256(file.data()+GdromArtifactHeaderSize,summary.payloadBytes)),
			"payload digest is incorrect");
	std::uint32_t storedCrc=0; for(unsigned i=0;i<4;++i)storedCrc|=std::uint32_t(file[GdromArtifactHeaderSize-4+i])<<(8*i);
	require(storedCrc==crc32(file.data(),GdromArtifactHeaderSize-4),"header CRC is incorrect");
	const auto tracks=authenticateGdi(identity);
	Reader reader(file.data()+GdromArtifactHeaderSize,summary.payloadBytes);
	ActiveCommand command; std::array<std::uint64_t,5> counts{}; std::uint64_t previousEmission=0,previousTick=0;
	for(std::uint64_t index=0;index<summary.eventCount;++index){
		const std::size_t eventStart=reader.tell(); const auto typeValue=reader.u32(); const auto eventSize=reader.u32();
		require(typeValue>=1 && typeValue<=5 && eventSize>=EventHeaderSize && eventSize<=reader.remaining()+8,
				"event header is invalid");
		require(reader.u64()==index,"artifact event ordinal is not contiguous");
		const auto emission=reader.u64(),tick=reader.u64();
		if(index!=0){ require(emission==previousEmission+1,"source emission ordinal is not contiguous"); require(tick>=previousTick,"event tick moved backwards"); }
		previousEmission=emission; previousTick=tick;
		const auto type=static_cast<GdromObservationType>(typeValue); ++counts[typeValue-1];
		const auto generation=reader.u64();
		if(type==GdromObservationType::CommandBegin){
			require(!command.active && generation!=0 && reader.u32()==static_cast<std::uint32_t>(GdromPath::ReiosHle),"command overlap or path is invalid");
			require(reader.byte()==1 && reader.byte()==backend,"command owner is absent or backend differs");
			reader.skip(2); reader.skip(2); reader.skip(2); const auto ownerGeneration=reader.u64(); const auto ownerTick=reader.u64(); const auto pc=reader.u32(); reader.u32();
			require(ownerGeneration!=0 && ownerTick<=tick && (pc&1)==0,"command owner is invalid");
			command={}; command.active=true; command.generation=generation; command.request=reader.u32();
			const auto gdCommand=reader.u32(); std::array<std::uint32_t,4> params{}; for(auto& p:params)p=reader.u32();
			require(gdCommand==0x11 && params[1]>0 && params[3]==0,"REIOS command is not a bounded DMAREAD");
			command.nextFad=params[0]&0xffffff; command.remaining=params[1]; command.nextDestination=params[2];
			const std::uint64_t bytes=std::uint64_t(params[1])*2048;
			require(command.nextDestination>=0x0c000000u && std::uint64_t(command.nextDestination)+bytes<=0x0d000000ull,
					"DMA destination is outside Dreamcast RAM");
		} else if(type==GdromObservationType::TransferChunk){
			require(command.active && generation==command.generation,"chunk has no matching command");
			const std::uint64_t ordinal=reader.u64();
			const std::uint32_t fad=reader.u32(),sectors=reader.u32(),
					destination=reader.u32(),byteCount=reader.u32();
			require(ordinal==command.nextChunk++ && sectors>0 && sectors<=5 && sectors<=command.remaining
					&& fad==command.nextFad && destination==command.nextDestination
					&& byteCount==std::uint64_t(sectors)*2048,"chunk geometry is inconsistent with REIOS");
			const auto observed=reader.bytes(byteCount); const auto expected=readUserSectors(tracks,fad,sectors);
			require(observed==expected,"chunk bytes differ from independent GDI decoding");
			command.nextFad+=sectors; command.remaining-=sectors; command.nextDestination+=byteCount; command.transferred+=byteCount;
		} else if(type==GdromObservationType::Complete){
			require(command.active && generation==command.generation && command.remaining==0,"completion has an incomplete or unrelated command");
			require(reader.u32()==static_cast<std::uint32_t>(GdromCompletionMechanism::Status),"REIOS DMAREAD completion is not status-based"); reader.u32();
			require(reader.u64()==command.transferred,"completion byte count is incorrect"); command={};
		} else if(type==GdromObservationType::Abort){
			require(command.active && generation==command.generation && reader.u32()==command.request,"abort has no matching request"); reader.u32();
			require(reader.u64()==command.transferred,"abort byte count is incorrect"); command={};
		} else {
			reader.u64(); command={};
		}
		require(reader.tell()==eventStart+eventSize,"event size does not match its payload");
	}
	require(reader.remaining()==0 && !command.active,"payload has trailing bytes or an incomplete command");
	require(counts==summary.typeCounts,"header event counts differ from payload");
	require(summary.startTick<=summary.endTick,"header tick range is invalid");
	return summary;
}

} // namespace research

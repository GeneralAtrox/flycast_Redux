#include "research/cdda_artifact.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace research
{
namespace
{

constexpr std::array<std::uint8_t,8> Magic {'F','C','C','D','D','A','0','2'};
constexpr std::uint32_t RecordHeaderSize=32;

bool isControlCommand(std::uint32_t path,std::uint32_t command)
{
	if(path==static_cast<std::uint32_t>(CddaControlPath::ReiosHle))
		return command==0x14||command==0x15||command==0x16||command==0x17||
				command==0x1b||command==0x21;
	if(path==static_cast<std::uint32_t>(CddaControlPath::GdromPacket))
		return command==0x20||command==0x21;
	return false;
}

[[noreturn]] void invalid(const char* message)
{ throw std::runtime_error(std::string("invalid CD-DA artifact: ")+message); }
void require(bool value,const char* message){if(!value)invalid(message);}
std::uint32_t crc32(const std::uint8_t* data,std::size_t size){std::uint32_t crc=0xffffffffu;
	for(std::size_t i=0;i<size;++i){crc^=data[i];for(unsigned bit=0;bit<8;++bit)
		crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}return ~crc;}

class Reader
{
public:
	Reader(const std::uint8_t* data,std::size_t size):begin(data),data(data),end(data+size){}
	std::uint8_t byte(){need(1);return *data++;}
	std::uint16_t u16(){std::uint16_t v=0;for(unsigned i=0;i<2;++i)v|=std::uint16_t(byte())<<(8*i);return v;}
	std::uint32_t u32(){std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t(byte())<<(8*i);return v;}
	std::uint64_t u64(){std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(byte())<<(8*i);return v;}
	std::vector<std::uint8_t> bytes(std::size_t n){need(n);std::vector<std::uint8_t> out(data,data+n);data+=n;return out;}
	void skip(std::size_t n){need(n);data+=n;}
	std::size_t tell()const{return static_cast<std::size_t>(data-begin);}
	std::size_t remaining()const{return static_cast<std::size_t>(end-data);}
private:void need(std::size_t n){if(n>remaining())invalid("record is truncated");}
	const std::uint8_t* begin;const std::uint8_t* data;const std::uint8_t* end;
};

std::vector<std::uint8_t> readFile(const std::filesystem::path& path,std::uint64_t maximum)
{
	const auto size=std::filesystem::file_size(path);require(size<=maximum,"file exceeds byte limit");
	std::ifstream input(path,std::ios::binary);require(static_cast<bool>(input),"cannot open artifact");
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
	input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
	require(static_cast<std::size_t>(input.gcount())==bytes.size(),"artifact is truncated");return bytes;
}

bool equal(const CddaDriveState& a,const CddaDriveState& b)
{
	return a.status==b.status&&a.repeats==b.repeats&&a.currentFad==b.currentFad&&
			a.startFad==b.startFad&&a.endFad==b.endFad;
}

CddaDriveState driveState(Reader& reader)
{
	CddaDriveState state;state.status=reader.u32();state.repeats=reader.u32();
	state.currentFad=reader.u32();state.startFad=reader.u32();state.endFad=reader.u32();return state;
}

struct Owner
{
	bool valid=false;Sh4ObservationBackend backend=Sh4ObservationBackend::Interpreter;
	std::uint64_t generation=0,tick=0;std::uint32_t pc=0,pr=0;std::uint16_t opcode=0,depth=0;
};
Owner owner(Reader& reader)
{
	Owner value;value.valid=reader.byte()!=0;value.backend=static_cast<Sh4ObservationBackend>(reader.byte());
	reader.u16();value.generation=reader.u64();value.tick=reader.u64();value.pc=reader.u32();
	value.pr=reader.u32();value.opcode=reader.u16();value.depth=reader.u16();return value;
}
bool equal(const Owner& a,const Owner& b)
{
	return a.valid==b.valid&&a.backend==b.backend&&a.generation==b.generation&&
			a.tick==b.tick&&a.pc==b.pc&&a.pr==b.pr&&a.opcode==b.opcode&&a.depth==b.depth;
}

struct GdiTrack
{
	std::uint32_t number=0,startFad=0,type=0,sectorSize=0;
	std::uint64_t offset=0,sectors=0;std::filesystem::path path;
};

std::vector<GdiTrack> authenticateGdi(const IdentityManifest& identity)
{
	require(identity.mediaKind=="gdi"&&!identity.mediaTracks.empty(),"identity is not a GDI with tracks");
	require(!identity.mediaSourcePath.empty(),"identity has no GDI descriptor path");
	require(std::filesystem::file_size(identity.mediaSourcePath)==identity.mediaSourceSize,
			"GDI descriptor size differs from identity");
	require(sha256Equal(hashFileExact(identity.mediaSourcePath,identity.mediaSourceSize),
			identity.mediaSourceDigest),"GDI descriptor digest differs from identity");
	std::ifstream input(identity.mediaSourcePath);require(static_cast<bool>(input),"cannot open GDI descriptor");
	std::size_t count=0;input>>count;require(count==identity.mediaTracks.size(),"GDI track count differs from identity");
	std::string line;std::getline(input,line);std::vector<GdiTrack> tracks;
	for(std::size_t index=0;index<count;++index){std::getline(input,line);require(!line.empty(),"GDI track line is missing");
		std::istringstream fields(line);std::uint32_t lba=0;std::string file;GdiTrack track;
		fields>>track.number>>lba>>track.type>>track.sectorSize>>std::quoted(file)>>track.offset;
		require(!fields.fail(),"GDI track line is malformed");fields>>std::ws;require(fields.eof(),"GDI track line has trailing fields");
		track.startFad=lba+150;const auto& expected=identity.mediaTracks[index];
		require(!expected.path.empty(),"identity has a track without a path");
		require(track.number==expected.track&&track.startFad==expected.startFad&&
				track.sectorSize==expected.sectorSize&&track.offset==expected.offset&&
				std::filesystem::path(file).filename()==expected.path.filename(),
				"GDI track mapping differs from identity");
		require(track.type==0||track.type==4,"GDI track type is unsupported");
		require(std::filesystem::file_size(expected.path)==expected.size,"track size differs from identity");
		require(sha256Equal(hashFileExact(expected.path,expected.size),expected.digest),"track digest differs from identity");
		require(expected.size>=expected.offset&&track.sectorSize!=0&&
				(expected.size-expected.offset)%track.sectorSize==0,"track length is not an exact sector sequence");
		track.sectors=(expected.size-expected.offset)/track.sectorSize;track.path=expected.path;
		tracks.push_back(std::move(track));}
	return tracks;
}

std::vector<std::uint8_t> readAudioSector(const std::vector<GdiTrack>& tracks,std::uint32_t fad)
{
	const GdiTrack* selected=nullptr;for(const auto& track:tracks)
		if(track.type==0&&track.startFad<=fad&&std::uint64_t(fad-track.startFad)<track.sectors)selected=&track;
	require(selected!=nullptr,"sector FAD is outside an authenticated audio track");
	require(selected->sectorSize==2352,"audio track sector size is not 2352");
	const std::uint64_t offset=selected->offset+std::uint64_t(fad-selected->startFad)*2352;
	std::ifstream input(selected->path,std::ios::binary);require(static_cast<bool>(input),"cannot open audio track");
	input.seekg(static_cast<std::streamoff>(offset));std::vector<std::uint8_t> bytes(2352);
	input.read(reinterpret_cast<char*>(bytes.data()),2352);require(input.gcount()==2352,"audio sector is truncated");return bytes;
}

std::int16_t pcm16(const std::vector<std::uint8_t>& bytes,std::size_t offset)
{ return static_cast<std::int16_t>(std::uint16_t(bytes[offset])|(std::uint16_t(bytes[offset+1])<<8)); }

struct Control
{
	std::uint32_t path=0,request=0,command=0;std::array<std::uint32_t,4> parameters{};
	Owner initiator;bool applied=false,success=false;CddaDriveState before,after;
};

void validateSectorTransition(const CddaDriveState& before,const CddaDriveState& after,
		std::uint32_t fad,bool readSuccessful)
{
	if(before.status!=1){require(!readSuccessful&&equal(before,after),"non-playing sector changed drive state");return;}
	require(fad==before.currentFad,"sector FAD differs from pre-read current FAD");
	if(!readSuccessful){CddaDriveState expected=before;--expected.currentFad;expected.status=3;
		require(equal(expected,after),"failed sector transition differs from Flycast behavior");return;}
	require(before.startFad<before.endFad&&fad>=before.startFad&&fad<before.endFad,
			"successful sector is outside the applied playback range");
	CddaDriveState expected=before;++expected.currentFad;
	if(expected.currentFad>=expected.endFad){if(expected.repeats==0)expected.status=3;
		else{if(expected.repeats!=15)--expected.repeats;expected.currentFad=expected.startFad;}}
	require(equal(expected,after),"successful sector transition differs from Flycast behavior");
}

std::array<std::uint8_t,12> packetBytes(const Control& control)
{
	require(control.parameters[3]==0,"GD-ROM packet reserved parameter is nonzero");
	std::array<std::uint8_t,12> packet{};
	for(std::size_t word=0;word<3;++word)
		for(std::size_t byte=0;byte<4;++byte)
			packet[word*4+byte]=static_cast<std::uint8_t>(
					control.parameters[word]>>(byte*8));
	require(packet[0]==control.command,"GD-ROM packet command differs from control");
	return packet;
}

std::uint32_t packetFad(const std::array<std::uint8_t,12>& packet,
		std::size_t offset,bool msf)
{
	if(msf)return std::uint32_t(packet[offset])*60*75+
			std::uint32_t(packet[offset+1])*75+packet[offset+2];
	return (std::uint32_t(packet[offset])<<16)|
			(std::uint32_t(packet[offset+1])<<8)|packet[offset+2];
}

// Disc::FillGDSession() defines the GD-ROM session lead-out used by
// libGDR_GetSessionInfo(..., 0). It is not the end of the last file-backed
// track, so an omitted CD_PLAY end address must reproduce this exact value.
constexpr std::uint32_t GdromSessionEndFad=549300;

bool validateAppliedControl(const Control& control,const CddaDriveState& before,
		const CddaDriveState& after)
{
	if(control.path==static_cast<std::uint32_t>(CddaControlPath::ReiosHle))
	{
		if(control.command==0x14||control.command==0x15)
		{
			require(after.status==1&&after.startFad<after.endFad,
					"successful REIOS PLAY did not establish a bounded playing range");
			return true;
		}
		return false;
	}
	require(control.path==static_cast<std::uint32_t>(CddaControlPath::GdromPacket),
			"applied control path is invalid");
	const auto packet=packetBytes(control);const auto parameterType=packet[1]&7u;
	CddaDriveState expected=before;
	if(control.command==0x20)
	{
		if(parameterType==1||parameterType==2)
		{
			const bool msf=parameterType==2;expected.currentFad=expected.startFad=
					packetFad(packet,2,msf);expected.endFad=packetFad(packet,8,msf);
			if(expected.endFad==0)expected.endFad=GdromSessionEndFad;
			expected.repeats=packet[6]&0x0fu;expected.status=1;
			require(expected.startFad<expected.endFad,
					"GD-ROM packet PLAY range is empty");
			require(equal(expected,after),
					"GD-ROM packet PLAY transition differs from packet bytes");
			return true;
		}
		require(parameterType==7,"GD-ROM packet PLAY parameter type is invalid");
		if(expected.status==2)
			expected.status=expected.currentFad>expected.endFad?3u:1u;
		require(equal(expected,after),
				"GD-ROM packet PLAY resume transition differs from packet bytes");
		return false;
	}
	require(control.command==0x21,"GD-ROM packet control command is invalid");
	if(expected.status==1)expected.status=2;
	if(parameterType==1||parameterType==2)
		expected.currentFad=expected.startFad=packetFad(packet,2,parameterType==2);
	else if(parameterType==3)
	{
		expected.currentFad=expected.startFad=150;expected.status=0;
	}
	else require(parameterType==4,"GD-ROM packet SEEK parameter type is invalid");
	require(equal(expected,after),
			"GD-ROM packet SEEK transition differs from packet bytes");
	return false;
}

} // namespace

CddaArtifactSummary validateCddaArtifactFile(const std::filesystem::path& artifact,
		const IdentityManifest& identity,const Sha256Digest& replayDigest,
		const Sha256Digest& configurationDigest,std::uint64_t maximumBytes,
		std::uint64_t maximumEvents)
{
	const auto file=readFile(artifact,maximumBytes);require(file.size()>=CddaArtifactHeaderSize,"file is smaller than its header");
	require(std::equal(Magic.begin(),Magic.end(),file.begin()),"magic is invalid");
	Reader header(file.data()+8,CddaArtifactHeaderSize-8);
	require(header.u32()==CddaArtifactSchemaVersion&&header.u32()==CddaArtifactHeaderSize,"schema/header size is invalid");
	require(header.u32()==0x01020304&&header.u32()==1,"artifact is incomplete");
	CddaArtifactSummary summary;const auto backend=header.u32();const auto flags=header.u32();
	require(backend==1||backend==2,"backend is invalid");summary.binding.backend=static_cast<Sh4ObservationBackend>(backend);
	summary.binding.dspEnabled=(flags&1)!=0;require((flags&~1u)==0,"header flags are invalid");
	summary.eventCount=header.u64();summary.payloadBytes=header.u64();summary.droppedCddaEvents=header.u64();
	summary.droppedAicaEvents=header.u64();summary.startTick=header.u64();summary.endTick=header.u64();
	summary.targetSampleFrames=header.u64();summary.sampleFrames=header.u64();summary.successfulSectors=header.u64();
	summary.contributingSampleFrames=header.u64();summary.appliedPlayControls=header.u64();
	for(auto& byte:summary.binding.identityDigest)byte=header.byte();for(auto& byte:summary.binding.replayDigest)byte=header.byte();
	for(auto& byte:summary.binding.configurationDigest)byte=header.byte();for(auto& byte:summary.payloadDigest)byte=header.byte();
	for(auto& byte:summary.pcmDigest)byte=header.byte();for(auto& count:summary.typeCounts)count=header.u64();
	require(summary.eventCount>0&&summary.eventCount<=maximumEvents&&summary.droppedCddaEvents==0&&summary.droppedAicaEvents==0,
			"event count or dropped count is invalid");
	require(summary.targetSampleFrames>0&&summary.targetSampleFrames<=MaximumCddaSampleFrames&&
			summary.sampleFrames==summary.targetSampleFrames,"sample target is invalid or incomplete");
	require(summary.payloadBytes==file.size()-CddaArtifactHeaderSize,"payload size is inconsistent");
	require(sha256Equal(summary.binding.identityDigest,identity.digest)&&
			sha256Equal(summary.binding.replayDigest,replayDigest)&&
			sha256Equal(summary.binding.configurationDigest,configurationDigest),"artifact binding is incorrect");
	require(sha256Equal(summary.payloadDigest,sha256(file.data()+CddaArtifactHeaderSize,summary.payloadBytes)),"payload digest is incorrect");
	std::uint32_t storedCrc=0;for(unsigned i=0;i<4;++i)storedCrc|=std::uint32_t(file[CddaArtifactHeaderSize-4+i])<<(8*i);
	require(storedCrc==crc32(file.data(),CddaArtifactHeaderSize-4),"header CRC is incorrect");
	const auto tracks=authenticateGdi(identity);

	Reader reader(file.data()+CddaArtifactHeaderSize,summary.payloadBytes);
	// The checkpoint is structural only. Samples cannot use it as evidence until
	// its control generation is authenticated by an accepted/applied event.
	require(reader.u32()==0,"checkpoint record is missing");const auto checkpointSize=reader.u32();
	require(checkpointSize==RecordHeaderSize+2392,"checkpoint size is invalid");require(reader.u64()==0,"checkpoint ordinal is invalid");
	reader.u64();const auto checkpointTick=reader.u64();const auto checkpointFlags=reader.u32();
	require((checkpointFlags&~3u)==0,"checkpoint flags are invalid");const auto checkpointIndex=reader.u32();
	reader.u64();reader.u64();reader.u32();reader.u32();reader.u32();require(reader.u32()==2352,"checkpoint sector length is invalid");
	reader.skip(2352);require(checkpointIndex<=1176,"checkpoint CD-DA index is invalid");
	require(checkpointTick==summary.startTick,"checkpoint tick differs from header");

	std::map<std::uint64_t,Control> controls;std::map<std::uint64_t,std::vector<std::uint8_t>> sectors;
	std::map<std::uint64_t,std::uint16_t> nextFrames;std::optional<CddaDriveState> drive;
	std::array<std::uint64_t,4> counts{};std::uint64_t successful=0,contributing=0,plays=0,samples=0;
	std::uint64_t previousTick=checkpointTick,firstSampleOrdinal=0;Sha256 pcmHasher;
	for(std::uint64_t index=0;index<summary.eventCount;++index){const auto start=reader.tell();const auto type=reader.u32();const auto size=reader.u32();
		require(type>=1&&type<=4&&size>=RecordHeaderSize&&size<=reader.remaining()+8,"event header is invalid");
		require(reader.u64()==index+1,"artifact event ordinal is not contiguous");reader.u64();const auto tick=reader.u64();
		require(tick>=previousTick,"event tick moved backwards");previousTick=tick;++counts[type-1];
		if(type==1){const auto generation=reader.u64();require(generation!=0&&controls.find(generation)==controls.end(),"control generation is zero or repeated");
			Control control;control.path=reader.u32();control.request=reader.u32();control.command=reader.u32();reader.u32();
			for(auto& parameter:control.parameters)parameter=reader.u32();control.initiator=owner(reader);
			require(isControlCommand(control.path,control.command),"accepted control path or command is invalid");
			if(control.path==static_cast<std::uint32_t>(CddaControlPath::GdromPacket))
				(void)packetBytes(control);
			require(control.initiator.valid&&control.initiator.backend==summary.binding.backend&&control.initiator.generation!=0&&
					control.initiator.tick<=tick&&(control.initiator.pc&1)==0,"accepted control owner is invalid");controls.emplace(generation,control);
		}else if(type==2){const auto generation=reader.u64();const auto found=controls.find(generation);require(found!=controls.end()&&!found->second.applied,"applied control has no unique acceptance");
			Control copy;copy.path=reader.u32();copy.request=reader.u32();copy.command=reader.u32();copy.success=reader.u32()!=0;
			for(auto& parameter:copy.parameters)parameter=reader.u32();copy.initiator=owner(reader);copy.before=driveState(reader);copy.after=driveState(reader);
			require(copy.path==found->second.path&&copy.request==found->second.request&&copy.command==found->second.command&&
					copy.parameters==found->second.parameters&&equal(copy.initiator,found->second.initiator),"applied control differs from acceptance");
			if(drive)require(equal(*drive,copy.before),"applied control pre-state breaks drive sequence");
			if(copy.success){require(copy.after.status<=3,"applied control status is invalid");
				if(validateAppliedControl(copy,copy.before,copy.after))++plays;drive=copy.after;}
			else require(equal(copy.before,copy.after),"failed control changed drive state");
			found->second.applied=true;found->second.success=copy.success;found->second.before=copy.before;found->second.after=copy.after;
		}else if(type==3){const auto controlGeneration=reader.u64(),aicaGeneration=reader.u64();const auto fad=reader.u32();const bool success=reader.u32()!=0;
			const auto before=driveState(reader),after=driveState(reader);const auto length=reader.u32();require(length==2352,"sector length is invalid");const auto bytes=reader.bytes(length);
			const auto control=controls.find(controlGeneration);require(control!=controls.end()&&control->second.applied&&control->second.success,
					"sector has no successfully applied control");if(drive)require(equal(*drive,before),"sector pre-state breaks drive sequence");
			validateSectorTransition(before,after,fad,success);drive=after;
			if(success){require(aicaGeneration!=0&&sectors.find(aicaGeneration)==sectors.end(),"successful AICA sector generation is invalid");
				const auto expected=readAudioSector(tracks,fad);require(bytes==expected,"sector bytes differ from authenticated raw GDI audio");
				sectors.emplace(aicaGeneration,bytes);nextFrames.emplace(aicaGeneration,std::uint16_t{0});++successful;}
			else require(std::all_of(bytes.begin(),bytes.end(),[](std::uint8_t value){return value==0;}),"unsuccessful sector is not silent");
		}else{const auto sampleOrdinal=reader.u64();reader.u64();const auto generation=reader.u64();const auto frame=reader.u16();
			const bool dsp=reader.byte()!=0;reader.byte();reader.u32();reader.u32();const auto inputLeft=static_cast<std::int32_t>(reader.u32());
			const auto inputRight=static_cast<std::int32_t>(reader.u32());const auto contributionLeft=static_cast<std::int32_t>(reader.u32());
			const auto contributionRight=static_cast<std::int32_t>(reader.u32());reader.u32();reader.u32();
			const auto finalLeft=static_cast<std::int16_t>(reader.u16());const auto finalRight=static_cast<std::int16_t>(reader.u16());
			require(dsp==summary.binding.dspEnabled,"sample DSP mode differs from binding");const auto sector=sectors.find(generation);
			require(sector!=sectors.end(),"sample references an unauthenticated sector");auto next=nextFrames.find(generation);
			require(next!=nextFrames.end()&&frame==next->second&&frame<588,"sample frame sequence is not contiguous from zero");++next->second;
			require(inputLeft==pcm16(sector->second,frame*4)&&inputRight==pcm16(sector->second,frame*4+2),"sample input differs from raw audio sector");
			if(samples==0)firstSampleOrdinal=sampleOrdinal;else require(sampleOrdinal==firstSampleOrdinal+samples,"sample ordinal is not contiguous");
			std::array<std::uint8_t,4> pcm{static_cast<std::uint8_t>(finalLeft),static_cast<std::uint8_t>(finalLeft>>8),
					static_cast<std::uint8_t>(finalRight),static_cast<std::uint8_t>(finalRight>>8)};pcmHasher.update(pcm.data(),pcm.size());
			if(contributionLeft!=0||contributionRight!=0)++contributing;++samples;
		}
		require(reader.tell()==start+size,"event size does not match payload");}
	require(reader.remaining()==0,"payload has trailing bytes");require(counts==summary.typeCounts,"header event counts differ from payload");
	require(samples==summary.sampleFrames&&successful==summary.successfulSectors&&contributing==summary.contributingSampleFrames&&
			plays==summary.appliedPlayControls,"header semantic counts differ from payload");
	require(plays>0&&successful>0&&contributing>0,"artifact has no proven CD-DA playback contribution");
	require(sha256Equal(summary.pcmDigest,pcmHasher.finalize()),"final PCM digest differs from samples");
	require(previousTick==summary.endTick&&summary.startTick<=summary.endTick,"header tick range is invalid");return summary;
}

} // namespace research

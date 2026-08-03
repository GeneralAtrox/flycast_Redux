#include "research/cdda_artifact.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#endif

namespace research
{
namespace
{

constexpr std::array<std::uint8_t, 8> Magic {'F','C','C','D','D','A','0','2'};
constexpr std::uint32_t Complete = 1;
constexpr std::uint32_t RecordHeaderSize = 32;
constexpr std::uint32_t CheckpointRecordType = 0;

[[noreturn]] void invalid(const char* message)
{ throw std::runtime_error(std::string("invalid CD-DA artifact: ") + message); }
void require(bool value, const char* message) { if (!value) invalid(message); }
void u8(std::vector<std::uint8_t>& out, std::uint8_t value) { out.push_back(value); }
void u16(std::vector<std::uint8_t>& out, std::uint16_t value)
{ u8(out,value);u8(out,value>>8); }
void u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{ for(unsigned i=0;i<4;++i)u8(out,value>>(8*i)); }
void u64(std::vector<std::uint8_t>& out, std::uint64_t value)
{ for(unsigned i=0;i<8;++i)u8(out,value>>(8*i)); }
void put32(std::vector<std::uint8_t>& out,std::size_t offset,std::uint32_t value)
{ for(unsigned i=0;i<4;++i)out[offset+i]=static_cast<std::uint8_t>(value>>(8*i)); }
void digest(std::vector<std::uint8_t>& out,const Sha256Digest& value)
{ out.insert(out.end(),value.begin(),value.end()); }
std::uint32_t crc32(const std::uint8_t* data,std::size_t size)
{
	std::uint32_t crc=0xffffffffu;for(std::size_t i=0;i<size;++i){crc^=data[i];
		for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}
	return ~crc;
}

void driveState(std::vector<std::uint8_t>& out,const CddaDriveState& state)
{
	u32(out,state.status);u32(out,state.repeats);u32(out,state.currentFad);
	u32(out,state.startFad);u32(out,state.endFad);
}

void owner(std::vector<std::uint8_t>& out,const Sh4InstructionOwnerToken& value,
		Sh4ObservationBackend backend)
{
	require(value.valid&&value.backend==backend&&value.generation!=0,
			"control has no authenticated SH-4 owner");
	u8(out,value.valid);u8(out,static_cast<std::uint8_t>(value.backend));u16(out,0);
	u64(out,value.generation);u64(out,value.tick);u32(out,value.pc);u32(out,value.pr);
	u16(out,value.opcode);u16(out,value.delaySlotDepth);
}

bool establishesPlayback(const CddaObservation& event)
{
	if (event.type != CddaObservationType::ControlApplied
			|| !event.appliedSuccessfully)
		return false;
	if (event.path == CddaControlPath::ReiosHle)
		return event.command == 0x14 || event.command == 0x15;
	if (event.path == CddaControlPath::GdromPacket && event.command == 0x20)
	{
		const std::uint32_t parameterType = (event.parameters[0] >> 8) & 7u;
		return parameterType == 1 || parameterType == 2;
	}
	return false;
}

std::vector<std::uint8_t> checkpointPayload(const AicaCheckpoint& checkpoint)
{
	std::vector<std::uint8_t> out;
	const std::uint32_t flags=(checkpoint.cddaSourceAvailable?1u:0u)|
			(checkpoint.cddaReadSuccessful?2u:0u);
	u32(out,flags);u32(out,checkpoint.cddaIndex);u64(out,checkpoint.cddaGeneration);
	u64(out,checkpoint.cddaControlGeneration);u32(out,checkpoint.cddaFad);
	u32(out,checkpoint.cddaStatus);u32(out,checkpoint.cddaRepeats);u32(out,2352);
	out.insert(out.end(),checkpoint.cddaSector.begin(),checkpoint.cddaSector.end());
	return out;
}

std::vector<std::uint8_t> cddaPayload(const CddaObservation& event,
		const CddaArtifactBinding& binding)
{
	std::vector<std::uint8_t> out;
	switch(event.type)
	{
	case CddaObservationType::ControlAccepted:
		u64(out,event.controlGeneration);u32(out,static_cast<std::uint32_t>(event.path));
		u32(out,event.requestId);u32(out,event.command);u32(out,0);
		for(auto parameter:event.parameters)u32(out,parameter);
		owner(out,event.initiator,binding.backend);break;
	case CddaObservationType::ControlApplied:
		u64(out,event.controlGeneration);u32(out,static_cast<std::uint32_t>(event.path));
		u32(out,event.requestId);u32(out,event.command);u32(out,event.appliedSuccessfully);
		for(auto parameter:event.parameters)u32(out,parameter);
		owner(out,event.initiator,binding.backend);driveState(out,event.before);
		driveState(out,event.after);break;
	case CddaObservationType::Sector:
		require(event.bytes.size()==2352,"sector is not 2352 bytes");
		u64(out,event.controlGeneration);u64(out,event.aicaGeneration);u32(out,event.fad);
		u32(out,event.readSuccessful);driveState(out,event.before);driveState(out,event.after);
		u32(out,static_cast<std::uint32_t>(event.bytes.size()));
		out.insert(out.end(),event.bytes.begin(),event.bytes.end());break;
	case CddaObservationType::Reset:
		u64(out,event.controlGeneration);break;
	default:invalid("CD-DA observation type is invalid");
	}
	return out;
}

std::vector<std::uint8_t> samplePayload(const AicaObservation& event)
{
	std::vector<std::uint8_t> out;
	u64(out,event.sampleOrdinal);u64(out,event.activeChannelMask);u64(out,event.cddaGeneration);
	u16(out,event.cddaFrameIndex);u8(out,event.dspEnabled);u8(out,0);
	u32(out,event.dryLeft);u32(out,event.dryRight);u32(out,event.cddaInputLeft);
	u32(out,event.cddaInputRight);u32(out,event.cddaContributionLeft);
	u32(out,event.cddaContributionRight);u32(out,event.dspContributionLeft);
	u32(out,event.dspContributionRight);u16(out,static_cast<std::uint16_t>(event.finalLeft));
	u16(out,static_cast<std::uint16_t>(event.finalRight));return out;
}

std::vector<std::uint8_t> record(std::uint32_t type,std::uint64_t ordinal,
		std::uint64_t sourceEmission,std::uint64_t tick,
		const std::vector<std::uint8_t>& payload)
{
	require(payload.size()<=std::numeric_limits<std::uint32_t>::max()-RecordHeaderSize,
			"record is too large");std::vector<std::uint8_t> out;
	u32(out,type);u32(out,static_cast<std::uint32_t>(RecordHeaderSize+payload.size()));
	u64(out,ordinal);u64(out,sourceEmission);u64(out,tick);
	out.insert(out.end(),payload.begin(),payload.end());return out;
}

std::vector<std::uint8_t> header(const CddaArtifactSummary& summary,bool complete)
{
	std::vector<std::uint8_t> out;out.insert(out.end(),Magic.begin(),Magic.end());
	u32(out,CddaArtifactSchemaVersion);u32(out,CddaArtifactHeaderSize);u32(out,0x01020304);
	u32(out,complete?Complete:0);u32(out,static_cast<std::uint32_t>(summary.binding.backend));
	u32(out,summary.binding.dspEnabled?1u:0u);u64(out,summary.eventCount);
	u64(out,summary.payloadBytes);u64(out,summary.droppedCddaEvents);u64(out,summary.droppedAicaEvents);
	u64(out,summary.startTick);u64(out,summary.endTick);u64(out,summary.targetSampleFrames);
	u64(out,summary.sampleFrames);u64(out,summary.successfulSectors);
	u64(out,summary.contributingSampleFrames);u64(out,summary.appliedPlayControls);
	digest(out,summary.binding.identityDigest);digest(out,summary.binding.replayDigest);
	digest(out,summary.binding.configurationDigest);digest(out,summary.payloadDigest);
	digest(out,summary.pcmDigest);for(auto count:summary.typeCounts)u64(out,count);
	out.resize(CddaArtifactHeaderSize-4,0);u32(out,0);
	put32(out,CddaArtifactHeaderSize-4,crc32(out.data(),CddaArtifactHeaderSize-4));return out;
}

} // namespace

class CddaArtifactWriter::OutputFile
{
public:
	explicit OutputFile(const std::filesystem::path& path)
	{
		std::error_code error;if(path.parent_path().empty()||
				!std::filesystem::is_directory(path.parent_path(),error)||error)
			throw std::runtime_error("CD-DA output parent is not a directory");
#ifdef _WIN32
		handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,
				nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
		if(handle==INVALID_HANDLE_VALUE)throw std::system_error(
				static_cast<int>(GetLastError()),std::system_category(),
				"cannot exclusively create CD-DA output");
#else
		fd=::open(path.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);
		if(fd<0)throw std::system_error(errno,std::generic_category(),
				"cannot exclusively create CD-DA output");
#endif
		pending.reserve(64*1024);
	}
	~OutputFile(){try{flushPending();}catch(...){}
#ifdef _WIN32
		if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
#else
		if(fd>=0)::close(fd);
#endif
	}
	void write(const std::vector<std::uint8_t>& bytes)
	{pending.insert(pending.end(),bytes.begin(),bytes.end());if(pending.size()>=64*1024)flushPending();}
	void seek(std::uint64_t offset){flushPending();
#ifdef _WIN32
		LARGE_INTEGER position;position.QuadPart=static_cast<LONGLONG>(offset);
		if(!SetFilePointerEx(handle,position,nullptr,FILE_BEGIN))throw std::system_error(
				static_cast<int>(GetLastError()),std::system_category(),"cannot seek CD-DA output");
#else
		if(::lseek(fd,static_cast<off_t>(offset),SEEK_SET)<0)throw std::system_error(
				errno,std::generic_category(),"cannot seek CD-DA output");
#endif
	}
	void flush(){flushPending();
#ifdef _WIN32
		if(!FlushFileBuffers(handle))throw std::system_error(static_cast<int>(GetLastError()),
				std::system_category(),"cannot flush CD-DA output");
#else
		if(::fsync(fd)!=0)throw std::system_error(errno,std::generic_category(),"cannot flush CD-DA output");
#endif
	}
private:
	void flushPending(){const std::uint8_t* data=pending.data();std::size_t size=pending.size();while(size){
#ifdef _WIN32
		DWORD written=0;const DWORD chunk=static_cast<DWORD>(std::min<std::size_t>(size,
				(std::numeric_limits<DWORD>::max)()));if(!WriteFile(handle,data,chunk,&written,nullptr)||written==0)
			throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"cannot write CD-DA output");
#else
		const ssize_t written=::write(fd,data,size);if(written<0&&errno==EINTR)continue;
		if(written<=0)throw std::system_error(errno,std::generic_category(),"cannot write CD-DA output");
#endif
		data+=written;size-=static_cast<std::size_t>(written);}pending.clear();}
	std::vector<std::uint8_t> pending;
#ifdef _WIN32
	HANDLE handle=INVALID_HANDLE_VALUE;
#else
	int fd=-1;
#endif
};

CddaArtifactWriter::CddaArtifactWriter(const std::filesystem::path& path,
		const CddaArtifactBinding& binding,std::uint64_t targetSampleFrames,
		std::uint64_t maximumBytes,std::uint64_t maximumEvents)
	:path(path),maximumBytes(maximumBytes),maximumEvents(maximumEvents)
{
	if(maximumBytes<CddaArtifactHeaderSize||maximumEvents==0||targetSampleFrames==0||
			targetSampleFrames>MaximumCddaSampleFrames)
		throw std::invalid_argument("CD-DA artifact limits are invalid");
	summary.binding=binding;summary.targetSampleFrames=targetSampleFrames;
	output=std::make_unique<OutputFile>(path);output->write(header(summary,false));output->flush();
}

CddaArtifactWriter::~CddaArtifactWriter(){if(!finalized)abandon();}

void CddaArtifactWriter::writeCheckpoint(const AicaCheckpoint& checkpoint)
{
	if(finalized||abandoned||checkpointWritten)throw std::logic_error("CD-DA checkpoint cannot be written");
	require(checkpoint.cddaIndex<=1176,"checkpoint CD-DA index is invalid");
	const auto bytes=record(CheckpointRecordType,0,0,checkpoint.tick,checkpointPayload(checkpoint));
	if(bytes.size()>maximumBytes-CddaArtifactHeaderSize)throw std::runtime_error("CD-DA checkpoint exceeds byte limit");
	output->write(bytes);payloadHasher.update(bytes.data(),bytes.size());summary.payloadBytes=bytes.size();
	summary.startTick=summary.endTick=checkpoint.tick;checkpointWritten=true;
}

void CddaArtifactWriter::writeRecord(std::uint32_t type,std::uint64_t sourceEmission,
		std::uint64_t tick,const std::vector<std::uint8_t>& payload)
{
	if(summary.eventCount>=maximumEvents)throw std::runtime_error("CD-DA event limit exceeded");
	if(tick<summary.endTick)throw std::runtime_error("CD-DA tick moved backwards");
	const auto bytes=record(type,summary.eventCount+1,sourceEmission,tick,payload);
	if(summary.payloadBytes>maximumBytes-CddaArtifactHeaderSize||
			bytes.size()>maximumBytes-CddaArtifactHeaderSize-summary.payloadBytes)
		throw std::runtime_error("CD-DA byte limit exceeded");
	output->write(bytes);payloadHasher.update(bytes.data(),bytes.size());summary.payloadBytes+=bytes.size();
	summary.endTick=tick;++summary.typeCounts[type-1];++summary.eventCount;
}

void CddaArtifactWriter::write(const CddaObservation& event)
{
	if(finalized||abandoned||!checkpointWritten)throw std::logic_error("CD-DA writer is not writable");
	if(event.schemaVersion!=CddaObservationSchemaVersion)throw std::runtime_error("CD-DA observation schema mismatch");
	if(event.type==CddaObservationType::Reset)throw std::runtime_error("CD-DA capture was reset");
	if(establishesPlayback(event))++summary.appliedPlayControls;
	if(event.type==CddaObservationType::Sector&&event.readSuccessful){
		require(event.controlGeneration!=0&&event.aicaGeneration!=0,"successful sector has no causal generation");
		successfulAicaGenerations.insert(event.aicaGeneration);++summary.successfulSectors;
	}
	writeRecord(static_cast<std::uint32_t>(event.type),event.emissionOrdinal,event.tick,
			cddaPayload(event,summary.binding));
}

void CddaArtifactWriter::write(const AicaObservation& event)
{
	if(finalized||abandoned||!checkpointWritten)throw std::logic_error("CD-DA writer is not writable");
	if(event.schemaVersion!=AicaObservationSchemaVersion||event.type!=AicaObservationType::SampleFrame)
		throw std::runtime_error("CD-DA artifact accepts only AICA sample frames");
	if(targetReached())throw std::logic_error("CD-DA sample target already reached");
	require(knowsSuccessfulAicaGeneration(event.cddaGeneration),
			"sample does not reference a successful captured sector");
	require(event.cddaFrameIndex<588,"sample CD-DA frame index is invalid");
	require(event.dspEnabled==summary.binding.dspEnabled,"sample DSP mode differs from binding");
	std::array<std::uint8_t,4> pcm{static_cast<std::uint8_t>(event.finalLeft),
			static_cast<std::uint8_t>(event.finalLeft>>8),static_cast<std::uint8_t>(event.finalRight),
			static_cast<std::uint8_t>(event.finalRight>>8)};pcmHasher.update(pcm.data(),pcm.size());
	++summary.sampleFrames;if(event.cddaContributionLeft!=0||event.cddaContributionRight!=0)
		++summary.contributingSampleFrames;
	writeRecord(4,event.emissionOrdinal,event.tick,samplePayload(event));
}

bool CddaArtifactWriter::knowsSuccessfulAicaGeneration(std::uint64_t generation) const noexcept
{ return successfulAicaGenerations.find(generation)!=successfulAicaGenerations.end(); }

bool CddaArtifactWriter::targetReached() const noexcept
{ return summary.sampleFrames==summary.targetSampleFrames; }

CddaArtifactSummary CddaArtifactWriter::finalize(std::uint64_t droppedCddaEvents,
		std::uint64_t droppedAicaEvents)
{
	if(finalized||abandoned)throw std::logic_error("CD-DA writer cannot finalize");
	if(!checkpointWritten||!targetReached()||summary.appliedPlayControls==0||
			summary.successfulSectors==0||summary.contributingSampleFrames==0)
		throw std::logic_error("CD-DA artifact is incomplete or has no proven contribution");
	summary.droppedCddaEvents=droppedCddaEvents;summary.droppedAicaEvents=droppedAicaEvents;
	if(droppedCddaEvents!=0||droppedAicaEvents!=0)throw std::runtime_error("CD-DA observation loss");
	summary.payloadDigest=payloadHasher.finalize();summary.pcmDigest=pcmHasher.finalize();
	output->seek(0);output->write(header(summary,true));output->flush();output.reset();
	finalized=true;return summary;
}

void CddaArtifactWriter::abandon() noexcept{abandoned=true;output.reset();}

} // namespace research

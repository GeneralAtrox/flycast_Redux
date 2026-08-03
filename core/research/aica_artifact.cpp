#include "research/aica_artifact.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
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

constexpr std::array<std::uint8_t, 8> Magic {'F','C','A','I','C','A','0','1'};
constexpr std::uint32_t Complete = 1;
constexpr std::uint32_t RecordHeaderSize = 32;
constexpr std::uint32_t CheckpointRecordType = 0;

[[noreturn]] void invalid(const char *message)
{ throw std::runtime_error(std::string("invalid AICA artifact: ") + message); }
void require(bool value, const char *message) { if (!value) invalid(message); }
void u8(std::vector<std::uint8_t>& out, std::uint8_t value) { out.push_back(value); }
void u16(std::vector<std::uint8_t>& out, std::uint16_t value)
{ u8(out, value); u8(out, value >> 8); }
void u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{ for (unsigned i=0;i<4;++i) u8(out, value >> (8*i)); }
void u64(std::vector<std::uint8_t>& out, std::uint64_t value)
{ for (unsigned i=0;i<8;++i) u8(out, value >> (8*i)); }
void put32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value)
{ for(unsigned i=0;i<4;++i) out[offset+i]=static_cast<std::uint8_t>(value>>(8*i)); }
std::uint32_t crc32(const std::uint8_t* data, std::size_t size)
{
	std::uint32_t crc=0xffffffffu;
	for(std::size_t i=0;i<size;++i){ crc^=data[i]; for(unsigned bit=0;bit<8;++bit)
		crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u))); }
	return ~crc;
}
void digest(std::vector<std::uint8_t>& out, const Sha256Digest& value)
{ out.insert(out.end(), value.begin(), value.end()); }

void serializeOwner(std::vector<std::uint8_t>& out, const AicaOwnerToken& owner,
		Sh4ObservationBackend backend)
{
	u8(out, static_cast<std::uint8_t>(owner.writer));
	u8(out, owner.arm7PcAvailable ? 1 : 0);
	u8(out, static_cast<std::uint8_t>(owner.sh4.backend));
	u8(out, owner.sh4.valid ? 1 : 0);
	u32(out, owner.arm7Pc); u64(out, owner.sh4.generation); u64(out, owner.sh4.tick);
	u32(out, owner.sh4.pc); u32(out, owner.sh4.pr); u16(out, owner.sh4.opcode);
	u16(out, owner.sh4.delaySlotDepth); u32(out, 0);
	if (owner.writer == AicaWriter::Sh4Direct || owner.writer == AicaWriter::Sh4G2Dma)
		require(owner.sh4.valid && owner.sh4.backend == backend
				&& owner.sh4.generation != 0, "SH-4 writer has no authenticated owner");
	if (owner.writer == AicaWriter::Arm7)
		require(!owner.arm7PcAvailable, "unsupported ARM7 PC claim");
}

std::vector<std::uint8_t> checkpointPayload(const AicaCheckpoint& checkpoint)
{
	require(checkpoint.ram.size() != 0 && checkpoint.ram.size() <= 8u*1024u*1024u,
			"checkpoint RAM size is invalid");
	std::vector<std::uint8_t> out;
	u64(out, checkpoint.activeChannelMask); u32(out, checkpoint.registers.size());
	u32(out, static_cast<std::uint32_t>(checkpoint.ram.size()));
	u32(out, checkpoint.cddaIndex); u32(out, 0); u64(out, checkpoint.cddaGeneration);
	out.insert(out.end(), checkpoint.registers.begin(), checkpoint.registers.end());
	out.insert(out.end(), checkpoint.ram.begin(), checkpoint.ram.end());
	for (const auto& channel : checkpoint.channels) {
		u32(out,channel.sampleAddress);u32(out,channel.currentAddress);u32(out,channel.step);
		u32(out,static_cast<std::uint32_t>(channel.sample0));u32(out,static_cast<std::uint32_t>(channel.sample1));
		u32(out,channel.looped);u32(out,static_cast<std::uint32_t>(channel.adpcmLastQuant));
		u32(out,static_cast<std::uint32_t>(channel.adpcmLoopQuant));u32(out,static_cast<std::uint32_t>(channel.adpcmLoopSample));
		u32(out,channel.adpcmInLoop);u32(out,channel.noiseState);u32(out,static_cast<std::uint32_t>(channel.aegValue));
		u32(out,channel.aegState);u32(out,channel.fegValue);u32(out,channel.fegState);
		u32(out,static_cast<std::uint32_t>(channel.fegPrevious1));u32(out,static_cast<std::uint32_t>(channel.fegPrevious2));
		u32(out,static_cast<std::uint32_t>(channel.fegFraction));u32(out,channel.lfoCounter);
		u32(out,channel.lfoState);u32(out,channel.enabled);
	}
	for(auto value:checkpoint.dspTemp)u32(out,static_cast<std::uint32_t>(value));
	for(auto value:checkpoint.dspMems)u32(out,static_cast<std::uint32_t>(value));
	for(auto value:checkpoint.dspMixs)u32(out,static_cast<std::uint32_t>(value));
	u32(out,checkpoint.dspRingBufferPointer);u32(out,checkpoint.dspRingBufferLength);
	u32(out,checkpoint.dspMemoryDecodeCounter);
	out.insert(out.end(),checkpoint.cddaSector.begin(),checkpoint.cddaSector.end());
	return out;
}

std::vector<std::uint8_t> observationPayload(const AicaObservation& event,
		const AicaArtifactBinding& binding)
{
	std::vector<std::uint8_t> out;
	switch(event.type) {
	case AicaObservationType::RegisterWrite:
		serializeOwner(out,event.owner,binding.backend);u32(out,event.address);u8(out,event.width);
		u8(out,0);u16(out,0);u32(out,event.value);break;
	case AicaObservationType::RamWrite:
		serializeOwner(out,event.owner,binding.backend);u32(out,event.address);
		u32(out,static_cast<std::uint32_t>(event.bytes.size()));out.insert(out.end(),event.bytes.begin(),event.bytes.end());break;
	case AicaObservationType::G2DmaBegin:
		serializeOwner(out,event.owner,binding.backend);u64(out,event.dmaGeneration);
		u32(out,event.sourceAddress);u32(out,event.destinationAddress);u32(out,event.transferLength);
		u32(out,event.aicaRamIsDestination);break;
	case AicaObservationType::G2DmaTransfer:
		u64(out,event.dmaGeneration);u32(out,event.sourceAddress);u32(out,event.destinationAddress);
		u32(out,event.transferLength);u32(out,event.aicaRamIsDestination);
		u32(out,static_cast<std::uint32_t>(event.bytes.size()));out.insert(out.end(),event.bytes.begin(),event.bytes.end());break;
	case AicaObservationType::G2DmaComplete:u64(out,event.dmaGeneration);break;
	case AicaObservationType::KeyOn: case AicaObservationType::KeyOff:
		serializeOwner(out,event.owner,binding.backend);u8(out,event.channel);u8(out,0);u16(out,0);
		out.insert(out.end(),event.channelRegisters.begin(),event.channelRegisters.end());break;
	case AicaObservationType::SampleFrame:
		u64(out,event.sampleOrdinal);u64(out,event.activeChannelMask);u64(out,event.cddaGeneration);
		u16(out,event.cddaFrameIndex);u8(out,event.dspEnabled);u8(out,0);
		u32(out,event.dryLeft);u32(out,event.dryRight);u32(out,event.cddaInputLeft);u32(out,event.cddaInputRight);
		u32(out,event.cddaContributionLeft);u32(out,event.cddaContributionRight);
		u32(out,event.dspContributionLeft);u32(out,event.dspContributionRight);
		u16(out,static_cast<std::uint16_t>(event.finalLeft));u16(out,static_cast<std::uint16_t>(event.finalRight));break;
	case AicaObservationType::Reset: u64(out,event.dmaGeneration);break;
	case AicaObservationType::KeyBatchComplete:
		serializeOwner(out,event.owner,binding.backend);u64(out,event.keyOnMask);u64(out,event.keyOffMask);break;
	case AicaObservationType::CddaSector:
		u64(out,event.cddaGeneration);u32(out,event.cddaFad);u32(out,event.cddaStatus);
		u32(out,event.cddaRepeats);u32(out,event.cddaReadSuccessful);
		u32(out,static_cast<std::uint32_t>(event.bytes.size()));out.insert(out.end(),event.bytes.begin(),event.bytes.end());break;
	case AicaObservationType::SampleSuppressed:u32(out,static_cast<std::uint32_t>(event.suppression));break;
	default: invalid("observation type is invalid");
	}
	return out;
}

std::vector<std::uint8_t> record(std::uint32_t type, std::uint64_t ordinal,
		std::uint64_t emissionOrdinal, std::uint64_t tick,
		const std::vector<std::uint8_t>& payload)
{
	require(payload.size() <= std::numeric_limits<std::uint32_t>::max()-RecordHeaderSize,
			"record is too large");
	std::vector<std::uint8_t> out; out.reserve(RecordHeaderSize+payload.size());
	u32(out,type);u32(out,static_cast<std::uint32_t>(RecordHeaderSize+payload.size()));
	u64(out,ordinal);u64(out,emissionOrdinal);u64(out,tick);
	out.insert(out.end(),payload.begin(),payload.end());return out;
}

std::vector<std::uint8_t> header(const AicaArtifactSummary& s, bool complete)
{
	std::vector<std::uint8_t> out;out.insert(out.end(),Magic.begin(),Magic.end());
	u32(out,AicaArtifactSchemaVersion);u32(out,AicaArtifactHeaderSize);u32(out,0x01020304);
	u32(out,complete?Complete:0);u32(out,static_cast<std::uint32_t>(s.binding.backend));
	u32(out,(s.binding.dspEnabled?1u:0u)|(s.binding.vmuSound?2u:0u));
	u64(out,s.eventCount);u64(out,s.payloadBytes);u64(out,s.droppedEvents);u64(out,s.startTick);u64(out,s.endTick);
	u64(out,s.targetSampleFrames);u64(out,s.sampleFrames);u64(out,s.keyOnCount);u64(out,s.keyedSourceCount);
	u64(out,s.nonzeroSampleFrames);u64(out,s.checkpointRamBytes);
	digest(out,s.binding.identityDigest);digest(out,s.binding.replayDigest);digest(out,s.binding.configurationDigest);
	digest(out,s.payloadDigest);digest(out,s.pcmDigest);digest(out,s.keyedSourceDigest);
	for(auto count:s.typeCounts)u64(out,count);
	out.resize(AicaArtifactHeaderSize-4,0);u32(out,0);
	put32(out,AicaArtifactHeaderSize-4,crc32(out.data(),AicaArtifactHeaderSize-4));return out;
}

void updateRam(std::vector<std::uint8_t>& ram, std::uint32_t address,
		const std::vector<std::uint8_t>& bytes)
{
	require(!ram.empty(),"RAM mirror is empty");
	for(std::size_t i=0;i<bytes.size();++i) ram[(std::uint64_t(address)+i)%ram.size()]=bytes[i];
}

bool hashKeySource(Sha256& hasher, const AicaObservation& event,
		const std::vector<std::uint8_t>& ram)
{
	const auto& r=event.channelRegisters;
	const std::uint16_t word0=std::uint16_t(r[0])|(std::uint16_t(r[1])<<8);
	const bool noise=((word0>>10)&1)!=0; if(noise)return false;
	const std::uint32_t pcms=(word0>>7)&3;
	std::uint32_t address=(std::uint32_t(word0&0x7f)<<16)|std::uint32_t(r[4])|(std::uint32_t(r[5])<<8);
	if(pcms==0)address&=~1u;
	const std::uint32_t lea=std::uint32_t(r[12])|(std::uint32_t(r[13])<<8);
	const std::uint64_t length=pcms==0?std::uint64_t(lea)*2:pcms==1?lea:(std::uint64_t(lea)+1)/2;
	require(length!=0 && length<=ram.size(),"key-on sample range is invalid");
	std::array<std::uint8_t,9> prefix{};prefix[0]=event.channel;
	for(unsigned i=0;i<4;++i){prefix[1+i]=address>>(8*i);prefix[5+i]=length>>(8*i);}
	hasher.update(prefix.data(),prefix.size());
	for(std::uint64_t i=0;i<length;++i){const auto b=ram[(std::uint64_t(address)+i)%ram.size()];hasher.update(&b,1);}
	return true;
}

} // namespace

class AicaArtifactWriter::OutputFile
{
public:
	explicit OutputFile(const std::filesystem::path& path)
	{
		std::error_code error;
		if(path.parent_path().empty()||!std::filesystem::is_directory(path.parent_path(),error)||error)
			throw std::runtime_error("AICA output parent is not a directory");
#ifdef _WIN32
		handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,nullptr,
				CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
		if(handle==INVALID_HANDLE_VALUE)throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"cannot exclusively create AICA output");
#else
		fd=::open(path.c_str(),O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600);
		if(fd<0)throw std::system_error(errno,std::generic_category(),"cannot exclusively create AICA output");
#endif
		pending.reserve(256*1024);
	}
	~OutputFile(){try{flushPending();}catch(...){}
#ifdef _WIN32
		if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
#else
		if(fd>=0)::close(fd);
#endif
	}
	void write(const std::vector<std::uint8_t>& bytes){pending.insert(pending.end(),bytes.begin(),bytes.end());if(pending.size()>=256*1024)flushPending();}
	void seek(std::uint64_t offset){flushPending();
#ifdef _WIN32
		LARGE_INTEGER p;p.QuadPart=static_cast<LONGLONG>(offset);if(!SetFilePointerEx(handle,p,nullptr,FILE_BEGIN))throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"cannot seek AICA output");
#else
		if(::lseek(fd,static_cast<off_t>(offset),SEEK_SET)<0)throw std::system_error(errno,std::generic_category(),"cannot seek AICA output");
#endif
	}
	void flush(){flushPending();
#ifdef _WIN32
		if(!FlushFileBuffers(handle))throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"cannot flush AICA output");
#else
		if(::fsync(fd)!=0)throw std::system_error(errno,std::generic_category(),"cannot flush AICA output");
#endif
	}
private:
	void flushPending(){const std::uint8_t* data=pending.data();std::size_t size=pending.size();while(size){
#ifdef _WIN32
		DWORD n=0;const DWORD chunk=static_cast<DWORD>(std::min<std::size_t>(size,(std::numeric_limits<DWORD>::max)()));if(!WriteFile(handle,data,chunk,&n,nullptr)||n==0)throw std::system_error(static_cast<int>(GetLastError()),std::system_category(),"cannot write AICA output");
#else
		const ssize_t n=::write(fd,data,size);if(n<0&&errno==EINTR)continue;if(n<=0)throw std::system_error(errno,std::generic_category(),"cannot write AICA output");
#endif
		data+=n;size-=static_cast<std::size_t>(n);}pending.clear();}
	std::vector<std::uint8_t> pending;
#ifdef _WIN32
	HANDLE handle=INVALID_HANDLE_VALUE;
#else
	int fd=-1;
#endif
};

AicaArtifactWriter::AicaArtifactWriter(const std::filesystem::path& path,
		const AicaArtifactBinding& binding,std::uint64_t targetSampleFrames,
		std::uint64_t maximumBytes,std::uint64_t maximumEvents)
	:path(path),maximumBytes(maximumBytes),maximumEvents(maximumEvents)
{
	if(maximumBytes<AicaArtifactHeaderSize||maximumEvents==0||targetSampleFrames==0
			||targetSampleFrames>MaximumAicaSampleFrames)throw std::invalid_argument("AICA artifact limits are invalid");
	summary.binding=binding;summary.targetSampleFrames=targetSampleFrames;
	output=std::make_unique<OutputFile>(path);output->write(header(summary,false));output->flush();
}
AicaArtifactWriter::~AicaArtifactWriter(){if(!finalized)abandon();}

void AicaArtifactWriter::writeCheckpoint(const AicaCheckpoint& checkpoint)
{
	if(finalized||abandoned||checkpointWritten)throw std::logic_error("AICA checkpoint cannot be written");
	const auto bytes=record(CheckpointRecordType,0,0,checkpoint.tick,checkpointPayload(checkpoint));
	if(bytes.size()>maximumBytes-AicaArtifactHeaderSize)throw std::runtime_error("AICA checkpoint exceeds byte limit");
	output->write(bytes);payloadHasher.update(bytes.data(),bytes.size());summary.payloadBytes=bytes.size();
	summary.startTick=summary.endTick=checkpoint.tick;summary.checkpointRamBytes=checkpoint.ram.size();
	ramMirror=checkpoint.ram;checkpointWritten=true;
}

void AicaArtifactWriter::write(const AicaObservation& event)
{
	if(finalized||abandoned||!checkpointWritten)throw std::logic_error("AICA writer is not writable");
	if(event.schemaVersion!=AicaObservationSchemaVersion)throw std::runtime_error("AICA observation schema mismatch");
	if(summary.eventCount>=maximumEvents)throw std::runtime_error("AICA event limit exceeded");
	if(event.tick<summary.endTick)throw std::runtime_error("AICA tick moved backwards");
	if(targetReached())throw std::logic_error("AICA sample target already reached");
	if(event.type==AicaObservationType::Reset||event.type==AicaObservationType::SampleSuppressed)
		throw std::runtime_error("AICA capture was reset or output was suppressed");
	if(event.type==AicaObservationType::KeyBatchComplete)keyBatchSeen=true;
	if(event.type==AicaObservationType::RamWrite)updateRam(ramMirror,event.address,event.bytes);
	if(event.type==AicaObservationType::G2DmaTransfer&&event.aicaRamIsDestination)
		updateRam(ramMirror,event.destinationAddress,event.bytes);
	if(event.type==AicaObservationType::KeyOn){++summary.keyOnCount;if(hashKeySource(sourceHasher,event,ramMirror))++summary.keyedSourceCount;}
	if(event.type==AicaObservationType::CddaSector)require(event.bytes.size()==2352,"CD-DA sector is not 2352 bytes");
	if(event.type==AicaObservationType::SampleFrame){
		require(event.dspEnabled==summary.binding.dspEnabled,
				"sample DSP mode differs from artifact binding");
		if(summary.sampleFrames!=0&&event.sampleOrdinal==0)invalid("sample ordinal reset");
		std::array<std::uint8_t,4> pcm {static_cast<std::uint8_t>(event.finalLeft),static_cast<std::uint8_t>(event.finalLeft>>8),static_cast<std::uint8_t>(event.finalRight),static_cast<std::uint8_t>(event.finalRight>>8)};
		pcmHasher.update(pcm.data(),pcm.size());++summary.sampleFrames;
		if(event.finalLeft!=0||event.finalRight!=0)++summary.nonzeroSampleFrames;
	}
	const auto payload=observationPayload(event,summary.binding);
	const auto bytes=record(static_cast<std::uint32_t>(event.type),summary.eventCount+1,event.emissionOrdinal,event.tick,payload);
	if(summary.payloadBytes>maximumBytes-AicaArtifactHeaderSize||bytes.size()>maximumBytes-AicaArtifactHeaderSize-summary.payloadBytes)
		throw std::runtime_error("AICA byte limit exceeded");
	output->write(bytes);payloadHasher.update(bytes.data(),bytes.size());summary.payloadBytes+=bytes.size();
	summary.endTick=event.tick;++summary.typeCounts[static_cast<unsigned>(event.type)-1];++summary.eventCount;
}

bool AicaArtifactWriter::targetReached() const noexcept{return summary.sampleFrames==summary.targetSampleFrames;}

AicaArtifactSummary AicaArtifactWriter::finalize(std::uint64_t droppedEvents,bool dmaActive)
{
	if(finalized||abandoned)throw std::logic_error("AICA writer cannot finalize");
	if(!checkpointWritten||!keyBatchSeen||!targetReached()||summary.keyOnCount==0
			||summary.keyedSourceCount==0||summary.nonzeroSampleFrames==0)
		throw std::logic_error("AICA artifact is incomplete or contains no inspectable keyed audio");
	summary.droppedEvents=droppedEvents;if(droppedEvents!=0||dmaActive)throw std::runtime_error("AICA observation loss or open DMA");
	summary.payloadDigest=payloadHasher.finalize();summary.pcmDigest=pcmHasher.finalize();summary.keyedSourceDigest=sourceHasher.finalize();
	output->seek(0);output->write(header(summary,true));output->flush();output.reset();finalized=true;return summary;
}

void AicaArtifactWriter::abandon() noexcept{abandoned=true;output.reset();}

} // namespace research

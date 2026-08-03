#include "research/aica_artifact.h"
#include "research/maple_trace.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace research
{
namespace
{
constexpr std::array<std::uint8_t,8> Magic {'F','C','A','I','C','A','0','1'};
constexpr std::uint32_t RecordHeaderSize=32;
[[noreturn]] void invalid(const std::string& message){throw std::runtime_error("invalid AICA artifact: "+message);}
void require(bool value,const std::string& message){if(!value)invalid(message);}

class Reader
{
public:
	Reader(const std::uint8_t* data,std::size_t size):data(data),size(size){}
	std::uint8_t u8(){need(1);return data[pos++];}
	std::uint16_t u16(){std::uint16_t v=u8();return v|(std::uint16_t(u8())<<8);}
	std::uint32_t u32(){std::uint32_t v=0;for(unsigned i=0;i<4;++i)v|=std::uint32_t(u8())<<(8*i);return v;}
	std::uint64_t u64(){std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(u8())<<(8*i);return v;}
	std::vector<std::uint8_t> bytes(std::size_t n){need(n);std::vector<std::uint8_t> v(data+pos,data+pos+n);pos+=n;return v;}
	void skip(std::size_t n){need(n);pos+=n;} std::size_t remaining()const{return size-pos;}
private:void need(std::size_t n){if(n>size-pos)invalid("binary input is truncated");}const std::uint8_t* data;std::size_t size,pos=0;
};

std::uint32_t crc32(const std::uint8_t* data,std::size_t size){std::uint32_t crc=0xffffffffu;for(std::size_t i=0;i<size;++i){crc^=data[i];for(unsigned b=0;b<8;++b)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}return ~crc;}
Sha256Digest readDigest(Reader& r){Sha256Digest d{};for(auto& b:d)b=r.u8();return d;}

void updateRam(std::vector<std::uint8_t>& ram,std::uint32_t address,const std::vector<std::uint8_t>& bytes)
{require(!ram.empty(),"RAM mirror is empty");for(std::size_t i=0;i<bytes.size();++i)ram[(std::uint64_t(address)+i)%ram.size()]=bytes[i];}

void parseOwner(Reader& r,const AicaArtifactBinding& binding,std::uint64_t eventTick)
{
	const auto writer=static_cast<AicaWriter>(r.u8());const bool armPc=r.u8()!=0;
	const auto backend=static_cast<Sh4ObservationBackend>(r.u8());const bool valid=r.u8()!=0;
	r.u32();const auto generation=r.u64();const auto tick=r.u64();r.u32();r.u32();r.u16();r.u16();require(r.u32()==0,"owner reserved field is nonzero");
	require(static_cast<unsigned>(writer)<=static_cast<unsigned>(AicaWriter::ReiosHle),"owner writer is invalid");
	if(writer==AicaWriter::Sh4Direct||writer==AicaWriter::Sh4G2Dma)
		require(valid&&backend==binding.backend&&generation!=0&&tick<=eventTick,"SH-4 owner is inconsistent");
	if(writer==AicaWriter::Arm7)require(!armPc,"artifact makes an unsupported ARM7 PC claim");
}

bool hashKeySource(Sha256& hasher,std::uint8_t channel,const std::array<std::uint8_t,0x80>& regs,
		const std::vector<std::uint8_t>& ram)
{
	const std::uint16_t w=std::uint16_t(regs[0])|(std::uint16_t(regs[1])<<8);if(((w>>10)&1)!=0)return false;
	const std::uint32_t pcms=(w>>7)&3;std::uint32_t address=(std::uint32_t(w&0x7f)<<16)|regs[4]|(std::uint32_t(regs[5])<<8);if(pcms==0)address&=~1u;
	const std::uint32_t lea=regs[12]|(std::uint32_t(regs[13])<<8);const std::uint64_t length=pcms==0?std::uint64_t(lea)*2:pcms==1?lea:(std::uint64_t(lea)+1)/2;
	require(length!=0&&length<=ram.size(),"key-on sample range is invalid");std::array<std::uint8_t,9> prefix{};prefix[0]=channel;
	for(unsigned i=0;i<4;++i){prefix[1+i]=address>>(8*i);prefix[5+i]=length>>(8*i);}hasher.update(prefix.data(),prefix.size());
	for(std::uint64_t i=0;i<length;++i){const auto b=ram[(std::uint64_t(address)+i)%ram.size()];hasher.update(&b,1);}return true;
}

std::int16_t pcm16(const std::vector<std::uint8_t>& bytes,std::size_t offset)
{return static_cast<std::int16_t>(std::uint16_t(bytes[offset])|(std::uint16_t(bytes[offset+1])<<8));}

} // namespace

AicaArtifactSummary validateAicaArtifactFile(const std::filesystem::path& artifact,
		const AicaArtifactBinding& expected,std::uint64_t maximumBytes,std::uint64_t maximumEvents)
{
	std::error_code error;const auto size=std::filesystem::file_size(artifact,error);
	require(!error&&size>=AicaArtifactHeaderSize&&size<=maximumBytes,"file size is outside the accepted bound");
	std::ifstream input(artifact,std::ios::binary);require(static_cast<bool>(input),"cannot open artifact");
	std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));input.read(reinterpret_cast<char*>(bytes.data()),bytes.size());
	require(static_cast<std::size_t>(input.gcount())==bytes.size(),"artifact read is incomplete");
	require(crc32(bytes.data(),AicaArtifactHeaderSize-4)==(std::uint32_t(bytes[508])|(std::uint32_t(bytes[509])<<8)|(std::uint32_t(bytes[510])<<16)|(std::uint32_t(bytes[511])<<24)),"header CRC mismatch");
	Reader h(bytes.data(),AicaArtifactHeaderSize);for(auto m:Magic)require(h.u8()==m,"magic mismatch");
	require(h.u32()==AicaArtifactSchemaVersion&&h.u32()==AicaArtifactHeaderSize&&h.u32()==0x01020304,"header version or endian marker mismatch");
	require(h.u32()==1,"artifact is incomplete");AicaArtifactSummary s;s.binding.backend=static_cast<Sh4ObservationBackend>(h.u32());const auto configurationFlags=h.u32();require((configurationFlags&~3u)==0,"header configuration flags are invalid");s.binding.dspEnabled=(configurationFlags&1u)!=0;s.binding.vmuSound=(configurationFlags&2u)!=0;
	s.eventCount=h.u64();s.payloadBytes=h.u64();s.droppedEvents=h.u64();s.startTick=h.u64();s.endTick=h.u64();s.targetSampleFrames=h.u64();s.sampleFrames=h.u64();s.keyOnCount=h.u64();s.keyedSourceCount=h.u64();s.nonzeroSampleFrames=h.u64();s.checkpointRamBytes=h.u64();
	s.binding.identityDigest=readDigest(h);s.binding.replayDigest=readDigest(h);s.binding.configurationDigest=readDigest(h);s.payloadDigest=readDigest(h);s.pcmDigest=readDigest(h);s.keyedSourceDigest=readDigest(h);for(auto& count:s.typeCounts)count=h.u64();
	require(s.binding.backend==expected.backend&&s.binding.dspEnabled==expected.dspEnabled&&s.binding.vmuSound==expected.vmuSound&&sha256Equal(s.binding.identityDigest,expected.identityDigest)&&sha256Equal(s.binding.replayDigest,expected.replayDigest)&&sha256Equal(s.binding.configurationDigest,expected.configurationDigest),"artifact binding mismatch");
	require(s.eventCount<=maximumEvents&&s.payloadBytes==size-AicaArtifactHeaderSize&&s.droppedEvents==0,"header counts or dropped count are invalid");
	require(s.targetSampleFrames>0&&s.targetSampleFrames<=MaximumAicaSampleFrames&&s.sampleFrames==s.targetSampleFrames,"sample target is invalid");
	Sha256 payloadHasher;payloadHasher.update(bytes.data()+AicaArtifactHeaderSize,bytes.size()-AicaArtifactHeaderSize);require(sha256Equal(payloadHasher.finalize(),s.payloadDigest),"payload digest mismatch");

	Reader records(bytes.data()+AicaArtifactHeaderSize,bytes.size()-AicaArtifactHeaderSize);
	require(records.u32()==0,"first record is not a checkpoint");const auto checkpointSize=records.u32();
	require(records.u64()==0,"checkpoint ordinal is invalid");records.u64();const auto tick=records.u64();
	require(checkpointSize>=RecordHeaderSize&&checkpointSize-RecordHeaderSize<=records.remaining(),"checkpoint record header is invalid");
	auto checkpointBytes=records.bytes(checkpointSize-RecordHeaderSize);Reader checkpoint(checkpointBytes.data(),checkpointBytes.size());
	require(tick==s.startTick,"checkpoint tick differs from header");
	checkpoint.u64();require(checkpoint.u32()==0x8000,"checkpoint register size is invalid");const auto ramSize=checkpoint.u32();require(ramSize==s.checkpointRamBytes&&ramSize>0&&ramSize<=8u*1024u*1024u,"checkpoint RAM size mismatch");
	const auto checkpointCddaIndex=checkpoint.u32();require(checkpoint.u32()==0,"checkpoint reserved field is nonzero");const auto checkpointCddaGeneration=checkpoint.u64();
	auto registers=checkpoint.bytes(0x8000);auto ram=checkpoint.bytes(ramSize);checkpoint.skip(64*21*4);checkpoint.skip((128+32+16+3)*4);auto checkpointCdda=checkpoint.bytes(2352);require(checkpoint.remaining()==0&&checkpointCddaIndex<=1176,"checkpoint layout or CD-DA index is invalid");
	std::map<std::uint64_t,std::vector<std::uint8_t>> cddaSectors;cddaSectors.emplace(checkpointCddaGeneration,std::move(checkpointCdda));
	Sha256 pcmHasher,sourceHasher;std::array<std::uint64_t,12> counts{};std::uint64_t samples=0,keyOns=0,keySources=0,nonzero=0;bool keyBatch=false,dmaOpen=false;std::uint64_t dmaGeneration=0,lastTick=tick,lastEmission=0;bool haveEmission=false;std::uint64_t firstSampleOrdinal=0;
	for(std::uint64_t ordinal=1;ordinal<=s.eventCount;++ordinal){
		const auto typeValue=records.u32();const auto recordSize=records.u32();const auto storedOrdinal=records.u64();const auto emission=records.u64();const auto eventTick=records.u64();
		require(typeValue>=1&&typeValue<=12&&storedOrdinal==ordinal&&recordSize>=RecordHeaderSize&&recordSize-RecordHeaderSize<=records.remaining(),"event record header is invalid");
		require(eventTick>=lastTick&&(!haveEmission||emission>lastEmission),"event ordering is invalid");lastTick=eventTick;lastEmission=emission;haveEmission=true;
		auto pbytes=records.bytes(recordSize-RecordHeaderSize);Reader p(pbytes.data(),pbytes.size());const auto type=static_cast<AicaObservationType>(typeValue);++counts[typeValue-1];
		switch(type){
		case AicaObservationType::RegisterWrite:{parseOwner(p,expected,eventTick);const auto address=p.u32();const auto width=p.u8();p.u8();p.u16();const auto value=p.u32();require(width==1||width==2||width==4,"register width is invalid");for(unsigned i=0;i<width;++i)registers[(address+i)&0x7fff]=value>>(8*i);break;}
		case AicaObservationType::RamWrite:{parseOwner(p,expected,eventTick);const auto address=p.u32();const auto n=p.u32();auto data=p.bytes(n);updateRam(ram,address,data);break;}
		case AicaObservationType::G2DmaBegin:{parseOwner(p,expected,eventTick);dmaGeneration=p.u64();p.u32();p.u32();p.u32();p.u32();require(!dmaOpen&&dmaGeneration!=0,"G2 DMA overlap or zero generation");dmaOpen=true;break;}
		case AicaObservationType::G2DmaTransfer:{const auto generation=p.u64();p.u32();const auto destination=p.u32();p.u32();const bool toRam=p.u32()!=0;const auto n=p.u32();auto data=p.bytes(n);require(dmaOpen&&generation==dmaGeneration,"G2 transfer has no matching begin");if(toRam)updateRam(ram,destination,data);break;}
		case AicaObservationType::G2DmaComplete:require(dmaOpen&&p.u64()==dmaGeneration,"G2 completion has no matching begin");dmaOpen=false;break;
		case AicaObservationType::KeyOn:case AicaObservationType::KeyOff:{parseOwner(p,expected,eventTick);const auto channel=p.u8();p.u8();p.u16();require(channel<64,"key channel is invalid");std::array<std::uint8_t,0x80> channelRegs{};for(auto& b:channelRegs)b=p.u8();auto mirrored=std::array<std::uint8_t,0x80>{};std::copy(registers.begin()+channel*0x80,registers.begin()+(channel+1)*0x80,mirrored.begin());mirrored[1]&=0x7f;auto normalized=channelRegs;normalized[1]&=0x7f;require(normalized==mirrored,"key register snapshot differs from register mirror");if(type==AicaObservationType::KeyOn){++keyOns;if(hashKeySource(sourceHasher,channel,channelRegs,ram))++keySources;}break;}
		case AicaObservationType::SampleFrame:{const auto sampleOrdinal=p.u64();p.u64();const auto cddaGeneration=p.u64();const auto frame=p.u16();const bool dsp=p.u8()!=0;p.u8();p.u32();p.u32();const auto inputLeft=static_cast<std::int32_t>(p.u32());const auto inputRight=static_cast<std::int32_t>(p.u32());p.u32();p.u32();p.u32();p.u32();const auto left=static_cast<std::int16_t>(p.u16());const auto right=static_cast<std::int16_t>(p.u16());require(dsp==expected.dspEnabled,"sample DSP mode differs from identity binding");if(samples==0)firstSampleOrdinal=sampleOrdinal;else require(sampleOrdinal==firstSampleOrdinal+samples,"sample ordinal is not contiguous");require(frame<588,"CD-DA frame index is invalid");const auto sector=cddaSectors.find(cddaGeneration);require(sector!=cddaSectors.end(),"sample references an unknown CD-DA generation");require(inputLeft==pcm16(sector->second,frame*4)&&inputRight==pcm16(sector->second,frame*4+2),"sample CD-DA inputs differ from captured sector");std::array<std::uint8_t,4> pcm{static_cast<std::uint8_t>(left),static_cast<std::uint8_t>(left>>8),static_cast<std::uint8_t>(right),static_cast<std::uint8_t>(right>>8)};pcmHasher.update(pcm.data(),pcm.size());++samples;if(left!=0||right!=0)++nonzero;break;}
		case AicaObservationType::Reset:invalid("reset event is not accepted");
		case AicaObservationType::KeyBatchComplete:parseOwner(p,expected,eventTick);p.u64();p.u64();keyBatch=true;break;
		case AicaObservationType::CddaSector:{const auto generation=p.u64();p.u32();p.u32();p.u32();p.u32();const auto n=p.u32();require(generation!=0&&n==2352&&cddaSectors.find(generation)==cddaSectors.end(),"CD-DA sector generation or length is invalid");cddaSectors.emplace(generation,p.bytes(n));break;}
		case AicaObservationType::SampleSuppressed:invalid("suppressed sample event is not accepted");
		default:invalid("event type is invalid");}
		require(p.remaining()==0,"event has trailing payload bytes");
	}
	require(records.remaining()==0&&lastTick==s.endTick&&!dmaOpen&&keyBatch,"artifact has trailing bytes, open DMA, or no stable key batch");
	require(counts==s.typeCounts&&samples==s.sampleFrames&&keyOns==s.keyOnCount&&keySources==s.keyedSourceCount&&nonzero==s.nonzeroSampleFrames,"reconstructed counts differ from header");
	require(keyOns>0&&keySources>0&&nonzero>0,"artifact has no inspectable keyed audio");
	require(sha256Equal(pcmHasher.finalize(),s.pcmDigest)&&sha256Equal(sourceHasher.finalize(),s.keyedSourceDigest),"PCM or keyed-source digest mismatch");
	return s;
}

AicaArtifactSummary validateAicaArtifactWithReplay(
		const std::filesystem::path& artifact,
		const IdentityManifest& identity,
		const std::filesystem::path& replay,
		std::uint64_t maximumArtifactBytes,
		std::uint64_t maximumEvents,
		std::uint64_t maximumReplayBytes)
{
	requireSh4EquivalenceIdentityV2(identity);
	const AicaConfiguration& configuration = identity.runtimeConfiguration.aicaConfiguration;
	if (!configuration.available)
		throw std::runtime_error("identity has no AICA configuration");
	if (identity.runtimeConfiguration.mapleDmaCheckpoint == 0)
		throw std::runtime_error("AICA validation requires an identity DMA checkpoint");

	const MapleTraceSummary replaySummary = validateProductionMapleTraceFile(replay,
			identity.mapleReplayIdentityDigest, maximumReplayBytes);
	if (replaySummary.dmaCount != identity.runtimeConfiguration.mapleDmaCheckpoint)
		throw std::runtime_error("Maple replay does not end at the identity DMA checkpoint");

	AicaArtifactBinding binding;
	binding.backend = identity.runtimeConfiguration.cpuBackend == "dynarec"
			? Sh4ObservationBackend::Dynarec : Sh4ObservationBackend::Interpreter;
	binding.identityDigest = identity.digest;
	binding.replayDigest = hashFileExact(replay, maximumReplayBytes);
	binding.configurationDigest = aicaConfigurationDigest(configuration);
	binding.dspEnabled = configuration.dspEnabled;
	binding.vmuSound = configuration.vmuSound;
	AicaArtifactSummary summary = validateAicaArtifactFile(artifact, binding,
			maximumArtifactBytes, maximumEvents);
	if (summary.startTick < replaySummary.startTick
			|| summary.endTick > replaySummary.endTick)
		throw std::runtime_error("AICA artifact lies outside the authenticated Maple replay boundary");
	return summary;
}

} // namespace research

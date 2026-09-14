#include "research/aica_artifact.h"
#include "research/maple_trace.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace research
{
namespace
{
constexpr std::array<std::uint8_t,8> Magic {'F','C','A','I','C','A','0','2'};
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

AicaWriter parseOwner(Reader& r,const AicaArtifactBinding& binding,std::uint64_t eventTick)
{
	const auto writer=static_cast<AicaWriter>(r.u8());const bool armPc=r.u8()!=0;
	const auto backend=static_cast<Sh4ObservationBackend>(r.u8());const bool valid=r.u8()!=0;
	const auto armAddress=r.u32();const auto generation=r.u64();const auto tick=r.u64();const auto pc=r.u32();const auto pr=r.u32();const auto opcode=r.u16();const auto delayDepth=r.u16();require(r.u32()==0,"owner reserved field is nonzero");
	require(writer!=AicaWriter::Unknown&&static_cast<unsigned>(writer)<=static_cast<unsigned>(AicaWriter::ReiosHle),"owner writer is invalid");
	if(writer==AicaWriter::Sh4Direct||writer==AicaWriter::Sh4G2Dma)
		require(!armPc&&armAddress==0&&valid&&backend==binding.backend&&generation!=0&&tick<=eventTick,"SH-4 owner is inconsistent");
	else require(!armPc&&armAddress==0&&!valid&&backend==Sh4ObservationBackend::Interpreter&&generation==0&&tick==0&&pc==0&&pr==0&&opcode==0&&delayDepth==0,"non-SH-4 owner has an unsupported instruction claim");
	return writer;
}

bool hashKeySource(Sha256& hasher,std::uint8_t channel,const std::array<std::uint8_t,0x80>& regs,
		const std::vector<std::uint8_t>& captured,const std::vector<std::uint8_t>& ram)
{
	const std::uint16_t w=std::uint16_t(regs[0])|(std::uint16_t(regs[1])<<8);if(((w>>10)&1)!=0){require(captured.empty(),"noise key-on has source bytes");return false;}
	const std::uint32_t pcms=(w>>7)&3;std::uint32_t address=(std::uint32_t(w&0x7f)<<16)|regs[4]|(std::uint32_t(regs[5])<<8);if(pcms==0)address&=~1u;
	const std::uint32_t lsa=regs[8]|(std::uint32_t(regs[9])<<8),lea=regs[12]|(std::uint32_t(regs[13])<<8),samples=std::max(lsa,lea);const std::uint64_t length=pcms==0?std::uint64_t(samples)*2:pcms==1?samples:(std::uint64_t(samples)+1)/2;
	require(length!=0&&length==captured.size()&&length<=ram.size(),"key-on sample range is invalid");std::array<std::uint8_t,9> prefix{};prefix[0]=channel;
	for(unsigned i=0;i<4;++i){prefix[1+i]=static_cast<std::uint8_t>(address>>(8*i));prefix[5+i]=static_cast<std::uint8_t>(length>>(8*i));}hasher.update(prefix.data(),prefix.size());
	for(std::uint64_t i=0;i<length;++i)require(captured[i]==ram[(std::uint64_t(address)+i)&(ram.size()-1)],"key-on source snapshot differs from live AICA RAM");hasher.update(captured.data(),captured.size());return true;
}

std::int16_t pcm16(const std::vector<std::uint8_t>& bytes,std::size_t offset)
{return static_cast<std::int16_t>(std::uint16_t(bytes[offset])|(std::uint16_t(bytes[offset+1])<<8));}

std::uint32_t register32(const std::vector<std::uint8_t>& registers,std::uint32_t address)
{
	require(address+4<=registers.size(),"DSP register address is outside the mirror");
	return std::uint32_t(registers[address])|(std::uint32_t(registers[address+1])<<8)
			|(std::uint32_t(registers[address+2])<<16)|(std::uint32_t(registers[address+3])<<24);
}

struct ChannelReplayState
{
	std::uint32_t sampleAddress=0,currentAddress=0,step=0;std::int32_t sample0=0,sample1=0;
	std::uint32_t looped=0;std::int32_t adpcmLastQuant=0,adpcmLoopQuant=0,adpcmLoopSample=0;
	std::uint32_t adpcmInLoop=0,noiseState=0;std::int32_t aegValue=0;std::uint32_t aegState=0;
	std::uint32_t fegValue=0,fegState=0;std::int32_t fegPrevious1=0,fegPrevious2=0,fegFraction=0;
	std::uint32_t lfoCounter=0,lfoState=0,enabled=0;
};

class ChannelReplay
{
public:
	ChannelReplay(std::vector<std::uint8_t>& registers,std::vector<std::uint8_t>& ram,
			const std::array<ChannelReplayState,64>& state,bool dspEnabled)
		:registers(registers),ram(ram),state(state),dspEnabled(dspEnabled)
	{
		for(unsigned i=0;i<256;++i)tl[i]=static_cast<std::int32_t>((1<<15)/std::pow(2.0,i/16.0));
		for(unsigned i=0;i<64;++i){aegAttack[i]=attackSteps(AttackTimes[i]);aegDecay[i]=egSteps(DecayTimes[i]);fegRates[i]=aegDecay[i];}
		for(unsigned scale=0;scale<8;++scale)for(int i=-128;i<128;++i)
			plfoScales[scale][i+128]=static_cast<std::int32_t>(1024.f*std::pow(2.f,PlfoScale[scale]*i/128.f/1200.f));
		for(unsigned channel=0;channel<64;++channel)deriveLfo(channel,true);
	}

	void applyRegisterWrite(std::uint32_t address)
	{
		address&=0x7fff;if(address>=0x2000)return;const unsigned channel=address>>7,offset=address&0x7f;
		if(offset==0||offset==1||offset==4||offset==5){const auto control=word(channel,0);state[channel].sampleAddress=(std::uint32_t(control&0x7f)<<16)|word(channel,4);if(((control>>7)&3)==0)state[channel].sampleAddress&=~1u;}
		if(offset==0x1c||offset==0x1d)deriveLfo(channel,false);
	}
	void keyOn(unsigned channel)
	{
		auto& s=state[channel];if(s.aegState!=3)return;s.enabled=1;s.aegState=0;s.aegValue=0x280<<16;
		s.fegState=0;s.fegValue=(word(channel,0x2c)&0x1fff)<<16;s.fegPrevious1=s.fegPrevious2=s.fegFraction=0;
		s.currentAddress=0;s.step=0;s.looped=0;s.adpcmLastQuant=127;s.adpcmLoopQuant=0;s.adpcmLoopSample=0;
		s.adpcmInLoop=0;s.sample0=0;decode(channel,true,0);
	}
	void keyOff(unsigned channel){auto& s=state[channel];if(s.aegState==3)return;s.aegState=s.fegState=3;clearKeyBit(channel);}
	void validateSample(std::uint64_t ordinal,std::uint64_t capturedMask,std::int32_t capturedLeft,
			std::int32_t capturedRight,const std::array<std::int32_t,16>& capturedInputs)
	{
		std::uint64_t mask=0;for(unsigned channel=0;channel<64;++channel)if(state[channel].enabled)mask|=std::uint64_t{1}<<channel;
		requireAt(ordinal,mask==capturedMask,"active channel mask differs");std::int32_t left=0,right=0;std::array<std::int32_t,16> inputs{};
		for(unsigned channel=0;channel<64;++channel)stepChannel(channel,left,right,inputs);
		requireAt(ordinal,left==capturedLeft&&right==capturedRight,"dry channel mix differs");
		requireAt(ordinal,inputs==capturedInputs,"DSP input lanes differ");
	}
private:
	static constexpr std::array<double,64> AttackTimes{-1,-1,8100.0,6900.0,6000.0,4800.0,4000.0,3400.0,3000.0,2400.0,2000.0,1700.0,1500.0,1200.0,1000.0,860.0,760.0,600.0,500.0,430.0,380.0,300.0,250.0,220.0,190.0,150.0,130.0,110.0,95.0,76.0,63.0,55.0,47.0,38.0,31.0,27.0,24.0,19.0,15.0,13.0,12.0,9.4,7.9,6.8,6.0,4.7,3.8,3.4,3.0,2.4,2.0,1.8,1.6,1.3,1.1,0.93,0.85,0.65,0.53,0.44,0.40,0.35,0.0,0.0};
	static constexpr std::array<double,64> DecayTimes{-1,-1,118200.0,101300.0,88600.0,70900.0,59100.0,50700.0,44300.0,35500.0,29600.0,25300.0,22200.0,17700.0,14800.0,12700.0,11100.0,8900.0,7400.0,6300.0,5500.0,4400.0,3700.0,3200.0,2800.0,2200.0,1800.0,1600.0,1400.0,1100.0,920.0,790.0,690.0,550.0,460.0,390.0,340.0,270.0,230.0,200.0,170.0,140.0,110.0,98.0,85.0,68.0,57.0,49.0,43.0,34.0,28.0,25.0,22.0,18.0,14.0,12.0,11.0,8.5,7.1,6.1,5.4,4.3,3.6,3.1};
	static constexpr std::array<float,8> PlfoScale{0.f,3.61f,7.22f,14.44f,28.88f,57.75f,115.5f,231.f};
	static constexpr std::array<std::int32_t,32> QTable{2048,1536,1024,512,0,-256,-512,-768,-1024,-1280,-1536,-1792,-2048,-2176,-2304,-2432,-2560,-2688,-2816,-2944,-3072,-3136,-3200,-3264,-3328,-3392,-3456,-3520,-3584,-3648,-3712,-3776};
	static constexpr std::array<std::int32_t,8> AdpcmQs{0x0e6,0x0e6,0x0e6,0x0e6,0x133,0x199,0x200,0x266};
	static constexpr std::array<std::int32_t,8> AdpcmScale{1,3,5,7,9,11,13,15};
	static constexpr std::array<std::uint32_t,16> SendLevel{255,14<<3,13<<3,12<<3,11<<3,10<<3,9<<3,8<<3,7<<3,6<<3,5<<3,4<<3,3<<3,2<<3,1<<3,0};
	static void requireAt(std::uint64_t ordinal,bool value,const char* message){if(!value){std::ostringstream out;out<<message<<" at sample "<<ordinal;invalid(out.str());}}
	static std::uint32_t egSteps(double ms){const double all=1024.0*(1<<16)-1;if(ms<0)return 0;if(ms==0)return static_cast<std::uint32_t>(all);return static_cast<std::uint32_t>(std::llround(all/(44.1*ms)));}
	static std::uint32_t attackSteps(double ms){if(ms<0)return 0;if(ms==0)return 1<<16;const double count=44.1*ms;return static_cast<std::uint32_t>(std::llround((1.0/(1.0-1.0/std::pow(0x280,1.0/count)))*(1<<16)));}
	std::uint16_t word(unsigned channel,unsigned offset)const{const auto address=channel*0x80+offset;return std::uint16_t(registers[address])|(std::uint16_t(registers[address+1])<<8);}
	void clearKeyBit(unsigned channel){registers[channel*0x80+1]&=0xbf;}
	std::uint32_t effectiveRate(unsigned channel,std::uint32_t rate)const{const auto pitch=word(channel,0x18),env=word(channel,0x14);auto result=rate*2;const auto krs=(env>>10)&15;if(krs<15){result+=(pitch>>9)&1;result+=std::max(0,static_cast<int>((krs+(((pitch>>11)&15)^8)-8)*2));}return std::min(result,63u);}
	void deriveLfo(unsigned channel,bool derived)
	{
		auto& s=state[channel];const auto value=word(channel,0x1c);const auto alfoType=(value>>3)&3,plfoType=(value>>8)&3,lfof=(value>>10)&31;
		std::uint32_t start;if(alfoType==3&&plfoType==3)start=1;else{const int n=lfof,shift=n>>2,m=(~n)&3,g=128>>shift;start=((g-1)<<2)+g*(m+1);}
		if(!derived)s.lfoCounter=start;if((value&0x8000)&&!derived){s.lfoState=0;s.lfoCounter=start;}
	}
	std::pair<std::uint32_t,std::int32_t> lfoValues(unsigned channel)const
	{
		const auto value=word(channel,0x1c);const std::uint32_t stateValue=state[channel].lfoState&0xff;auto wave=[&](unsigned type)->std::uint32_t{switch(type){case 0:return stateValue;case 1:return stateValue&0x80?255u:0u;case 2:return ((stateValue&0x7f)^((stateValue&0x80)?0x7f:0))<<1;default:return (stateValue*0x41c64e6du+0x3039u)&0xff;}};
		const auto alfo=wave((value>>3)&3)>>(8-(value&7));const auto plfo=plfoScales[(value>>5)&7][static_cast<std::uint8_t>(wave((value>>8)&3))];return {alfo,plfo};
	}
	void stepLfo(unsigned channel)
	{
		auto& s=state[channel];if(--s.lfoCounter==0){++s.lfoState;const auto value=word(channel,0x1c);const auto a=(value>>3)&3;const auto p=(value>>8)&3;const auto n=(value>>10)&31;if(a==3&&p==3)s.lfoCounter=1;else{const int shift=n>>2,m=(~static_cast<int>(n))&3,g=128>>shift;s.lfoCounter=((g-1)<<2)+g*(m+1);}}
	}
	std::int32_t adpcm(std::uint8_t nibble,std::int32_t previous,std::int32_t& quant)const
	{
		const auto data=nibble&7;auto delta=(quant*AdpcmScale[data])>>3;if(delta>0x7fff)delta=0x7fff;auto result=(nibble&8?-1:1)*delta+previous;quant=std::clamp((quant*AdpcmQs[data])>>8,127,24576);return std::clamp(result,-32768,32767);
	}
	std::uint8_t byte(std::uint32_t address)const{return ram[address&(ram.size()-1)];}
	std::int16_t signedWord(std::uint32_t address)const{address&=static_cast<std::uint32_t>(ram.size()-1)&~1u;return static_cast<std::int16_t>(std::uint16_t(ram[address])|(std::uint16_t(ram[address+1])<<8));}
	void decode(unsigned channel,bool last,std::uint32_t ca)
	{
		auto& s=state[channel];const auto control=word(channel,0);const auto format=(control>>7)&3;const auto lsa=word(channel,8);const auto lea=word(channel,12);auto next=ca+1;if(next>=lea&&lea>lsa)next=lsa;std::int32_t a=0,b=0;
		if(control&0x400){s.noiseState=s.noiseState*0x41c64e6du+0x3039u;a=static_cast<std::int32_t>(s.noiseState)>>16;s.noiseState=s.noiseState*0x41c64e6du+0x3039u;b=static_cast<std::int32_t>(s.noiseState)>>16;}
		else if(format==0){a=signedWord(s.sampleAddress+ca*2);b=signedWord(s.sampleAddress+next*2);}
		else if(format==1){a=static_cast<std::int8_t>(byte(s.sampleAddress+ca))<<8;b=static_cast<std::int8_t>(byte(s.sampleAddress+next))<<8;}
		else{auto n1=byte(s.sampleAddress+(ca>>1))>>((ca&1)*4),n2=byte(s.sampleAddress+(next>>1))>>((next&1)*4);std::int32_t q=s.adpcmLastQuant;if(format==2&&ca==lsa){if(!s.adpcmInLoop){s.adpcmInLoop=1;s.adpcmLoopQuant=q;s.adpcmLoopSample=s.sample0;}else{q=s.adpcmLoopQuant;s.sample0=s.adpcmLoopSample;}}a=adpcm(n1&15,s.sample0,q);s.adpcmLastQuant=q;if(last){auto previous=a;if(format==2&&next==lsa&&s.adpcmInLoop){q=s.adpcmLoopQuant;previous=s.adpcmLoopSample;}b=adpcm(n2&15,previous,q);}}
		s.sample0=a;s.sample1=b;
	}
	void disable(unsigned channel){auto& s=state[channel];s.enabled=0;s.aegState=3;s.aegValue=0x3ff<<16;s.currentAddress=0;clearKeyBit(channel);}
	void stepStream(unsigned channel,std::int32_t plfo)
	{
		auto& s=state[channel];const auto control=word(channel,0);const auto format=(control>>7)&3;const bool loop=control&0x200;const auto pitch=word(channel,0x18);std::uint32_t rate=1024|(pitch&0x3ff);const auto oct=(pitch>>11)&15;if(oct&8)rate>>=16-oct;else rate<<=oct;s.step+=static_cast<std::uint32_t>((std::uint64_t(rate)*plfo)>>10);auto advances=s.step>>10;s.step&=0x3ff;while(advances--){auto ca=s.currentAddress+1;auto comparison=ca;if(format==3)comparison&=~3u;const auto lsa=word(channel,8);const auto lea=word(channel,12);if((word(channel,0x14)&0x4000)&&s.aegState==0&&ca>=lsa)s.aegState=1;if(comparison>=lea){if(lsa>lea){if(comparison>=lsa){s.looped=1;ca=0;disable(channel);}}else{s.looped=1;if(!loop){ca=0;disable(channel);}else ca=lsa;}}s.currentAddress=ca;if(advances==0)decode(channel,true,ca);else if(format>=2)decode(channel,false,ca);}}
	void stepAeg(unsigned channel)
	{
		auto& s=state[channel];const auto e0=word(channel,0x10),e1=word(channel,0x14);switch(s.aegState){case 0:{const auto rate=aegAttack[effectiveRate(channel,e0&31)];if(rate){s.aegValue-=static_cast<std::int32_t>(((std::uint64_t(s.aegValue)<<16)/rate)+1);if((s.aegValue>>16)<=0){if(!(e1&0x4000))s.aegState=1;s.aegValue=0;}}break;}case 1:s.aegValue+=aegDecay[effectiveRate(channel,(e0>>6)&31)];if(static_cast<std::uint32_t>(s.aegValue>>16)>=std::uint32_t((e1>>5)&31)*32)s.aegState=2;break;case 2:s.aegValue+=aegDecay[effectiveRate(channel,(e0>>11)&31)];if((s.aegValue>>16)>=0x3ff){s.aegValue=0x3ff<<16;s.aegState=3;clearKeyBit(channel);}break;default:s.aegValue+=aegDecay[effectiveRate(channel,e1&31)];if((s.aegValue>>16)>=0x3ff)disable(channel);break;}}
	bool filterActive(unsigned channel)const{return !(word(channel,0x28)&0x20)&&(word(channel,0x2c)<0x1ff8||word(channel,0x30)<0x1ff8||word(channel,0x34)<0x1ff8||word(channel,0x38)<0x1ff8||word(channel,0x3c)<0x1ff8||(word(channel,0x28)&31)!=4);}
	void stepFeg(unsigned channel)
	{
		auto& s=state[channel];if(!filterActive(channel))return;const std::array<unsigned,4> targetOffset{0x30,0x34,0x38,0x3c};const auto rates0=word(channel,0x40),rates1=word(channel,0x44);const std::array<std::uint32_t,4> raw{std::uint32_t((rates0>>8)&31),std::uint32_t(rates0&31),std::uint32_t((rates1>>8)&31),std::uint32_t(rates1&31)};const auto delta=fegRates[effectiveRate(channel,raw[s.fegState])],target=std::uint32_t(word(channel,targetOffset[s.fegState])&0x1fff)<<16;if(s.fegValue<target)s.fegValue+=std::min(delta,target-s.fegValue);else if(s.fegValue>target)s.fegValue-=std::min(delta,s.fegValue-target);else if(s.fegState<2)++s.fegState;
	}
	std::int32_t lowPass(unsigned channel,std::int32_t sample)
	{
		auto& s=state[channel];if(!filterActive(channel))return sample;const auto fv=s.fegValue>>16,exp=fv>>9,mant=(fv&0x1ff)|0x200;std::uint64_t a0=(std::uint64_t(mant)<<30)>>((15-exp)*2);a0*=(mant-1)/8;a0>>=17;std::int64_t f=(std::int64_t(mant)<<exp)<<5;f+=std::int64_t(QTable[word(channel,0x28)&31])*f/4096;const std::int64_t b1=128ll*1024*(1<<14)-(f+a0),b2=64ll*1024*(1<<14)-f;if(exp==0)s.fegFraction=0;const auto mac=-std::int64_t(a0)*sample+b1*s.fegPrevious1-b2*s.fegPrevious2-s.fegFraction;sample=static_cast<std::int32_t>(mac>>30);s.fegFraction=static_cast<std::int32_t>((std::int64_t(sample)<<30)-mac);s.fegPrevious2=s.fegPrevious1;sample=std::clamp(sample,-512*1024,512*1024-1);s.fegPrevious1=sample;return sample;
	}
	void stepChannel(unsigned channel,std::int32_t& left,std::int32_t& right,std::array<std::int32_t,16>& inputs)
	{
		auto& s=state[channel];if(!s.enabled)return;const auto fp=s.step&0x3ff;auto sample=((std::int64_t(s.sample0)*(1024-fp))>>10)+((std::int64_t(s.sample1)*fp)>>10);sample=lowPass(channel,static_cast<std::int32_t>(sample)<<4);const auto mix=word(channel,0x20);const auto direct=word(channel,0x24);const auto level=word(channel,0x28);const auto [alfo,plfo]=lfoValues(channel);const std::uint32_t offset=(level&0x40)?0:std::min<std::uint32_t>(255,alfo+(s.aegValue>>18));const auto maxAtt=255-offset;const auto total=(level&0x40)?0:(level>>8);const auto full=total+SendLevel[(direct>>8)&15];const auto pan=full+SendLevel[(~direct)&15];std::uint32_t dl=0,dr=0;if(direct&16){dl=full;dr=pan;}else{dl=pan;dr=full;}const auto dspAtt=total+SendLevel[(mix>>4)&15];const auto table=[&](std::uint32_t att){return att+offset<tl.size()?tl[att+offset]:0;};auto outLeft=static_cast<std::int32_t>((sample*table(std::min(dl,maxAtt)))>>19);auto outRight=static_cast<std::int32_t>((sample*table(std::min(dr,maxAtt)))>>19);const auto outDsp=static_cast<std::int32_t>((sample*table(std::min(dspAtt,maxAtt)))>>15);if(outLeft+outRight==0&&!dspEnabled)outLeft=outRight=outDsp>>4;left+=outLeft;right+=outRight;inputs[mix&15]+=outDsp;stepAeg(channel);if(s.enabled){stepFeg(channel);stepStream(channel,plfo);stepLfo(channel);}}
	std::vector<std::uint8_t>& registers;std::vector<std::uint8_t>& ram;std::array<ChannelReplayState,64> state{};bool dspEnabled=false;
	std::array<std::int32_t,1024> tl{};std::array<std::uint32_t,64> aegAttack{},aegDecay{},fegRates{};std::array<std::array<std::int32_t,256>,8> plfoScales{};
};

class DspReplay
{
public:
	struct Snapshot
	{
		std::array<std::int32_t,128> temp{};std::array<std::int32_t,32> mems{};
		std::array<std::int32_t,16> mixs{};std::array<std::int16_t,16> effects{};
		std::uint32_t rbp=0,rbl=0,mdec=0;std::int32_t extLeft=0,extRight=0;
		std::vector<std::uint8_t> dspRegisters;std::vector<std::uint8_t> ring;
		Sha256Digest stateDigest()const
		{
			Sha256 hasher;
			auto u16=[&](std::uint16_t value){const std::array<std::uint8_t,2> bytes{
					static_cast<std::uint8_t>(value),static_cast<std::uint8_t>(value>>8)};
				hasher.update(bytes.data(),bytes.size());};
			auto u32=[&](std::uint32_t value){const std::array<std::uint8_t,4> bytes{
					static_cast<std::uint8_t>(value),static_cast<std::uint8_t>(value>>8),
					static_cast<std::uint8_t>(value>>16),static_cast<std::uint8_t>(value>>24)};
				hasher.update(bytes.data(),bytes.size());};
			auto u64=[&](std::uint64_t value){for(unsigned shift=0;shift<64;shift+=8){
				const auto byte=static_cast<std::uint8_t>(value>>shift);hasher.update(&byte,1);}};
			for(auto value:temp)u32(static_cast<std::uint32_t>(value));
			for(auto value:mems)u32(static_cast<std::uint32_t>(value));
			for(auto value:mixs)u32(static_cast<std::uint32_t>(value));
			for(auto value:effects)u16(static_cast<std::uint16_t>(value));
			u32(rbp);u32(rbl);u32(mdec);u32(static_cast<std::uint32_t>(extLeft));
			u32(static_cast<std::uint32_t>(extRight));
			u64(dspRegisters.size());hasher.update(dspRegisters.data(),dspRegisters.size());
			u64(ring.size());hasher.update(ring.data(),ring.size());
			return hasher.finalize();
		}
		bool equals(const Snapshot& other)const{return temp==other.temp&&mems==other.mems&&mixs==other.mixs
				&&effects==other.effects&&rbp==other.rbp&&rbl==other.rbl&&mdec==other.mdec
				&&extLeft==other.extLeft&&extRight==other.extRight&&dspRegisters==other.dspRegisters&&ring==other.ring;}
		std::string difference(const Snapshot& other)const
		{
			auto indexed=[](const char* name,const auto& left,const auto& right){if(left.size()!=right.size()){std::ostringstream out;out<<name<<" length anchor="<<left.size()<<" terminal="<<right.size();return out.str();}for(std::size_t i=0;i<left.size();++i)if(left[i]!=right[i]){std::ostringstream out;out<<name<<'['<<i<<"] anchor="<<+left[i]<<" terminal="<<+right[i];return out.str();}return std::string{};};
			for(const auto& item:std::array<std::string,6>{indexed("TEMP",temp,other.temp),indexed("MEMS",mems,other.mems),indexed("MIXS",mixs,other.mixs),indexed("EFREG",effects,other.effects),indexed("DSP register",dspRegisters,other.dspRegisters),indexed("ring byte",ring,other.ring)})if(!item.empty())return item;
			if(rbp!=other.rbp)return "RBP";if(rbl!=other.rbl)return "RBL";if(mdec!=other.mdec)return "MDEC";
			if(extLeft!=other.extLeft)return "EXTS left";if(extRight!=other.extRight)return "EXTS right";return "unknown field";
		}
	};
	DspReplay(std::vector<std::uint8_t>& registers,std::vector<std::uint8_t>& ram,
			const std::array<std::int32_t,128>& temp,const std::array<std::int32_t,32>& mems,
			std::uint32_t rbp,std::uint32_t rbl,std::uint32_t mdec)
		: registers(registers),ram(ram),temp(temp),mems(mems),rbp(rbp),rbl(rbl),mdec(mdec)
	{
		for(unsigned i=0;i<effects.size();++i)
			effects[i]=static_cast<std::int16_t>(register32(registers,0x4580+i*4));
		for(unsigned i=1;i<volume.size();++i)
			volume[i]=static_cast<std::int32_t>((1<<15)/std::pow(2.0,(15-i)/2.0));
	}

	void applyRegisterWrite(std::uint32_t address,std::uint8_t width,std::uint32_t value)
	{
		address&=0x7fff;
		const unsigned actualWidth=width==4?2:width;
		if(address>=0x3000&&(address&2))return;
		if(address>=0x4000&&address<0x4580)
		{
			if(address>=0x4500){ applyBuffered(mixs[(address-0x4500)/8],address,width,value,true); }
			else if(address<0x4500){ auto& v=address<0x4400?temp[(address-0x4000)/8]:mems[(address-0x4400)/8];applyBuffered(v,address,width,value,false); }
			return;
		}
		for(unsigned i=0;i<actualWidth&&address+i<registers.size();++i)
			registers[address+i]=static_cast<std::uint8_t>(value>>(8*i));
		if((address<=0x2805&&address+actualWidth>0x2804))decodeRing();
		if(address<0x45c0&&address+actualWidth>0x4580)
			for(unsigned i=0;i<effects.size();++i)
				effects[i]=static_cast<std::int16_t>(register32(registers,0x4580+i*4));
	}

	bool validateSample(std::uint64_t ordinal,const std::array<std::int32_t,16>& inputs,
			const std::array<std::int16_t,16>& capturedEffects,std::int32_t dryLeft,
			std::int32_t dryRight,std::int32_t cddaLeft,std::int32_t cddaRight,
			std::int32_t capturedCddaLeft,std::int32_t capturedCddaRight,
			std::int32_t capturedDspLeft,std::int32_t capturedDspRight,
			std::int16_t finalLeft,std::int16_t finalRight,bool enabled,bool vmuSound)
	{
		mixs=inputs;
		std::int32_t cddaMixLeft=0,cddaMixRight=0;
		volumePan(cddaLeft,16,cddaMixLeft,cddaMixRight);
		volumePan(cddaRight,17,cddaMixLeft,cddaMixRight);
		requireAt(ordinal,cddaMixLeft==capturedCddaLeft&&cddaMixRight==capturedCddaRight,
				"CD-DA routing contribution differs");
		std::int32_t dspLeft=0,dspRight=0;
		bool mdecStepped=false;
		if(enabled)
		{
			mdecStepped=run(cddaLeft,cddaRight);
			requireAt(ordinal,effects==capturedEffects,"DSP effect registers differ");
			for(unsigned i=0;i<effects.size();++i)volumePan(effects[i],i,dspLeft,dspRight);
		}
		else
			requireAt(ordinal,capturedEffects==std::array<std::int16_t,16>{},"disabled DSP has effect output");
		requireAt(ordinal,dspLeft==capturedDspLeft&&dspRight==capturedDspRight,
				"DSP return routing contribution differs");
		requireAt(ordinal,!vmuSound,"VMU beep is not represented in the sample contract");
		std::int32_t left=dryLeft+cddaMixLeft+dspLeft,right=dryRight+cddaMixRight+dspRight;
		const auto common=register32(registers,0x2800);
		if((common>>15)&1){const auto mono=(left+right)>>1;left=right=mono;}
		left=static_cast<std::int32_t>((std::int64_t(left)*volume[common&15])>>15);
		right=static_cast<std::int32_t>((std::int64_t(right)*volume[common&15])>>15);
		if((common>>8)&1){left>>=2;right>>=2;}
		left=std::clamp(left,-32768,32767);right=std::clamp(right,-32768,32767);
		requireAt(ordinal,left==finalLeft&&right==finalRight,"final master mix differs");
		return mdecStepped;
	}

	std::uint32_t memoryDecodeCounter()const{return mdec;}
	std::uint32_t ringPointer()const{return rbp;}
	std::uint32_t ringLength()const{return rbl;}
	Snapshot snapshot()const
	{
		Snapshot result;result.temp=temp;result.mems=mems;result.mixs=mixs;result.effects=effects;
		result.rbp=rbp;result.rbl=rbl;result.mdec=mdec;result.extLeft=extLeft;result.extRight=extRight;
		auto append=[&](std::uint32_t first,std::uint32_t last){result.dspRegisters.insert(result.dspRegisters.end(),registers.begin()+first,registers.begin()+last);};
		append(0x2000,0x2048);append(0x2800,0x2808);append(0x3000,0x3c00);
		const auto mask=ram.size()-1;result.ring.reserve((std::uint64_t(rbl)+1)*2);
		for(std::uint64_t i=0;i<(std::uint64_t(rbl)+1)*2;++i)result.ring.push_back(ram[(std::uint64_t(rbp)+i)&mask]);
		return result;
	}

private:
	static std::uint16_t pack(std::int32_t value)
	{
		const int sign=(value>>23)&1;std::uint32_t temp=(value^(value<<1))&0xffffff;int exponent=0;
		for(int k=0;k<12;++k){if(temp&0x800000)break;temp<<=1;++exponent;}
		if(exponent<12)value<<=exponent;else value<<=11;
		value=(value>>11)&0x7ff;value|=sign<<15;value|=exponent<<11;return static_cast<std::uint16_t>(value);
	}
	static std::int32_t unpack(std::uint16_t value)
	{
		const int sign=(value>>15)&1;int exponent=(value>>11)&15;const int mantissa=value&0x7ff;
		std::int32_t result=mantissa<<11;result|=sign<<22;if(exponent>11)exponent=11;else result^=1<<22;
		result|=sign<<23;result<<=8;result>>=8;result>>=exponent;return result;
	}
	static void requireAt(std::uint64_t ordinal,bool value,const char* message)
	{
		if(!value){std::ostringstream out;out<<message<<" at sample "<<ordinal;invalid(out.str());}
	}
	void decodeRing()
	{
		const auto common=register32(registers,0x2804);rbl=(8192u<<((common>>13)&3))-1;
		rbp=((common&0xfff)*2048u)&static_cast<std::uint32_t>(ram.size()-1);
	}
	static void applyBuffered(std::int32_t& target,std::uint32_t address,std::uint8_t width,
			std::uint32_t value,bool input20)
	{
		if(address&4)
		{
			if(width==1){if(address&1)target=(target&(input20?0x00000fff:0x0000ffff))
					|((static_cast<std::int32_t>(value)<<24)>>(input20?12:8));
				else target=(target&(input20?0xfffff00f:0xffff00ff))|((value&0xff)<<(input20?4:8));}
			else target=(target&(input20?0xf:0xff))|((static_cast<std::int32_t>(value)<<16)>>(input20?12:8));
		}
		else if(width!=1||(address&1)==0)target=(target&~(input20?0xf:0xff))|(value&(input20?0xf:0xff));
	}
	void volumePan(std::int32_t value,unsigned route,std::int32_t& left,std::int32_t& right)const
	{
		const auto control=register32(registers,0x2000+route*4);const auto pan=control&31;const auto vol=(control>>8)&15;
		const auto direct=static_cast<std::int32_t>((std::int64_t(value)*volume[vol])>>15);
		const auto attenuated=static_cast<std::int32_t>((std::int64_t(direct)*volume[15-(pan&15)])>>15);
		if(pan&16){left+=direct;right+=attenuated;}else{left+=attenuated;right+=direct;}
	}
	bool run(std::int32_t externalLeft,std::int32_t externalRight)
	{
		this->extLeft=externalLeft;this->extRight=externalRight;
		bool stopped=true;for(unsigned i=0;i<128*4;++i)if(register32(registers,0x3400+i*4)!=0){stopped=false;break;}
		if(stopped)return false;
		std::int32_t acc=0,shifted=0,x=0,y=0,b=0,input=0,memval[4]{},frc=0,yreg=0;std::uint32_t adrs=0;
		for(unsigned step=0;step<128;++step)
		{
			std::array<std::uint32_t,4> ins{};for(unsigned w=0;w<4;++w)ins[w]=register32(registers,0x3400+(step*4+w)*4);
			if(ins[0]==0&&ins[1]==0&&ins[2]==0&&ins[3]==0){x=temp[mdec&0x7f];y=frc;acc=static_cast<std::int32_t>((std::int64_t(x)*y>>12)+x);continue;}
			const auto tra=(ins[0]>>9)&0x7f;const bool twt=ins[0]&0x100;const bool xsel=ins[1]&0x8000;
			const auto ysel=(ins[1]>>13)&3,ira=(ins[1]>>7)&0x3f;const bool iwt=ins[1]&0x40;
			const bool ewt=ins[2]&0x1000,adrl=ins[2]&0x80,frcl=ins[2]&0x40;const auto shift=(ins[2]>>4)&3;
			const bool yrl=ins[2]&8,negb=ins[2]&4,zero=ins[2]&2,bsel=ins[2]&1;
			if(ira<=0x1f)input=mems[ira];else if(ira<=0x2f)input=mixs[ira-0x20]<<4;
			else if(ira==0x30)input=externalLeft<<8;else if(ira==0x31)input=externalRight<<8;else input=0;
			if(iwt)mems[(ins[1]>>1)&0x1f]=memval[step&3];
			if(!zero){b=bsel?acc:temp[(tra+mdec)&0x7f];if(negb)b=-b;}else b=0;
			x=xsel?input:temp[(tra+mdec)&0x7f];
			if(ysel==0)y=frc;else if(ysel==1)y=static_cast<std::int16_t>(register32(registers,0x3000+step*4))>>3;
			else if(ysel==2)y=yreg>>11;else y=(yreg>>4)&0xfff;if(yrl)yreg=input;
			shifted=(shift==0||shift==3)?acc:acc<<1;if(shift<2)shifted=std::clamp(shifted,-0x800000,0x7fffff);
			acc=static_cast<std::int32_t>((std::int64_t(x)*y>>12)+b);
			if(twt)temp[((ins[0]>>1)&0x7f)+mdec&0x7f]=shifted;
			if(frcl)frc=shift==3?(shifted&0xfff):(shifted>>11);
			if(step&1)
			{
				const bool mwt=ins[2]&0x4000,mrd=ins[2]&0x2000;if(mrd||mwt){const bool table=ins[2]&0x8000;
					const auto masa=(ins[3]>>9)&0x3f;std::uint32_t addr=register32(registers,0x3200+masa*4)&0xffff;
					if(ins[3]&0x100)addr+=adrs&0xfff;if(ins[3]&0x80)++addr;if(!table){addr+=mdec;addr&=rbl;}else addr&=0xffff;
					addr=(addr<<1)+rbp;addr&=static_cast<std::uint32_t>(ram.size()-1);const auto word=std::uint16_t(ram[addr])|(std::uint16_t(ram[(addr+1)&(ram.size()-1)])<<8);
					if(mrd)memval[(step+2)&3]=unpack(static_cast<std::uint16_t>(word));if(mwt){const auto packed=pack(shifted);ram[addr]=static_cast<std::uint8_t>(packed);ram[(addr+1)&(ram.size()-1)]=static_cast<std::uint8_t>(packed>>8);}}
			}
			if(adrl)adrs=shift==3?static_cast<std::uint32_t>(shifted>>12):static_cast<std::uint32_t>(input>>16);
			if(ewt)effects[(ins[2]>>8)&15]=static_cast<std::int16_t>(shifted>>8);
		}
		if(--mdec==0)mdec=rbl+1;
		return true;
	}

	std::vector<std::uint8_t>& registers;std::vector<std::uint8_t>& ram;
	std::array<std::int32_t,128> temp{};std::array<std::int32_t,32> mems{};std::array<std::int32_t,16> mixs{};
	std::array<std::int16_t,16> effects{};std::array<std::int32_t,16> volume{};
	std::uint32_t rbp=0,rbl=0,mdec=0;std::int32_t extLeft=0,extRight=0;
};

} // namespace

AicaArtifactSummary validateAicaArtifactFile(const std::filesystem::path& artifact,
		const AicaArtifactBinding& expected,std::uint64_t maximumBytes,std::uint64_t maximumEvents,
		bool requireDspTailProof)
{
	std::vector<std::uint8_t> bytes;
	AicaArtifactSummary s;s.binding=expected;
	try
	{
	bytes=readFileExact(artifact,maximumBytes);s.artifactBytes=bytes.size();
	s.artifactDigest=sha256(bytes.data(),bytes.size());s.artifactDigestAvailable=true;
	const auto size=bytes.size();
	require(size>=AicaArtifactHeaderSize,"file size is outside the accepted bound");
	require(crc32(bytes.data(),AicaArtifactHeaderSize-4)==(std::uint32_t(bytes[508])|(std::uint32_t(bytes[509])<<8)|(std::uint32_t(bytes[510])<<16)|(std::uint32_t(bytes[511])<<24)),"header CRC mismatch");
	Reader h(bytes.data(),AicaArtifactHeaderSize);for(auto m:Magic)require(h.u8()==m,"magic mismatch");
	require(h.u32()==AicaArtifactSchemaVersion&&h.u32()==AicaArtifactHeaderSize&&h.u32()==0x01020304,"header version or endian marker mismatch");
	require(h.u32()==1,"artifact is incomplete");AicaArtifactBinding claimedBinding;
	claimedBinding.backend=static_cast<Sh4ObservationBackend>(h.u32());const auto configurationFlags=h.u32();require((configurationFlags&~3u)==0,"header configuration flags are invalid");claimedBinding.dspEnabled=(configurationFlags&1u)!=0;claimedBinding.vmuSound=(configurationFlags&2u)!=0;
	s.eventCount=h.u64();s.payloadBytes=h.u64();s.droppedEvents=h.u64();s.startTick=h.u64();s.endTick=h.u64();s.targetSampleFrames=h.u64();s.sampleFrames=h.u64();s.keyOnCount=h.u64();s.keyedSourceCount=h.u64();s.nonzeroSampleFrames=h.u64();s.checkpointRamBytes=h.u64();
	s.checkpointNextSampleOrdinal=h.u64();s.checkpointPhase=static_cast<AicaCheckpointPhase>(h.u32());require(h.u32()==0,"header checkpoint reserved field is nonzero");
	claimedBinding.identityDigest=readDigest(h);claimedBinding.replayDigest=readDigest(h);claimedBinding.configurationDigest=readDigest(h);s.payloadDigest=readDigest(h);s.pcmDigest=readDigest(h);s.keyedSourceDigest=readDigest(h);for(auto& count:s.typeCounts)count=h.u64();
	require(claimedBinding.backend==expected.backend&&claimedBinding.dspEnabled==expected.dspEnabled&&claimedBinding.vmuSound==expected.vmuSound&&sha256Equal(claimedBinding.identityDigest,expected.identityDigest)&&sha256Equal(claimedBinding.replayDigest,expected.replayDigest)&&sha256Equal(claimedBinding.configurationDigest,expected.configurationDigest),"artifact binding mismatch");
	require(s.eventCount<=maximumEvents&&s.payloadBytes==size-AicaArtifactHeaderSize&&s.droppedEvents==0,"header counts or dropped count are invalid");
	require(s.targetSampleFrames>0&&s.targetSampleFrames<=MaximumAicaSampleFrames&&s.sampleFrames==s.targetSampleFrames,"sample target is invalid");
	Sha256 payloadHasher;payloadHasher.update(bytes.data()+AicaArtifactHeaderSize,bytes.size()-AicaArtifactHeaderSize);require(sha256Equal(payloadHasher.finalize(),s.payloadDigest),"payload digest mismatch");

	Reader records(bytes.data()+AicaArtifactHeaderSize,bytes.size()-AicaArtifactHeaderSize);
	require(records.u32()==0,"first record is not a checkpoint");const auto checkpointSize=records.u32();
	require(records.u64()==0,"checkpoint ordinal is invalid");records.u64();const auto tick=records.u64();
	require(checkpointSize>=RecordHeaderSize&&checkpointSize-RecordHeaderSize<=records.remaining(),"checkpoint record header is invalid");
	auto checkpointBytes=records.bytes(checkpointSize-RecordHeaderSize);Reader checkpoint(checkpointBytes.data(),checkpointBytes.size());
	require(tick==s.startTick,"checkpoint tick differs from header");
	const auto checkpointPhase=static_cast<AicaCheckpointPhase>(checkpoint.u32());require(checkpoint.u32()==0,"checkpoint phase reserved field is nonzero");const auto checkpointNextSampleOrdinal=checkpoint.u64();const auto checkpointActiveMask=checkpoint.u64();require(checkpoint.u32()==0x8000,"checkpoint register size is invalid");const auto ramSize=checkpoint.u32();require(ramSize==s.checkpointRamBytes&&ramSize>=131072&&ramSize<=8u*1024u*1024u&&(ramSize&(ramSize-1))==0,"checkpoint RAM size mismatch");
	require(checkpointPhase==AicaCheckpointPhase::PreKeyBatch&&checkpointPhase==s.checkpointPhase&&checkpointNextSampleOrdinal==s.checkpointNextSampleOrdinal,"checkpoint phase or sample cut differs from header");
	const auto checkpointCddaIndex=checkpoint.u32();require(checkpoint.u32()==0,"checkpoint reserved field is nonzero");const auto checkpointCddaGeneration=checkpoint.u64();
	auto registers=checkpoint.bytes(0x8000);auto ram=checkpoint.bytes(ramSize);std::array<ChannelReplayState,64> checkpointChannels{};std::uint64_t reconstructedActiveMask=0;for(unsigned channel=0;channel<64;++channel){auto& value=checkpointChannels[channel];value.sampleAddress=checkpoint.u32();value.currentAddress=checkpoint.u32();value.step=checkpoint.u32();value.sample0=static_cast<std::int32_t>(checkpoint.u32());value.sample1=static_cast<std::int32_t>(checkpoint.u32());value.looped=checkpoint.u32();value.adpcmLastQuant=static_cast<std::int32_t>(checkpoint.u32());value.adpcmLoopQuant=static_cast<std::int32_t>(checkpoint.u32());value.adpcmLoopSample=static_cast<std::int32_t>(checkpoint.u32());value.adpcmInLoop=checkpoint.u32();value.noiseState=checkpoint.u32();value.aegValue=static_cast<std::int32_t>(checkpoint.u32());value.aegState=checkpoint.u32();value.fegValue=checkpoint.u32();value.fegState=checkpoint.u32();value.fegPrevious1=static_cast<std::int32_t>(checkpoint.u32());value.fegPrevious2=static_cast<std::int32_t>(checkpoint.u32());value.fegFraction=static_cast<std::int32_t>(checkpoint.u32());value.lfoCounter=checkpoint.u32();value.lfoState=checkpoint.u32();value.enabled=checkpoint.u32();require(value.enabled<=1,"checkpoint channel enabled field is invalid");if(value.enabled)reconstructedActiveMask|=std::uint64_t{1}<<channel;}require(reconstructedActiveMask==checkpointActiveMask,"checkpoint active-channel mask differs from channel runtime");
	std::array<std::int32_t,128> checkpointTemp{};for(auto& value:checkpointTemp)value=static_cast<std::int32_t>(checkpoint.u32());
	std::array<std::int32_t,32> checkpointMems{};for(auto& value:checkpointMems)value=static_cast<std::int32_t>(checkpoint.u32());
	std::array<std::int32_t,16> checkpointMixs{};for(auto& value:checkpointMixs)value=static_cast<std::int32_t>(checkpoint.u32());
	const auto checkpointRbp=checkpoint.u32();const auto checkpointRbl=checkpoint.u32();const auto checkpointMdec=checkpoint.u32();require(checkpointRbl==8191||checkpointRbl==16383||checkpointRbl==32767||checkpointRbl==65535,"checkpoint DSP ring length is invalid");require(checkpointMdec>0&&checkpointMdec<=checkpointRbl+1,"checkpoint DSP memory-decode counter is invalid");require(checkpointRbp<ramSize,"checkpoint DSP ring pointer is invalid");auto checkpointCdda=checkpoint.bytes(2352);const auto cddaSourceAvailable=checkpoint.u32();checkpoint.u32();checkpoint.u32();checkpoint.u32();const auto cddaReadSuccessful=checkpoint.u32();require(checkpoint.u32()==0,"checkpoint CD-DA reserved field is nonzero");checkpoint.u64();require(checkpoint.remaining()==0&&checkpointCddaIndex<=1176&&cddaSourceAvailable<=1&&cddaReadSuccessful<=1,"checkpoint layout or CD-DA state is invalid");
	const auto checkpointRingRegister=register32(registers,0x2804);
	const auto decodedCheckpointRbl=(8192u<<((checkpointRingRegister>>13)&3))-1;
	const auto decodedCheckpointRbp=((checkpointRingRegister&0xfff)*2048u)&static_cast<std::uint32_t>(ramSize-1);
	require(checkpointRbl==decodedCheckpointRbl&&checkpointRbp==decodedCheckpointRbp,
			"checkpoint DSP ring geometry differs from common register 0x2804");
	DspReplay dspReplay(registers,ram,checkpointTemp,checkpointMems,checkpointRbp,checkpointRbl,checkpointMdec);
	ChannelReplay channelReplay(registers,ram,checkpointChannels,expected.dspEnabled);
	constexpr std::uint64_t RequiredTailSamples=65536;
	require(!requireDspTailProof||expected.dspEnabled,"DSP tail proof was requested for a DSP-disabled artifact");
	require(!requireDspTailProof||(dspReplay.ringLength()==RequiredTailSamples-1&&s.targetSampleFrames>RequiredTailSamples),"DSP tail proof requires a 65536-word ring and an anchor sample");
	const std::uint64_t tailAnchorAfterSamples=requireDspTailProof?s.targetSampleFrames-RequiredTailSamples:0;
	std::optional<DspReplay::Snapshot> tailAnchor,tailTerminal;std::optional<std::string> tailZeroFailure,tailGeometryFailure;
	std::optional<AicaDspTailFailure> tailOutputFailure;
	std::uint64_t tailNoInputSamples=0,tailMdecSteps=0;
	require((checkpointCddaIndex&1)==0,"checkpoint CD-DA index is not stereo aligned");
	std::uint64_t expectedCddaGeneration=checkpointCddaGeneration;std::uint16_t expectedCddaFrame=static_cast<std::uint16_t>(checkpointCddaIndex/2);
	std::map<std::uint64_t,std::vector<std::uint8_t>> cddaSectors;cddaSectors.emplace(checkpointCddaGeneration,std::move(checkpointCdda));
	Sha256 pcmHasher,sourceHasher;std::array<std::uint64_t,13> counts{};std::uint64_t samples=0,keyOns=0,keySources=0,nonzero=0;bool keyBatch=false,keyBatchOpen=true,dmaOpen=false;std::uint64_t batchKeyOnMask=0,batchKeyOffMask=0,dmaGeneration=0,maximumTick=tick,lastEmission=0;std::uint32_t dmaSource=0,dmaDestination=0,dmaLength=0;bool dmaToRam=false,haveEmission=false;std::uint64_t firstSampleOrdinal=0,currentSampleCut=checkpointNextSampleOrdinal;
	std::uint64_t capturedKeyTransitionMask=0,capturedObservedActiveMask=0;
	bool finalSampleSeen=false;
	auto recordOwner=[&](Reader& ownerReader,std::uint64_t ownerTick){const auto writer=parseOwner(ownerReader,expected,ownerTick);s.ownerWriterMask|=1u<<static_cast<unsigned>(writer);};
	for(std::uint64_t ordinal=1;ordinal<=s.eventCount;++ordinal){
		const auto typeValue=records.u32();const auto recordSize=records.u32();const auto storedOrdinal=records.u64();const auto emission=records.u64();const auto eventTick=records.u64();
		require(!finalSampleSeen,"record follows the final AICA sample");
		require(typeValue>=1&&typeValue<=13&&storedOrdinal==ordinal&&recordSize>=RecordHeaderSize&&recordSize-RecordHeaderSize<=records.remaining(),"event record header is invalid");
		require(eventTick>=tick&&(!haveEmission||emission>lastEmission),"event ordering is invalid");maximumTick=std::max(maximumTick,eventTick);lastEmission=emission;haveEmission=true;
		auto pbytes=records.bytes(recordSize-RecordHeaderSize);Reader p(pbytes.data(),pbytes.size());const auto type=static_cast<AicaObservationType>(typeValue);++counts[typeValue-1];
		switch(type){
		case AicaObservationType::RegisterWrite:{recordOwner(p,eventTick);const auto address=p.u32();const auto width=p.u8();p.u8();p.u16();const auto value=p.u32();require(width==1||width==2||width==4,"register width is invalid");dspReplay.applyRegisterWrite(address,width,value);channelReplay.applyRegisterWrite(address);break;}
		case AicaObservationType::RamWrite:{recordOwner(p,eventTick);const auto address=p.u32();const auto n=p.u32();auto data=p.bytes(n);updateRam(ram,address,data);break;}
		case AicaObservationType::G2DmaBegin:{recordOwner(p,eventTick);dmaGeneration=p.u64();dmaSource=p.u32();dmaDestination=p.u32();dmaLength=p.u32();const auto toRam=p.u32();require(!dmaOpen&&dmaGeneration!=0&&dmaLength!=0&&toRam<=1,"G2 DMA begin is invalid");dmaToRam=toRam!=0;dmaOpen=true;break;}
		case AicaObservationType::G2DmaTransfer:{const auto generation=p.u64();const auto source=p.u32();const auto destination=p.u32();const auto length=p.u32();const auto toRam=p.u32();const auto n=p.u32();auto data=p.bytes(n);require(dmaOpen&&generation==dmaGeneration&&source==dmaSource&&destination==dmaDestination&&length==dmaLength&&toRam==static_cast<std::uint32_t>(dmaToRam)&&n==dmaLength,"G2 transfer differs from its begin");if(dmaToRam)updateRam(ram,destination,data);break;}
		case AicaObservationType::G2DmaComplete:require(dmaOpen&&p.u64()==dmaGeneration,"G2 completion has no matching begin");dmaOpen=false;break;
		case AicaObservationType::KeyOn:case AicaObservationType::KeyOff:{recordOwner(p,eventTick);const auto channel=p.u8();p.u8();p.u16();const auto sampleCut=p.u64();require(keyBatchOpen&&channel<64&&sampleCut==currentSampleCut,"key channel, batch, or sample cut is invalid");capturedKeyTransitionMask|=std::uint64_t{1}<<channel;std::array<std::uint8_t,0x80> channelRegs{};for(auto& b:channelRegs)b=p.u8();const auto sourceSize=p.u32();auto sourceBytes=p.bytes(sourceSize);auto mirrored=std::array<std::uint8_t,0x80>{};std::copy(registers.begin()+channel*0x80,registers.begin()+(channel+1)*0x80,mirrored.begin());mirrored[1]&=0x7f;auto normalized=channelRegs;normalized[1]&=0x7f;require(normalized==mirrored,"key register snapshot differs from register mirror");if(type==AicaObservationType::KeyOn){batchKeyOnMask|=std::uint64_t{1}<<channel;channelReplay.keyOn(channel);++keyOns;if(hashKeySource(sourceHasher,channel,channelRegs,sourceBytes,ram))++keySources;}else{batchKeyOffMask|=std::uint64_t{1}<<channel;require(sourceBytes.empty(),"key-off has source bytes");channelReplay.keyOff(channel);}break;}
		case AicaObservationType::SampleFrame:
		{
			const auto sampleOrdinal=p.u64();const auto activeMask=p.u64();
			capturedObservedActiveMask|=activeMask;
			const auto cddaGeneration=p.u64();const auto frame=p.u16();
			const bool dsp=p.u8()!=0;p.u8();
			const auto dryLeft=static_cast<std::int32_t>(p.u32());const auto dryRight=static_cast<std::int32_t>(p.u32());
			const auto inputLeft=static_cast<std::int32_t>(p.u32());const auto inputRight=static_cast<std::int32_t>(p.u32());
			const auto cddaLeft=static_cast<std::int32_t>(p.u32());const auto cddaRight=static_cast<std::int32_t>(p.u32());
			const auto dspLeft=static_cast<std::int32_t>(p.u32());const auto dspRight=static_cast<std::int32_t>(p.u32());
			std::array<std::int32_t,16> dspInputs{};for(auto& value:dspInputs)value=static_cast<std::int32_t>(p.u32());
			std::array<std::int16_t,16> dspEffects{};for(auto& value:dspEffects)value=static_cast<std::int16_t>(p.u16());
			const auto left=static_cast<std::int16_t>(p.u16());const auto right=static_cast<std::int16_t>(p.u16());
			require(dsp==expected.dspEnabled,"sample DSP mode differs from identity binding");
			if(samples==0)firstSampleOrdinal=sampleOrdinal;else require(sampleOrdinal==firstSampleOrdinal+samples,"sample ordinal is not contiguous");
			require(sampleOrdinal==currentSampleCut,"sample ordinal differs from the checkpoint/event cut");++currentSampleCut;
			require(frame<588&&cddaGeneration==expectedCddaGeneration&&frame==expectedCddaFrame,"CD-DA generation or frame cadence is invalid");++expectedCddaFrame;const auto sector=cddaSectors.find(cddaGeneration);
			require(sector!=cddaSectors.end(),"sample references an unknown CD-DA generation");
			require(inputLeft==pcm16(sector->second,frame*4)&&inputRight==pcm16(sector->second,frame*4+2),"sample CD-DA inputs differ from captured sector");
			channelReplay.validateSample(sampleOrdinal,activeMask,dryLeft,dryRight,dspInputs);
			const bool mdecStepped=dspReplay.validateSample(sampleOrdinal,dspInputs,dspEffects,dryLeft,dryRight,inputLeft,inputRight,cddaLeft,cddaRight,dspLeft,dspRight,left,right,dsp,expected.vmuSound);
			if(requireDspTailProof&&samples>=tailAnchorAfterSamples&&mdecStepped)++tailMdecSteps;
			if(dryLeft!=0||dryRight!=0||std::any_of(dspInputs.begin(),dspInputs.end(),[](auto value){return value!=0;}))s.lastNonzeroDrySampleOrdinal=sampleOrdinal;
			if(dspLeft!=0||dspRight!=0||std::any_of(dspEffects.begin(),dspEffects.end(),[](auto value){return value!=0;}))s.lastNonzeroDspSampleOrdinal=sampleOrdinal;
			if(left!=0||right!=0)s.lastNonzeroFinalSampleOrdinal=sampleOrdinal;
			if(requireDspTailProof&&samples+1>=tailAnchorAfterSamples)
			{
				if(!tailGeometryFailure&&(dspReplay.ringLength()!=RequiredTailSamples-1
						||dspReplay.memoryDecodeCounter()==0
						||dspReplay.memoryDecodeCounter()>RequiredTailSamples
						||(tailAnchor&&(dspReplay.ringPointer()!=tailAnchor->rbp
								||dspReplay.ringLength()!=tailAnchor->rbl))))
				{
					std::ostringstream detail;detail<<"DSP tail ring geometry changed at sample "
							<<sampleOrdinal<<" rbp="<<dspReplay.ringPointer()
							<<" rbl="<<dspReplay.ringLength()
							<<" mdec="<<dspReplay.memoryDecodeCounter();
					tailGeometryFailure=detail.str();
				}
				const bool terminalZero=activeMask==0&&dryLeft==0&&dryRight==0
						&&inputLeft==0&&inputRight==0&&cddaLeft==0&&cddaRight==0
						&&dspLeft==0&&dspRight==0&&left==0&&right==0
						&&std::all_of(dspInputs.begin(),dspInputs.end(),[](auto value){return value==0;})
						&&std::all_of(dspEffects.begin(),dspEffects.end(),[](auto value){return value==0;});
				if(!terminalZero&&!tailZeroFailure){std::ostringstream detail;detail<<"DSP tail is not zero at sample "<<sampleOrdinal<<" active_mask="<<activeMask<<" dry="<<dryLeft<<","<<dryRight<<" cdda_input="<<inputLeft<<","<<inputRight<<" cdda="<<cddaLeft<<","<<cddaRight<<" dsp="<<dspLeft<<","<<dspRight<<" final="<<left<<","<<right;tailZeroFailure=detail.str();AicaDspTailFailure failure;failure.outputSampleAvailable=true;failure.firstNonzeroSampleOrdinal=sampleOrdinal;failure.activeChannelMask=activeMask;failure.dry={dryLeft,dryRight};failure.cddaInput={inputLeft,inputRight};failure.cdda={cddaLeft,cddaRight};failure.dsp={dspLeft,dspRight};failure.final={left,right};tailOutputFailure=failure;}
				++tailNoInputSamples;
			}
			std::array<std::uint8_t,4> pcm{static_cast<std::uint8_t>(left),static_cast<std::uint8_t>(left>>8),static_cast<std::uint8_t>(right),static_cast<std::uint8_t>(right>>8)};
			pcmHasher.update(pcm.data(),pcm.size());++samples;
			if(requireDspTailProof&&samples==tailAnchorAfterSamples)tailAnchor=dspReplay.snapshot();
			if(samples==s.targetSampleFrames){if(requireDspTailProof)tailTerminal=dspReplay.snapshot();finalSampleSeen=true;}
			if(left!=0||right!=0)++nonzero;break;
		}
		case AicaObservationType::Reset:invalid("reset event is not accepted");
		case AicaObservationType::KeyBatchComplete:{recordOwner(p,eventTick);const auto sampleCut=p.u64();const auto keyOnMask=p.u64();const auto keyOffMask=p.u64();require(keyBatchOpen&&sampleCut==currentSampleCut&&keyOnMask==batchKeyOnMask&&keyOffMask==batchKeyOffMask,"key batch masks or sample cut are invalid");keyBatchOpen=false;keyBatch=true;break;}
		case AicaObservationType::CddaSector:{const auto generation=p.u64();p.u32();p.u32();p.u32();const auto readSuccessful=p.u32();const auto n=p.u32();require(expectedCddaFrame==588&&generation!=0&&readSuccessful<=1&&n==2352&&cddaSectors.find(generation)==cddaSectors.end(),"CD-DA sector generation, cadence, or length is invalid");expectedCddaGeneration=generation;expectedCddaFrame=0;cddaSectors.emplace(generation,p.bytes(n));break;}
		case AicaObservationType::SampleSuppressed:invalid("suppressed sample event is not accepted");
		case AicaObservationType::KeyBatchBegin:{recordOwner(p,eventTick);require(!keyBatchOpen&&p.u64()==currentSampleCut,"key-batch-begin sample cut is invalid");keyBatchOpen=true;batchKeyOnMask=batchKeyOffMask=0;break;}
		default:invalid("event type is invalid");}
		require(p.remaining()==0,"event has trailing payload bytes");
	}
	require(records.remaining()==0&&maximumTick==s.endTick&&!dmaOpen&&!keyBatchOpen&&keyBatch&&finalSampleSeen,"artifact has trailing bytes, open DMA, no stable key batch, or no terminal sample");
	require(counts==s.typeCounts&&samples==s.sampleFrames&&keyOns==s.keyOnCount&&keySources==s.keyedSourceCount&&nonzero==s.nonzeroSampleFrames,"reconstructed counts differ from header");
	require(keyOns>0&&keySources>0&&nonzero>0,"artifact has no inspectable keyed audio");
	if(requireDspTailProof)
	{
		require(tailAnchor.has_value()&&tailTerminal.has_value()
				&&tailNoInputSamples==RequiredTailSamples+1,
				"DSP tail interval is incomplete");
		const auto& terminal=*tailTerminal;s.dspTailAnchorMdec=tailAnchor->mdec;
		s.dspTailTerminalMdec=terminal.mdec;s.dspTailMdecSteps=tailMdecSteps;
		s.dspTailAnchorStateDigest=tailAnchor->stateDigest();
		s.dspTailTerminalStateDigest=terminal.stateDigest();s.dspTailStateHashesAvailable=true;
		const bool statesEqual=tailAnchor->equals(terminal);
		const bool hashesEqual=sha256Equal(s.dspTailAnchorStateDigest,s.dspTailTerminalStateDigest);
		require(!statesEqual||hashesEqual,"equal DSP states produced different canonical hashes");
		const auto stateDifference=statesEqual?std::string{}:tailAnchor->difference(terminal);
		if(tailZeroFailure||tailGeometryFailure||tailMdecSteps!=RequiredTailSamples
				||!statesEqual||!hashesEqual)
		{
			std::string detail="DSP terminal-cycle proof failed";
			if(tailZeroFailure)detail+="; "+*tailZeroFailure;
			else detail+="; DSP tail output is zero";
			if(tailGeometryFailure)detail+="; "+*tailGeometryFailure;
			if(tailMdecSteps!=RequiredTailSamples)detail+="; DSP MDEC steps="
					+std::to_string(tailMdecSteps)+" required="+std::to_string(RequiredTailSamples);
			else detail+="; DSP MDEC steps="+std::to_string(tailMdecSteps);
			detail+="; anchor_mdec="+std::to_string(s.dspTailAnchorMdec)
					+" terminal_mdec="+std::to_string(s.dspTailTerminalMdec);
			if(!stateDifference.empty())detail+="; DSP persistent state differs across one full ring: "+stateDifference;
			detail+="; anchor_state_sha256="+sha256ToHex(s.dspTailAnchorStateDigest)
					+" terminal_state_sha256="+sha256ToHex(s.dspTailTerminalStateDigest);
			AicaDspTailFailure failure=tailOutputFailure.value_or(AicaDspTailFailure{});
			failure.anchorSampleOrdinal=firstSampleOrdinal+tailAnchorAfterSamples-1;
			failure.terminalSampleOrdinal=firstSampleOrdinal+s.targetSampleFrames-1;
			failure.mdecSteps=tailMdecSteps;failure.anchorMdec=s.dspTailAnchorMdec;
			failure.terminalMdec=s.dspTailTerminalMdec;
			failure.anchorTemp0=tailAnchor->temp[0];failure.terminalTemp0=terminal.temp[0];
			failure.anchorStateDigest=s.dspTailAnchorStateDigest;
			failure.terminalStateDigest=s.dspTailTerminalStateDigest;
			failure.outputZero=!tailZeroFailure;failure.geometryStable=!tailGeometryFailure;
			failure.statesEqual=statesEqual;failure.stateHashesEqual=hashesEqual;
			failure.stateDifference=stateDifference;
			throw AicaDspTailValidationError("invalid AICA artifact: "+detail,s,failure);
		}
		s.dspTailProofVerified=true;s.dspTailSampleFrames=RequiredTailSamples;
	}
	require(sha256Equal(pcmHasher.finalize(),s.pcmDigest)&&sha256Equal(sourceHasher.finalize(),s.keyedSourceDigest),"PCM or keyed-source digest mismatch");
	const std::uint64_t checkpointPresent=AicaCoverageHeaderBindingCounts
			|AicaCoverageCheckpointBoundary|AicaCoverageCheckpointRegisters
			|AicaCoverageCheckpointRam|AicaCoverageCheckpointChannels
			|AicaCoverageCheckpointDspPersistent|AicaCoverageCheckpointMixs
			|AicaCoverageCheckpointCddaAudio|AicaCoverageCddaMetadata
			|AicaCoverageChannelLoopStatus|AicaCoverageEventOrdering;
	s.fieldCoveragePresentMask|=checkpointPresent;
	s.fieldCoverageConsumedMask|=checkpointPresent
			&~(AicaCoverageCheckpointMixs|AicaCoverageCddaMetadata
					|AicaCoverageChannelLoopStatus);
	s.fieldCoverageProvenIrrelevantMask|=AicaCoverageCheckpointMixs
			|AicaCoverageCddaMetadata|AicaCoverageChannelLoopStatus;
	auto coverPresent=[&](bool present,std::uint64_t group){if(present){s.fieldCoveragePresentMask|=group;s.fieldCoverageConsumedMask|=group;}};
	coverPresent(counts[0]!=0,AicaCoverageRegisterMutations);
	coverPresent(counts[1]!=0,AicaCoverageRamMutations);
	coverPresent(counts[2]!=0||counts[3]!=0||counts[4]!=0,AicaCoverageDmaMutations);
	coverPresent(counts[5]!=0||counts[6]!=0,AicaCoverageKeyTransitions);
	coverPresent(counts[9]!=0||counts[12]!=0,AicaCoverageKeyBatchMasks);
	coverPresent(counts[10]!=0,AicaCoverageCddaSectors);
	coverPresent(counts[7]!=0,AicaCoverageChannelReplay|AicaCoverageDspReplay
			|AicaCoverageOutputComposition);
	if(s.ownerWriterMask!=0){s.fieldCoveragePresentMask|=AicaCoverageEventOwners;s.fieldCoverageConsumedMask|=AicaCoverageEventOwners;}
	if(requireDspTailProof){s.fieldCoveragePresentMask|=AicaCoverageTerminalCycle;s.fieldCoverageConsumedMask|=AicaCoverageTerminalCycle;}
	s.channelOwnerPresentMask=std::numeric_limits<std::uint64_t>::max();
	s.channelOwnerCheckpointActiveMask=checkpointActiveMask;
	s.channelOwnerKeyTargetMask=capturedKeyTransitionMask;
	s.channelOwnerObservedActiveMask=capturedObservedActiveMask;
	s.channelOwnerConsumedMask=checkpointActiveMask
			|capturedKeyTransitionMask|capturedObservedActiveMask;
	s.channelOwnerInactiveUnkeyedMask=s.channelOwnerPresentMask
			&~(checkpointActiveMask|capturedKeyTransitionMask|capturedObservedActiveMask);
	s.fieldCoverageComplete=(s.fieldCoveragePresentMask
			&~(s.fieldCoverageConsumedMask|s.fieldCoverageProvenIrrelevantMask))==0;
	require(s.fieldCoverageComplete,"authoritative field coverage is incomplete");
	return s;
	}
	catch(const AicaArtifactValidationError&){throw;}
	catch(const std::exception& error){throw AicaArtifactValidationError(error.what(),s);}
}

AicaArtifactSummary validateAicaArtifactWithReplay(
		const std::filesystem::path& artifact,
		const IdentityManifest& identity,
		const std::filesystem::path& replay,
		std::uint64_t maximumArtifactBytes,
		std::uint64_t maximumEvents,
		std::uint64_t maximumReplayBytes,
		bool requireDspTailProof)
{
	try
	{
		requireSh4EquivalenceIdentityV2(identity);
		const AicaConfiguration& configuration = identity.runtimeConfiguration.aicaConfiguration;
		if (!configuration.available)
			throw std::runtime_error("identity has no AICA configuration");
		if (identity.runtimeConfiguration.mapleDmaCheckpoint == 0)
			throw std::runtime_error("AICA validation requires an identity DMA checkpoint");
	}
	catch(const std::exception& error)
	{
		throw AicaInputValidationError(error.what(),AicaInputFailureStage::Identity,
				identity.digest);
	}

	MapleTraceSummary replaySummary;
	try
	{
		replaySummary=validateProductionMapleTraceFile(replay,
				identity.mapleReplayIdentityDigest,maximumReplayBytes);
	}
	catch(const std::exception& error)
	{
		throw AicaInputValidationError(error.what(),AicaInputFailureStage::Replay,
				identity.digest);
	}
	auto replayFailure=[&](const std::string& message){
		return AicaInputValidationError(message,AicaInputFailureStage::Replay,
				identity.digest,true,replaySummary.fileDigest,replaySummary.fileBytes,
				replaySummary.schemaVersion);};
	if(replaySummary.schemaVersion!=MapleTraceSchemaVersionV2)
		throw replayFailure("AICA validation requires a schema-v2 Maple replay");
	if (replaySummary.dmaCount != identity.runtimeConfiguration.mapleDmaCheckpoint)
		throw replayFailure("Maple replay does not end at the identity DMA checkpoint");
	const AicaConfiguration& configuration = identity.runtimeConfiguration.aicaConfiguration;

	AicaArtifactBinding binding;
	binding.backend = identity.runtimeConfiguration.cpuBackend == "dynarec"
			? Sh4ObservationBackend::Dynarec : Sh4ObservationBackend::Interpreter;
	binding.identityDigest = identity.digest;
	binding.replayDigest = replaySummary.fileDigest;
	binding.configurationDigest = aicaConfigurationDigest(configuration);
	binding.dspEnabled = configuration.dspEnabled;
	binding.vmuSound = configuration.vmuSound;
	AicaArtifactSummary summary;
	try
	{
		summary=validateAicaArtifactFile(artifact,binding,maximumArtifactBytes,
				maximumEvents,requireDspTailProof);
	}
	catch(AicaArtifactValidationError& error)
	{
		error.summary.replayBytes=replaySummary.fileBytes;
		error.summary.replaySchemaVersion=replaySummary.schemaVersion;
		throw;
	}
	summary.replayBytes=replaySummary.fileBytes;
	summary.replaySchemaVersion=replaySummary.schemaVersion;
	if (summary.startTick < replaySummary.startTick
			|| summary.endTick > replaySummary.endTick)
		throw AicaArtifactValidationError(
				"AICA artifact lies outside the authenticated Maple replay boundary",summary);
	return summary;
}

} // namespace research

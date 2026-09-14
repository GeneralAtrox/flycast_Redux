#include "research/aica_artifact.h"
#include "research/maple_trace.h"

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <limits>
#include <optional>

namespace
{
static_assert(research::AicaSamplesPerLodossLogicTick==1764);
static_assert(research::MaximumAicaSampleFrames==5'500'000);
static_assert(research::DefaultMaximumAicaArtifactEvents==20'000'000);
static_assert(research::DefaultMaximumAicaArtifactBytes==3ull*1024*1024*1024);
std::uint32_t crc32(const std::uint8_t* data,std::size_t size)
{
	std::uint32_t crc=0xffffffffu;for(std::size_t i=0;i<size;++i){crc^=data[i];
		for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}
	return ~crc;
}
void patchHeaderSampleCounts(const std::filesystem::path& path,std::uint64_t samples)
{
	std::array<std::uint8_t,research::AicaArtifactHeaderSize> header{};
	std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);
	file.read(reinterpret_cast<char*>(header.data()),header.size());
	ASSERT_EQ(static_cast<std::streamsize>(header.size()),file.gcount());
	auto put64=[&](std::size_t offset,std::uint64_t value){for(unsigned i=0;i<8;++i)header[offset+i]=static_cast<std::uint8_t>(value>>(i*8));};
	auto put32=[&](std::size_t offset,std::uint32_t value){for(unsigned i=0;i<4;++i)header[offset+i]=static_cast<std::uint8_t>(value>>(i*8));};
	put64(72,samples);put64(80,samples);put32(508,crc32(header.data(),508));
	file.seekp(0);file.write(reinterpret_cast<const char*>(header.data()),header.size());
	ASSERT_TRUE(static_cast<bool>(file));
}
class TemporaryDirectory
{
public:
	TemporaryDirectory(){static std::atomic<unsigned> n{0};path=std::filesystem::temp_directory_path()/("flycast-aica-test-"+std::to_string(n++));std::filesystem::create_directories(path);}
	~TemporaryDirectory(){std::error_code error;std::filesystem::remove_all(path,error);}
	std::filesystem::path file(const char* name)const{return path/name;}
private:std::filesystem::path path;
};

struct Fixture
{
	TemporaryDirectory directory;
	std::filesystem::path artifact=directory.file("capture.fcaica");
	research::AicaArtifactBinding binding;
	research::AicaCheckpoint checkpoint;
	std::array<std::int32_t,16> sampleDspInputs{};
	std::array<std::int16_t,16> sampleDspEffects{};
	std::uint64_t sampleActiveMask=0;
	std::int32_t sampleInputLeft=1,sampleInputRight=-1;
	std::int32_t sampleDryLeft=0,sampleDryRight=0,sampleCddaLeft=1,sampleCddaRight=-1,sampleDspLeft=0,sampleDspRight=0;
	std::int16_t sampleFinalLeft=1,sampleFinalRight=-1;
	bool repeatSample=false,corruptKeySource=false;
	std::optional<std::uint64_t> ringGeometryWriteAtSample;
	std::uint16_t ringGeometryWriteValue=0;
	std::optional<std::uint64_t> stopDspAtSample,restartDspAtSample;
	Fixture(){
		binding.identityDigest=research::sha256("identity",8);binding.replayDigest=research::sha256("replay",6);binding.configurationDigest=research::sha256("aica-config",11);binding.dspEnabled=true;
		checkpoint.tick=9;checkpoint.nextSampleOrdinal=50;checkpoint.phase=research::AicaCheckpointPhase::PreKeyBatch;checkpoint.dspRingBufferLength=8191;checkpoint.dspMemoryDecodeCounter=1;checkpoint.ram.resize(2*1024*1024);for(std::size_t i=0;i<checkpoint.ram.size();++i)checkpoint.ram[i]=static_cast<std::uint8_t>(i*13);
		checkpoint.registers[4]=0x10;checkpoint.registers[12]=4;checkpoint.registers[0x2800]=0x0f;checkpoint.cddaIndex=0;
		checkpoint.cddaSector[0]=1;checkpoint.cddaSector[2]=0xff;checkpoint.cddaSector[3]=0xff;put16(0x2040,0x0f1f);put16(0x2044,0x0f0f);
	}
	void put16(std::size_t address,std::uint16_t value){checkpoint.registers[address]=value;checkpoint.registers[address+1]=value>>8;}
	void configureFullDspRing(){checkpoint.dspRingBufferLength=65535;checkpoint.dspMemoryDecodeCounter=65536;put16(0x2804,0x6000);}
	void configureDspImpulse(){
		put16(0x3000,0x7fff);put16(0x3404,0xb800);put16(0x3408,0x0002);
		put16(0x3418,0x1002);put16(0x2000,0x0f00);put16(0x2040,0);put16(0x2044,0);sampleCddaLeft=sampleCddaRight=0;
		sampleInputLeft=4096;sampleInputRight=0;checkpoint.cddaSector[0]=0;checkpoint.cddaSector[1]=0x10;checkpoint.cddaSector[2]=checkpoint.cddaSector[3]=0;
		sampleDspEffects[0]=4095;sampleDryLeft=sampleDryRight=0;sampleDspLeft=sampleDspRight=4095;
		sampleFinalLeft=sampleFinalRight=4095;
	}
	void configureDspIdleResidue(){
		put16(0x2000,0x0f00);put16(0x2040,0);put16(0x2044,0);put16(0x4580,0xffff);
		sampleInputLeft=sampleInputRight=0;checkpoint.cddaSector[0]=checkpoint.cddaSector[1]=0;
		checkpoint.cddaSector[2]=checkpoint.cddaSector[3]=0;sampleCddaLeft=sampleCddaRight=0;
		sampleDspEffects[0]=-1;sampleDspLeft=sampleDspRight=-1;
		sampleFinalLeft=sampleFinalRight=-1;repeatSample=true;
	}
	void configureDspPersistentStateDrift(){put16(0x3418,0x4002);}
	void configureDspDisabledDryFallback(){
		binding.dspEnabled=false;checkpoint.activeChannelMask=1;auto& channel=checkpoint.channels[0];
		channel.enabled=1;channel.sample0=channel.sample1=32767;channel.aegState=2;
		put16(0x20,0x00f0);put16(0x28,0x0060);put16(0x2040,0);put16(0x2044,0);
		checkpoint.cddaSector[0]=checkpoint.cddaSector[1]=checkpoint.cddaSector[2]=checkpoint.cddaSector[3]=0;
		sampleActiveMask=1;sampleInputLeft=sampleInputRight=0;sampleCddaLeft=sampleCddaRight=0;
		sampleDryLeft=sampleDryRight=32767;sampleDspInputs[0]=524272;
		sampleFinalLeft=sampleFinalRight=32767;
	}
	research::AicaObservation key()const{
		research::AicaObservation e;e.type=research::AicaObservationType::KeyOn;e.emissionOrdinal=1;e.tick=10;e.owner.writer=research::AicaWriter::Arm7;e.channel=0;e.sampleCutOrdinal=50;
		std::copy(checkpoint.registers.begin(),checkpoint.registers.begin()+0x80,e.channelRegisters.begin());
		const auto address=std::uint32_t(e.channelRegisters[4])|(std::uint32_t(e.channelRegisters[5])<<8);
		const auto samples=std::max<std::uint32_t>(e.channelRegisters[8]|(std::uint32_t(e.channelRegisters[9])<<8),e.channelRegisters[12]|(std::uint32_t(e.channelRegisters[13])<<8));
		const auto length=std::uint64_t(samples)*2;e.bytes.resize(static_cast<std::size_t>(length));for(std::size_t i=0;i<e.bytes.size();++i)e.bytes[i]=checkpoint.ram[(address+i)&(checkpoint.ram.size()-1)];return e;
	}
	void capture(std::uint64_t targetSamples=1){
		research::AicaArtifactWriter writer(artifact,binding,targetSamples);writer.writeCheckpoint(checkpoint);
		auto keyed=key();keyed.emissionOrdinal=1;if(corruptKeySource&&!keyed.bytes.empty())keyed.bytes[0]^=0xff;writer.write(keyed);
		research::AicaObservation batch;batch.type=research::AicaObservationType::KeyBatchComplete;batch.owner.writer=research::AicaWriter::Arm7;batch.emissionOrdinal=2;batch.tick=11;batch.keyOnMask=1;batch.sampleCutOrdinal=50;writer.write(batch);
		std::uint64_t emission=3,cddaGeneration=checkpoint.cddaGeneration;
		auto writeRegister=[&](std::uint32_t address,std::uint16_t value){research::AicaObservation write;write.type=research::AicaObservationType::RegisterWrite;write.emissionOrdinal=emission++;write.tick=12;write.owner.writer=research::AicaWriter::Arm7;write.address=address;write.width=2;write.value=value;writer.write(write);};
		for(std::uint64_t i=0;i<targetSamples;++i)
		{
			if(i!=0&&i%588==0){research::AicaObservation sector;sector.type=research::AicaObservationType::CddaSector;sector.emissionOrdinal=emission++;sector.tick=12;sector.cddaGeneration=++cddaGeneration;sector.cddaReadSuccessful=true;sector.bytes.resize(2352);writer.write(sector);}
			if(ringGeometryWriteAtSample&&i==*ringGeometryWriteAtSample)writeRegister(0x2804,ringGeometryWriteValue);
			if(stopDspAtSample&&i==*stopDspAtSample)writeRegister(0x3418,0);
			if(restartDspAtSample&&i==*restartDspAtSample)writeRegister(0x3418,0x4002);
			research::AicaObservation sample;sample.type=research::AicaObservationType::SampleFrame;sample.emissionOrdinal=emission++;sample.tick=12;sample.sampleOrdinal=50+i;sample.dspEnabled=binding.dspEnabled;sample.cddaGeneration=cddaGeneration;sample.cddaFrameIndex=static_cast<std::uint16_t>(i%588);if(i==0||repeatSample){sample.activeChannelMask=sampleActiveMask;sample.dryLeft=sampleDryLeft;sample.dryRight=sampleDryRight;sample.cddaInputLeft=sampleInputLeft;sample.cddaInputRight=sampleInputRight;sample.cddaContributionLeft=sampleCddaLeft;sample.cddaContributionRight=sampleCddaRight;sample.dspInputs=sampleDspInputs;sample.dspEffectOutputs=sampleDspEffects;sample.dspContributionLeft=sampleDspLeft;sample.dspContributionRight=sampleDspRight;sample.finalLeft=sampleFinalLeft;sample.finalRight=sampleFinalRight;}writer.write(sample);
		}
		writer.finalize();
	}
};

research::IdentityManifest bindReplay(Fixture& fixture,
		const std::filesystem::path& replay,
		std::uint32_t replaySchema=research::MapleTraceSchemaVersionV2)
{
	research::IdentityManifest identity;
	identity.schemaVersion = 2;
	identity.digest = research::sha256("replay-identity-v2", 18);
	identity.hasMapleReplayIdentityDigest = true;
	identity.mapleReplayIdentityDigest = research::sha256("maple-record-v3", 15);
	identity.runtimeConfiguration.cpuBackend = "interpreter";
	identity.runtimeConfiguration.dynarecObservation = false;
	identity.runtimeConfiguration.mapleDmaCheckpoint = 1;
	identity.runtimeConfiguration.aicaConfiguration.available = true;
	identity.runtimeConfiguration.aicaConfiguration.dspEnabled = false;
	identity.runtimeConfiguration.aicaConfiguration.vmuSound = false;
	identity.runtimeConfiguration.aicaConfiguration.sampleRate = 44100;
	identity.runtimeConfiguration.aicaConfiguration.sampleFormat =
			"signed-pcm16-le-stereo";
	identity.runtimeConfiguration.aicaConfiguration.outputStage =
			"pre-backend-pre-user-volume";

	research::MapleTraceWriter replayWriter(replay,
			identity.mapleReplayIdentityDigest,
			research::DefaultMaximumMapleTraceBytes,replaySchema);
	research::MapleDmaBeginEvent begin;
	begin.tick = 8;
	begin.descriptorAddress = 0x0c001000;
	begin.mden = 1;
	begin.mdst = 1;
	begin.mmsel = 1;
	begin.trigger = research::MapleDmaTrigger::Software;
	const std::uint64_t dmaOrdinal = replayWriter.beginDma(begin);
	research::MapleTransactionEvent transaction;
	transaction.dmaOrdinal = dmaOrdinal;
	transaction.tick = 8;
	transaction.descriptorAddress = 0x0c001000;
	transaction.destinationAddress = 0x0c002000;
	transaction.descriptorHeader1 = replaySchema==research::MapleTraceSchemaVersionV2
			?0x00000001:0x80000001;
	transaction.descriptorHeader2 = 0x0c002000;
	transaction.deviceType = 0;
	transaction.bus = 0;
	transaction.port = 5;
	transaction.command = 0x09;
	transaction.flags = research::MapleTransactionDevicePresent;
	transaction.request = {0x09, 0x20, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01};
	transaction.response = {
			0x08, 0x20, 0x00, 0x02, 0x00, 0x00, 0x00, 0x01,
			0x00, 0x00, 0xff, 0xff,
	};
	replayWriter.writeTransaction(transaction);
	if(replaySchema==research::MapleTraceSchemaVersionV2)
	{
		research::MapleControlDescriptorEvent control;
		control.dmaOrdinal=dmaOrdinal;control.tick=transaction.tick;
		control.descriptorAddress=transaction.descriptorAddress+8
				+static_cast<std::uint32_t>(transaction.request.size());
		control.descriptorHeader=0x80000700;
		control.operation=research::MapleControlOperation::Nop;control.last=true;
		replayWriter.writeControlDescriptor(control);
	}
	research::MapleDmaScheduleEvent schedule;
	schedule.dmaOrdinal = dmaOrdinal;
	schedule.tick = 8;
	schedule.inputWireBytes = 11;
	schedule.outputWireBytes = 15;
	schedule.scheduledCycles = 1000;
	schedule.responseCount = 1;
	replayWriter.scheduleDma(schedule);
	research::MapleDmaCommitEvent commit;
	commit.dmaOrdinal = dmaOrdinal;
	commit.tick = 1008;
	commit.callbackCycles = 1000;
	commit.responseCount = 1;
	commit.flags = research::MapleCommitInterruptRaised;
	replayWriter.commitDma(commit);
	replayWriter.finalize();

	fixture.binding.identityDigest = identity.digest;
	fixture.binding.replayDigest = research::hashFileExact(replay,
			research::DefaultMaximumMapleTraceBytes);
	fixture.binding.configurationDigest = research::aicaConfigurationDigest(
			identity.runtimeConfiguration.aicaConfiguration);
	fixture.binding.dspEnabled = false;
	return identity;
}
}

TEST(ResearchAicaArtifact, AcceptsIdentityBoundLodossDspDisabledMode)
{
	Fixture fixture;fixture.binding.dspEnabled=false;fixture.capture();
	const auto summary=research::validateAicaArtifactFile(fixture.artifact,fixture.binding);
	EXPECT_FALSE(summary.binding.dspEnabled);
}

TEST(ResearchAicaArtifact, IndependentValidatorAcceptsBoundedTypedCapture)
{
	Fixture fixture;fixture.capture();const auto summary=research::validateAicaArtifactFile(fixture.artifact,fixture.binding);
	EXPECT_EQ(3u,summary.eventCount);EXPECT_EQ(1u,summary.sampleFrames);EXPECT_EQ(1u,summary.keyedSourceCount);EXPECT_EQ(2u*1024u*1024u,summary.checkpointRamBytes);EXPECT_TRUE(summary.fieldCoverageComplete);
	EXPECT_EQ(1u<<static_cast<unsigned>(research::AicaWriter::Arm7),summary.ownerWriterMask);
	EXPECT_EQ(std::numeric_limits<std::uint64_t>::max(),summary.channelOwnerPresentMask);
	EXPECT_EQ(0u,summary.channelOwnerCheckpointActiveMask);
	EXPECT_EQ(1u,summary.channelOwnerKeyTargetMask);
	EXPECT_EQ(0u,summary.channelOwnerObservedActiveMask);
	EXPECT_EQ(1u,summary.channelOwnerConsumedMask);
	EXPECT_EQ(~std::uint64_t{1},summary.channelOwnerInactiveUnkeyedMask);
	EXPECT_EQ(0u,summary.fieldCoveragePresentMask
			&~(summary.fieldCoverageConsumedMask|summary.fieldCoverageProvenIrrelevantMask));
	EXPECT_NE(0u,summary.fieldCoverageProvenIrrelevantMask
			&research::AicaCoverageCheckpointMixs);
}

TEST(ResearchAicaArtifact, ReplaysDspDisabledDryFallbackExactly)
{
	Fixture fixture;fixture.configureDspDisabledDryFallback();fixture.capture();
	const auto summary=research::validateAicaArtifactFile(fixture.artifact,fixture.binding);
	EXPECT_FALSE(summary.binding.dspEnabled);EXPECT_TRUE(summary.fieldCoverageComplete);
}

TEST(ResearchAicaArtifact, BindingMismatchFailureRetainsAuthenticatedExpectation)
{
	Fixture fixture;fixture.capture();auto expected=fixture.binding;
	expected.backend=research::Sh4ObservationBackend::Dynarec;
	expected.identityDigest=research::sha256("expected-identity",17);
	expected.replayDigest=research::sha256("expected-replay",15);
	expected.configurationDigest=research::sha256("expected-aica",13);
	expected.dspEnabled=false;expected.vmuSound=true;
	try
	{
		research::validateAicaArtifactFile(fixture.artifact,expected);
		FAIL()<<"artifact with a different claimed binding was accepted";
	}
	catch(const research::AicaArtifactValidationError& error)
	{
		EXPECT_NE(std::string::npos,std::string(error.what()).find("binding mismatch"));
		EXPECT_EQ(expected.backend,error.summary.binding.backend);
		EXPECT_EQ(expected.dspEnabled,error.summary.binding.dspEnabled);
		EXPECT_EQ(expected.vmuSound,error.summary.binding.vmuSound);
		EXPECT_TRUE(research::sha256Equal(expected.identityDigest,
				error.summary.binding.identityDigest));
		EXPECT_TRUE(research::sha256Equal(expected.replayDigest,
				error.summary.binding.replayDigest));
		EXPECT_TRUE(research::sha256Equal(expected.configurationDigest,
				error.summary.binding.configurationDigest));
		EXPECT_TRUE(error.summary.artifactDigestAvailable);
	}
}

TEST(ResearchAicaArtifact, IndependentlyExecutesDspProgramAndRejectsFalseEffectOutput)
{
	Fixture valid;valid.configureDspImpulse();valid.capture();
	EXPECT_NO_THROW(research::validateAicaArtifactFile(valid.artifact,valid.binding));
	Fixture invalid;invalid.configureDspImpulse();invalid.sampleDspEffects[0]=4094;
	invalid.sampleDspLeft=invalid.sampleDspRight=4094;invalid.sampleFinalLeft=invalid.sampleFinalRight=4094;
	invalid.capture();EXPECT_THROW(research::validateAicaArtifactFile(invalid.artifact,invalid.binding),std::runtime_error);
}

TEST(ResearchAicaArtifact, RejectsKeySourceSnapshotThatDiffersFromLiveRam)
{
	Fixture fixture;fixture.corruptKeySource=true;fixture.capture();
	EXPECT_THROW(research::validateAicaArtifactFile(fixture.artifact,fixture.binding),std::runtime_error);
}

TEST(ResearchAicaArtifact, RejectsCapturedActiveOwnerMaskDivergence)
{
	Fixture fixture;fixture.sampleActiveMask=1;fixture.capture();
	try
	{
		research::validateAicaArtifactFile(fixture.artifact,fixture.binding);
		FAIL()<<"captured active owner without replay activity was accepted";
	}
	catch(const std::runtime_error& error)
	{
		EXPECT_NE(std::string::npos,std::string(error.what()).find(
				"active channel mask differs at sample 50"));
	}
}

TEST(ResearchAicaArtifact, RejectsCheckpointRingGeometryInconsistentWithRegisters)
{
	Fixture fixture;fixture.checkpoint.dspRingBufferLength=65535;
	fixture.checkpoint.dspMemoryDecodeCounter=65536;fixture.capture();
	try
	{
		research::validateAicaArtifactFile(fixture.artifact,fixture.binding);
		FAIL()<<"inconsistent checkpoint ring geometry was accepted";
	}
	catch(const std::runtime_error& error)
	{
		EXPECT_NE(std::string::npos,std::string(error.what()).find(
				"differs from common register 0x2804"));
	}
}

TEST(ResearchAicaArtifact, ProvesPersistentIdleCycleAcrossFullDspRing)
{
	Fixture fixture;fixture.configureDspImpulse();fixture.configureFullDspRing();fixture.capture(65538);
	const auto summary=research::validateAicaArtifactFile(fixture.artifact,fixture.binding,
			research::DefaultMaximumAicaArtifactBytes,research::DefaultMaximumAicaArtifactEvents,true);
	EXPECT_TRUE(summary.dspTailProofVerified);EXPECT_EQ(65536u,summary.dspTailSampleFrames);
	EXPECT_EQ(65536u,summary.dspTailMdecSteps);
	EXPECT_EQ(summary.dspTailAnchorMdec,summary.dspTailTerminalMdec);
	EXPECT_TRUE(summary.dspTailStateHashesAvailable);
	EXPECT_TRUE(research::sha256Equal(summary.dspTailAnchorStateDigest,
			summary.dspTailTerminalStateDigest));
	EXPECT_NE(research::Sha256Digest{},summary.dspTailAnchorStateDigest);
	EXPECT_TRUE(summary.fieldCoverageComplete);
}

TEST(ResearchAicaArtifact, RejectsNonzeroDspIdleResidueEvenWhenPersistentStateRepeats)
{
	Fixture fixture;fixture.configureDspIdleResidue();fixture.configureFullDspRing();fixture.capture(65538);
	try
	{
		research::validateAicaArtifactFile(fixture.artifact,fixture.binding,
				research::DefaultMaximumAicaArtifactBytes,
				research::DefaultMaximumAicaArtifactEvents,true);
		FAIL()<<"nonzero idle residue was accepted";
	}
	catch(const research::AicaDspTailValidationError& error)
	{
		const std::string message=error.what();
		EXPECT_NE(std::string::npos,message.find("anchor_mdec="));
		EXPECT_NE(std::string::npos,message.find("terminal_mdec="));
		EXPECT_NE(std::string::npos,message.find("anchor_state_sha256="));
		EXPECT_NE(std::string::npos,message.find("terminal_state_sha256="));
		EXPECT_EQ(std::filesystem::file_size(fixture.artifact),error.summary.artifactBytes);
		EXPECT_TRUE(research::sha256Equal(
				research::hashFileExact(fixture.artifact,
						research::DefaultMaximumAicaArtifactBytes),
				error.summary.artifactDigest));
		EXPECT_EQ(51u,error.failure.anchorSampleOrdinal);
		EXPECT_EQ(65587u,error.failure.terminalSampleOrdinal);
		EXPECT_EQ(51u,error.failure.firstNonzeroSampleOrdinal);
		EXPECT_EQ((std::array<std::int32_t,2>{-1,-1}),error.failure.dsp);
		EXPECT_EQ((std::array<std::int16_t,2>{-1,-1}),error.failure.final);
		EXPECT_EQ(0u,error.failure.mdecSteps);
		EXPECT_FALSE(error.failure.outputZero);
		EXPECT_TRUE(error.failure.outputSampleAvailable);
		EXPECT_TRUE(error.failure.geometryStable);
		EXPECT_TRUE(error.failure.statesEqual);
		EXPECT_TRUE(error.failure.stateHashesEqual);
	}
}

TEST(ResearchAicaArtifact, RejectsStoppedProgramWithoutAFullMdecCycle)
{
	Fixture fixture;fixture.configureFullDspRing();fixture.capture(65538);
	try
	{
		research::validateAicaArtifactFile(fixture.artifact,fixture.binding,
				research::DefaultMaximumAicaArtifactBytes,
				research::DefaultMaximumAicaArtifactEvents,true);
		FAIL()<<"stopped DSP program was accepted as a full MDEC cycle";
	}
	catch(const research::AicaDspTailValidationError& error)
	{
		EXPECT_NE(std::string::npos,std::string(error.what()).find(
				"DSP MDEC steps=0 required=65536"));
		EXPECT_TRUE(error.failure.outputZero);
		EXPECT_FALSE(error.failure.outputSampleAvailable);
	}
}

TEST(ResearchAicaArtifact, RejectsZeroOutputPersistentStateDrift)
{
	Fixture fixture;fixture.configureDspPersistentStateDrift();
	fixture.configureFullDspRing();fixture.capture(65538);
	try
	{
		research::validateAicaArtifactFile(fixture.artifact,fixture.binding,
				research::DefaultMaximumAicaArtifactBytes,
				research::DefaultMaximumAicaArtifactEvents,true);
		FAIL()<<"zero-output persistent DSP drift was accepted";
	}
	catch(const research::AicaDspTailValidationError& error)
	{
		const std::string message=error.what();
		EXPECT_NE(std::string::npos,message.find("DSP persistent state differs"));
		EXPECT_NE(std::string::npos,message.find("ring byte["));
		EXPECT_NE(std::string::npos,message.find("anchor_state_sha256="));
		EXPECT_NE(std::string::npos,message.find("terminal_state_sha256="));
		EXPECT_TRUE(error.failure.outputZero);
		EXPECT_FALSE(error.failure.outputSampleAvailable);
	}
}

TEST(ResearchAicaArtifact, RejectsOneMissingMdecStep)
{
	Fixture fixture;fixture.configureDspPersistentStateDrift();fixture.configureFullDspRing();
	fixture.stopDspAtSample=2;fixture.restartDspAtSample=3;fixture.capture(65538);
	try
	{
		research::validateAicaArtifactFile(fixture.artifact,fixture.binding,
				research::DefaultMaximumAicaArtifactBytes,
				research::DefaultMaximumAicaArtifactEvents,true);
		FAIL()<<"65,535-step DSP interval was accepted";
	}
	catch(const std::runtime_error& error)
	{
		EXPECT_NE(std::string::npos,std::string(error.what()).find(
				"DSP MDEC steps=65535 required=65536"));
	}
}

TEST(ResearchAicaArtifact, RejectsMissingMdecStepAtTerminalSample)
{
	Fixture fixture;fixture.configureDspPersistentStateDrift();fixture.configureFullDspRing();
	fixture.stopDspAtSample=65537;fixture.capture(65538);
	try
	{
		research::validateAicaArtifactFile(fixture.artifact,fixture.binding,
				research::DefaultMaximumAicaArtifactBytes,
				research::DefaultMaximumAicaArtifactEvents,true);
		FAIL()<<"terminal-sample MDEC omission was accepted";
	}
	catch(const std::runtime_error& error)
	{
		EXPECT_NE(std::string::npos,std::string(error.what()).find(
				"DSP MDEC steps=65535 required=65536"));
	}
}

TEST(ResearchAicaArtifact, RejectsTailRingGeometryChangeWithoutUnsafeDifference)
{
	Fixture fixture;fixture.configureDspPersistentStateDrift();
	fixture.configureFullDspRing();
	fixture.ringGeometryWriteAtSample=2;fixture.capture(65538);
	try
	{
		research::validateAicaArtifactFile(fixture.artifact,fixture.binding,
				research::DefaultMaximumAicaArtifactBytes,
				research::DefaultMaximumAicaArtifactEvents,true);
		FAIL()<<"tail ring geometry change was accepted";
	}
	catch(const std::runtime_error& error)
	{
		const std::string message=error.what();
		EXPECT_NE(std::string::npos,message.find("DSP tail ring geometry changed"));
		EXPECT_NE(std::string::npos,message.find("DSP register["));
	}
}

TEST(ResearchAicaArtifact, RejectsRbpOnlyChangeInsideTail)
{
	Fixture fixture;fixture.configureDspPersistentStateDrift();fixture.configureFullDspRing();
	fixture.ringGeometryWriteAtSample=2;fixture.ringGeometryWriteValue=0x6001;
	fixture.capture(65538);
	try
	{
		research::validateAicaArtifactFile(fixture.artifact,fixture.binding,
				research::DefaultMaximumAicaArtifactBytes,
				research::DefaultMaximumAicaArtifactEvents,true);
		FAIL()<<"RBP-only tail geometry change was accepted";
	}
	catch(const std::runtime_error& error)
	{
		const std::string message=error.what();
		EXPECT_NE(std::string::npos,message.find("DSP tail ring geometry changed"));
		EXPECT_NE(std::string::npos,message.find("rbp=2048 rbl=65535"));
	}
}

TEST(ResearchAicaArtifact, RejectsRecordFollowingFinalSampleBeforeMutation)
{
	Fixture fixture;fixture.ringGeometryWriteAtSample=1;fixture.ringGeometryWriteValue=1;
	fixture.capture(2);patchHeaderSampleCounts(fixture.artifact,1);
	try
	{
		research::validateAicaArtifactFile(fixture.artifact,fixture.binding);
		FAIL()<<"record following the declared final sample was accepted";
	}
	catch(const std::runtime_error& error)
	{
		EXPECT_NE(std::string::npos,std::string(error.what()).find(
				"record follows the final AICA sample"));
	}
}

TEST(ResearchAicaArtifact, StrictModeRejectsMutationFollowingFinalSample)
{
	Fixture fixture;fixture.configureDspImpulse();fixture.configureFullDspRing();
	fixture.ringGeometryWriteAtSample=65538;fixture.ringGeometryWriteValue=0x6001;
	fixture.capture(65539);patchHeaderSampleCounts(fixture.artifact,65538);
	try
	{
		research::validateAicaArtifactFile(fixture.artifact,fixture.binding,
				research::DefaultMaximumAicaArtifactBytes,
				research::DefaultMaximumAicaArtifactEvents,true);
		FAIL()<<"strict validation applied a mutation after its final sample";
	}
	catch(const std::runtime_error& error)
	{
		EXPECT_NE(std::string::npos,std::string(error.what()).find(
				"record follows the final AICA sample"));
	}
}

TEST(ResearchAicaArtifact, RefusesIncompleteAndExistingOutputs)
{
	Fixture fixture;{research::AicaArtifactWriter writer(fixture.artifact,fixture.binding,1);writer.writeCheckpoint(fixture.checkpoint);writer.abandon();}
	EXPECT_THROW(research::validateAicaArtifactFile(fixture.artifact,fixture.binding),std::runtime_error);
	EXPECT_THROW(research::AicaArtifactWriter(fixture.artifact,fixture.binding,1),std::system_error);
}

TEST(ResearchAicaArtifact, RejectsUnknownOwnerAndNonPowerOfTwoRam)
{
	Fixture ownerFixture;
	{
		research::AicaArtifactWriter writer(ownerFixture.artifact,ownerFixture.binding,1);
		writer.writeCheckpoint(ownerFixture.checkpoint);
		research::AicaObservation begin;begin.type=research::AicaObservationType::KeyBatchBegin;
		begin.tick=10;begin.sampleCutOrdinal=50;
		EXPECT_THROW(writer.write(begin),std::runtime_error);
	}
	Fixture ramFixture;ramFixture.checkpoint.ram.resize(3*1024*1024);
	research::AicaArtifactWriter writer(ramFixture.artifact,ramFixture.binding,1);
	EXPECT_THROW(writer.writeCheckpoint(ramFixture.checkpoint),std::runtime_error);
}

TEST(ResearchAicaArtifact, RejectsSuppressionAndMissingNonzeroAudio)
{
	Fixture fixture;research::AicaArtifactWriter writer(fixture.artifact,fixture.binding,1);writer.writeCheckpoint(fixture.checkpoint);
	research::AicaObservation suppressed;suppressed.type=research::AicaObservationType::SampleSuppressed;suppressed.tick=10;suppressed.suppression=research::AicaSampleSuppression::Muted;
	EXPECT_THROW(writer.write(suppressed),std::runtime_error);
}

TEST(ResearchAicaArtifact, IndependentlyAuthenticatesReplayAndTerminalBoundary)
{
	Fixture fixture;
	const std::filesystem::path replay = fixture.directory.file("replay.fcmt");
	research::IdentityManifest identity = bindReplay(fixture, replay);
	fixture.capture();
	const auto summary = research::validateAicaArtifactWithReplay(
			fixture.artifact, identity, replay,
			research::DefaultMaximumAicaArtifactBytes,
			research::DefaultMaximumAicaArtifactEvents,
			512ull*1024*1024, false);
	const auto replaySummary=research::validateProductionMapleTraceFile(replay,
			identity.mapleReplayIdentityDigest);
	EXPECT_EQ(8u,replaySummary.startTick);
	EXPECT_EQ(1008u,replaySummary.endTick);
	EXPECT_EQ(std::filesystem::file_size(replay),replaySummary.fileBytes);
	EXPECT_TRUE(research::sha256Equal(fixture.binding.replayDigest,
			replaySummary.fileDigest));
	EXPECT_EQ(9u, summary.startTick);
	EXPECT_EQ(12u, summary.endTick);
	EXPECT_EQ(std::filesystem::file_size(fixture.artifact),summary.artifactBytes);
	EXPECT_TRUE(research::sha256Equal(research::hashFileExact(fixture.artifact,
			research::DefaultMaximumAicaArtifactBytes),summary.artifactDigest));

	identity.runtimeConfiguration.mapleDmaCheckpoint = 2;
	EXPECT_THROW(research::validateAicaArtifactWithReplay(
			fixture.artifact, identity, replay,
			research::DefaultMaximumAicaArtifactBytes,
			research::DefaultMaximumAicaArtifactEvents,
			512ull*1024*1024, false), std::runtime_error);
}

TEST(ResearchAicaArtifact, RejectsSchemaV1ReplayForAicaAuthority)
{
	Fixture fixture;
	const std::filesystem::path replay=fixture.directory.file("replay-v1.fcmt");
	research::IdentityManifest identity=bindReplay(fixture,replay,
			research::MapleTraceSchemaVersionV1);
	fixture.capture();
	try
	{
		research::validateAicaArtifactWithReplay(fixture.artifact,identity,replay,
				research::DefaultMaximumAicaArtifactBytes,
				research::DefaultMaximumAicaArtifactEvents,512ull*1024*1024,false);
		FAIL()<<"schema-v1 replay was accepted as AICA authority";
	}
	catch(const research::AicaInputValidationError& error)
	{
		EXPECT_EQ(research::AicaInputFailureStage::Replay,error.stage);
		EXPECT_TRUE(error.replayDigestAvailable);
		EXPECT_EQ(research::MapleTraceSchemaVersionV1,error.replaySchemaVersion);
		EXPECT_NE(std::string::npos,std::string(error.what()).find("schema-v2"));
	}
}

#include "research/aica_artifact.h"
#include "research/maple_trace.h"

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>

namespace
{
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
	Fixture(){
		binding.identityDigest=research::sha256("identity",8);binding.replayDigest=research::sha256("replay",6);binding.configurationDigest=research::sha256("aica-config",11);binding.dspEnabled=true;
		checkpoint.tick=9;checkpoint.ram.resize(2*1024*1024);for(std::size_t i=0;i<checkpoint.ram.size();++i)checkpoint.ram[i]=static_cast<std::uint8_t>(i*13);
		checkpoint.registers[4]=0x10;checkpoint.registers[12]=4;checkpoint.cddaIndex=0;
	}
	research::AicaObservation key()const{
		research::AicaObservation e;e.type=research::AicaObservationType::KeyOn;e.emissionOrdinal=1;e.tick=10;e.owner.writer=research::AicaWriter::Arm7;e.channel=0;
		std::copy(checkpoint.registers.begin(),checkpoint.registers.begin()+0x80,e.channelRegisters.begin());return e;
	}
	void capture(){
		research::AicaArtifactWriter writer(artifact,binding,1);writer.writeCheckpoint(checkpoint);writer.write(key());
		research::AicaObservation batch;batch.type=research::AicaObservationType::KeyBatchComplete;batch.emissionOrdinal=2;batch.tick=11;batch.keyOnMask=1;writer.write(batch);
		research::AicaObservation sample;sample.type=research::AicaObservationType::SampleFrame;sample.emissionOrdinal=3;sample.tick=12;sample.sampleOrdinal=50;sample.dspEnabled=binding.dspEnabled;sample.cddaFrameIndex=0;sample.finalLeft=1;sample.finalRight=-1;writer.write(sample);writer.finalize();
	}
};

research::IdentityManifest bindReplay(Fixture& fixture,
		const std::filesystem::path& replay)
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
	identity.runtimeConfiguration.aicaConfiguration.dspEnabled = true;
	identity.runtimeConfiguration.aicaConfiguration.vmuSound = false;
	identity.runtimeConfiguration.aicaConfiguration.sampleRate = 44100;
	identity.runtimeConfiguration.aicaConfiguration.sampleFormat =
			"signed-pcm16-le-stereo";
	identity.runtimeConfiguration.aicaConfiguration.outputStage =
			"pre-backend-pre-user-volume";

	research::MapleTraceWriter replayWriter(replay,
			identity.mapleReplayIdentityDigest);
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
	transaction.descriptorHeader1 = 0x80000001;
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
	fixture.binding.dspEnabled = true;
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
	EXPECT_EQ(3u,summary.eventCount);EXPECT_EQ(1u,summary.sampleFrames);EXPECT_EQ(1u,summary.keyedSourceCount);EXPECT_EQ(2u*1024u*1024u,summary.checkpointRamBytes);
}

TEST(ResearchAicaArtifact, RefusesIncompleteAndExistingOutputs)
{
	Fixture fixture;{research::AicaArtifactWriter writer(fixture.artifact,fixture.binding,1);writer.writeCheckpoint(fixture.checkpoint);writer.abandon();}
	EXPECT_THROW(research::validateAicaArtifactFile(fixture.artifact,fixture.binding),std::runtime_error);
	EXPECT_THROW(research::AicaArtifactWriter(fixture.artifact,fixture.binding,1),std::system_error);
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
			fixture.artifact, identity, replay);
	EXPECT_EQ(8u, research::validateProductionMapleTraceFile(replay,
			identity.mapleReplayIdentityDigest).startTick);
	EXPECT_EQ(1008u, research::validateProductionMapleTraceFile(replay,
			identity.mapleReplayIdentityDigest).endTick);
	EXPECT_EQ(9u, summary.startTick);
	EXPECT_EQ(12u, summary.endTick);

	identity.runtimeConfiguration.mapleDmaCheckpoint = 2;
	EXPECT_THROW(research::validateAicaArtifactWithReplay(
			fixture.artifact, identity, replay), std::runtime_error);
}

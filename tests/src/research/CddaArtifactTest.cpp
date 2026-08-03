#include "research/cdda_artifact.h"

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>

namespace
{
class TemporaryDirectory
{
public:
	TemporaryDirectory(){static std::atomic<unsigned> next{0};path=std::filesystem::temp_directory_path()/
			("flycast-cdda-test-"+std::to_string(next++));std::filesystem::create_directories(path);}
	~TemporaryDirectory(){std::error_code error;std::filesystem::remove_all(path,error);}
	std::filesystem::path file(const char* name)const{return path/name;}
private:std::filesystem::path path;
};

void write(const std::filesystem::path& path,const std::vector<std::uint8_t>& bytes)
{std::ofstream output(path,std::ios::binary);output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
void writeText(const std::filesystem::path& path,const std::string& text)
{std::ofstream output(path,std::ios::binary);output<<text;}

research::Sh4InstructionOwnerToken owner()
{
	research::Sh4InstructionOwnerToken token;token.valid=true;
	token.backend=research::Sh4ObservationBackend::Interpreter;token.generation=1;
	token.tick=10;token.pc=0x8c010100;token.pr=0x8c020000;token.opcode=0x2102;return token;
}

struct Fixture
{
	TemporaryDirectory directory;
	std::filesystem::path descriptor=directory.file("disc.gdi");
	std::filesystem::path audio=directory.file("track02.bin");
	std::filesystem::path artifact=directory.file("capture.fccdda");
	std::vector<std::uint8_t> sector=std::vector<std::uint8_t>(2352);
	research::IdentityManifest identity;
	research::CddaArtifactBinding binding;
	Fixture()
	{
		for(std::size_t i=0;i<sector.size();++i)sector[i]=static_cast<std::uint8_t>((i*29+3)&0xff);
		sector[0]=0x34;sector[1]=0x12;sector[2]=0x78;sector[3]=0x56;
		write(audio,sector);writeText(descriptor,"1\n2 450 0 2352 \"track02.bin\" 0\n");
		identity.mediaKind="gdi";identity.mediaSourcePath=descriptor;
		identity.mediaSourceSize=std::filesystem::file_size(descriptor);
		identity.mediaSourceDigest=research::hashFileExact(descriptor,identity.mediaSourceSize);
		research::MediaTrackIdentity track;track.path=audio;track.size=sector.size();
		track.digest=research::hashFileExact(audio,track.size);track.track=2;track.startFad=600;
		track.sectorSize=2352;identity.mediaTracks.push_back(track);identity.mediaTrackCount=1;
		identity.digest=research::sha256("identity",8);
		binding.backend=research::Sh4ObservationBackend::Interpreter;
		binding.identityDigest=identity.digest;binding.replayDigest=research::sha256("replay",6);
		binding.configurationDigest=research::sha256("aica-config",11);binding.dspEnabled=true;
	}

	void capture(bool corruptSector=false,bool swappedInput=false,bool repeat=false)
	{
		research::CddaArtifactWriter writer(artifact,binding,2);
		research::AicaCheckpoint checkpoint;checkpoint.tick=9;checkpoint.cddaIndex=1176;
		writer.writeCheckpoint(checkpoint);
		research::CddaObservation accepted;accepted.type=research::CddaObservationType::ControlAccepted;
		accepted.emissionOrdinal=4;accepted.tick=10;accepted.controlGeneration=1;
		accepted.initiator=owner();accepted.requestId=7;accepted.command=0x15;
		accepted.parameters={600,601,repeat?1u:0u,0};writer.write(accepted);
		research::CddaObservation applied=accepted;applied.type=research::CddaObservationType::ControlApplied;
		applied.emissionOrdinal=5;applied.tick=11;applied.appliedSuccessfully=true;
		applied.before.status=0;applied.after.status=1;applied.after.repeats=repeat?1u:0u;
		applied.after.currentFad=applied.after.startFad=600;applied.after.endFad=601;writer.write(applied);
		research::CddaObservation read;read.type=research::CddaObservationType::Sector;
		read.emissionOrdinal=6;read.tick=12;read.controlGeneration=1;read.aicaGeneration=9;
		read.fad=600;read.readSuccessful=true;read.before=applied.after;read.after=read.before;
		if(repeat){read.after.repeats=0;read.after.currentFad=600;}
		else{read.after.status=3;read.after.currentFad=601;}
		read.bytes=sector;if(corruptSector)read.bytes[10]^=1;writer.write(read);
		for(std::uint16_t frame=0;frame<2;++frame){research::AicaObservation sample;
			sample.type=research::AicaObservationType::SampleFrame;sample.emissionOrdinal=7+frame;
			sample.tick=13+frame;sample.sampleOrdinal=100+frame;sample.cddaGeneration=9;
			sample.cddaFrameIndex=frame;sample.dspEnabled=true;
			const std::size_t offset=frame*4;sample.cddaInputLeft=static_cast<std::int16_t>(
					std::uint16_t(sector[offset])|(std::uint16_t(sector[offset+1])<<8));
			sample.cddaInputRight=static_cast<std::int16_t>(std::uint16_t(sector[offset+2])|
					(std::uint16_t(sector[offset+3])<<8));
			if(swappedInput&&frame==0)sample.cddaInputLeft=0x3412;
			sample.cddaContributionLeft=sample.cddaInputLeft;sample.cddaContributionRight=sample.cddaInputRight;
			sample.finalLeft=static_cast<std::int16_t>(sample.cddaInputLeft);
			sample.finalRight=static_cast<std::int16_t>(sample.cddaInputRight);writer.write(sample);}
		writer.finalize();
	}

	void capturePacket(bool mutateAppliedState=false,bool pauseResume=false,
			bool mutatePause=false,bool omittedEnd=false)
	{
		research::CddaArtifactWriter writer(artifact,binding,1);
		research::AicaCheckpoint checkpoint;checkpoint.tick=9;checkpoint.cddaIndex=1176;
		writer.writeCheckpoint(checkpoint);
		research::CddaObservation accepted;accepted.type=research::CddaObservationType::ControlAccepted;
		accepted.emissionOrdinal=4;accepted.tick=10;accepted.controlGeneration=2;
		accepted.path=research::CddaControlPath::GdromPacket;accepted.initiator=owner();
		accepted.requestId=8;accepted.command=0x20;
		const std::uint8_t packet[12] {0x20,0x01,0x00,0x02,0x58,0x00,
				0x00,0x00,0x00,omittedEnd?0x00u:0x02u,omittedEnd?0x00u:0x59u,0x00};
		for(std::size_t word=0;word<3;++word)for(std::size_t byte=0;byte<4;++byte)
			accepted.parameters[word]|=std::uint32_t(packet[word*4+byte])<<(byte*8);
		writer.write(accepted);
		research::CddaObservation applied=accepted;applied.type=research::CddaObservationType::ControlApplied;
		applied.emissionOrdinal=5;applied.tick=11;applied.appliedSuccessfully=true;
		applied.after.status=1;applied.after.currentFad=applied.after.startFad=
				mutateAppliedState?599u:600u;
		applied.after.endFad=omittedEnd?549300u:601u;writer.write(applied);
		research::CddaObservation finalState=applied;
		if(pauseResume)
		{
			research::CddaObservation seek=accepted;seek.emissionOrdinal=6;seek.tick=12;
			seek.controlGeneration=3;seek.requestId=9;seek.command=0x21;seek.parameters={};
			const std::uint8_t seekPacket[12] {0x21,0x04};
			for(std::size_t word=0;word<3;++word)for(std::size_t byte=0;byte<4;++byte)
				seek.parameters[word]|=std::uint32_t(seekPacket[word*4+byte])<<(byte*8);
			writer.write(seek);research::CddaObservation paused=seek;
			paused.type=research::CddaObservationType::ControlApplied;paused.emissionOrdinal=7;
			paused.tick=13;paused.appliedSuccessfully=true;paused.before=applied.after;
			paused.after=paused.before;paused.after.status=mutatePause?0u:2u;writer.write(paused);
			research::CddaObservation resume=accepted;resume.emissionOrdinal=8;resume.tick=14;
			resume.controlGeneration=4;resume.requestId=10;resume.command=0x20;resume.parameters={};
			resume.parameters[0]=0x00000720;writer.write(resume);
			finalState=resume;finalState.type=research::CddaObservationType::ControlApplied;
			finalState.emissionOrdinal=9;finalState.tick=15;finalState.appliedSuccessfully=true;
			finalState.before=paused.after;finalState.after=finalState.before;
			if(finalState.after.status==2)finalState.after.status=1;writer.write(finalState);
		}
		research::CddaObservation read;read.type=research::CddaObservationType::Sector;
		read.emissionOrdinal=pauseResume?10:6;read.tick=pauseResume?16:12;
		read.controlGeneration=pauseResume?4:2;read.aicaGeneration=10;
		read.fad=600;read.readSuccessful=true;read.before=finalState.after;read.after=read.before;
		read.after.status=omittedEnd?1u:3u;read.after.currentFad=601;read.bytes=sector;writer.write(read);
		research::AicaObservation sample;sample.type=research::AicaObservationType::SampleFrame;
		sample.emissionOrdinal=pauseResume?11:7;sample.tick=pauseResume?17:13;
		sample.sampleOrdinal=200;sample.cddaGeneration=10;
		sample.cddaFrameIndex=0;sample.dspEnabled=true;sample.cddaInputLeft=0x1234;
		sample.cddaInputRight=0x5678;sample.cddaContributionLeft=0x1234;
		sample.finalLeft=0x1234;sample.finalRight=0x5678;writer.write(sample);writer.finalize();
	}
};
}

TEST(ResearchCddaArtifact, IndependentlyAuthenticatesRawGdiAndLittleEndianSamples)
{
	Fixture fixture;fixture.capture();const auto summary=research::validateCddaArtifactFile(
			fixture.artifact,fixture.identity,fixture.binding.replayDigest,
			fixture.binding.configurationDigest);EXPECT_EQ(1u,summary.successfulSectors);
	EXPECT_EQ(2u,summary.sampleFrames);EXPECT_EQ(2u,summary.contributingSampleFrames);
}

TEST(ResearchCddaArtifact, RejectsSectorBytesThatDifferFromAuthenticatedGdi)
{
	Fixture fixture;fixture.capture(true);EXPECT_THROW(research::validateCddaArtifactFile(
			fixture.artifact,fixture.identity,fixture.binding.replayDigest,
			fixture.binding.configurationDigest),std::runtime_error);
}

TEST(ResearchCddaArtifact, RejectsWrongPcmEndianness)
{
	Fixture fixture;fixture.capture(false,true);EXPECT_THROW(research::validateCddaArtifactFile(
			fixture.artifact,fixture.identity,fixture.binding.replayDigest,
			fixture.binding.configurationDigest),std::runtime_error);
}

TEST(ResearchCddaArtifact, AcceptsExactRepeatBoundaryTransition)
{
	Fixture fixture;fixture.capture(false,false,true);EXPECT_NO_THROW(
			research::validateCddaArtifactFile(fixture.artifact,fixture.identity,
					fixture.binding.replayDigest,fixture.binding.configurationDigest));
}

TEST(ResearchCddaArtifact, CannotFinalizeFailedOrNoncontributingCapture)
{
	Fixture fixture;research::CddaArtifactWriter writer(fixture.artifact,fixture.binding,1);
	research::AicaCheckpoint checkpoint;checkpoint.tick=1;writer.writeCheckpoint(checkpoint);
	EXPECT_THROW(writer.finalize(),std::logic_error);writer.abandon();
}

TEST(ResearchCddaArtifact, IndependentlyReconstructsGdromPacketPlay)
{
	Fixture fixture;fixture.capturePacket();EXPECT_NO_THROW(research::validateCddaArtifactFile(
			fixture.artifact,fixture.identity,fixture.binding.replayDigest,
			fixture.binding.configurationDigest));
}

TEST(ResearchCddaArtifact, ReconstructsOmittedPacketEndAsGdromSessionLeadout)
{
	Fixture fixture;fixture.capturePacket(false,false,false,true);EXPECT_NO_THROW(
			research::validateCddaArtifactFile(fixture.artifact,fixture.identity,
					fixture.binding.replayDigest,fixture.binding.configurationDigest));
}

TEST(ResearchCddaArtifact, RejectsGdromPacketStateNotDerivedFromRawPacket)
{
	Fixture fixture;fixture.capturePacket(true);EXPECT_THROW(research::validateCddaArtifactFile(
			fixture.artifact,fixture.identity,fixture.binding.replayDigest,
			fixture.binding.configurationDigest),std::runtime_error);
}

TEST(ResearchCddaArtifact, ReconstructsPacketPauseAndResumeSequence)
{
	Fixture fixture;fixture.capturePacket(false,true);EXPECT_NO_THROW(
			research::validateCddaArtifactFile(fixture.artifact,fixture.identity,
					fixture.binding.replayDigest,fixture.binding.configurationDigest));
}

TEST(ResearchCddaArtifact, RejectsPacketPauseStateMutation)
{
	Fixture fixture;fixture.capturePacket(false,true,true);EXPECT_THROW(
			research::validateCddaArtifactFile(fixture.artifact,fixture.identity,
					fixture.binding.replayDigest,fixture.binding.configurationDigest),
			std::runtime_error);
}

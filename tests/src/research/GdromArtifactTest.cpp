#include "research/gdrom_artifact.h"

#include <gtest/gtest.h>

#include <atomic>
#include <fstream>

namespace
{
class TemporaryDirectory
{
public:
	TemporaryDirectory(){ static std::atomic<unsigned> n{0}; path=std::filesystem::temp_directory_path()/("flycast-gdrom-test-"+std::to_string(n++)); std::filesystem::create_directories(path); }
	~TemporaryDirectory(){ std::error_code error; std::filesystem::remove_all(path,error); }
	std::filesystem::path file(const char* name)const{return path/name;}
private: std::filesystem::path path;
};
void write(const std::filesystem::path& path,const std::vector<std::uint8_t>& bytes)
{ std::ofstream output(path,std::ios::binary); output.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); }
void writeText(const std::filesystem::path& path,const std::string& text)
{ std::ofstream output(path,std::ios::binary); output<<text; }
research::Sh4InstructionOwnerToken owner()
{ research::Sh4InstructionOwnerToken o; o.valid=true; o.backend=research::Sh4ObservationBackend::Interpreter; o.generation=1;o.tick=10;o.pc=0x8c010100;o.opcode=0x2102;return o; }

struct Fixture
{
	TemporaryDirectory directory;
	std::filesystem::path descriptor=directory.file("disc.gdi"),track=directory.file("track03.bin"),artifact=directory.file("capture.fcgd");
	std::vector<std::uint8_t> user;
	research::IdentityManifest identity;
	research::Sha256Digest replay=research::sha256("replay",6);
	Fixture(){
		std::vector<std::uint8_t> raw(2*2352,0); user.resize(4096);
		for(std::size_t i=0;i<user.size();++i)user[i]=static_cast<std::uint8_t>((i*17)&0xff);
		std::copy(user.begin(),user.begin()+2048,raw.begin()+16);
		std::copy(user.begin()+2048,user.end(),raw.begin()+2352+16); write(track,raw);
		writeText(descriptor,"1\n3 45000 4 2352 \"track03.bin\" 0\n");
		identity.mediaKind="gdi"; identity.mediaSourcePath=descriptor; identity.mediaSourceSize=std::filesystem::file_size(descriptor);
		identity.mediaSourceDigest=research::hashFileExact(descriptor,identity.mediaSourceSize);
		research::MediaTrackIdentity t; t.path=track;t.size=raw.size();t.digest=research::hashFileExact(track,t.size);t.track=3;t.startFad=45150;t.sectorSize=2352;
		identity.mediaTracks.push_back(t);identity.mediaTrackCount=1;identity.digest=research::sha256("identity",8);
	}
	void capture(bool corrupt=false){
		research::GdromArtifactBinding binding;binding.identityDigest=identity.digest;binding.replayDigest=replay;
		research::GdromArtifactWriter writer(artifact,binding);
		research::GdromObservation begin;begin.type=research::GdromObservationType::CommandBegin;begin.emissionOrdinal=4;begin.tick=10;begin.commandGeneration=1;begin.initiator=owner();begin.requestId=7;begin.command=0x11;begin.parameters={45150,2,0x0c100000,0};writer.write(begin);
		research::GdromObservation chunk;chunk.type=research::GdromObservationType::TransferChunk;chunk.emissionOrdinal=5;chunk.tick=20;chunk.commandGeneration=1;chunk.fad=45150;chunk.sectorCount=2;chunk.destination=0x0c100000;chunk.bytes=user;if(corrupt)chunk.bytes[3]^=1;writer.write(chunk);
		research::GdromObservation complete;complete.type=research::GdromObservationType::Complete;complete.emissionOrdinal=6;complete.tick=30;complete.commandGeneration=1;complete.completion=research::GdromCompletionMechanism::Status;complete.transferredBytes=user.size();writer.write(complete);writer.finalize();
	}
};
}

TEST(ResearchGdromArtifact, IndependentGdiDecoderAcceptsExactReiosTransfer)
{
	Fixture f;f.capture();const auto summary=research::validateGdromArtifactFile(f.artifact,f.identity,f.replay);EXPECT_EQ(3u,summary.eventCount);EXPECT_EQ(1u,summary.typeCounts[1]);
}
TEST(ResearchGdromArtifact, RejectsBytesThatDifferFromAuthenticatedTrack)
{
	Fixture f;f.capture(true);EXPECT_THROW(research::validateGdromArtifactFile(f.artifact,f.identity,f.replay),std::runtime_error);
}
TEST(ResearchGdromArtifact, RejectsIncompleteCandidate)
{
	Fixture f;research::GdromArtifactBinding binding;binding.identityDigest=f.identity.digest;binding.replayDigest=f.replay;research::GdromArtifactWriter writer(f.artifact,binding);
	research::GdromObservation begin;begin.type=research::GdromObservationType::CommandBegin;begin.tick=10;begin.commandGeneration=1;begin.initiator=owner();begin.requestId=1;begin.command=0x11;begin.parameters={45150,1,0x0c100000,0};writer.write(begin);writer.abandon();
	EXPECT_THROW(research::validateGdromArtifactFile(f.artifact,f.identity,f.replay),std::runtime_error);
}

TEST(ResearchGdromArtifact, RefusesToOverwriteExistingOutput)
{
	Fixture f;writeText(f.artifact,"owned");research::GdromArtifactBinding binding;
	EXPECT_THROW(research::GdromArtifactWriter(f.artifact,binding),std::system_error);
	std::ifstream input(f.artifact,std::ios::binary);std::string bytes;input>>bytes;
	EXPECT_EQ("owned",bytes);
}

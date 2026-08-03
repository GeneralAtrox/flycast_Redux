#include "research/aica_artifact.h"
#include "research/identity_manifest.h"

#include <iostream>

int main(int argc,char** argv)
{
	try{
		if(argc!=4){std::cerr<<"usage: flycast-research-validate-aica <artifact.fcaica> <identity.json> <maple-replay.fcmt>\n";return 2;}
		const auto identity=research::loadIdentityManifest(argv[2]);research::requireSh4EquivalenceIdentityV2(identity);
		const auto summary=research::validateAicaArtifactWithReplay(argv[1],identity,argv[3]);
		std::cout<<"{\"schema\":\"flycast-aica-validation-v1\",\"accepted\":true,\"identity_sha256\":\""<<research::sha256ToHex(summary.binding.identityDigest)<<"\",\"replay_sha256\":\""<<research::sha256ToHex(summary.binding.replayDigest)<<"\",\"start_tick\":"<<summary.startTick<<",\"end_tick\":"<<summary.endTick<<",\"events\":"<<summary.eventCount<<",\"sample_frames\":"<<summary.sampleFrames<<",\"key_on_count\":"<<summary.keyOnCount<<",\"keyed_source_count\":"<<summary.keyedSourceCount<<",\"nonzero_sample_frames\":"<<summary.nonzeroSampleFrames<<",\"pcm_sha256\":\""<<research::sha256ToHex(summary.pcmDigest)<<"\",\"keyed_source_sha256\":\""<<research::sha256ToHex(summary.keyedSourceDigest)<<"\"}\n";return 0;
	}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

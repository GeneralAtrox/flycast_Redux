#include "research/cdda_artifact.h"
#include "research/identity_manifest.h"
#include "research/maple_trace.h"

#include <iostream>

int main(int argc,char** argv)
{
	try
	{
		if(argc!=4)
		{
			std::cerr<<"usage: flycast-research-validate-cdda <artifact.fccdda> <identity.json> <maple-replay.fcmt>\n";
			return 2;
		}
		const auto identity=research::loadIdentityManifest(argv[2]);
		research::requireSh4EquivalenceIdentityV2(identity);
		const auto replaySummary=research::validateProductionMapleTraceFile(argv[3],
				identity.mapleReplayIdentityDigest);
		if(identity.runtimeConfiguration.mapleDmaCheckpoint==0||
				replaySummary.dmaCount!=identity.runtimeConfiguration.mapleDmaCheckpoint)
			throw std::runtime_error("CD-DA Maple replay does not end at the identity DMA checkpoint");
		if(!identity.runtimeConfiguration.aicaConfiguration.available)
			throw std::runtime_error("CD-DA identity has no aica_configuration");
		const auto replayDigest=research::hashFileExact(argv[3],
				research::DefaultMaximumMapleTraceBytes);
		const auto configurationDigest=research::aicaConfigurationDigest(
				identity.runtimeConfiguration.aicaConfiguration);
		const auto summary=research::validateCddaArtifactFile(argv[1],identity,
				replayDigest,configurationDigest);
		std::cout<<"{\"schema\":\"flycast-cdda-validation-v2\",\"accepted\":true,"
				"\"identity_sha256\":\""<<research::sha256ToHex(summary.binding.identityDigest)
				<<"\",\"replay_sha256\":\""<<research::sha256ToHex(summary.binding.replayDigest)
				<<"\",\"start_tick\":"<<summary.startTick<<",\"end_tick\":"<<summary.endTick
				<<",\"events\":"<<summary.eventCount<<",\"sample_frames\":"<<summary.sampleFrames
				<<",\"successful_sectors\":"<<summary.successfulSectors
				<<",\"contributing_sample_frames\":"<<summary.contributingSampleFrames
				<<",\"applied_play_controls\":"<<summary.appliedPlayControls
				<<",\"pcm_sha256\":\""<<research::sha256ToHex(summary.pcmDigest)<<"\"}\n";
		return 0;
	}
	catch(const std::exception& exception)
	{
		std::cerr<<exception.what()<<'\n';return 1;
	}
}

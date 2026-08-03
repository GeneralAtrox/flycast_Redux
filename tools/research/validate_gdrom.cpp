#include "research/gdrom_artifact.h"
#include "research/gdrom_hardware_artifact.h"
#include "research/identity_manifest.h"
#include "research/sha256.h"

#include <cstdio>
#include <filesystem>
#include <string>

int main(int argc, char** argv)
{
	std::filesystem::path artifact,identity,replay,replayIdentity,bios,flash;
	for(int i=1;i<argc;++i){
		const std::string arg=argv[i];
		if((arg=="--artifact"||arg=="--identity"||arg=="--replay"
				||arg=="--replay-identity"||arg=="--bios"||arg=="--flash")
				&& i+1<argc){
			const auto value=std::filesystem::u8path(argv[++i]);
			if(arg=="--artifact")artifact=value;
			else if(arg=="--identity")identity=value;
			else if(arg=="--replay")replay=value;
			else if(arg=="--replay-identity")replayIdentity=value;
			else if(arg=="--bios")bios=value;
			else flash=value;
		} else { std::fprintf(stderr,"unknown or incomplete argument: %s\n",arg.c_str()); return 2; }
	}
	if(artifact.empty()||identity.empty()||replay.empty()){
		std::fprintf(stderr,"Usage: %s --artifact <gdrom.fcgd> --identity <identity.json> --replay <maple.fcmaple> [--replay-identity <maple-record-identity.json>] [--bios <dc_boot.bin> --flash <dc_nvmem.bin>]\n",argv[0]); return 2;
	}
	try{
		auto loaded=research::loadIdentityManifest(identity);
		if(!bios.empty() || !flash.empty()) {
			if(bios.empty() || flash.empty())
				throw std::runtime_error("BIOS and flash overrides must be supplied together");
			loaded.firmware.bios.path=bios;
			loaded.firmware.initialFlash.path=flash;
		}
		const auto replayDigest=research::hashFileExact(replay,512ull*1024*1024);
		if (loaded.firmware.mode == research::FirmwareMode::Real) {
			if(replayIdentity.empty())
				throw std::runtime_error("real-firmware validation requires --replay-identity");
			const auto recordIdentity=research::loadIdentityManifest(replayIdentity);
			const auto replaySummary=research::validateGdromHardwareReplayFile(
					replay,loaded,recordIdentity);
			const auto summary=research::validateGdromHardwareArtifactFile(
					artifact,loaded,replayDigest);
			research::requireGdromHardwareReplayTimeline(summary,replaySummary);
			std::printf("ACCEPTED flycast-research-gdrom-hardware-v1\nevents=%llu\ncommands=%llu\ndma_chunks=%llu\npio_words=%llu\nmaple_dma_count=%llu\nmaple_record_identity_sha256=%s\n",
					static_cast<unsigned long long>(summary.eventCount),
					static_cast<unsigned long long>(summary.completedCommands),
					static_cast<unsigned long long>(summary.typeCounts[3]),
					static_cast<unsigned long long>(summary.typeCounts[5]),
					static_cast<unsigned long long>(replaySummary.dmaCount),
					research::sha256ToHex(recordIdentity.digest).c_str());
		} else {
			const auto summary=research::validateGdromArtifactFile(artifact,loaded,replayDigest);
			std::printf("ACCEPTED flycast-research-gdrom-v1\nevents=%llu\ncommands=%llu\nchunks=%llu\n",
					static_cast<unsigned long long>(summary.eventCount),
					static_cast<unsigned long long>(summary.typeCounts[0]),
					static_cast<unsigned long long>(summary.typeCounts[1]));
		}
		return 0;
	}catch(const std::exception& e){ std::fprintf(stderr,"REJECTED flycast-research-gdrom: %s\n",e.what()); return 1; }
}

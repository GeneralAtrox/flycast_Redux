#include "research/aica_artifact.h"
#include "research/identity_manifest.h"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace
{
std::string maskHex(std::uint64_t value)
{
	std::ostringstream out;out<<"0x"<<std::hex<<std::setw(16)<<std::setfill('0')<<value;
	return out.str();
}

std::string jsonString(std::string_view value)
{
	std::ostringstream out;out<<'"';
	for(const unsigned char c:value)
	{
		switch(c)
		{
		case '"':out<<"\\\"";break;
		case '\\':out<<"\\\\";break;
		case '\b':out<<"\\b";break;
		case '\f':out<<"\\f";break;
		case '\n':out<<"\\n";break;
		case '\r':out<<"\\r";break;
		case '\t':out<<"\\t";break;
		default:
			if(c<0x20)out<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')
					<<static_cast<unsigned>(c)<<std::dec;
			else out<<static_cast<char>(c);
		}
	}
	out<<'"';return out.str();
}

std::filesystem::path runningExecutablePath()
{
#ifdef _WIN32
	std::vector<wchar_t> path(32768);
	const DWORD length=GetModuleFileNameW(nullptr,path.data(),
			static_cast<DWORD>(path.size()));
	if(length==0||length>=path.size())
		throw std::runtime_error("cannot resolve the running validator executable");
	return std::filesystem::path(std::wstring(path.data(),length));
#elif defined(__linux__)
	std::vector<char> path(4096);
	const ssize_t length=readlink("/proc/self/exe",path.data(),path.size());
	if(length<=0||static_cast<std::size_t>(length)>=path.size())
		throw std::runtime_error("cannot resolve the running validator executable");
	return std::filesystem::path(std::string(path.data(),static_cast<std::size_t>(length)));
#elif defined(__APPLE__)
	std::uint32_t size=0;
	_NSGetExecutablePath(nullptr,&size);
	std::vector<char> path(size);
	if(_NSGetExecutablePath(path.data(),&size)!=0)
		throw std::runtime_error("cannot resolve the running validator executable");
	return std::filesystem::weakly_canonical(path.data());
#else
	throw std::runtime_error("running validator executable resolution is unsupported");
#endif
}
}

int main(int argc,char** argv)
{
	std::uint64_t validatorSize=0;std::string validatorSha256;
	std::string failureStage="arguments";
	bool prefix=false,validatorAvailable=false;
	try{
		if(argc!=4&&argc!=5){std::cerr<<"usage: flycast-research-validate-aica <artifact.fcaica> <identity.json> <maple-replay.fcmt> [--semantic-prefix]\n";return 2;}
		prefix=argc==5&&std::string(argv[4])=="--semantic-prefix";if(argc==5&&!prefix){std::cerr<<"unknown option\n";return 2;}
		failureStage="validator-identity";
		const auto validatorBytes=research::readFileExact(
				runningExecutablePath(),512ull*1024*1024);
		validatorSize=validatorBytes.size();validatorSha256=research::sha256ToHex(
				research::sha256(validatorBytes.data(),validatorBytes.size()));validatorAvailable=true;
		failureStage="identity";const auto identity=research::loadIdentityManifest(argv[2]);
		failureStage="validation";
		const auto summary=research::validateAicaArtifactWithReplay(argv[1],identity,argv[3],research::DefaultMaximumAicaArtifactBytes,research::DefaultMaximumAicaArtifactEvents,512ull*1024*1024,!prefix);
		std::cout<<"{\"schema\":\"flycast-aica-validation-v9\",\"accepted\":true"
				<<",\"evidence_class\":\""<<(prefix?"diagnostic-semantic-prefix":"terminal-cycle")<<"\""
				<<",\"terminal_cycle_coverage_requested\":"<<(!prefix?"true":"false")
				<<",\"validator_identity_available\":true"
				<<",\"validator_size\":"<<validatorSize
				<<",\"validator_sha256\":\""<<validatorSha256<<"\""
				<<",\"artifact_size\":"<<summary.artifactBytes
				<<",\"artifact_digest_available\":true"
				<<",\"artifact_sha256\":\""<<research::sha256ToHex(summary.artifactDigest)<<"\""
				<<",\"sample_rate_hz\":"<<research::AicaSampleRateHz
				<<",\"lodoss_logic_rate_hz\":"<<research::LodossLogicRateHz
				<<",\"samples_per_lodoss_logic_tick\":"<<research::AicaSamplesPerLodossLogicTick
				<<",\"identity_sha256\":\""<<research::sha256ToHex(summary.binding.identityDigest)<<"\""
				<<",\"identity_digest_available\":true,\"replay_digest_available\":true"
				<<",\"replay_sha256\":\""<<research::sha256ToHex(summary.binding.replayDigest)<<"\""
				<<",\"replay_size\":"<<summary.replayBytes
				<<",\"replay_schema_version\":"<<summary.replaySchemaVersion
				<<",\"start_tick\":"<<summary.startTick<<",\"end_tick\":"<<summary.endTick
				<<",\"events\":"<<summary.eventCount<<",\"sample_frames\":"<<summary.sampleFrames
				<<",\"key_on_count\":"<<summary.keyOnCount<<",\"keyed_source_count\":"<<summary.keyedSourceCount
				<<",\"nonzero_sample_frames\":"<<summary.nonzeroSampleFrames
				<<",\"last_nonzero_dry_sample\":"<<summary.lastNonzeroDrySampleOrdinal
				<<",\"last_nonzero_dsp_sample\":"<<summary.lastNonzeroDspSampleOrdinal
				<<",\"last_nonzero_final_sample\":"<<summary.lastNonzeroFinalSampleOrdinal
				<<",\"field_coverage_complete\":"<<(summary.fieldCoverageComplete?"true":"false")
				<<",\"field_coverage_present_mask\":"<<summary.fieldCoveragePresentMask
				<<",\"field_coverage_consumed_mask\":"<<summary.fieldCoverageConsumedMask
				<<",\"field_coverage_proven_irrelevant_mask\":"<<summary.fieldCoverageProvenIrrelevantMask
				<<",\"channel_owner_mask_encoding\":\"hex-u64\""
				<<",\"channel_owner_present_mask\":\""<<maskHex(summary.channelOwnerPresentMask)<<"\""
				<<",\"channel_owner_checkpoint_active_mask\":\""<<maskHex(summary.channelOwnerCheckpointActiveMask)<<"\""
				<<",\"channel_owner_key_target_mask\":\""<<maskHex(summary.channelOwnerKeyTargetMask)<<"\""
				<<",\"channel_owner_observed_active_mask\":\""<<maskHex(summary.channelOwnerObservedActiveMask)<<"\""
				<<",\"channel_owner_consumed_mask\":\""<<maskHex(summary.channelOwnerConsumedMask)<<"\""
				<<",\"channel_owner_inactive_unkeyed_mask\":\""<<maskHex(summary.channelOwnerInactiveUnkeyedMask)<<"\""
				<<",\"channel_owner_scope\":\"captured-interval\""
				<<",\"channel_owner_key_binding\":\"artifact-event-plus-register-mirror\""
				<<",\"owner_writer_mask\":"<<summary.ownerWriterMask
				<<",\"dsp_tail_proof_verified\":"<<(summary.dspTailProofVerified?"true":"false")
				<<",\"dsp_tail_state_hashes_available\":"<<(summary.dspTailStateHashesAvailable?"true":"false")
				<<",\"dsp_tail_sample_frames\":"<<summary.dspTailSampleFrames
				<<",\"dsp_tail_mdec_steps\":"<<summary.dspTailMdecSteps
				<<",\"dsp_tail_anchor_mdec\":"<<summary.dspTailAnchorMdec
				<<",\"dsp_tail_terminal_mdec\":"<<summary.dspTailTerminalMdec
				<<",\"dsp_tail_anchor_state_sha256\":\""<<research::sha256ToHex(summary.dspTailAnchorStateDigest)<<"\""
				<<",\"dsp_tail_terminal_state_sha256\":\""<<research::sha256ToHex(summary.dspTailTerminalStateDigest)<<"\""
				<<",\"pcm_sha256\":\""<<research::sha256ToHex(summary.pcmDigest)<<"\""
				<<",\"keyed_source_sha256\":\""<<research::sha256ToHex(summary.keyedSourceDigest)<<"\"}\n";
		return 0;
	}catch(const research::AicaDspTailValidationError& e){
		const auto& s=e.summary;const auto& f=e.failure;
		std::cout<<"{\"schema\":\"flycast-aica-validation-v9\",\"accepted\":false"
				<<",\"evidence_class\":\"terminal-cycle\",\"failure_kind\":\"dsp-terminal-cycle\""
				<<",\"terminal_cycle_coverage_requested\":true"
				<<",\"validator_identity_available\":true,\"artifact_digest_available\":true"
				<<",\"identity_digest_available\":true,\"replay_digest_available\":true"
				<<",\"validator_size\":"<<validatorSize
				<<",\"validator_sha256\":\""<<validatorSha256<<"\""
				<<",\"artifact_size\":"<<s.artifactBytes
				<<",\"artifact_sha256\":\""<<research::sha256ToHex(s.artifactDigest)<<"\""
				<<",\"identity_sha256\":\""<<research::sha256ToHex(s.binding.identityDigest)<<"\""
				<<",\"replay_sha256\":\""<<research::sha256ToHex(s.binding.replayDigest)<<"\""
				<<",\"replay_size\":"<<s.replayBytes
				<<",\"replay_schema_version\":"<<s.replaySchemaVersion
				<<",\"dsp_tail_failure\":{\"anchor_sample\":"<<f.anchorSampleOrdinal
				<<",\"terminal_sample\":"<<f.terminalSampleOrdinal
				<<",\"output_sample_available\":"<<(f.outputSampleAvailable?"true":"false");
		if(f.outputSampleAvailable)
			std::cout<<",\"first_nonzero_sample\":"<<f.firstNonzeroSampleOrdinal
					<<",\"active_channel_mask\":\""<<maskHex(f.activeChannelMask)<<"\""
					<<",\"dry\":["<<f.dry[0]<<','<<f.dry[1]<<']'
					<<",\"cdda_input\":["<<f.cddaInput[0]<<','<<f.cddaInput[1]<<']'
					<<",\"cdda\":["<<f.cdda[0]<<','<<f.cdda[1]<<']'
					<<",\"dsp\":["<<f.dsp[0]<<','<<f.dsp[1]<<']'
					<<",\"final\":["<<f.final[0]<<','<<f.final[1]<<']';
		std::cout
				<<",\"mdec_steps\":"<<f.mdecSteps
				<<",\"anchor_mdec\":"<<f.anchorMdec<<",\"terminal_mdec\":"<<f.terminalMdec
				<<",\"anchor_temp_0\":"<<f.anchorTemp0<<",\"terminal_temp_0\":"<<f.terminalTemp0
				<<",\"anchor_state_sha256\":\""<<research::sha256ToHex(f.anchorStateDigest)<<"\""
				<<",\"terminal_state_sha256\":\""<<research::sha256ToHex(f.terminalStateDigest)<<"\""
				<<",\"output_zero\":"<<(f.outputZero?"true":"false")
				<<",\"geometry_stable\":"<<(f.geometryStable?"true":"false")
				<<",\"states_equal\":"<<(f.statesEqual?"true":"false")
				<<",\"state_hashes_equal\":"<<(f.stateHashesEqual?"true":"false")
				<<",\"state_difference\":"<<jsonString(f.stateDifference)<<'}'
				<<",\"error\":"<<jsonString(e.what())<<"}\n";
		std::cerr<<e.what()<<'\n';return 1;
	}catch(const research::AicaInputValidationError& e){
		const char* kind=e.stage==research::AicaInputFailureStage::Identity
				?"identity-validation":"replay-validation";
		std::cout<<"{\"schema\":\"flycast-aica-validation-v9\",\"accepted\":false"
				<<",\"evidence_class\":\""<<(prefix?"diagnostic-semantic-prefix":"terminal-cycle")<<"\""
				<<",\"terminal_cycle_coverage_requested\":"<<(!prefix?"true":"false")
				<<",\"failure_kind\":\""<<kind<<"\",\"validator_identity_available\":true"
				<<",\"validator_size\":"<<validatorSize<<",\"validator_sha256\":\""<<validatorSha256<<"\""
				<<",\"identity_digest_available\":true,\"identity_sha256\":\""
				<<research::sha256ToHex(e.identityDigest)<<"\""
				<<",\"replay_digest_available\":"<<(e.replayDigestAvailable?"true":"false");
		if(e.replayDigestAvailable)
			std::cout<<",\"replay_sha256\":\""<<research::sha256ToHex(e.replayDigest)<<"\""
					<<",\"replay_size\":"<<e.replayBytes
					<<",\"replay_schema_version\":"<<e.replaySchemaVersion;
		std::cout<<",\"error\":"<<jsonString(e.what())<<"}\n";
		std::cerr<<e.what()<<'\n';return 1;
	}catch(const research::AicaArtifactValidationError& e){
		const auto& s=e.summary;
		std::cout<<"{\"schema\":\"flycast-aica-validation-v9\",\"accepted\":false"
				<<",\"evidence_class\":\""<<(prefix?"diagnostic-semantic-prefix":"terminal-cycle")<<"\""
				<<",\"terminal_cycle_coverage_requested\":"<<(!prefix?"true":"false")
				<<",\"failure_kind\":\"artifact-validation\",\"validator_identity_available\":true"
				<<",\"identity_digest_available\":true,\"replay_digest_available\":true"
				<<",\"validator_size\":"<<validatorSize
				<<",\"validator_sha256\":\""<<validatorSha256<<"\""
				<<",\"artifact_digest_available\":"<<(s.artifactDigestAvailable?"true":"false")
				<<",\"artifact_size\":"<<s.artifactBytes
				;
		if(s.artifactDigestAvailable)
			std::cout<<",\"artifact_sha256\":\""<<research::sha256ToHex(s.artifactDigest)<<"\"";
		std::cout<<",\"identity_sha256\":\""<<research::sha256ToHex(s.binding.identityDigest)<<"\""
				<<",\"replay_sha256\":\""<<research::sha256ToHex(s.binding.replayDigest)<<"\""
				<<",\"replay_size\":"<<s.replayBytes
				<<",\"replay_schema_version\":"<<s.replaySchemaVersion
				<<",\"error\":"<<jsonString(e.what())<<"}\n";
		std::cerr<<e.what()<<'\n';return 1;
	}catch(const std::exception& e){
		std::cout<<"{\"schema\":\"flycast-aica-validation-v9\",\"accepted\":false"
				<<",\"evidence_class\":\""<<(prefix?"diagnostic-semantic-prefix":"terminal-cycle")<<"\""
				<<",\"terminal_cycle_coverage_requested\":"<<(!prefix?"true":"false")
				<<",\"failure_kind\":"<<jsonString(failureStage)
				<<",\"validator_identity_available\":"<<(validatorAvailable?"true":"false");
		if(validatorAvailable)
			std::cout<<",\"validator_size\":"<<validatorSize
					<<",\"validator_sha256\":\""<<validatorSha256<<"\"";
		if(failureStage=="identity")std::cout<<",\"identity_digest_available\":false";
		std::cout<<",\"error\":"<<jsonString(e.what())<<"}\n";
		std::cerr<<e.what()<<'\n';return 1;}
}

#include "research/aica_capture_runtime.h"

#include "cfg/option.h"
#include "hw/aica/sgc_if.h"
#include "log/Log.h"
#include "research/aica_artifact.h"
#include "research/aica_observation.h"
#include "research/identity_manifest.h"
#include "research/maple_trace.h"
#include "types.h"

#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace research
{
namespace
{
std::filesystem::path pathFor(const std::string& value)
{
#ifdef __cpp_char8_t
	std::u8string utf8(value.size(),u8'\0');std::memcpy(utf8.data(),value.data(),value.size());return std::filesystem::path(utf8);
#else
	return std::filesystem::u8path(value);
#endif
}
struct Configuration{IdentityManifest identity;std::filesystem::path identityPath,replayPath,outputPath;Sha256Digest replayDigest{};std::uint64_t replayMaximum=0,artifactMaximum=0,targetFrames=0;};
struct Session{Configuration configuration;std::unique_ptr<AicaArtifactWriter> writer;AicaObservationSubscription subscription=0;std::uint64_t droppedBaseline=0;std::vector<AicaObservation> pendingKeys;std::exception_ptr failure;};
std::unique_ptr<Configuration> configured;std::unique_ptr<Session> session;

std::filesystem::path runningExecutable()
{
#ifdef _WIN32
	std::vector<wchar_t> buffer(1024);while(buffer.size()<=32768){const DWORD n=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));if(n==0)throw FlycastException("cannot resolve the running Flycast executable");if(n<buffer.size())return std::filesystem::path(std::wstring(buffer.data(),n));buffer.resize(buffer.size()*2);}throw FlycastException("running Flycast executable path is too long");
#elif defined(__APPLE__)
	std::uint32_t size=0;_NSGetExecutablePath(nullptr,&size);std::vector<char> buffer(size);if(_NSGetExecutablePath(buffer.data(),&size)!=0)throw FlycastException("cannot resolve the running Flycast executable");return std::filesystem::weakly_canonical(buffer.data());
#elif defined(__linux__)
	return std::filesystem::read_symlink("/proc/self/exe");
#elif defined(__FreeBSD__)
	return std::filesystem::read_symlink("/proc/curproc/file");
#else
	throw FlycastException("running executable authentication is unsupported");
#endif
}
void authenticateExecutable(const IdentityManifest& identity)
{
	if(identity.emulatorExecutable.size==0)throw FlycastException("AICA identity has no executable authority");const auto executable=runningExecutable();std::error_code error;
	if(std::filesystem::file_size(executable,error)!=identity.emulatorExecutable.size||error||!sha256Equal(hashFileExact(executable,identity.emulatorExecutable.size),identity.emulatorExecutable.digest))throw FlycastException("running Flycast executable differs from AICA identity");
}
void reauthenticate(const Configuration& c)
{
	authenticateExecutable(c.identity);if(!sha256Equal(hashFileExact(c.identityPath,MaxIdentityManifestBytes),c.identity.digest)||!sha256Equal(hashFileExact(c.replayPath,c.replayMaximum),c.replayDigest))throw FlycastException("AICA immutable input changed during capture");
}
}

void configureAicaCaptureRuntime()
{
	abortAicaCaptureRuntime();if(config::ResearchAicaRecordPath.get().empty())return;
	if(config::ResearchIdentityManifestPath.get().empty()||config::ResearchMapleReplayPath.get().empty())throw FlycastException("AICA capture requires identity and Maple replay paths");
	for(const char* key:{"IdentityManifest","MapleReplay","AicaRecord"})if(!config::isTransient("research",key))throw FlycastException("AICA capture paths must be transient options");
	if(config::ResearchAicaMaxBytes.get()<static_cast<std::int64_t>(AicaArtifactHeaderSize)||config::ResearchAicaSampleFrames.get()<=0||config::ResearchAicaSampleFrames.get()>static_cast<std::int64_t>(MaximumAicaSampleFrames))throw FlycastException("AICA capture bounds are invalid");
	auto next=std::make_unique<Configuration>();next->identityPath=pathFor(config::ResearchIdentityManifestPath.get());next->replayPath=pathFor(config::ResearchMapleReplayPath.get());next->outputPath=pathFor(config::ResearchAicaRecordPath.get());
	if(pathsAlias(next->identityPath,next->replayPath)||pathsAlias(next->identityPath,next->outputPath)||pathsAlias(next->replayPath,next->outputPath))throw FlycastException("AICA capture paths alias");
	next->identity=loadIdentityManifest(next->identityPath);requireSh4EquivalenceIdentityV2(next->identity);authenticateExecutable(next->identity);
	next->replayMaximum=static_cast<std::uint64_t>(config::ResearchMapleTraceMaxBytes.get());
	const MapleTraceSummary replaySummary=validateProductionMapleTraceFile(next->replayPath,next->identity.mapleReplayIdentityDigest,next->replayMaximum);
	if(next->identity.runtimeConfiguration.mapleDmaCheckpoint==0||replaySummary.dmaCount!=next->identity.runtimeConfiguration.mapleDmaCheckpoint)throw FlycastException("AICA Maple replay does not end at the identity DMA checkpoint");
	next->replayDigest=hashFileExact(next->replayPath,next->replayMaximum);next->artifactMaximum=static_cast<std::uint64_t>(config::ResearchAicaMaxBytes.get());next->targetFrames=static_cast<std::uint64_t>(config::ResearchAicaSampleFrames.get());
	const auto& aica=next->identity.runtimeConfiguration.aicaConfiguration;
	if(!aica.available)throw FlycastException("AICA capture requires identity-bound aica_configuration");
	const bool dynarec=next->identity.runtimeConfiguration.cpuBackend=="dynarec";config::DynarecEnabled.override(dynarec);config::ResearchDynarecObservation.override(dynarec);config::DSPEnabled.override(aica.dspEnabled);config::VmuSound.override(aica.vmuSound);configured=std::move(next);
}

void startAicaCaptureRuntime()
{
	if(configured==nullptr)return;if(session!=nullptr)throw FlycastException("AICA capture is already active");
	const auto& aica=configured->identity.runtimeConfiguration.aicaConfiguration;const bool dynarec=configured->identity.runtimeConfiguration.cpuBackend=="dynarec";config::DynarecEnabled.override(dynarec);config::ResearchDynarecObservation.override(dynarec);config::DSPEnabled.override(aica.dspEnabled);config::VmuSound.override(aica.vmuSound);authenticateExecutable(configured->identity);reauthenticate(*configured);
	if(settings.aica.muteAudio)throw FlycastException("AICA capture cannot start while audio is muted");
	AicaArtifactBinding binding;binding.backend=dynarec?Sh4ObservationBackend::Dynarec:Sh4ObservationBackend::Interpreter;binding.identityDigest=configured->identity.digest;binding.replayDigest=configured->replayDigest;binding.configurationDigest=aicaConfigurationDigest(aica);binding.dspEnabled=aica.dspEnabled;binding.vmuSound=aica.vmuSound;
	auto next=std::make_unique<Session>();next->configuration=*configured;next->writer=std::make_unique<AicaArtifactWriter>(configured->outputPath,binding,configured->targetFrames,configured->artifactMaximum);next->droppedBaseline=aicaObservationDroppedCount();Session* raw=next.get();
	next->subscription=subscribeAicaEvidenceObservations([raw](const AicaObservation& event){
		if(raw->failure||raw->writer->targetReached())return;try{
			if(event.type==AicaObservationType::KeyOn||event.type==AicaObservationType::KeyOff){if(raw->pendingKeys.size()>=64)throw std::runtime_error("AICA key batch exceeds 64 transitions");raw->pendingKeys.push_back(event);return;}
			if(event.type==AicaObservationType::KeyBatchComplete){
				if(raw->writer->getSummary().checkpointRamBytes==0){
					if(event.keyOnMask==0){raw->pendingKeys.clear();return;}
					if(aicaObservationDmaActive())throw std::runtime_error("AICA trigger occurred during G2 DMA");
					raw->writer->writeCheckpoint(aica::sgc::captureResearchCheckpoint());
				}
				for(const auto& key:raw->pendingKeys)raw->writer->write(key);raw->pendingKeys.clear();raw->writer->write(event);return;
			}
			if(raw->writer->getSummary().checkpointRamBytes!=0)raw->writer->write(event);
		}catch(...){raw->failure=std::current_exception();}
	});
	session=std::move(next);NOTICE_LOG(AICA,"Armed typed AICA artifact capture to %s",configured->outputPath.string().c_str());
}

void stopAicaCaptureRuntime(bool clean)
{
	configured.reset();if(session==nullptr)return;auto finishing=std::move(session);const bool dmaActive=aicaObservationDmaActive();if(finishing->subscription)unsubscribeAicaObservations(finishing->subscription);
	if(!clean){finishing->writer->abandon();return;}if(finishing->failure){finishing->writer->abandon();std::rethrow_exception(finishing->failure);}
	try{reauthenticate(finishing->configuration);const auto dropped=aicaObservationDroppedCount()-finishing->droppedBaseline;const auto summary=finishing->writer->finalize(dropped,dmaActive);NOTICE_LOG(AICA,"Finalized typed AICA artifact (%llu samples)",static_cast<unsigned long long>(summary.sampleFrames));}catch(...){finishing->writer->abandon();throw;}
}
void abortAicaCaptureRuntime() noexcept{configured.reset();if(session==nullptr)return;auto abandoning=std::move(session);if(abandoning->subscription)unsubscribeAicaObservations(abandoning->subscription);abandoning->writer->abandon();}
bool aicaCaptureRuntimeActive(){return session!=nullptr;}
}

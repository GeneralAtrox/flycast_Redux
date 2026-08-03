#include "research/cdda_capture_runtime.h"

#include "cfg/option.h"
#include "hw/aica/sgc_if.h"
#include "log/Log.h"
#include "research/aica_observation.h"
#include "research/cdda_artifact.h"
#include "research/cdda_observation.h"
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

struct Configuration
{
	IdentityManifest identity;
	std::filesystem::path identityPath,replayPath,outputPath;
	Sha256Digest replayDigest {};
	std::uint64_t replayMaximum=0,artifactMaximum=0,targetFrames=0;
};
struct Session
{
	Configuration configuration;
	std::unique_ptr<CddaArtifactWriter> writer;
	CddaObservationSubscription cddaSubscription=0;
	AicaObservationSubscription aicaSubscription=0;
	std::uint64_t cddaDroppedBaseline=0,aicaDroppedBaseline=0;
	std::exception_ptr failure;
};
std::unique_ptr<Configuration> configured;
std::unique_ptr<Session> session;

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
	if(identity.emulatorExecutable.size==0)throw FlycastException("CD-DA identity has no executable authority");const auto executable=runningExecutable();std::error_code error;
	if(std::filesystem::file_size(executable,error)!=identity.emulatorExecutable.size||error||!sha256Equal(hashFileExact(executable,identity.emulatorExecutable.size),identity.emulatorExecutable.digest))throw FlycastException("running Flycast executable differs from CD-DA identity");
}
void reauthenticate(const Configuration& configuration)
{
	authenticateExecutable(configuration.identity);
	if(!sha256Equal(hashFileExact(configuration.identityPath,MaxIdentityManifestBytes),configuration.identity.digest)||
			!sha256Equal(hashFileExact(configuration.replayPath,configuration.replayMaximum),configuration.replayDigest))
		throw FlycastException("CD-DA immutable input changed during capture");
}
void unsubscribe(Session& current) noexcept
{
	if(current.aicaSubscription)unsubscribeAicaObservations(current.aicaSubscription);
	if(current.cddaSubscription)unsubscribeCddaObservations(current.cddaSubscription);
	current.aicaSubscription=0;current.cddaSubscription=0;
}

} // namespace

void configureCddaCaptureRuntime()
{
	abortCddaCaptureRuntime();if(config::ResearchCddaRecordPath.get().empty())return;
	if(!config::ResearchAicaRecordPath.get().empty())throw FlycastException("CD-DA and AICA captures cannot own the AICA observation bus together");
	if(config::ResearchIdentityManifestPath.get().empty()||config::ResearchMapleReplayPath.get().empty())
		throw FlycastException("CD-DA capture requires identity and Maple replay paths");
	for(const char* key:{"IdentityManifest","MapleReplay","CddaRecord"})if(!config::isTransient("research",key))
		throw FlycastException("CD-DA capture paths must be transient options");
	if(config::ResearchCddaMaxBytes.get()<static_cast<std::int64_t>(CddaArtifactHeaderSize)||
			config::ResearchCddaSampleFrames.get()<=0||
			config::ResearchCddaSampleFrames.get()>static_cast<std::int64_t>(MaximumCddaSampleFrames))
		throw FlycastException("CD-DA capture bounds are invalid");
	auto next=std::make_unique<Configuration>();next->identityPath=pathFor(config::ResearchIdentityManifestPath.get());
	next->replayPath=pathFor(config::ResearchMapleReplayPath.get());next->outputPath=pathFor(config::ResearchCddaRecordPath.get());
	if(pathsAlias(next->identityPath,next->replayPath)||pathsAlias(next->identityPath,next->outputPath)||
			pathsAlias(next->replayPath,next->outputPath))throw FlycastException("CD-DA capture paths alias");
	next->identity=loadIdentityManifest(next->identityPath);requireSh4EquivalenceIdentityV2(next->identity);
	if(next->identity.mediaKind!="gdi")throw FlycastException("CD-DA v2 capture currently requires identity-bound GDI media");
	authenticateExecutable(next->identity);next->replayMaximum=static_cast<std::uint64_t>(config::ResearchMapleTraceMaxBytes.get());
	const auto replaySummary=validateProductionMapleTraceFile(next->replayPath,next->identity.mapleReplayIdentityDigest,next->replayMaximum);
	if(next->identity.runtimeConfiguration.mapleDmaCheckpoint==0||replaySummary.dmaCount!=next->identity.runtimeConfiguration.mapleDmaCheckpoint)
		throw FlycastException("CD-DA Maple replay does not end at the identity DMA checkpoint");
	next->replayDigest=hashFileExact(next->replayPath,next->replayMaximum);
	next->artifactMaximum=static_cast<std::uint64_t>(config::ResearchCddaMaxBytes.get());
	next->targetFrames=static_cast<std::uint64_t>(config::ResearchCddaSampleFrames.get());
	const auto& aica=next->identity.runtimeConfiguration.aicaConfiguration;
	if(!aica.available)throw FlycastException("CD-DA capture requires identity-bound aica_configuration");
	const bool dynarec=next->identity.runtimeConfiguration.cpuBackend=="dynarec";
	config::DynarecEnabled.override(dynarec);config::ResearchDynarecObservation.override(dynarec);
	config::DSPEnabled.override(aica.dspEnabled);config::VmuSound.override(aica.vmuSound);configured=std::move(next);
}

void startCddaCaptureRuntime()
{
	if(configured==nullptr)return;if(session!=nullptr)throw FlycastException("CD-DA capture is already active");
	const auto& aica=configured->identity.runtimeConfiguration.aicaConfiguration;
	const bool dynarec=configured->identity.runtimeConfiguration.cpuBackend=="dynarec";
	config::DynarecEnabled.override(dynarec);config::ResearchDynarecObservation.override(dynarec);
	config::DSPEnabled.override(aica.dspEnabled);config::VmuSound.override(aica.vmuSound);
	authenticateExecutable(configured->identity);reauthenticate(*configured);
	if(settings.aica.muteAudio)throw FlycastException("CD-DA capture cannot start while audio is muted");
	CddaArtifactBinding binding;binding.backend=dynarec?Sh4ObservationBackend::Dynarec:Sh4ObservationBackend::Interpreter;
	binding.identityDigest=configured->identity.digest;binding.replayDigest=configured->replayDigest;
	binding.configurationDigest=aicaConfigurationDigest(aica);binding.dspEnabled=aica.dspEnabled;
	auto next=std::make_unique<Session>();next->configuration=*configured;
	next->writer=std::make_unique<CddaArtifactWriter>(configured->outputPath,binding,
			configured->targetFrames,configured->artifactMaximum);
	next->writer->writeCheckpoint(aica::sgc::captureResearchCheckpoint());
	next->cddaDroppedBaseline=cddaObservationDroppedCount();next->aicaDroppedBaseline=aicaObservationDroppedCount();
	Session* raw=next.get();
	try
	{
		next->cddaSubscription=subscribeCddaEvidenceObservations([raw](const CddaObservation& event){
			if(raw->failure||raw->writer->targetReached())return;try{raw->writer->write(event);}catch(...){raw->failure=std::current_exception();}});
		next->aicaSubscription=subscribeAicaEvidenceObservations([raw](const AicaObservation& event){
			if(raw->failure||raw->writer->targetReached()||event.type!=AicaObservationType::SampleFrame||
					!raw->writer->knowsSuccessfulAicaGeneration(event.cddaGeneration))return;
			try{raw->writer->write(event);}catch(...){raw->failure=std::current_exception();}});
	}
	catch(...){unsubscribe(*next);next->writer->abandon();throw;}
	session=std::move(next);NOTICE_LOG(AICA,"Armed typed CD-DA v2 capture to %s",configured->outputPath.string().c_str());
}

void stopCddaCaptureRuntime(bool clean)
{
	configured.reset();if(session==nullptr)return;auto finishing=std::move(session);unsubscribe(*finishing);
	if(!clean){finishing->writer->abandon();return;}if(finishing->failure){finishing->writer->abandon();std::rethrow_exception(finishing->failure);}
	try{reauthenticate(finishing->configuration);const auto summary=finishing->writer->finalize(
			cddaObservationDroppedCount()-finishing->cddaDroppedBaseline,
			aicaObservationDroppedCount()-finishing->aicaDroppedBaseline);
		NOTICE_LOG(AICA,"Finalized typed CD-DA v2 artifact (%llu samples, %llu sectors)",
				static_cast<unsigned long long>(summary.sampleFrames),static_cast<unsigned long long>(summary.successfulSectors));}
	catch(...){finishing->writer->abandon();throw;}
}

void abortCddaCaptureRuntime() noexcept
{
	configured.reset();if(session==nullptr)return;auto abandoning=std::move(session);unsubscribe(*abandoning);abandoning->writer->abandon();
}
bool cddaCaptureRuntimeActive(){return session!=nullptr;}

} // namespace research

#include "research/gdrom_capture_runtime.h"

#include "cfg/option.h"
#include "hw/flashrom/nvmem.h"
#include "log/Log.h"
#include "research/gdrom_artifact.h"
#include "research/gdrom_hardware_artifact.h"
#include "research/gdrom_hardware_observation.h"
#include "research/gdrom_observation.h"
#include "research/identity_manifest.h"
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
	std::u8string utf8(value.size(), u8'\0'); std::memcpy(utf8.data(),value.data(),value.size()); return std::filesystem::path(utf8);
#else
	return std::filesystem::u8path(value);
#endif
}

struct Configuration
{
	IdentityManifest identity;
	std::filesystem::path identityPath,replayPath,outputPath;
	Sha256Digest replayDigest{};
	std::uint64_t replayMaximum=0,artifactMaximum=0;
};
struct Session
{
	Configuration configuration;
	std::unique_ptr<GdromArtifactWriter> writer;
	std::unique_ptr<GdromHardwareArtifactWriter> hardwareWriter;
	GdromObservationSubscription subscription=0;
	GdromHardwareObservationSubscription hardwareSubscription=0;
	std::uint64_t droppedBaseline=0;
	std::exception_ptr failure;
};
std::unique_ptr<Configuration> configured;
std::unique_ptr<Session> session;

std::filesystem::path runningExecutable()
{
#ifdef _WIN32
	std::vector<wchar_t> buffer(1024);
	while(buffer.size()<=32768){
		const DWORD length=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
		if(length==0) throw FlycastException("cannot resolve the running Flycast executable");
		if(length<buffer.size()) return std::filesystem::path(std::wstring(buffer.data(),length));
		buffer.resize(buffer.size()*2);
	}
	throw FlycastException("running Flycast executable path is too long");
#elif defined(__APPLE__)
	std::uint32_t size=0; _NSGetExecutablePath(nullptr,&size); std::vector<char> buffer(size);
	if(_NSGetExecutablePath(buffer.data(),&size)!=0) throw FlycastException("cannot resolve the running Flycast executable");
	return std::filesystem::weakly_canonical(buffer.data());
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
	if(identity.emulatorExecutable.size==0)
		throw FlycastException("GD-ROM identity has no executable authority");
	const auto executable=runningExecutable();
	std::error_code error;
	if(std::filesystem::file_size(executable,error)!=identity.emulatorExecutable.size || error
			|| !sha256Equal(hashFileExact(executable,identity.emulatorExecutable.size),
					identity.emulatorExecutable.digest))
		throw FlycastException("running Flycast executable differs from GD-ROM identity");
}

void reauthenticate(const Configuration& value)
{
	authenticateExecutable(value.identity);
	authenticateFirmwareFiles(value.identity);
	if(!sha256Equal(hashFileExact(value.identityPath,MaxIdentityManifestBytes),value.identity.digest)
			|| !sha256Equal(hashFileExact(value.replayPath,value.replayMaximum),value.replayDigest))
		throw FlycastException("GD-ROM immutable input changed during capture");
}

} // namespace

void configureGdromCaptureRuntime()
{
	abortGdromCaptureRuntime();
	if(config::ResearchGdromRecordPath.get().empty()) return;
	if(config::ResearchIdentityManifestPath.get().empty() || config::ResearchMapleReplayPath.get().empty())
		throw FlycastException("GD-ROM capture requires identity and Maple replay paths");
	for(const char* key:{"IdentityManifest","MapleReplay","GdromRecord"})
		if(!config::isTransient("research",key)) throw FlycastException("GD-ROM capture paths must be transient options");
	auto next=std::make_unique<Configuration>();
	next->identityPath=pathFor(config::ResearchIdentityManifestPath.get());
	next->replayPath=pathFor(config::ResearchMapleReplayPath.get());
	next->outputPath=pathFor(config::ResearchGdromRecordPath.get());
	if(pathsAlias(next->identityPath,next->replayPath)||pathsAlias(next->identityPath,next->outputPath)||pathsAlias(next->replayPath,next->outputPath))
		throw FlycastException("GD-ROM capture paths alias");
	next->identity=loadIdentityManifest(next->identityPath);
	requireSh4EquivalenceIdentityV2(next->identity);
	authenticateFirmwareFiles(next->identity);
	authenticateExecutable(next->identity);
	if(next->identity.mediaKind!="gdi" || next->identity.mediaTracks.empty())
		throw FlycastException("GD-ROM capture requires identity-bound GDI tracks");
	const std::uint64_t requiredHeader = next->identity.firmware.mode == FirmwareMode::Real
			? GdromHardwareArtifactHeaderSize : GdromArtifactHeaderSize;
	if (config::ResearchGdromMaxBytes.get()
			< static_cast<std::int64_t>(requiredHeader))
		throw FlycastException("research.GdromMaxBytes is smaller than the selected artifact header");
	next->replayMaximum=static_cast<std::uint64_t>(config::ResearchMapleTraceMaxBytes.get());
	next->replayDigest=hashFileExact(next->replayPath,next->replayMaximum);
	next->artifactMaximum=static_cast<std::uint64_t>(config::ResearchGdromMaxBytes.get());
	const bool dynarec=next->identity.runtimeConfiguration.cpuBackend=="dynarec";
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	config::UseReios.override(next->identity.firmware.mode==FirmwareMode::Hle);
	configured=std::move(next);
}

void startGdromCaptureRuntime()
{
	if(configured==nullptr) return;
	if(session!=nullptr) throw FlycastException("GD-ROM capture is already active");
	const bool dynarec=configured->identity.runtimeConfiguration.cpuBackend=="dynarec";
	config::DynarecEnabled.override(dynarec);
	config::ResearchDynarecObservation.override(dynarec);
	config::UseReios.override(configured->identity.firmware.mode==FirmwareMode::Hle);
	authenticateExecutable(configured->identity);
	reauthenticate(*configured);
	authenticateLoadedDreamcastFirmware(configured->identity,
			config::UseReios.get(), nvmem::getBiosData(), DreamcastBiosBytes);
	if (configured->identity.firmware.mode == FirmwareMode::Real)
		authenticateLoadedDreamcastFlash(configured->identity,
				nvmem::getInitialFlashData(), nvmem::getInitialFlashSize());
	auto next=std::make_unique<Session>(); next->configuration=*configured;
	Session* raw=next.get();
	const auto backend=configured->identity.runtimeConfiguration.cpuBackend=="dynarec"
			?Sh4ObservationBackend::Dynarec:Sh4ObservationBackend::Interpreter;
	if (configured->identity.firmware.mode == FirmwareMode::Real)
	{
		GdromHardwareArtifactBinding binding;
		binding.backend=backend; binding.identityDigest=configured->identity.digest;
		binding.replayDigest=configured->replayDigest;
		binding.biosDigest=configured->identity.firmware.bios.digest;
		binding.flashDigest=configured->identity.firmware.initialFlash.digest;
		next->hardwareWriter=std::make_unique<GdromHardwareArtifactWriter>(
				configured->outputPath,binding,configured->artifactMaximum);
		next->droppedBaseline=gdromHardwareObservationDroppedCount();
		next->hardwareSubscription=subscribeGdromHardwareEvidenceObservations(
				[raw](const GdromHardwareObservation& observation){
			if(raw->failure) return;
			try{ raw->hardwareWriter->write(observation); }
			catch(...){ raw->failure=std::current_exception(); }
		});
	}
	else
	{
		GdromArtifactBinding binding;
		binding.backend=backend; binding.identityDigest=configured->identity.digest;
		binding.replayDigest=configured->replayDigest;
		next->writer=std::make_unique<GdromArtifactWriter>(configured->outputPath,
				binding,configured->artifactMaximum);
		next->droppedBaseline=gdromObservationDroppedCount();
		next->subscription=subscribeGdromEvidenceObservations(
				[raw](const GdromObservation& observation){
			if(raw->failure) return;
			try{ raw->writer->write(observation); }
			catch(...){ raw->failure=std::current_exception(); }
		});
	}
	session=std::move(next);
	NOTICE_LOG(GDROM,"Recording typed GD-ROM artifact to %s",configured->outputPath.string().c_str());
}

void stopGdromCaptureRuntime(bool clean)
{
	configured.reset(); if(session==nullptr) return;
	auto finishing=std::move(session);
	if(finishing->subscription) unsubscribeGdromObservations(finishing->subscription);
	if(finishing->hardwareSubscription)
		unsubscribeGdromHardwareObservations(finishing->hardwareSubscription);
	if(!clean){
		if(finishing->writer) finishing->writer->abandon();
		if(finishing->hardwareWriter) finishing->hardwareWriter->abandon();
		return;
	}
	if(finishing->failure){
		if(finishing->writer) finishing->writer->abandon();
		if(finishing->hardwareWriter) finishing->hardwareWriter->abandon();
		std::rethrow_exception(finishing->failure);
	}
	try{
		reauthenticate(finishing->configuration);
		if(finishing->hardwareWriter){
			const auto dropped=gdromHardwareObservationDroppedCount()-finishing->droppedBaseline;
			const auto summary=finishing->hardwareWriter->finalize(dropped);
			NOTICE_LOG(GDROM,"Finalized typed real-BIOS GD-ROM hardware artifact (%llu events)",static_cast<unsigned long long>(summary.eventCount));
		}else{
			const auto dropped=gdromObservationDroppedCount()-finishing->droppedBaseline;
			const auto summary=finishing->writer->finalize(dropped);
			NOTICE_LOG(GDROM,"Finalized typed GD-ROM artifact (%llu events)",static_cast<unsigned long long>(summary.eventCount));
		}
	}catch(...){
		if(finishing->writer) finishing->writer->abandon();
		if(finishing->hardwareWriter) finishing->hardwareWriter->abandon();
		throw;
	}
}

void abortGdromCaptureRuntime() noexcept
{
	configured.reset(); if(session==nullptr) return; auto abandoning=std::move(session);
	if(abandoning->subscription) unsubscribeGdromObservations(abandoning->subscription);
	if(abandoning->hardwareSubscription)
		unsubscribeGdromHardwareObservations(abandoning->hardwareSubscription);
	if(abandoning->writer) abandoning->writer->abandon();
	if(abandoning->hardwareWriter) abandoning->hardwareWriter->abandon();
}
bool gdromCaptureRuntimeActive(){ return session!=nullptr; }

} // namespace research

#include "research/research_control.h"

#include "cfg/cfg.h"
#include "cfg/option.h"
#include "emulator.h"
#include "json.hpp"
#include "log/Log.h"
#include "research/aica_observation.h"
#include "research/gdrom_observation.h"
#include "research/pvr_draw_observation.h"
#include "research/pvr_presentation_observation.h"
#include "research/pvr_ta_observation.h"
#include "research/sha256.h"
#include "research/sh4_observation.h"
#include "stdclass.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <sddl.h>
#endif

namespace research
{
namespace
{
using json = nlohmann::json;

constexpr std::size_t MaximumRequestBytes = 16 * 1024;
constexpr std::size_t MaximumResponseBytes = 64 * 1024;

json responseBase()
{
	return {{"schema", "flycast-research-control-response"},
			{"schema_version", 1}};
}

bool canonicalPipeLeaf(const std::string& value)
{
	if (value.empty() || value.size() > 96)
		return false;
	for (const unsigned char character : value)
	{
		if (!std::isalnum(character) && character != '-' && character != '_'
				&& character != '.')
			return false;
	}
	return true;
}

bool lowercaseHex(const std::string& value, std::size_t length)
{
	if (value.size() != length)
		return false;
	for (const char character : value)
	{
		if (!((character >= '0' && character <= '9')
				|| (character >= 'a' && character <= 'f')))
			return false;
	}
	return true;
}

Sha256Digest hashFile(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	if (!input.is_open())
		throw std::runtime_error("research control client executable cannot be read");
	Sha256 digest;
	std::array<char, 64 * 1024> buffer {};
	while (input)
	{
		input.read(buffer.data(), buffer.size());
		const auto count = input.gcount();
		if (count > 0)
			digest.update(buffer.data(), static_cast<std::size_t>(count));
	}
	if (!input.eof())
		throw std::runtime_error("research control client executable read failed");
	return digest.finalize();
}

std::filesystem::path canonicalRegularFile(const std::string& text)
{
	if (text.empty())
		throw std::runtime_error("research control client executable is empty");
	std::error_code error;
	const std::filesystem::path absolute = std::filesystem::absolute(
			std::filesystem::u8path(text), error);
	if (error || !absolute.is_absolute())
		throw std::runtime_error("research control client executable is not absolute");
	const auto status = std::filesystem::symlink_status(absolute, error);
	if (error || !std::filesystem::is_regular_file(status)
			|| std::filesystem::is_symlink(status))
		throw std::runtime_error(
				"research control client executable is not a regular non-link file");
	const auto canonical = std::filesystem::canonical(absolute, error);
	if (error)
		throw std::runtime_error("research control client executable cannot be canonicalized");
	return canonical;
}

struct ControlConfiguration
{
	std::string pipeName;
	std::string nonce;
	std::filesystem::path clientExecutable;
	Sha256Digest clientDigest {};
	std::string gameId;
	std::string mediaPath;
	std::string cpuBackend;
};

std::mutex controlMutex;
std::unique_ptr<ControlConfiguration> configured;
std::thread serverThread;
std::atomic<bool> serverRunning {false};
std::atomic<bool> gameLoaded {false};
std::atomic<bool> emulatorRunning {false};
std::atomic<bool> exitRequested {false};
std::atomic<std::uint64_t> lifecycleGeneration {0};
std::function<void()> exitCallback;

void lifecycleEvent(Event event, void *)
{
	switch (event)
	{
	case Event::Start:
		gameLoaded.store(true, std::memory_order_release);
		break;
	case Event::Resume:
		emulatorRunning.store(true, std::memory_order_release);
		break;
	case Event::Pause:
		emulatorRunning.store(false, std::memory_order_release);
		break;
	case Event::Terminate:
		gameLoaded.store(false, std::memory_order_release);
		emulatorRunning.store(false, std::memory_order_release);
		break;
	default:
		break;
	}
	lifecycleGeneration.fetch_add(1, std::memory_order_acq_rel);
}

ResearchControlStatus currentStatus(const ControlConfiguration& configuration)
{
	ResearchControlStatus status;
	status.gameLoaded = gameLoaded.load(std::memory_order_acquire);
	status.running = emulatorRunning.load(std::memory_order_acquire);
	status.paused = status.gameLoaded && !status.running;
	status.exitRequested = exitRequested.load(std::memory_order_acquire);
	status.lifecycleGeneration = lifecycleGeneration.load(std::memory_order_acquire);
	status.gameId = configuration.gameId;
	status.mediaPath = configuration.mediaPath;
	status.cpuBackend = configuration.cpuBackend;
	const auto addTool = [&status](const char *name, const std::string& path) {
		if (!path.empty())
			status.configuredTools.emplace_back(name);
	};
	addTool("maple-record", config::ResearchMapleRecordPath.get());
	addTool("maple-replay", config::ResearchMapleReplayPath.get());
	addTool("memory-ranges", config::ResearchMemoryRangesRecordPath.get());
	addTool("sh4-events", config::ResearchSh4EventsRecordPath.get());
	addTool("sh4-observation", config::ResearchSh4ObservationRecordPath.get());
	addTool("sh4-profile", config::ResearchSh4ProfileRecordPath.get());
	addTool("pvr-ta", config::ResearchPvrTaRecordPath.get());
	addTool("pvr-presentation", config::ResearchPvrPresentationRecordPath.get());
	addTool("pvr-draw", config::ResearchPvrDrawRecordPath.get());
	addTool("gdrom", config::ResearchGdromRecordPath.get());
	addTool("aica", config::ResearchAicaRecordPath.get());
	status.sh4Subscribers = sh4ObservationSubscriberCount();
	status.pvrTaActive = pvrTaObservationBusActive();
	status.pvrTaDropped = pvrTaObservationDroppedCount();
	status.pvrPresentationActive = pvrPresentationObservationBusActive();
	status.pvrPresentationDropped = pvrPresentationObservationDroppedCount();
	status.pvrDrawActive = pvrDrawObservationBusActive();
	status.pvrDrawDropped = pvrDrawObservationDroppedCount();
	status.gdromActive = gdromObservationBusActive();
	status.gdromDropped = gdromObservationDroppedCount();
	status.aicaActive = aicaObservationBusActive();
	status.aicaDropped = aicaObservationDroppedCount();
	return status;
}

#ifdef _WIN32
struct LocalFreeDeleter
{
	void operator()(void *value) const noexcept
	{
		if (value != nullptr)
			LocalFree(value);
	}
};

std::wstring utf16Path(const std::filesystem::path& path)
{
	return path.wstring();
}

std::unique_ptr<void, LocalFreeDeleter> currentUserSecurityDescriptor()
{
	HANDLE rawToken = nullptr;
	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
		throw std::runtime_error("research control cannot query the server token");
	const std::unique_ptr<void, decltype(&CloseHandle)> token(rawToken, CloseHandle);
	DWORD size = 0;
	GetTokenInformation(rawToken, TokenUser, nullptr, 0, &size);
	if (size == 0)
		throw std::runtime_error("research control cannot size the server token user");
	std::vector<std::uint8_t> bytes(size);
	if (!GetTokenInformation(rawToken, TokenUser, bytes.data(), size, &size))
		throw std::runtime_error("research control cannot read the server token user");
	const auto user = reinterpret_cast<const TOKEN_USER *>(bytes.data());
	LPWSTR rawSid = nullptr;
	if (!ConvertSidToStringSidW(user->User.Sid, &rawSid))
		throw std::runtime_error("research control cannot format the server SID");
	const std::unique_ptr<void, LocalFreeDeleter> sid(rawSid);
	const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" +
			std::wstring(static_cast<const wchar_t *>(sid.get())) + L")";
	PSECURITY_DESCRIPTOR descriptor = nullptr;
	if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
			SDDL_REVISION_1, &descriptor, nullptr))
		throw std::runtime_error("research control cannot create its pipe ACL");
	return std::unique_ptr<void, LocalFreeDeleter>(descriptor);
}

bool samePath(const std::filesystem::path& lhs, const std::filesystem::path& rhs)
{
	const std::wstring left = lhs.wstring();
	const std::wstring right = rhs.wstring();
	return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(),
			static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

bool authenticateClient(HANDLE pipe, const ControlConfiguration& configuration)
{
	ULONG clientPid = 0;
	if (!GetNamedPipeClientProcessId(pipe, &clientPid) || clientPid == 0)
		return false;
	HANDLE rawProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, clientPid);
	if (rawProcess == nullptr)
		return false;
	const std::unique_ptr<void, decltype(&CloseHandle)> process(rawProcess, CloseHandle);
	std::array<wchar_t, 32768> pathBuffer {};
	DWORD pathSize = static_cast<DWORD>(pathBuffer.size());
	if (!QueryFullProcessImageNameW(rawProcess, 0, pathBuffer.data(), &pathSize)
			|| pathSize == 0)
		return false;
	std::error_code error;
	const std::filesystem::path clientPath = std::filesystem::canonical(
			std::filesystem::path(std::wstring(pathBuffer.data(), pathSize)), error);
	if (error || !samePath(clientPath, configuration.clientExecutable))
		return false;
	try
	{
		return sha256Equal(hashFile(clientPath), configuration.clientDigest);
	}
	catch (...)
	{
		return false;
	}
}

std::string readRequest(HANDLE pipe)
{
	std::array<char, MaximumRequestBytes + 1> buffer {};
	DWORD bytesRead = 0;
	const BOOL result = ReadFile(pipe, buffer.data(),
			static_cast<DWORD>(MaximumRequestBytes), &bytesRead, nullptr);
	if (!result)
	{
		if (GetLastError() == ERROR_MORE_DATA)
			throw std::runtime_error("request exceeds the protocol limit");
		throw std::runtime_error("request could not be read");
	}
	if (bytesRead == 0)
		throw std::runtime_error("request is empty");
	return std::string(buffer.data(), bytesRead);
}

void serveClient(HANDLE pipe, const ControlConfiguration& configuration)
{
	std::string response;
	if (!authenticateClient(pipe, configuration))
	{
		json rejected = responseBase();
		rejected["ok"] = false;
		rejected["error"] = "client authentication failed";
		response = rejected.dump();
	}
	else
	{
		try
		{
			bool cleanExit = false;
			response = handleResearchControlRequest(readRequest(pipe),
					configuration.nonce, currentStatus(configuration), cleanExit);
			if (cleanExit)
				exitRequested.store(true, std::memory_order_release);
		}
		catch (const std::exception& exception)
		{
			json rejected = responseBase();
			rejected["ok"] = false;
			rejected["error"] = exception.what();
			response = rejected.dump();
		}
	}
	if (response.size() > MaximumResponseBytes)
		response = R"({"schema":"flycast-research-control-response","schema_version":1,"ok":false,"error":"response exceeds the protocol limit"})";
	DWORD written = 0;
	WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &written, nullptr);
	FlushFileBuffers(pipe);
}

void runServer(ControlConfiguration configuration)
{
	try
	{
		const std::wstring pipePath = L"\\\\.\\pipe\\" +
				std::wstring(configuration.pipeName.begin(), configuration.pipeName.end());
		auto descriptor = currentUserSecurityDescriptor();
		SECURITY_ATTRIBUTES attributes {};
		attributes.nLength = sizeof(attributes);
		attributes.lpSecurityDescriptor = descriptor.get();
		while (serverRunning.load(std::memory_order_acquire))
		{
			HANDLE pipe = CreateNamedPipeW(pipePath.c_str(), PIPE_ACCESS_DUPLEX,
					PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT
							| PIPE_REJECT_REMOTE_CLIENTS,
					1, static_cast<DWORD>(MaximumResponseBytes),
					static_cast<DWORD>(MaximumRequestBytes), 0, &attributes);
			if (pipe == INVALID_HANDLE_VALUE)
				throw std::runtime_error("research control cannot create its named pipe");
			const BOOL connected = ConnectNamedPipe(pipe, nullptr);
			const DWORD connectError = connected ? ERROR_SUCCESS : GetLastError();
			if (connected || connectError == ERROR_PIPE_CONNECTED)
			{
				if (serverRunning.load(std::memory_order_acquire))
					serveClient(pipe, configuration);
				DisconnectNamedPipe(pipe);
			}
			CloseHandle(pipe);
		}
	}
	catch (const std::exception& exception)
	{
		ERROR_LOG(COMMON, "Research control server failed: %s", exception.what());
	}
	serverRunning.store(false, std::memory_order_release);
}

void wakeServer(const std::string& pipeName) noexcept
{
	try
	{
		const std::wstring pipePath = L"\\\\.\\pipe\\" +
				std::wstring(pipeName.begin(), pipeName.end());
		HANDLE pipe = CreateFileW(pipePath.c_str(), GENERIC_READ | GENERIC_WRITE,
				0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (pipe != INVALID_HANDLE_VALUE)
			CloseHandle(pipe);
	}
	catch (...)
	{
	}
}
#endif

} // namespace

void configureResearchControl()
{
	stopResearchControl();
	const std::array<std::string, 4> values {
		config::ResearchControlPipeName.get(), config::ResearchControlNonce.get(),
		config::ResearchControlClientExecutable.get(),
		config::ResearchControlClientSha256.get()};
	const bool any = std::any_of(values.begin(), values.end(),
			[](const std::string& value) { return !value.empty(); });
	const bool all = std::all_of(values.begin(), values.end(),
			[](const std::string& value) { return !value.empty(); });
	if (!any)
		return;
	if (!all)
		throw FlycastException("research control requires pipe, nonce, client path and hash");
	for (const char *key : {"ControlPipe", "ControlNonce", "ControlClientExecutable",
			"ControlClientSha256"})
	{
		if (!config::isTransient("research", key))
			throw FlycastException("research control configuration must be transient");
	}
	if (!canonicalPipeLeaf(values[0]) || !lowercaseHex(values[1], 64)
			|| !lowercaseHex(values[3], 64))
		throw FlycastException("research control pipe, nonce or client hash is invalid");
#ifndef _WIN32
	throw FlycastException("research control is available only on desktop Windows");
#else
	auto next = std::make_unique<ControlConfiguration>();
	next->pipeName = values[0];
	next->nonce = values[1];
	next->clientExecutable = canonicalRegularFile(values[2]);
	if (!sha256FromHex(values[3], next->clientDigest)
			|| !sha256Equal(hashFile(next->clientExecutable), next->clientDigest))
		throw FlycastException("research control client executable hash differs");
	next->gameId = settings.content.gameId;
	next->mediaPath = settings.content.path;
	next->cpuBackend = config::DynarecEnabled.get() ? "dynarec" : "interpreter";
	const std::lock_guard<std::mutex> lock(controlMutex);
	configured = std::move(next);
#endif
}

void startResearchControl(std::function<void()> cleanExitCallback)
{
	stopResearchControl();
	std::unique_ptr<ControlConfiguration> launch;
	{
		const std::lock_guard<std::mutex> lock(controlMutex);
		if (configured == nullptr)
			return;
		launch = std::make_unique<ControlConfiguration>(*configured);
		exitCallback = std::move(cleanExitCallback);
	}
	exitRequested.store(false, std::memory_order_release);
	gameLoaded.store(true, std::memory_order_release);
	emulatorRunning.store(false, std::memory_order_release);
	lifecycleGeneration.store(1, std::memory_order_release);
	EventManager::listen(Event::Start, lifecycleEvent);
	EventManager::listen(Event::Resume, lifecycleEvent);
	EventManager::listen(Event::Pause, lifecycleEvent);
	EventManager::listen(Event::Terminate, lifecycleEvent);
#ifdef _WIN32
	serverRunning.store(true, std::memory_order_release);
	serverThread = std::thread(runServer, *launch);
#endif
}

void stopResearchControl() noexcept
{
	try
	{
		EventManager::unlisten(Event::Start, lifecycleEvent);
		EventManager::unlisten(Event::Resume, lifecycleEvent);
		EventManager::unlisten(Event::Pause, lifecycleEvent);
		EventManager::unlisten(Event::Terminate, lifecycleEvent);
		serverRunning.store(false, std::memory_order_release);
#ifdef _WIN32
		std::string pipeName;
		{
			const std::lock_guard<std::mutex> lock(controlMutex);
			if (configured != nullptr)
				pipeName = configured->pipeName;
		}
		if (!pipeName.empty())
			wakeServer(pipeName);
		if (serverThread.joinable())
			serverThread.join();
#endif
		const std::lock_guard<std::mutex> lock(controlMutex);
		exitCallback = {};
	}
	catch (...)
	{
	}
}

void pollResearchControl() noexcept
{
	if (!exitRequested.exchange(false, std::memory_order_acq_rel))
		return;
	try
	{
		std::function<void()> callback;
		{
			const std::lock_guard<std::mutex> lock(controlMutex);
			callback = exitCallback;
		}
		if (callback)
			callback();
	}
	catch (...)
	{
	}
}

} // namespace research

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace research
{

struct ResearchControlStatus
{
	bool gameLoaded = false;
	bool running = false;
	bool paused = false;
	bool exitRequested = false;
	std::uint64_t lifecycleGeneration = 0;
	std::string gameId;
	std::string mediaPath;
	std::string cpuBackend;
	std::vector<std::string> configuredTools;
	std::size_t sh4Subscribers = 0;
	bool pvrTaActive = false;
	std::uint64_t pvrTaDropped = 0;
	bool pvrPresentationActive = false;
	std::uint64_t pvrPresentationDropped = 0;
	bool pvrDrawActive = false;
	std::uint64_t pvrDrawDropped = 0;
	bool gdromActive = false;
	std::uint64_t gdromDropped = 0;
	bool aicaActive = false;
	std::uint64_t aicaDropped = 0;
};

// Parses one bounded authenticated request. This platform-independent boundary
// is also used by the protocol tests; the Windows server authenticates the pipe
// peer before calling it.
std::string handleResearchControlRequest(const std::string& request,
		const std::string& expectedNonce, const ResearchControlStatus& status,
		bool& requestCleanExit);

#ifdef LIBRETRO
inline void configureResearchControl() {}
inline void startResearchControl(std::function<void()>) {}
inline void stopResearchControl() noexcept {}
inline void pollResearchControl() noexcept {}
#else
void configureResearchControl();
void startResearchControl(std::function<void()> cleanExitCallback);
void stopResearchControl() noexcept;
void pollResearchControl() noexcept;
#endif

} // namespace research

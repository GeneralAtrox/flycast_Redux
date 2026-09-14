#include "research/research_control.h"

#include "cfg/option.h"
#include "log/Log.h"
#include "research/control/control_actions.h"
#include "research/control/control_server.h"
#include "research/control/control_ui_tasks.h"
#include "types.h"

#include <memory>

namespace research
{
namespace
{

control::UiTaskQueue uiTasks;
std::unique_ptr<control::ControlServer> server;
bool configured = false;

} // namespace

void configureResearchControl()
{
	configured = false;
	const std::int64_t port = config::ResearchControlPort.get();
	if (port == 0)
		return;
	if (port < 1 || port > 65535)
		throw FlycastException("research.ControlPort must be in [1, 65535] (0 disables)");
	configured = true;
}

void startResearchControl(std::function<void()>)
{
	stopResearchControl();
	if (!configured)
		return;
	uiTasks.reset();
	control::installLifecycleListeners();
	try
	{
		server = std::make_unique<control::ControlServer>(
				static_cast<std::uint16_t>(config::ResearchControlPort.get()),
				config::ResearchControlToken.get(), control::makeEmulatorActions(uiTasks));
	}
	catch (...)
	{
		control::removeLifecycleListeners();
		throw;
	}
	NOTICE_LOG(COMMON, "Research control endpoint listening on 127.0.0.1:%u%s",
			server->port(), config::ResearchControlToken.get().empty() ? " (no token)" : "");
}

void stopResearchControl() noexcept
{
	if (server == nullptr)
		return;
	uiTasks.shutdown();
	server.reset();
	control::removeLifecycleListeners();
}

void pollResearchControl() noexcept
{
	uiTasks.poll();
}

} // namespace research

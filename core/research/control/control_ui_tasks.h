#pragma once

// Runs work on the UI/frontend thread on behalf of the control server thread.
// The UI thread drains the queue once per rendered frame through
// research::pollResearchControl(); callers block until their task ran or the
// timeout elapsed.

#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace research::control
{

class UiTaskQueue
{
public:
	template<typename F>
	auto run(F&& function, std::chrono::milliseconds timeout) -> std::invoke_result_t<F>
	{
		using Result = std::invoke_result_t<F>;
		auto task = std::make_shared<std::packaged_task<Result()>>(std::forward<F>(function));
		std::future<Result> future = task->get_future();
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (!accepting)
				throw std::runtime_error("the control endpoint is shutting down");
			pending.push_back([task] { (*task)(); });
		}
		if (future.wait_for(timeout) != std::future_status::ready)
			throw std::runtime_error("the UI thread did not service the request in time");
		return future.get();
	}

	// UI thread only.
	void poll() noexcept
	{
		std::vector<std::function<void()>> batch;
		{
			std::lock_guard<std::mutex> lock(mutex);
			batch.swap(pending);
		}
		for (std::function<void()>& task : batch)
		{
			try
			{
				task();
			}
			catch (...)
			{
				// packaged_task captures exceptions for the waiter; nothing to do.
			}
		}
	}

	void shutdown() noexcept
	{
		std::lock_guard<std::mutex> lock(mutex);
		accepting = false;
		pending.clear();
	}

	void reset() noexcept
	{
		std::lock_guard<std::mutex> lock(mutex);
		accepting = true;
		pending.clear();
	}

private:
	std::mutex mutex;
	std::vector<std::function<void()>> pending;
	bool accepting = true;
};

} // namespace research::control

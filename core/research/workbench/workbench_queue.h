#pragma once

// Bounded hand-off queue between the emulator threads that publish
// observations and the single writer thread that turns them into rows.
// Producers never block: a full queue drops the newest event and counts it.

#include "research/aica_observation.h"
#include "research/cdda_observation.h"
#include "research/gdrom_hardware_observation.h"
#include "research/gdrom_observation.h"
#include "research/maple_observation.h"
#include "research/pvr_draw_observation.h"
#include "research/pvr_presentation_observation.h"
#include "research/pvr_ta_observation.h"
#include "research/sh4_observation.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <variant>
#include <vector>

namespace research::workbench
{

using WorkbenchEvent = std::variant<Sh4Observation, MapleObservation, PvrTaObservation,
		PvrDrawObservation, PvrPresentationObservation, GdromObservation,
		GdromHardwareObservation, AicaObservation, CddaObservation>;

class EventQueue
{
public:
	explicit EventQueue(std::size_t capacity)
		: capacity(capacity)
	{
	}

	// Returns false (and counts a drop) when the queue is full or closed.
	bool push(WorkbenchEvent&& event) noexcept
	{
		try
		{
			std::lock_guard<std::mutex> lock(mutex);
			if (closedFlag || pending.size() >= capacity)
			{
				droppedCount.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			pending.push_back(std::move(event));
		}
		catch (...)
		{
			droppedCount.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		available.notify_one();
		return true;
	}

	// Moves up to `maximum` events into `out`, waiting up to `wait` for the
	// first one. Returns the number moved. Zero with closed() true means the
	// producer side is finished and the queue is empty.
	std::size_t drain(std::vector<WorkbenchEvent>& out, std::size_t maximum,
			std::chrono::milliseconds wait)
	{
		std::unique_lock<std::mutex> lock(mutex);
		available.wait_for(lock, wait, [this] { return !pending.empty() || closedFlag; });
		std::size_t moved = 0;
		while (moved < maximum && !pending.empty())
		{
			out.push_back(std::move(pending.front()));
			pending.pop_front();
			++moved;
		}
		return moved;
	}

	void close() noexcept
	{
		{
			std::lock_guard<std::mutex> lock(mutex);
			closedFlag = true;
		}
		available.notify_all();
	}

	bool closed() const noexcept
	{
		std::lock_guard<std::mutex> lock(mutex);
		return closedFlag;
	}

	std::size_t size() const noexcept
	{
		std::lock_guard<std::mutex> lock(mutex);
		return pending.size();
	}

	std::uint64_t dropped() const noexcept
	{
		return droppedCount.load(std::memory_order_relaxed);
	}

private:
	const std::size_t capacity;
	mutable std::mutex mutex;
	std::condition_variable available;
	std::deque<WorkbenchEvent> pending;
	std::atomic<std::uint64_t> droppedCount {0};
	bool closedFlag = false;
};

} // namespace research::workbench

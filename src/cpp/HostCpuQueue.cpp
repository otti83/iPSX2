// HostCpuQueue.cpp — deferred work queue for the emulation (CPU) thread
// SPDX-License-Identifier: GPL-3.0+
// See HostCpuQueue.h for why this exists.

#include "HostCpuQueue.h"

#include "pcsx2/Host.h"
#include "pcsx2/VMManager.h"
#include "common/Console.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace
{
	// One queued closure. `claimed` is the whole concurrency story: the drain and a blocking
	// caller both try to claim it, and exactly one wins, so a task can never run twice and can
	// never be stranded if the CPU thread stops pumping before it is reached.
	struct Task
	{
		std::function<void()> fn;
		std::atomic<bool> claimed{false};
		std::atomic<bool> done{false};
	};

	std::mutex s_mutex;
	std::condition_variable s_done_cv;
	std::deque<std::shared_ptr<Task>> s_queue;

	std::atomic<bool> s_have_cpu_thread{false};
	std::thread::id s_cpu_thread_id;

	// Runs the task unless somebody else already claimed it. Returns true if we ran it.
	bool ClaimAndRun(const std::shared_ptr<Task>& task)
	{
		if (task->claimed.exchange(true, std::memory_order_acq_rel))
			return false;

		if (task->fn)
			task->fn();

		{
			std::lock_guard<std::mutex> lock(s_mutex);
			task->done.store(true, std::memory_order_release);
		}
		s_done_cv.notify_all();
		return true;
	}

	// Is there a CPU thread that will actually reach a drain? PumpMessagesOnCPUThread only runs
	// while the VM is alive, so with no VM there is nobody to hand work to and the caller must
	// run it itself rather than wait forever.
	bool CPUThreadWillDrain()
	{
		if (!s_have_cpu_thread.load(std::memory_order_acquire))
			return false;

		switch (VMManager::GetState())
		{
			case VMState::Running:
			case VMState::Paused:
			case VMState::Resetting:
				return true;
			default:
				return false;
		}
	}
} // namespace

void iPSX2::DrainCPUThreadTasks()
{
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		s_cpu_thread_id = std::this_thread::get_id();
	}
	s_have_cpu_thread.store(true, std::memory_order_release);

	// Snapshot then run outside the lock: a task is allowed to call RunOnCPUThread itself, and
	// holding s_mutex across arbitrary emulator code would deadlock on the re-entrant enqueue.
	for (;;)
	{
		std::shared_ptr<Task> task;
		{
			std::lock_guard<std::mutex> lock(s_mutex);
			if (s_queue.empty())
				break;
			task = std::move(s_queue.front());
			s_queue.pop_front();
		}
		ClaimAndRun(task);
	}
}

bool iPSX2::IsCPUThread()
{
	if (!s_have_cpu_thread.load(std::memory_order_acquire))
		return false;

	std::lock_guard<std::mutex> lock(s_mutex);
	return std::this_thread::get_id() == s_cpu_thread_id;
}

void iPSX2::ResetCPUThreadQueue()
{
	std::deque<std::shared_ptr<Task>> dropped;
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		dropped.swap(s_queue);
		s_have_cpu_thread.store(false, std::memory_order_release);
		s_cpu_thread_id = std::thread::id();
	}

	// Release any blocked caller rather than leaving it on the condition variable. Their work is
	// deliberately not run: the VM it was queued against is gone.
	for (const auto& task : dropped)
	{
		if (!task->claimed.exchange(true, std::memory_order_acq_rel))
		{
			std::lock_guard<std::mutex> lock(s_mutex);
			task->done.store(true, std::memory_order_release);
		}
	}
	if (!dropped.empty())
	{
		Console.WriteLn("[F1] Dropped %zu queued CPU-thread task(s) at VM teardown.", dropped.size());
		s_done_cv.notify_all();
	}
}

void Host::RunOnCPUThread(std::function<void()> function, bool block /* = false */)
{
	if (!function)
		return;

	// Already on the CPU thread, or there is no CPU thread to defer to: just run it. The second
	// case covers settings work done from the menus with no VM booted, which is the common one.
	if (iPSX2::IsCPUThread() || !CPUThreadWillDrain())
	{
		function();
		return;
	}

	auto task = std::make_shared<Task>();
	task->fn = std::move(function);

	{
		std::lock_guard<std::mutex> lock(s_mutex);
		s_queue.push_back(task);
	}

	if (!block)
		return;

	std::unique_lock<std::mutex> lock(s_mutex);
	// Bounded wait so a VM that dies before reaching the drain cannot hang the UI thread. On
	// timeout we claim the task back and run it here — worse thread affinity than intended, but
	// strictly better than the previous behaviour of dropping it silently.
	if (!s_done_cv.wait_for(lock, std::chrono::seconds(5),
			[&task] { return task->done.load(std::memory_order_acquire); }))
	{
		lock.unlock();
		if (ClaimAndRun(task))
			Console.Warning("[F1] CPU thread did not drain within 5s; ran a blocking task inline.");
	}
}

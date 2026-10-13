#pragma once
// SavestateParallel.h
// Tiny helper to run independent work items on several host threads.
// Used for the big memory copies / (de)compression of savestates, which are
// memory-bandwidth bound and therefore scale well over a few threads.

#include <thread>
#include <atomic>
#include <vector>
#include <exception>
#include <mutex>
#include <algorithm>
#include <cstddef>

namespace SavestateParallel
{
	inline unsigned GetWorkerCount(unsigned maxWorkers = 8)
	{
		unsigned hw = std::thread::hardware_concurrency();
		if (hw == 0)
			hw = 2;
		return std::max(1u, std::min(hw, maxWorkers));
	}

	// Calls fn(i) for every i in [0, count). Items are handed out one by one,
	// so uneven item costs are balanced automatically. The first exception
	// thrown by any item is rethrown on the calling thread after all workers
	// have stopped.
	template <typename Fn>
	void For(size_t count, Fn&& fn, unsigned maxWorkers = 8)
	{
		if (count == 0)
			return;
		unsigned workers = (unsigned)std::min<size_t>(GetWorkerCount(maxWorkers), count);
		if (workers <= 1)
		{
			for (size_t i = 0; i < count; i++)
				fn(i);
			return;
		}
		std::atomic<size_t> next{ 0 };
		std::exception_ptr firstError;
		std::mutex errorMutex;
		auto body = [&]()
		{
			try
			{
				while (true)
				{
					size_t i = next.fetch_add(1, std::memory_order_relaxed);
					if (i >= count)
						break;
					fn(i);
				}
			}
			catch (...)
			{
				std::lock_guard lock(errorMutex);
				if (!firstError)
					firstError = std::current_exception();
				next.store(count); // stop the others
			}
		};
		std::vector<std::thread> threads;
		threads.reserve(workers - 1);
		for (unsigned i = 1; i < workers; i++)
			threads.emplace_back(body);
		body(); // the calling thread works too
		for (auto& t : threads)
			t.join();
		if (firstError)
			std::rethrow_exception(firstError);
	}
}

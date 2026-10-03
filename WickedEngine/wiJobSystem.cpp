#include "wiJobSystem.h"
#include "wiSpinLock.h"
#include "wiBackLog.h"
#include "wiContainers.h"
#include "wiPlatform.h"

#include <thread>
#include <condition_variable>
#include <string>
#include <algorithm>
#ifdef GGREDUCED
#include <chrono>

void (*g_pfnWickedJobStall)(const WickedJobStallInfo& info) = nullptr;
double g_dWickedJobStallMilliseconds = 20.0;
#endif

namespace wiJobSystem
{
	struct Job
	{
		context* ctx;
		std::function<void(wiJobArgs)> task;
		uint32_t groupID;
		uint32_t groupJobOffset;
		uint32_t groupJobEnd;
		uint32_t sharedmemory_size;
#ifdef GGREDUCED
		double queued = 0; // GG: when it was queued (JobNow), for the job stall report
#endif
	};

	uint32_t numThreads = 0;
	wiContainers::ThreadSafeRingBuffer<Job, 256> jobQueue;
	std::condition_variable wakeCondition;
	std::mutex wakeMutex;

#ifdef GGREDUCED
	static inline double JobNow()
	{
		return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
	}
	thread_local const context* waitingFor = nullptr; // GG: the context this thread is in Wait for, if any
#endif

	// This function executes the next item from the job queue. Returns true if successful, false if there was no job available
	inline bool work()
	{
		Job job;
		if (jobQueue.pop_front(job))
		{
#ifdef GGREDUCED
			const double start = g_pfnWickedJobStall ? JobNow() : 0;
			const uint32_t coreStart = g_pfnWickedJobStall ? GetCurrentProcessorNumber() : 0;
#endif
			wiJobArgs args;
			args.groupID = job.groupID;
			if (job.sharedmemory_size > 0)
			{
				args.sharedmemory = alloca(job.sharedmemory_size);
			}
			else
			{
				args.sharedmemory = nullptr;
			}

			for (uint32_t i = job.groupJobOffset; i < job.groupJobEnd; ++i)
			{
				args.jobIndex = i;
				args.groupIndex = i - job.groupJobOffset;
				args.isFirstJobInGroup = (i == job.groupJobOffset);
				args.isLastJobInGroup = (i == job.groupJobEnd - 1);
				job.task(args);
			}

#ifdef GGREDUCED
			if (g_pfnWickedJobStall)
			{
				const double end = JobNow();
				if (end - start >= g_dWickedJobStallMilliseconds || start - job.queued >= g_dWickedJobStallMilliseconds)
				{
					WickedJobStallInfo info;
					info.name = job.task.target_type().name();
					info.context = job.ctx;
					info.waitingFor = waitingFor;
					info.queuedMilliseconds = start - job.queued;
					info.runMilliseconds = end - start;
					info.jobs = job.groupJobEnd - job.groupJobOffset;
					info.coreStart = coreStart;
					info.coreEnd = GetCurrentProcessorNumber();
					g_pfnWickedJobStall(info);
				}
			}
#endif
			job.ctx->counter.fetch_sub(1);
			return true;
		}
		return false;
	}

#ifdef GGREDUCED
	static int affinityMode = 1;
	void SetAffinityMode(int mode)
	{
		affinityMode = mode;
	}
#endif

	void Initialize()
	{
		// Retrieve the number of hardware threads in this system:
		auto numCores = std::thread::hardware_concurrency();

		// Calculate the actual number of worker threads we want (-1 main thread):
		numThreads = std::max(1u, numCores - 1);

		for (uint32_t threadID = 0; threadID < numThreads; ++threadID)
		{
			std::thread worker([] {

				while (true)
				{
					if (!work())
					{
						// no job, put thread to sleep
						std::unique_lock<std::mutex> lock(wakeMutex);
						wakeCondition.wait(lock);
					}
				}

			});

#ifdef _WIN32
			// Do Windows-specific thread setup:
			HANDLE handle = (HANDLE)worker.native_handle();

#ifdef GGREDUCED
			if (affinityMode == 1)
			{
				// GG: the core as a preference, so the scheduler can move the worker
				SetThreadIdealProcessor(handle, threadID);
			}
			else if (affinityMode == 0)
#endif
			{
			// Put each thread on to dedicated core:
			DWORD_PTR affinityMask = 1ull << threadID;
			DWORD_PTR affinity_result = SetThreadAffinityMask(handle, affinityMask);
			assert(affinity_result > 0);
			}

			//// Increase thread priority:
			//BOOL priority_result = SetThreadPriority(handle, THREAD_PRIORITY_HIGHEST);
			//assert(priority_result != 0);

			// Name the thread:
			std::wstring wthreadname =  L"wiJobSystem_" + std::to_wstring(threadID);
			HRESULT hr = SetThreadDescription(handle, wthreadname.c_str());
			assert(SUCCEEDED(hr));
#endif // _WIN32

			worker.detach();
		}

#ifdef GGREDUCED
		wiBackLog::post(("wiJobSystem Initialized with [" + std::to_string(numCores) + " cores] [" + std::to_string(numThreads) + " threads] [affinity mode " + std::to_string(affinityMode) + "]").c_str());
#else
		wiBackLog::post(("wiJobSystem Initialized with [" + std::to_string(numCores) + " cores] [" + std::to_string(numThreads) + " threads]").c_str());
#endif
	}

	uint32_t GetThreadCount()
	{
		return numThreads;
	}

	void Execute(context& ctx, const std::function<void(wiJobArgs)>& task)
	{
		// Context state is updated:
		ctx.counter.fetch_add(1);

		Job job;
		job.ctx = &ctx;
		job.task = task;
		job.groupID = 0;
		job.groupJobOffset = 0;
		job.groupJobEnd = 1;
		job.sharedmemory_size = 0;
#ifdef GGREDUCED
		job.queued = g_pfnWickedJobStall ? JobNow() : 0;

		// Try to push a new job until it is pushed successfully (GG: moved in, the queue keeps it as it is until then):
		while (!jobQueue.push_back(std::move(job))) { wakeCondition.notify_all(); work(); }
#else
		// Try to push a new job until it is pushed successfully:
		while (!jobQueue.push_back(job)) { wakeCondition.notify_all(); work(); }
#endif

		// Wake any one thread that might be sleeping:
		wakeCondition.notify_one();
	}

	void Dispatch(context& ctx, uint32_t jobCount, uint32_t groupSize, const std::function<void(wiJobArgs)>& task, size_t sharedmemory_size)
	{
		if (jobCount == 0 || groupSize == 0)
		{
			return;
		}

		const uint32_t groupCount = DispatchGroupCount(jobCount, groupSize);

		// Context state is updated:
		ctx.counter.fetch_add(groupCount);

		Job job;
		job.ctx = &ctx;
		job.task = task;
		job.sharedmemory_size = (uint32_t)sharedmemory_size;
#ifdef GGREDUCED
		job.queued = g_pfnWickedJobStall ? JobNow() : 0;
#endif

		for (uint32_t groupID = 0; groupID < groupCount; ++groupID)
		{
			// For each group, generate one real job:
			job.groupID = groupID;
			job.groupJobOffset = groupID * groupSize;
			job.groupJobEnd = std::min(job.groupJobOffset + groupSize, jobCount);

#ifdef GGREDUCED
			// GG: each group's copy made here, before the queue's lock, and moved in
			Job groupJob = job;
			while (!jobQueue.push_back(std::move(groupJob))) { wakeCondition.notify_all(); work(); }
#else
			// Try to push a new job until it is pushed successfully:
			while (!jobQueue.push_back(job)) { wakeCondition.notify_all(); work(); }
#endif
		}

#ifdef GGREDUCED
		// GG: wake as many sleeping workers as there are groups, not all of them for one group (each woken worker that finds
		// nothing goes back to sleep, after taking the queue's lock)
		const uint32_t wake = std::min(groupCount, numThreads);
		if (wake >= numThreads)
		{
			wakeCondition.notify_all();
		}
		else
		{
			for (uint32_t i = 0; i < wake; ++i)
			{
				wakeCondition.notify_one();
			}
		}
#else
		// Wake any threads that might be sleeping:
		wakeCondition.notify_all();
#endif
	}

	uint32_t DispatchGroupCount(uint32_t jobCount, uint32_t groupSize)
	{
		// Calculate the amount of job groups to dispatch (overestimate, or "ceil"):
		return (jobCount + groupSize - 1) / groupSize;
	}

	bool IsBusy(const context& ctx)
	{
		// Whenever the context label is greater than zero, it means that there is still work that needs to be done
		return ctx.counter.load() > 0;
	}

	void Wait(const context& ctx)
	{
		// Wake any threads that might be sleeping:
		wakeCondition.notify_all();

		// Waiting will also put the current thread to good use by working on an other job if it can:
#ifdef GGREDUCED
		const context* outer = waitingFor;
		waitingFor = &ctx;
		while (IsBusy(ctx)) { work(); }
		waitingFor = outer;
#else
		while (IsBusy(ctx)) { work(); }
#endif
	}

	void WaitSleep(const context& ctx, uint32_t time)
	{
		// Wake any threads that might be sleeping:
		wakeCondition.notify_all();

		//PE: Give time for free threads to take over jobs, before jumping in.
		Sleep(time);

		// Waiting will also put the current thread to good use by working on an other job if it can:
		const context* outer = waitingFor;
		waitingFor = &ctx;
		while (IsBusy(ctx)) { work(); }
		waitingFor = outer;
	}

}

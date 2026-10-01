#pragma once

#include <functional>
#include <atomic>

struct wiJobArgs
{
	uint32_t jobIndex;		// job index relative to dispatch (like SV_DispatchThreadID in HLSL)
	uint32_t groupID;		// group index relative to dispatch (like SV_GroupID in HLSL)
	uint32_t groupIndex;	// job index relative to group (like SV_GroupIndex in HLSL)
	bool isFirstJobInGroup;	// is the current job the first one in the group?
	bool isLastJobInGroup;	// is the current job the last one in the group?
	void* sharedmemory;		// stack memory shared within the current group (jobs within a group execute serially)
};

namespace wiJobSystem
{
	void Initialize();

#ifdef GGREDUCED
	// GG: how the workers are tied to cores, set before Initialize: 0 each pinned to its own core (as stock Wicked), 1 that
	// core as a preference only (SetThreadIdealProcessor, the default), 2 not tied. A pinned worker cannot move when its core
	// is slow to run it (an efficiency or low power core on a hybrid CPU), and a job it holds waits with it, for seconds
	void SetAffinityMode(int mode);
#endif

	uint32_t GetThreadCount();

	// Defines a state of execution, can be waited on
	struct context
	{
		std::atomic<uint32_t> counter{ 0 };
	};

	// Add a task to execute asynchronously. Any idle thread will execute this.
	void Execute(context& ctx, const std::function<void(wiJobArgs)>& task);

	// Divide a task onto multiple jobs and execute in parallel.
	//	jobCount	: how many jobs to generate for this task.
	//	groupSize	: how many jobs to execute per thread. Jobs inside a group execute serially. It might be worth to increase for small jobs
	//	task		: receives a wiJobArgs as parameter
	void Dispatch(context& ctx, uint32_t jobCount, uint32_t groupSize, const std::function<void(wiJobArgs)>& task, size_t sharedmemory_size = 0);

	// Returns the amount of job groups that will be created for a set number of jobs and group size
	uint32_t DispatchGroupCount(uint32_t jobCount, uint32_t groupSize);

	// Check if any threads are working currently or not
	bool IsBusy(const context& ctx);

	// Wait until all threads become idle
	void Wait(const context& ctx);
#ifdef GGREDUCED
	void WaitSleep(const context& ctx,uint32_t time);
#endif
}

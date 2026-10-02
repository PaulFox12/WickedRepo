#include "wiProfiler.h"
#include "wiGraphicsDevice.h"
#include "wiRenderer.h"
#include "wiFont.h"
#include "wiImage.h"
#include "wiTimer.h"
#include "wiTextureHelper.h"
#include "wiHelper.h"

#include <string>
#include <unordered_map>
#include <stack>
#include <mutex>
#include <atomic>
#include <sstream>
#include <chrono>

using namespace wiGraphics;

#ifdef GGREDUCED
void (*g_pfnWickedProfilerQueries)(uint32_t queries) = nullptr;
void (*g_pfnWickedProfilerLockWait)(double dMilliseconds) = nullptr;
void (*g_pfnWickedProfilerLockHold)(double dMilliseconds) = nullptr;
void (*g_pfnWickedFramePhase)(const char* name, const char* parents, double dMilliseconds) = nullptr;
#endif

namespace wiProfiler
{
	bool ENABLED = false;
	bool initialized = false;
#ifdef GGREDUCED
	int iDrawCalls = 0, iOldDrawCalls = 0;
	int iDrawCallsShadows = 0, iOldDrawCallsShadows = 0;
	int iDrawCallsShadowsCube = 0, iOldDrawCallsShadowsCube = 0;
	int iDrawCallsTransparent = 0, iOldDrawCallsTransparent = 0;
	int iPolygonsDrawn = 0, iOldPolygonsDrawn = 0;
	int iPolygonsDrawnShadows = 0, iOldPolygonsDrawnShadows = 0;
	int iPolygonsDrawnTransparent = 0, iOldPolygonsDrawnTransparent = 0;
	int iFrustumculled = 0;
#endif
	//std::mutex lock;
	std::recursive_mutex lock;
#ifdef GGREDUCED
	bool GPU_ENABLED = true; // GG: GPU ranges and queries (SetGPUEnabled), taken up at the next BeginFrame
	bool gpu_this_frame = true;

	// GG: the ranges' lock, its waits and holds reported for the stall probes (the hold after the release, on the holder's
	// thread, so a probe can take its stack)
	std::chrono::high_resolution_clock::time_point lockHeldSince;
	int lockDepth = 0;
	void LockRanges()
	{
		if (!g_pfnWickedProfilerLockWait)
		{
			lock.lock();
		}
		else
		{
			auto start = std::chrono::high_resolution_clock::now();
			lock.lock();
			g_pfnWickedProfilerLockWait(std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - start).count());
		}
		if (lockDepth++ == 0) lockHeldSince = std::chrono::high_resolution_clock::now();
	}
	void UnlockRanges()
	{
		double held = -1;
		if (--lockDepth == 0 && g_pfnWickedProfilerLockHold)
		{
			held = std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now() - lockHeldSince).count();
		}
		lock.unlock();
		if (held >= 0) g_pfnWickedProfilerLockHold(held);
	}
#else
	void LockRanges() { lock.lock(); }
	void UnlockRanges() { lock.unlock(); }
#endif
#ifdef GGREDUCED
	// GG: the main thread's CPU ranges timed whether or not profiling is on, without the lock (that thread only): each range's
	// own time goes to g_pfnWickedFramePhase, and its whole time to an average by name, which GetRangeTime answers while
	// profiling is off
	struct PhaseRange
	{
		range_id id = 0;
		char name[64] = "";
		double begin = 0;
		double inner = 0; // the time of the ranges inside it
	};
	struct PhaseTime
	{
		double frame = 0;
		float times[20] = {};
		int avg_counter = 0;
		float time = 0;
	};
	thread_local bool phaseThread = false; // the thread that calls BeginFrame
	PhaseRange phaseStack[32];
	int phaseDepth = 0;
	double phaseFrameBegin = 0;
	double phaseOutermost = 0; // this frame's ranges with no parent
	bool phaseOutsideValid = false;
	std::unordered_map<size_t, PhaseTime> phaseTimes;

	double PhaseNow()
	{
		return std::chrono::duration<double, std::milli>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
	}
	void PhaseBegin(range_id id, const char* name)
	{
		if (phaseDepth >= (int)arraysize(phaseStack))
			return;
		PhaseRange& phase = phaseStack[phaseDepth++];
		phase.id = id;
		strncpy_s(phase.name, name, _TRUNCATE);
		phase.inner = 0;
		phase.begin = PhaseNow();
	}
	void PhaseEnd(range_id id)
	{
		double now = PhaseNow();
		int i = phaseDepth - 1;
		while (i >= 0 && phaseStack[i].id != id)
			i--;
		if (i < 0)
			return; // not a CPU range timed here (a GPU range, or one begun before the first frame)
		PhaseRange& phase = phaseStack[i];
		double total = now - phase.begin;
		if (i > 0)
			phaseStack[i - 1].inner += total;
		else
			phaseOutermost += total;
		phaseTimes[wiHelper::string_hash(phase.name)].frame += total;
		if (g_pfnWickedFramePhase)
		{
			char parents[200] = "";
			for (int p = i - 1; p >= 0 && p >= i - 3; p--)
			{
				if (p < i - 1)
					strncat_s(parents, " < ", _TRUNCATE);
				strncat_s(parents, phaseStack[p].name, _TRUNCATE);
			}
			g_pfnWickedFramePhase(phase.name, parents, total - phase.inner);
		}
		phaseDepth = i; // a range left open inside it is dropped
	}
	void PhaseFrame()
	{
		phaseThread = true;
		double now = PhaseNow();
		if (g_pfnWickedFramePhase && phaseFrameBegin > 0)
		{
			// once a frame, which also closes the frame for the game's frame records: the time outside every range, or 0
			// when the frame began inside ranges (a Run called from inside one, or a range never ended), named as parents
			if (phaseOutsideValid && phaseDepth == 0)
			{
				g_pfnWickedFramePhase("outside ranges", "", now - phaseFrameBegin - phaseOutermost);
			}
			else
			{
				char open[200] = "";
				for (int p = phaseDepth - 1; p >= 0 && p >= phaseDepth - 3; p--)
				{
					if (p < phaseDepth - 1)
						strncat_s(open, " < ", _TRUNCATE);
					strncat_s(open, phaseStack[p].name, _TRUNCATE);
				}
				g_pfnWickedFramePhase("outside ranges", open[0] ? open : "(a range begun before the last frame)", 0.0);
			}
		}
		phaseFrameBegin = now;
		phaseOutermost = 0;
		phaseOutsideValid = phaseDepth == 0; // a frame begun inside a range (a nested Run) has no outside time
		for (auto& it : phaseTimes)
		{
			PhaseTime& phaseTime = it.second;
			phaseTime.times[phaseTime.avg_counter++ % arraysize(phaseTime.times)] = (float)phaseTime.frame;
			phaseTime.frame = 0;
			if (phaseTime.avg_counter > arraysize(phaseTime.times))
			{
				float sum = 0;
				for (int t = 0; t < arraysize(phaseTime.times); ++t)
					sum += phaseTime.times[t];
				phaseTime.time = sum / arraysize(phaseTime.times);
			}
		}
	}
#endif

	range_id cpu_frame;
	range_id gpu_frame;
#ifdef GGREDUCED
	// GG: a frame run from inside another (StartForceRender: a video played from Lua, the title and loading screens) begins
	// its own "CPU Frame" and "GPU Frame"; the outer frame's are kept here meanwhile and given back when the inner one ends,
	// so each EndFrame ends its own frame's ranges (before, the outer EndFrame ended the inner ones a second time, and its
	// own "CPU Frame" stayed on the frame phase stack, one more for every inner frame)
	struct OuterFrame
	{
		range_id cpu = 0;
		range_id gpu = 0;
		bool gpu_this_frame = false;
	};
	OuterFrame outerFrames[4];
	int frameDepth = 0; // the frames begun and not yet ended
#endif
#ifdef GGREDUCED
	// GG: read back eight frames late rather than two, past the frames the GPU may still be working on (four was not enough
	// when the GPU is the bottleneck)
	GPUQueryHeap queryHeap[wiGraphics::GraphicsDevice::GetBufferCount() + 7];
#else
	GPUQueryHeap queryHeap[wiGraphics::GraphicsDevice::GetBufferCount() + 1];
#endif
	std::vector<uint64_t> queryResults;
	std::atomic<uint32_t> nextQuery{ 0 };
	uint32_t writtenQueries[arraysize(queryHeap)] = {};
	int queryheap_idx = 0;

	struct Range
	{
		bool in_use = false;
		std::string name;
		float times[20] = {};
		int avg_counter = 0;
		float time = 0;
		float peek = 0;
#ifdef GGREDUCED
		int stale = 0; // GG: frames since a GPU range last had a result; its time is the older one meanwhile
#endif
		CommandList cmd = COMMANDLIST_COUNT;

		wiTimer cpuBegin, cpuEnd;

		int gpuBegin[arraysize(queryHeap)];
		int gpuEnd[arraysize(queryHeap)];

		bool IsCPURange() const { return cmd == COMMANDLIST_COUNT; }
	};
	std::unordered_map<size_t, Range> ranges;
	std::vector<range_id> rangeOrder[COMMANDLIST_COUNT + 1];

	void BeginFrame()
	{
#ifdef GGREDUCED
		if (frameDepth > 0 && frameDepth <= (int)arraysize(outerFrames))
		{
			OuterFrame& outer = outerFrames[frameDepth - 1];
			outer.cpu = cpu_frame;
			outer.gpu = gpu_frame;
			outer.gpu_this_frame = gpu_this_frame;
			cpu_frame = 0;
			gpu_frame = 0;
		}
		frameDepth++;

		PhaseFrame();

		// the draw call and polygon counts are taken every frame, as the renderer counts whether or not profiling is on
		// (before, with it off they were never reset, so they only grew)
		iOldDrawCalls = iDrawCalls;
		iOldDrawCallsShadows = iDrawCallsShadows;
		iOldDrawCallsShadowsCube = iDrawCallsShadowsCube;
		iOldDrawCallsTransparent = iDrawCallsTransparent;
		iDrawCalls = 0;
		iDrawCallsShadows = 0;
		iDrawCallsShadowsCube = 0;
		iDrawCallsTransparent = 0;

		iOldPolygonsDrawn = iPolygonsDrawn;
		iOldPolygonsDrawnShadows = iPolygonsDrawnShadows;
		iOldPolygonsDrawnTransparent = iPolygonsDrawnTransparent;
		iPolygonsDrawn = 0;
		iPolygonsDrawnShadows = 0;
		iPolygonsDrawnTransparent = 0;
#endif

		if (!ENABLED)
			return;

		if (!initialized)
		{
			initialized = true;

			ranges.reserve(100);
			
			GPUQueryHeapDesc desc;
			desc.type = GPU_QUERY_TYPE_TIMESTAMP;
			desc.queryCount = 1024;
			for (int i = 0; i < arraysize(queryHeap); ++i)
			{
				bool success = wiRenderer::GetDevice()->CreateQueryHeap(&desc, &queryHeap[i]);
				assert(success);
			}

			queryResults.resize(desc.queryCount);
		}

		cpu_frame = BeginRangeCPU("CPU Frame");

#ifdef GGREDUCED
		gpu_this_frame = GPU_ENABLED;
		if (!gpu_this_frame)
			return;
#endif
		CommandList cmd = wiRenderer::GetDevice()->BeginCommandList();
		gpu_frame = BeginRangeGPU("GPU Frame", cmd);
	}
#ifdef GGREDUCED
	void EndFrameRanges(CommandList cmd)
#else
	void EndFrame(CommandList cmd)
#endif
	{
#ifdef GGREDUCED
		if ((!ENABLED || !initialized) && phaseThread && phaseFrameBegin > 0)
		{
			// GG: "CPU Frame" for GetRangeTime while profiling is off, timed as the profiler times it (BeginFrame to EndFrame).
			// Profiling turned off since BeginFrame began the "CPU Frame" range (Tab Tab closed): that range is ended here,
			// which times it, as otherwise it stays open on the frame phase stack under every later frame
			int depth = phaseDepth;
			if (cpu_frame)
			{
				PhaseEnd(cpu_frame);
				cpu_frame = 0;
			}
			if (phaseDepth == depth)
				phaseTimes[wiHelper::string_hash("CPU Frame")].frame += PhaseNow() - phaseFrameBegin;
		}
#endif
		if (!ENABLED || !initialized)
			return;

		GraphicsDevice* device = wiRenderer::GetDevice();
		double gpu_frequency = (double)device->GetTimestampFrequency() / 1000.0;

#ifdef GGREDUCED
		if (!gpu_this_frame)
		{
			// GG: CPU ranges only, no GPU Frame query, resolve or read back
			EndRange(cpu_frame);
			if (g_pfnWickedProfilerQueries) g_pfnWickedProfilerQueries(0);
		}
		else
		{
#endif
		// note: read the GPU Frame end range manually because it will be on a separate command list than start point: 
		auto& gpu_range = ranges[gpu_frame];
		gpu_range.gpuEnd[queryheap_idx] = nextQuery.fetch_add(1);
		device->QueryEnd(&queryHeap[queryheap_idx], gpu_range.gpuEnd[queryheap_idx], cmd);

		EndRange(cpu_frame);

		device->QueryResolve(&queryHeap[queryheap_idx], 0, nextQuery.load(), cmd);

		writtenQueries[queryheap_idx] = nextQuery.load();
#ifdef GGREDUCED
		if (g_pfnWickedProfilerQueries) g_pfnWickedProfilerQueries(writtenQueries[queryheap_idx]);
#endif
		nextQuery.store(0);
		queryheap_idx = (queryheap_idx + 1) % arraysize(queryHeap);
		if (writtenQueries[queryheap_idx] > 0)
		{
#ifdef GGREDUCED
			// GG: QueryRead leaves a query that isn't ready untouched, and the results are shared by every heap, so it
			// kept another frame's (often another range's) timestamp; mark them all unread first
			for (uint32_t i = 0; i < writtenQueries[queryheap_idx]; ++i) queryResults[i] = UINT64_MAX;
#endif
			wiRenderer::GetDevice()->QueryRead(&queryHeap[queryheap_idx], 0, writtenQueries[queryheap_idx], queryResults.data());
		}
#ifdef GGREDUCED
		}
#endif

		for (auto& x : ranges)
		{
			auto& range = x.second;

#ifdef GGREDUCED
			float previous_time = range.time;
			bool skip_sample = false;
#endif
			range.time = 0;
			if (range.IsCPURange())
			{
				range.time = (float)abs(range.cpuEnd.elapsed() - range.cpuBegin.elapsed());
			}
			else
			{
				int begin_query = range.gpuBegin[queryheap_idx];
				int end_query = range.gpuEnd[queryheap_idx];
				if (begin_query >= 0 && end_query >= 0)
				{
#ifdef GGREDUCED
					// a query outside what this heap wrote, or not ready when read, has no result for this frame; such a
					// sample is skipped and the range keeps its last time
					uint32_t written = writtenQueries[queryheap_idx];
					uint64_t begin_result = (uint32_t)begin_query < written ? queryResults[begin_query] : UINT64_MAX;
					uint64_t end_result = (uint32_t)end_query < written ? queryResults[end_query] : UINT64_MAX;
					if (begin_result == UINT64_MAX || end_result == UINT64_MAX || end_result <= begin_result)
						skip_sample = true;
					else
#else
					uint64_t begin_result = queryResults[begin_query];
					uint64_t end_result = queryResults[end_query];
#endif
					range.time = (float)abs((double)(end_result - begin_result) / gpu_frequency);
				}
				range.gpuBegin[queryheap_idx] = -1;
				range.gpuEnd[queryheap_idx] = -1;
			}
#ifdef GGREDUCED
			if (skip_sample)
			{
				range.time = previous_time;
				range.stale++;
				range.in_use = false;
				continue;
			}
			range.stale = 0;
#endif
			range.times[range.avg_counter++ % arraysize(range.times)] = range.time;

			if (range.avg_counter > arraysize(range.times))
			{
				float avg_time = 0;
				for (int i = 0; i < arraysize(range.times); ++i)
				{
					avg_time += range.times[i];
				}
				range.time = avg_time / arraysize(range.times);
			}

			if(range.time> range.peek) range.peek = range.time;

			range.in_use = false;
		}
	}
#ifdef GGREDUCED
	void EndFrame(CommandList cmd)
	{
		EndFrameRanges(cmd);

		// an inner frame ended: the outer frame's ranges back (BeginFrame). ForceRender's EndFrame, which has no BeginFrame
		// of its own, ends the frame it is called in, as before, and that frame's own EndFrame then finds none open
		if (frameDepth > 1 && frameDepth - 1 <= (int)arraysize(outerFrames))
		{
			const OuterFrame& outer = outerFrames[frameDepth - 2];
			cpu_frame = outer.cpu;
			gpu_frame = outer.gpu;
			gpu_this_frame = outer.gpu_this_frame;
		}
		if (frameDepth > 0)
			frameDepth--;
	}
#endif

	range_id BeginRangeCPU(const char* name)
	{
		if (!ENABLED || !initialized)
		{
#ifdef GGREDUCED
			if (phaseThread && name)
			{
				range_id id = wiHelper::string_hash(name);
				PhaseBegin(id, name);
				return id;
			}
#endif
			return 0;
		}

		range_id id = wiHelper::string_hash(name);

		LockRanges();

		// If one range name is hit multiple times, differentiate between them!
		size_t differentiator = 0;
		while (ranges[id].in_use)
		{
			wiHelper::hash_combine(id, differentiator++);
		}
		ranges[id].in_use = true;
		if ( ranges[id].name.length() == 0 ) rangeOrder[0].push_back( id );
		ranges[id].name = name;

		ranges[id].cpuBegin.record();

		UnlockRanges();

#ifdef GGREDUCED
		if (phaseThread)
			PhaseBegin(id, name);
#endif

		return id;
	}
	range_id BeginRangeGPU(const char* name, CommandList cmd)
	{
		if (!ENABLED || !initialized)
			return 0;
#ifdef GGREDUCED
		if (!gpu_this_frame)
			return 0;
#endif

		range_id id = wiHelper::string_hash(name);

		LockRanges();

		// If one range name is hit multiple times, differentiate between them!
		size_t differentiator = 0;
		while (ranges[id].in_use)
		{
			wiHelper::hash_combine(id, differentiator++);
		}
		ranges[id].in_use = true;
		if ( ranges[id].name.length() == 0 ) rangeOrder[cmd+1].push_back( id );
		ranges[id].name = name;

		ranges[id].cmd = cmd;

		const int heap = queryheap_idx;
		const uint32_t query = nextQuery.fetch_add(1);
		ranges[id].gpuBegin[heap] = query;

		UnlockRanges();

		// GG: the driver call after the lock, which every thread's ranges share: a driver call that stalled held up every
		// range on every thread, a frame stall of seconds. The command list is this thread's own
		wiRenderer::GetDevice()->QueryEnd(&queryHeap[heap], query, cmd);

		return id;
	}
	void EndRange(range_id id)
	{
#ifdef GGREDUCED
		if (phaseThread && id != 0)
			PhaseEnd(id);
#endif
		if (!ENABLED || !initialized)
			return;
#ifdef GGREDUCED
		if (id == 0)
			return; // a GPU range skipped while GPU timing is off
#endif

		LockRanges();

		bool gpu = false;
		int heap = queryheap_idx;
		uint32_t query = 0;
		CommandList cmd = COMMANDLIST_COUNT;
		auto it = ranges.find(id);
		if (it != ranges.end())
		{
			if (it->second.IsCPURange())
			{
				it->second.cpuEnd.record();
			}
			else
			{
				query = nextQuery.fetch_add(1);
				it->second.gpuEnd[heap] = query;
				cmd = it->second.cmd;
				gpu = true;
			}
		}
		else
		{
			assert(0);
		}

		UnlockRanges();

		// GG: the driver call after the lock (BeginRangeGPU)
		if (gpu)
		{
			wiRenderer::GetDevice()->QueryEnd(&queryHeap[heap], query, cmd);
		}
	}

#ifdef GGREDUCED
	void CountDrawCalls(void) { iDrawCalls++; }
	void CountDrawCallsShadows(void) { iDrawCallsShadows++; }
	void CountDrawCallsShadowsCube(void) { iDrawCallsShadowsCube++; }
	void CountDrawCallsTransparent(void) { iDrawCallsTransparent++; }

	int GetDrawCalls(void) { return(iOldDrawCalls); }
	int GetDrawCallsShadows(void) { return(iOldDrawCallsShadows); };
	int GetDrawCallsShadowsCube(void) { return(iOldDrawCallsShadowsCube); };
	int GetDrawCallsTransparent(void) { return(iOldDrawCallsTransparent); };

	void CountPolygons(int iPoly) { iPolygonsDrawn += iPoly; }
	void CountPolygonsShadows(int iPoly) { iPolygonsDrawnShadows += iPoly; }
	void CountPolygonsTransparent(int iPoly) { iPolygonsDrawnTransparent += iPoly; }

	int GetPolygons(void) { return iOldPolygonsDrawn; }
	int GetPolygonsShadows(void) { return iOldPolygonsDrawnShadows; }
	int GetPolygonsTransparent(void) { return iOldPolygonsDrawnTransparent; }

	int GetFrustumCulled(void) { return(iFrustumculled); }
	void SetFrustumCulled(int iFrustum) { iFrustumculled = iFrustum; }

	// the time of the first range with this name ("GPU Frame", "CPU Frame", ...) in ms, averaged over the last frames; -1 while
	// profiling is off or until the range has been timed for as many frames as the average takes (the first GPU results
	// arrive some frames late). pStaleFrames, if given, gets how many frames the time has gone without a new result.
	// While profiling is off, a CPU range on the main thread is answered from the frame phase timing: all of the frame's
	// ranges with that name, averaged over the last frames (GPU ranges -1)
	float GetRangeTime(const char* name, int* pStaleFrames)
	{
		if (pStaleFrames) *pStaleFrames = 0;
#ifdef GGREDUCED
		if ((!ENABLED || !initialized) && name && phaseThread)
		{
			auto it = phaseTimes.find(wiHelper::string_hash(name));
			if (it != phaseTimes.end() && it->second.avg_counter > arraysize(it->second.times))
				return it->second.time;
			return -1;
		}
#endif
		if (!ENABLED || !initialized || !name)
			return -1;

		float time = -1;
		LockRanges();
		auto it = ranges.find(wiHelper::string_hash(name));
		if (it != ranges.end() && it->second.avg_counter > arraysize(it->second.times))
		{
			time = it->second.time;
			if (pStaleFrames) *pStaleFrames = it->second.stale;
		}
		UnlockRanges();
		return time;
	}


	//We need the data returned here.
	std::string GetProfilerData(void)
	{
		if (!ENABLED || !initialized)
			return "Profiler not initialized.";

		std::stringstream ss("");
		ss.precision(2);
		//ss << "Profiler:" << std::endl << "----------------------------" << std::endl; //PE: Make more room.

		// Print CPU ranges:
		for (auto& index : rangeOrder[0])
		{
			Range range = ranges[index];
			if (range.IsCPURange())
			{
				ss << range.name << ": " << std::fixed << range.time << " ms (" << range.peek << " ms)" << std::endl;
			}
		}
		ss << std::endl;

		// Print GPU ranges:
		float shadowTerrainTotal = 0;
		float shadowTreesTotal = 0;
		for( int i = 0; i < COMMANDLIST_COUNT; i++ )
		{
			for (auto& index : rangeOrder[i+1])
			{
				Range range = ranges[index];
				if (!range.IsCPURange())
				{
					if ( range.name.compare( 0, strlen("Shadow Rendering - Terrain"), "Shadow Rendering - Terrain" ) == 0 )
					{
						shadowTerrainTotal += range.time;
					}
					else if ( range.name.compare( 0, strlen("Shadow Rendering - Tree"), "Shadow Rendering - Tree" ) == 0 )
					{
						shadowTreesTotal += range.time;
					}
					else
					{
						if ( shadowTerrainTotal > 0 )
						{
							ss << "Shadow Rendering - Terrain" << ": " << std::fixed << shadowTerrainTotal << " ms" << std::endl;
							shadowTerrainTotal = 0;
						}
						if ( shadowTreesTotal > 0 )
						{
							ss << "Shadow Rendering - Trees" << ": " << std::fixed << shadowTreesTotal << " ms" << std::endl;
							shadowTreesTotal = 0;
						}
						ss << range.name << ": " << std::fixed << range.time << " ms (" << range.peek << " ms)" << std::endl;
					}
				}
			}
		}
		return ss.str();
	}
	std::string GetProfilerDataFilter(char *filter)
	{
		if (!ENABLED || !initialized)
			return "Profiler not initialized.";
		if (!filter)
			return "No filter set.";

		std::stringstream ss("");
		ss.precision(2);
		//ss << "Profiler:" << std::endl << "----------------------------" << std::endl; //PE: Make more room.
		ss << "Profiler Filter: " << filter << std::endl;

		// Print CPU ranges:
		bool bGotSome = false;
		float FilterTotal = 0;
		for (auto& index : rangeOrder[0])
		{
			Range range = ranges[index];
			if (range.IsCPURange())
			{
				if (range.name.compare(0, strlen(filter), filter) == 0)
				{
					ss << range.name << ": " << std::fixed << range.time << " ms" << std::endl;
					bGotSome = true;
					FilterTotal += range.time;
				}
			}
		}
		if(bGotSome)
			ss << std::endl;

		// Print GPU ranges:
		for (int i = 0; i < COMMANDLIST_COUNT; i++)
		{
			for (auto& index : rangeOrder[i + 1])
			{
				Range range = ranges[index];
				if (!range.IsCPURange())
				{
					std::size_t found = range.name.find(filter);
					if (found != std::string::npos)
					{
						ss << range.name << ": " << std::fixed << range.time << " ms" << std::endl;
						FilterTotal += range.time;
					}
				}
			}
		}
		ss << "Filter Total" << ": " << std::fixed << FilterTotal << " ms" << std::endl;

		return ss.str();
	}

#endif
	struct Hits
	{
		uint32_t num_hits = 0;
		float total_time = 0;
	};
	std::unordered_map<std::string, Hits> time_cache_cpu;
	std::unordered_map<std::string, Hits> time_cache_gpu;
	void DrawData(const wiCanvas& canvas, float x, float y, CommandList cmd)
	{
#ifdef GGREDUCED
		//PE: Never draw profiler data in GG.
		return;
#endif
		if (!ENABLED || !initialized)
			return;

		wiImage::SetCanvas(canvas, cmd);
		wiFont::SetCanvas(canvas, cmd);

		std::stringstream ss("");
		ss.precision(2);
		ss << "Frame Profiler Ranges:" << std::endl << "----------------------------" << std::endl;


		for (auto& x : ranges)
		{
			if (x.second.IsCPURange())
			{
				time_cache_cpu[x.second.name].num_hits++;
				time_cache_cpu[x.second.name].total_time += x.second.time;
			}
			else
			{
				time_cache_gpu[x.second.name].num_hits++;
				time_cache_gpu[x.second.name].total_time += x.second.time;
		}
		}

		// Print CPU ranges:
		for (auto& x : time_cache_cpu)
		{
			ss << x.first << " (" << x.second.num_hits << "x)" << ": " << std::fixed << x.second.total_time << " ms" << std::endl;
			x.second.num_hits = 0;
			x.second.total_time = 0;
		}
		ss << std::endl;

		// Print GPU ranges:
		for (auto& x : time_cache_gpu)
		{
			ss << x.first << " (" << x.second.num_hits << "x)" << ": " << std::fixed << x.second.total_time << " ms" << std::endl;
			x.second.num_hits = 0;
			x.second.total_time = 0;
			}

		wiFontParams params = wiFontParams(x, y, WIFONTSIZE_DEFAULT - 4, WIFALIGN_LEFT, WIFALIGN_TOP, wiColor(255, 255, 255, 255), wiColor(0, 0, 0, 255));

		wiImageParams fx;
		fx.pos.x = (float)params.posX;
		fx.pos.y = (float)params.posY;
		fx.siz.x = (float)wiFont::textWidth(ss.str(), params);
		fx.siz.y = (float)wiFont::textHeight(ss.str(), params);
		fx.color = wiColor(20, 20, 20, 230);
		wiImage::Draw(wiTextureHelper::getWhite(), fx, cmd);

		wiFont::Draw(ss.str(), params, cmd);
	}

	void ResetPeek (void)
	{
		// first clear any peek values
		for (auto& x : ranges) x.second.peek = 0;
		for (int i = 0; i < COMMANDLIST_COUNT + 1; i++) for (auto& x : ranges) x.second.peek = 0;
	}

#ifdef GGREDUCED
	void SetGPUEnabled(bool value)
	{
		GPU_ENABLED = value;
	}
#endif

	void SetEnabled(bool value)
	{
		if (value != ENABLED)
		{
			initialized = false;
			ranges.clear();
			for( int i = 0; i < COMMANDLIST_COUNT+1; i++ ) rangeOrder[i].clear();
#ifdef GGREDUCED
			// GG: the heaps are made again, so nothing written before counts
			for (int i = 0; i < arraysize(writtenQueries); ++i) writtenQueries[i] = 0;
			nextQuery.store(0);
			queryheap_idx = 0;
#endif
			ENABLED = value;
		}
	}

	bool IsEnabled()
	{
		return ENABLED;
	}

}

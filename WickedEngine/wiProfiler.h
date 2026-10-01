#pragma once
#include "wiGraphicsDevice.h"
#include "wiCanvas.h"

namespace wiProfiler
{
	typedef size_t range_id;

	// Begin collecting profiling data for the current frame
	void BeginFrame();

	// Finalize collecting profiling data for the current frame
	void EndFrame(wiGraphics::CommandList cmd);

	// Start a CPU profiling range
	range_id BeginRangeCPU(const char* name);

	// Start a GPU profiling range
	range_id BeginRangeGPU(const char* name, wiGraphics::CommandList cmd);

	// End a profiling range
	void EndRange(range_id id);

	// Renders a basic text of the Profiling results to the (x,y) screen coordinate
	void DrawData(const wiCanvas& canvas, float x, float y, wiGraphics::CommandList cmd);

#ifdef GGREDUCED
	// GG: a CPU range over the scope it is declared in (a render job, a wait), so an early return still ends it
	struct ScopedRangeCPU
	{
		range_id id;
		ScopedRangeCPU(const char* name) : id(BeginRangeCPU(name)) {}
		~ScopedRangeCPU() { EndRange(id); }
	};

	std::string GetProfilerData(void);
	std::string GetProfilerDataFilter(char* filter);

	void CountDrawCalls(void);
	void CountDrawCallsShadows(void);
	void CountDrawCallsShadowsCube(void);
	void CountDrawCallsTransparent(void);

	int GetDrawCalls(void);
	int GetDrawCallsShadows(void);
	int GetDrawCallsShadowsCube(void);
	int GetDrawCallsTransparent(void);

	void CountPolygons(int iPoly);
	void CountPolygonsShadows(int iPoly);
	void CountPolygonsTransparent(int iPoly);
	
	int GetPolygons(void);
	int GetPolygonsShadows(void);
	int GetPolygonsTransparent(void);

	int GetFrustumCulled(void);
	void SetFrustumCulled(int iFrustum);

	float GetRangeTime(const char* name, int* pStaleFrames = nullptr);

	void ResetPeek (void);

#endif

	// Enable/disable profiling
	void SetEnabled(bool value);

	bool IsEnabled();

#ifdef GGREDUCED
	// GG: false times CPU ranges only: no GPU range, query, resolve or read back, from the next BeginFrame (true by default)
	void SetGPUEnabled(bool value);
#endif
};

#ifdef GGREDUCED
// GG: when set, told each frame's GPU query count and every wait for the lock the ranges share (the game's stall probes)
extern void (*g_pfnWickedProfilerQueries)(uint32_t queries);
extern void (*g_pfnWickedProfilerLockWait)(double dMilliseconds);
extern void (*g_pfnWickedProfilerLockHold)(double dMilliseconds);
// GG: when set, told the main thread's CPU ranges whether or not profiling is on (the game's frame phase probe): each range's
// own time, without the ranges inside it, and its parents innermost first ("B < A"); and once a frame the time outside every
// range, named "outside ranges"
extern void (*g_pfnWickedFramePhase)(const char* name, const char* parents, double dMilliseconds);
#endif


#include "Cafe/Savestate/SavestateSerializer_GPU.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "util/highresolutiontimer/HighResolutionTimer.h"

namespace TCL
{
	uint64 Savestate_GetRetireMarker();
	void Savestate_SetRetireMarker(uint64 value);
}

namespace SavestateSerializer_GPU
{
	// On-disk record. Deliberately NOT LatteGPUState_t itself even
	// though most fields match 1:1 - see exclusions below, and this
	// insulates the file format from unrelated struct changes.
	struct GPUStateRecord
	{
		uint32 contextRegister[LATTE_MAX_REGISTER];
		MPTR contextRegisterShadowAddr[LATTE_MAX_REGISTER];
		uint32 contextControl0;
		uint32 contextControl1;

		uint32 frameCounter;
		uint32 flipCounter;
		uint32 currentDrawCallTick;
		uint32 drawCallCounter;
		uint32 textureBindCounter;
		uint64 flipRequestCount;

		// Rebased relative to timer_bootUp rather than stored absolute -
		// see DoState() below for why.
		sint64 vsyncDeltaFromBootUp;

		// TCL::s_currentRetireMarker (host side). The guest holds timestamps derived from it and waits for
		// gpuRetireMarker >= timestamp, so both must stay on the same timeline after a load.
		uint64 tclRetireMarker;

		MPTR sharedAreaAddr; // sharedArea* pointer recomputed from this, never stored raw
		uint32 gx2InitCalled;

		bool isDRCPrimary;
		bool tvBufferUsesSRGB;
		bool drcBufferUsesSRGB;
		float tvGamma;
		float drcGamma;

		// osScreen: per-screen enabled flag + guest physPtr + counters.
		// flipRequestCount/flipExecuteCount stored as plain uint32
		// (the live struct uses std::atomic<uint32>, which is not
		// itself trivially copyable for DoPOD - unwrap on both sides).
		struct
		{
			bool isEnabled;
			MPTR physPtr;
			uint32 flipRequestCount;
			uint32 flipExecuteCount;
		} osScreen[2];

		// Deliberately excluded, and why:
		//
		// - glVendor: host GPU vendor. Re-detected locally on every
		//   launch (GetVendorInformation() override seen in
		//   OpenGLRenderer.h) - never loaded from a file written on
		//   a possibly different PC.
		// - activeShaderHasError / repeatTextureInitialization /
		//   requiresTextureBarrier: transient per-drawcall flags,
		//   recomputed naturally on the next draw. Reset to false on
		//   load rather than restored.
		// - allowFramebufferSizeOptimization: derived from settings,
		//   not game state.
	};

	void SyncDirtyTexturesToGuestMemory()
	{
		// Intentionally empty. Textures/render targets that live only on the GPU are not
		// written back into guest RAM here: after a load every cached GPU object is dropped
		// (InvalidateHostCachesAfterLoad) and rebuilt from guest memory. Content that exists
		// only on the GPU (e.g. a render target the game does not redraw every frame) can
		// therefore look wrong for a moment after loading. This is a known limitation.
	}

	void DoState(Serializer& s)
	{
		s.DoMarker("GPU");

		if (s.IsWriting() || s.IsMeasuring())
		{
			GPUStateRecord record{};
			memcpy(record.contextRegister, LatteGPUState.contextRegister, sizeof(record.contextRegister));
			memcpy(record.contextRegisterShadowAddr, LatteGPUState.contextRegisterShadowAddr, sizeof(record.contextRegisterShadowAddr));
			record.contextControl0 = LatteGPUState.contextControl0;
			record.contextControl1 = LatteGPUState.contextControl1;
			record.frameCounter = LatteGPUState.frameCounter;
			record.flipCounter = LatteGPUState.flipCounter;
			record.currentDrawCallTick = LatteGPUState.currentDrawCallTick;
			record.drawCallCounter = LatteGPUState.drawCallCounter;
			record.textureBindCounter = LatteGPUState.textureBindCounter;
			record.flipRequestCount = LatteGPUState.flipRequestCount.load();

			// Rebasing: store how far in the future the next vsync is
			// relative to boot-up, in host timer ticks. On load we pick
			// a NEW timer_bootUp (current host HPC reading) and add
			// this same delta back - never carry the absolute HPC
			// value across a save/load boundary, it means nothing on
			// a different run (or different machine).
			record.vsyncDeltaFromBootUp = (sint64)(LatteGPUState.timer_nextVSync - LatteGPUState.timer_bootUp);

			record.tclRetireMarker = TCL::Savestate_GetRetireMarker();
			record.sharedAreaAddr = LatteGPUState.sharedAreaAddr;
			record.gx2InitCalled = LatteGPUState.gx2InitCalled;
			record.isDRCPrimary = LatteGPUState.isDRCPrimary;
			record.tvBufferUsesSRGB = LatteGPUState.tvBufferUsesSRGB;
			record.drcBufferUsesSRGB = LatteGPUState.drcBufferUsesSRGB;
			record.tvGamma = LatteGPUState.tvGamma;
			record.drcGamma = LatteGPUState.drcGamma;

			for (int i = 0; i < 2; i++)
			{
				record.osScreen[i].isEnabled = LatteGPUState.osScreen.screen[i].isEnabled;
				record.osScreen[i].physPtr = LatteGPUState.osScreen.screen[i].physPtr;
				record.osScreen[i].flipRequestCount = LatteGPUState.osScreen.screen[i].flipRequestCount.load();
				record.osScreen[i].flipExecuteCount = LatteGPUState.osScreen.screen[i].flipExecuteCount.load();
			}

			s.DoPOD(record);
			return;
		}

		// --- Reading -----------------------------------------------
		GPUStateRecord record{};
		s.DoPOD(record);

		memcpy(LatteGPUState.contextRegister, record.contextRegister, sizeof(record.contextRegister));
		memcpy(LatteGPUState.contextRegisterShadowAddr, record.contextRegisterShadowAddr, sizeof(record.contextRegisterShadowAddr));
		LatteGPUState.contextControl0 = record.contextControl0;
		LatteGPUState.contextControl1 = record.contextControl1;
		LatteGPUState.frameCounter = record.frameCounter;
		LatteGPUState.flipCounter = record.flipCounter;
		LatteGPUState.currentDrawCallTick = record.currentDrawCallTick;
		LatteGPUState.drawCallCounter = record.drawCallCounter;
		LatteGPUState.textureBindCounter = record.textureBindCounter;
		LatteGPUState.flipRequestCount = record.flipRequestCount;

		// timer_frequency is a host HPC property (ticks per second),
		// not game state - leave whatever the current process already
		// initialized it to. Only rebase bootUp/nextVSync.
		// same clock (QueryPerformanceCounter / clock_gettime ticks) that LatteTiming.cpp uses; a different
		// clock here (e.g. std::chrono, nanoseconds) puts the next vsync far in the future and freezes the game
		uint64 newBootUp = HighResolutionTimer::now().getTick();
		LatteGPUState.timer_bootUp = newBootUp;
		LatteGPUState.timer_nextVSync = newBootUp + (uint64)record.vsyncDeltaFromBootUp;

		TCL::Savestate_SetRetireMarker(record.tclRetireMarker);
		LatteGPUState.sharedAreaAddr = record.sharedAreaAddr;
		LatteGPUState.sharedArea = (gx2GPUSharedArea_t*)memory_getPointerFromVirtualOffsetAllowNull(record.sharedAreaAddr);
		LatteGPUState.gx2InitCalled = record.gx2InitCalled;
		LatteGPUState.isDRCPrimary = record.isDRCPrimary;
		LatteGPUState.tvBufferUsesSRGB = record.tvBufferUsesSRGB;
		LatteGPUState.drcBufferUsesSRGB = record.drcBufferUsesSRGB;
		LatteGPUState.tvGamma = record.tvGamma;
		LatteGPUState.drcGamma = record.drcGamma;

		// glVendor: intentionally NOT touched, keeps whatever this
		// process already detected.
		LatteGPUState.activeShaderHasError = false;
		LatteGPUState.repeatTextureInitialization = true; // force a re-init pass, cheap and safe
		LatteGPUState.requiresTextureBarrier = false;

		for (int i = 0; i < 2; i++)
		{
			LatteGPUState.osScreen.screen[i].isEnabled = record.osScreen[i].isEnabled;
			LatteGPUState.osScreen.screen[i].physPtr = record.osScreen[i].physPtr;
			LatteGPUState.osScreen.screen[i].flipRequestCount = record.osScreen[i].flipRequestCount;
			LatteGPUState.osScreen.screen[i].flipExecuteCount = record.osScreen[i].flipExecuteCount;
		}
	}

	void InvalidateHostCachesAfterLoad()
	{
		// Every cached texture/FBO/buffer object in the active
		// renderer is now stale, since guest memory just changed out
		// from under it. Marshaled onto the GPU thread itself (see
		// Latte.h) since that thread owns the GL context even while
		// parked for the savestate.
		Latte_RequestCacheInvalidationWhilePaused();

		// Also: recompiled JIT code that assumed old memory contents
		// must be invalidated - this is handled by
		// SavestateSerializer_RPL's load path, listed as a required
		// call after module list restoration, not here.
	}
}

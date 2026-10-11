#pragma once
#include "Cafe/Savestate/SavestateSerializer.h"

namespace SavestateSerializer_GPU
{
	// Must be called BEFORE SavestateSerializer_MMU::DoState() on the
	// save path (and is a no-op skipped entirely on the load path -
	// there is nothing to "sync back" when loading). Forces every
	// GPU-dirty texture back into guest RAM via the existing
	// LatteTextureReadback_ReadbackToLinearBlocking() path, so the
	// subsequent flat MMU dump captures it with no separate GPU-texture
	// section needed.
	void SyncDirtyTexturesToGuestMemory();

	// Serializes LatteGPUState_t. Almost entirely POD; see the
	// exclusions documented in the .cpp (glVendor, transient draw
	// flags, vsync timer rebasing).
	void DoState(Serializer& s);

	// Must be called AFTER DoState() on the load path, before resuming
	// any GPU thread activity: drops every cached OpenGL texture/FBO/
	// buffer object, since none of them are valid after a jump in
	// guest memory contents.
	//
	// Internally marshals onto the GPU thread itself via
	// Latte_RequestCacheInvalidationWhilePaused() (see Latte.h) - the
	// GPU thread only pauses during a savestate, it never releases its
	// GL context, so the actual glDelete*() calls must run on that
	// thread specifically.
	void InvalidateHostCachesAfterLoad();
}

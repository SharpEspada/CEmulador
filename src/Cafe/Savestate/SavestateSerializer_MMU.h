#pragma once
#include "Cafe/Savestate/SavestateSerializer.h"

// Serializes / restores every currently mapped MMURange in one flat
// pass. See MMU.h/.cpp: this is deliberately simple because
// memory_getMMURanges() already returns the complete, fixed list of
// regions, virtual==physical 1:1, and the game's own heap metadata
// lives inside the dumped bytes (nothing extra to reconstruct there).
//
// IMPORTANT ordering requirement: this must run AFTER
// SavestateSerializer_GPU's texture readback step (LatteTextureReadback_
// ReadbackToLinearBlocking on GPU-dirty textures) and AFTER
// SavestateSerializer_RPL has finished any in-flight allocator work, so
// that everything those steps write into guest RAM is captured here.
namespace SavestateSerializer_MMU
{
	void DoState(Serializer& s);
}

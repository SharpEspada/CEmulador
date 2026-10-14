#pragma once
#include "Cafe/Savestate/SavestateSerializer.h"

// Host-side (C++ global) state of HLE modules that is NOT part of the guest RAM dump
// but points into / depends on it (heap registry, GX2 command buffer pointers,
// GX2 event callbacks, async callback queue, FS client list, AX frame counters...).
// Without it a loaded state can freeze (e.g. GX2 keeps writing in the old command buffer).
namespace SavestateSerializer_HostState
{
	// Works in both directions (the Serializer knows whether it is saving or loading).
	void DoState(Serializer& s);
}

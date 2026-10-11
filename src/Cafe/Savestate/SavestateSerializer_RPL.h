#pragma once
#include "Cafe/Savestate/SavestateSerializer.h"

// Reloading modules from disk during a load re-runs every HLE module's
// RPLMapped(), which is not guaranteed to reproduce the saved host state.
// Disabled: a load is only accepted when the running game already has the
// same modules as the savestate. See SavestateSerializer_RPL.cpp.
constexpr bool SAVESTATE_ALLOW_MODULE_RELOAD = false;

namespace SavestateSerializer_RPL
{
	// Refuses to save if any module is mid-load (tempRegionPtr != nullptr
	// on any entry in s_rplModuleList) - this should never happen during
	// normal gameplay, only ever mid-boot or mid dynamic-library-load,
	// neither of which is a supported savestate point for V1.
	bool CanSafelySave();

	void DoState_Save(Serializer& s);

	// Loading is NOT symmetric with saving for this section (unlike
	// every other DoState() in this project) - see the long comment in
	// the .cpp for why: real modules are reloaded from disk by name
	// rather than their bytes being replayed from the file.
	void DoState_Load(Serializer& s);
}

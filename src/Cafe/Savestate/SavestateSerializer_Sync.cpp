#include "Cafe/Savestate/SavestateSerializer_Sync.h"

namespace SavestateSerializer_Sync
{
	void DoState(Serializer& s)
	{
		s.DoMarker("Sync");
		// Intentionally empty for now - see header comment. If your
		// real coreinit_Synchronization.cpp keeps ANY host-side state
		// alongside the guest OSMutex/OSSemaphore/OSEvent structs
		// (e.g. a native std::mutex/condition_variable per primitive,
		// used to actually block the host fiber rather than a pure
		// spin/queue mechanism), that state must be added here as
		// explicit DoPOD()/DoMPTR() calls before this section can be
		// considered complete. Do not delete this marker even if the
		// body stays empty - it keeps the section list positionally
		// stable for SavestateManager's ordering.
	}
}

#include "Cafe/Savestate/SavestateSerializer_HostState.h"

namespace coreinit
{
	void Savestate_DoHostState_MEM(Serializer& s);
	void Savestate_DoHostState_VirtMem(Serializer& s);
	void Savestate_DoHostState_FS(Serializer& s);
}
namespace GX2
{
	void Savestate_DoHostState_Command(Serializer& s);
	void Savestate_DoHostState_Event(Serializer& s);
}
namespace snd_core
{
	void Savestate_DoHostState_AX(Serializer& s);
}
void Savestate_DoHostState_AsyncCallbacks(Serializer& s);
void Savestate_DoHostState_GX2ContextState(Serializer& s);

namespace SavestateSerializer_HostState
{
	void DoState(Serializer& s)
	{
		s.DoMarker("HostState-begin");
		coreinit::Savestate_DoHostState_MEM(s);
		s.DoMarker("HostState-mem");
		coreinit::Savestate_DoHostState_VirtMem(s);
		s.DoMarker("HostState-virtmem");
		coreinit::Savestate_DoHostState_FS(s);
		s.DoMarker("HostState-fs");
		Savestate_DoHostState_AsyncCallbacks(s);
		s.DoMarker("HostState-asynccb");
		GX2::Savestate_DoHostState_Command(s);
		s.DoMarker("HostState-gx2cmd");
		Savestate_DoHostState_GX2ContextState(s);
		s.DoMarker("HostState-gx2ctx");
		GX2::Savestate_DoHostState_Event(s);
		s.DoMarker("HostState-gx2event");
		snd_core::Savestate_DoHostState_AX(s);
		s.DoMarker("HostState-ax");
	}
}

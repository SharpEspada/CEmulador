#pragma once
#include "Cafe/Savestate/SavestateSerializer.h"

// SavestateSerializer_IOSU
//
// The emulated IOSU (the "IOS" side of the console: file system service,
// account service, ...) runs on host threads and keeps its state in host
// memory, so it is NOT part of the guest memory dump. What the game holds
// references to is saved/restored here:
//   - the IPC device handles the game opened (/dev/fsa, ...), with the same
//     handle values the game has stored in its memory
//   - the FSA clients (working directory)
//   - the open files and directories of the game (path, mode, position)
// Files are reopened from their path. File CONTENT is never rolled back (like
// the memory card in Dolphin): data the game wrote to disk after the
// savestate stays on disk.
namespace SavestateSerializer_IOSU
{
	// True when no IPC request is in flight. Must be checked with the PPC side stopped.
	bool IsQuiescent();

	void DoState_Save(Serializer& s);
	// throws std::runtime_error with a readable message on failure
	void DoState_Load(Serializer& s);
}

#pragma once
#include "Cafe/Savestate/SavestateSerializer.h"

// Captures / restores every currently mapped MMURange (the guest RAM).
//
// Speed design (guest RAM is ~2 GB of address space, but most of it is untouched zeros):
//   * memory is processed in 64 KiB blocks, in parallel on several host threads
//   * all-zero blocks are not stored at all (one bit in a bitmap)
//   * the snapshot is a plain uninitialized buffer, compression and disk writing are done
//     later by the caller while the emulation is already running again
//   * on load, blocks that are already zero in the live memory and zero in the savestate
//     are not touched, so untouched pages are never committed.
//
// The section is self-describing; layout is documented in the .cpp.
//
// IMPORTANT ordering requirement: Capture must run AFTER SavestateSerializer_RPL has finished
// any in-flight allocator work, so that everything written into guest RAM is captured here.
namespace SavestateSerializer_MMU
{
	// Emulation must be paused (the barrier is held).
	SavestateBuffer Capture();

	// Emulation must be paused (the barrier is held). Throws on malformed data.
	void Restore(const SavestateBuffer& section);
}

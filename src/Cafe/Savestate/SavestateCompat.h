#pragma once
// SavestateCompat.h
//
// Everything that can silently change the MEANING of a recorded
// emulation state must feed into this hash. If it doesn't, a
// savestate saved under one configuration could load "successfully"
// under a different one and produce wrong (not just crashed) behavior
// - which is worse than a refused load.

std::string SavestateCompat_GetBuildIdString();

// Feeds: active graphic pack names+versions (GraphicPack2::GetActiveRAMMappings()
// changes MMURange sizes, so graphic packs MUST be part of this - see
// MMU.cpp's memory_mapForCurrentTitle()), CPU mode (single/multicore),
// and any other setting that changes emulated behavior rather than
// just presentation (resolution/vsync/etc. do NOT need to be here).
uint64 SavestateCompat_ComputeCurrentHash();

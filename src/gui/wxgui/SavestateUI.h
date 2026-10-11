#pragma once

#include <filesystem>
#include <string>

// GUI-side entry points for the savestate feature. All of them are safe to
// call from the UI thread: the actual work runs on a short-lived worker thread
// (saving/loading stops the emulation and waits for the GPU, which must never
// block the UI thread) and the result is reported with an on-screen notification.
namespace SavestateUI
{
	constexpr int NUM_SLOTS = 10;

	void SaveToSlot(int slotIndex);   // slotIndex 1..10
	void LoadFromSlot(int slotIndex); // slotIndex 1..10
	void SaveToFile(const std::filesystem::path& path);
	void LoadFromFile(const std::filesystem::path& path);

	// "Slot 3 - 2026-10-09 23:12" or "Slot 3 - empty" (needs a running game)
	std::string GetSlotLabel(int slotIndex);
}

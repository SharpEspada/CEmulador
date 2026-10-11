#pragma once
#include <filesystem>
#include "Cafe/Savestate/SavestateFormat.h"

namespace SavestateSlots
{
	constexpr int SLOT_COUNT = 10;

	// <userdata>/savestates/<TitleID>/slot_N.cess
	// ActiveSettings::GetUserDataPath() is already used the same way by
	// MMU.cpp's memory_createDump() - reusing that exact pattern here.
	std::filesystem::path GetSlotPath(int slotIndex);

	struct SlotInfo
	{
		bool exists;
		uint64 creationTimestamp;
		std::vector<uint8> thumbnailPng; // empty if none
	};

	// Reads only the header (SavestateFile::Reader::OpenAndValidate does
	// a full read+checksum pass, which is unnecessarily expensive just
	// to populate a slot list UI) - a lighter header-only peek belongs
	// here rather than reusing the full Reader for this purpose.
	SlotInfo GetSlotInfo(int slotIndex);
}

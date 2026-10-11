#include "Cafe/Savestate/SavestateSlots.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"
#include "Common/FileStream.h"

namespace SavestateSlots
{
	std::filesystem::path GetSlotPath(int slotIndex)
	{
		cemu_assert_debug(slotIndex >= 1 && slotIndex <= SLOT_COUNT);
		uint64 titleId = CafeSystem::GetForegroundTitleId();
		return ActiveSettings::GetUserDataPath("savestates/{:016x}/slot_{}.cess", titleId, slotIndex);
	}

	SlotInfo GetSlotInfo(int slotIndex)
	{
		SlotInfo info{};
		auto path = GetSlotPath(slotIndex);

		FileStream* fs = FileStream::openFile2(path);
		if (!fs)
		{
			info.exists = false;
			return info;
		}

		SavestateFileHeader header{};
		if (fs->readData(&header, sizeof(header)) != sizeof(header) ||
			header.magic1 != SAVESTATE_MAGIC1 || header.magic2 != SAVESTATE_MAGIC2)
		{
			// Present but not a valid savestate file - treat as absent
			// rather than surfacing a confusing slot entry.
			delete fs;
			info.exists = false;
			return info;
		}

		info.exists = true;
		info.creationTimestamp = header.creationTimestamp;

		if (header.thumbnailSize > 0)
		{
			info.thumbnailPng.resize(header.thumbnailSize);
			fs->SetPosition(header.thumbnailOffset);
			fs->readData(info.thumbnailPng.data(), header.thumbnailSize);
		}

		delete fs;
		return info;
	}
}

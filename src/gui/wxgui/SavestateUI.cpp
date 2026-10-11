#include "wxgui/SavestateUI.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/Savestate/SavestateManager.h"
#include "Cafe/Savestate/SavestateSlots.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"

#include <thread>

namespace SavestateUI
{
	static std::string ResultToText(SavestateManager::Result result)
	{
		using R = SavestateManager::Result;
		switch (result)
		{
		case R::OK: return "";
		case R::Busy: return "Savestate: another save/load is in progress";
		case R::RefusedOnlineSession: return "Savestate refused: online features are enabled";
		case R::RefusedMidModuleLoad: return "Savestate refused: the game is loading a module, try again";
		case R::RefusedThreadInOSWaitCond: return "Savestate refused: a thread is in a condition wait, try again";
		case R::RefusedAlarmCallbackRunning: return "Savestate refused: an alarm callback is running, try again";
		case R::RefusedIOSUBusy: return "Savestate refused: the game is accessing files right now, try again";
		case R::FileError: return "Savestate: file error (missing or unreadable)";
		case R::IncompatibleFile: return "Savestate: file is corrupted or made with another build/settings";
		case R::WrongTitle: return "Savestate: this file belongs to another game";
		}
		return "Savestate: unknown error";
	}

	template<typename Fn>
	static void RunAsync(Fn fn, std::string successText)
	{
		if (!CafeSystem::IsTitleRunning())
		{
			LatteOverlay_pushNotification("Savestate: no game is running", 2500);
			return;
		}
		std::thread([fn = std::move(fn), successText = std::move(successText)]() {
			SavestateManager::Result result;
			try
			{
				result = fn();
			}
			catch (const std::exception& e)
			{
				// a failure while loading may leave the emulation in an inconsistent state
				LatteOverlay_pushNotification(std::string("Savestate error: ") + e.what(), 6000);
				return;
			}
			if (result == SavestateManager::Result::OK)
				LatteOverlay_pushNotification(successText, 2000);
			else
				LatteOverlay_pushNotification(ResultToText(result), 4000);
		}).detach();
	}

	void SaveToSlot(int slotIndex)
	{
		RunAsync([slotIndex]() { return SavestateManager::RequestSave(slotIndex); }, "Saved state to slot " + std::to_string(slotIndex));
	}

	void LoadFromSlot(int slotIndex)
	{
		RunAsync([slotIndex]() { return SavestateManager::RequestLoad(slotIndex); }, "Loaded state from slot " + std::to_string(slotIndex));
	}

	void SaveToFile(const std::filesystem::path& path)
	{
		RunAsync([path]() { return SavestateManager::RequestSaveToFile(path); }, "Savestate exported");
	}

	void LoadFromFile(const std::filesystem::path& path)
	{
		RunAsync([path]() { return SavestateManager::RequestLoadFromFile(path); }, "Savestate imported");
	}

	std::string GetSlotLabel(int slotIndex)
	{
		std::string label = "Slot " + std::to_string(slotIndex);
		auto info = SavestateSlots::GetSlotInfo(slotIndex);
		if (!info.exists)
			return label + " - empty";
		time_t t = (time_t)info.creationTimestamp;
		struct tm tmLocal{};
#if BOOST_OS_WINDOWS
		localtime_s(&tmLocal, &t);
#else
		localtime_r(&t, &tmLocal);
#endif
		char buf[64];
		strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tmLocal);
		return label + " - " + buf;
	}
}

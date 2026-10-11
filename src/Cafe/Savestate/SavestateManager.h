#pragma once
#include <filesystem>

// SavestateManager.h
//
// Single entry point the GUI (MainWindow.cpp / HotkeySettings.cpp)
// talks to. Owns the request queue and the save/load state machine;
// every SavestateSerializer_*.cpp module is only ever driven from here,
// in the fixed order documented in RunSaveSequence()/RunLoadSequence()
// below - that order is not arbitrary, see the ordering comments in
// SavestateSerializer_Thread.cpp and _MMU.h.

namespace SavestateManager
{
	enum class Result
	{
		OK,
		Busy,					// a save/load is already in progress
		RefusedOnlineSession,	// see spec doc §8 - never save/load while online
		RefusedMidModuleLoad,	// SavestateSerializer_RPL::CanSafelySave() returned false
		RefusedThreadInOSWaitCond, // a thread is parked in OSWaitCond - see SavestateSerializer_Thread.h, confirmed unsafe to resume
		RefusedAlarmCallbackRunning, // alarm thread is executing a guest callback, retry
		RefusedIOSUBusy,		// an IOSU request (file access...) is in flight, retry
		FileError,
		IncompatibleFile,		// SavestateFile::LoadError, see SavestateSerializer.cpp
		WrongTitle,
	};

	// Slots 1-10, or an arbitrary path for import/export (see
	// SavestateSlots.h for path resolution).
	Result RequestSave(int slotIndex);
	Result RequestSaveToFile(const std::filesystem::path& path);
	Result RequestLoad(int slotIndex);
	Result RequestLoadFromFile(const std::filesystem::path& path);

	bool IsBusy();
}

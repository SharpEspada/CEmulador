#include "Cafe/Savestate/SavestateManager.h"
#include "Cafe/Savestate/SavestateFormat.h"
#include "Cafe/Savestate/SavestateSerializer.h"
#include "Cafe/Savestate/SavestateSerializer_MMU.h"
#include "Cafe/Savestate/SavestateSerializer_RPL.h"
#include "Cafe/Savestate/SavestateSerializer_Thread.h"
#include "Cafe/Savestate/SavestateSerializer_Sync.h"
#include "Cafe/Savestate/SavestateSerializer_Alarm.h"
#include "Cafe/Savestate/SavestateSerializer_GPU.h"
#include "Cafe/Savestate/SavestateSerializer_IOSU.h"
#include "Cafe/Savestate/SavestateSlots.h"
#include "Cafe/CafeSystem.h"
#include "config/ActiveSettings.h"
#include "Cafe/HW/Latte/Core/Latte.h" // Latte_Stop/Latte_GetStopSignal

namespace SavestateManager
{
	static std::atomic<bool> s_busy{ false };

	bool IsBusy() { return s_busy.load(); }

	// ------------------------------------------------------------------
	// The barrier. Every host thread that could be mid-execution of
	// guest code must reach a safe, non-mid-syscall point before ANY
	// DoState() below touches memory.
	//
	// CPU/thread side: reuses coreinit::OSSchedulerEnd()/OSSchedulerBegin(),
	// real existing engine code (CafeSystem.cpp's _LaunchTitleThread()
	// is the only other caller). Verified against coreinit_Thread.cpp:
	// every host fiber's context is already flushed to guest memory
	// (OSContext_t, via __OSThreadStoreContext) by the time
	// OSSchedulerEnd() returns - see Cafe/Savestate/NOTE_pas_de_section_CPU.md.
	//
	// GPU side: Latte_Stop()/Latte_Start() were deliberately NOT reused
	// here - they tear down and fully reinitialize the renderer (see
	// Latte.h for the full reasoning). A separate, lightweight pause
	// was added instead (Latte_RequestPause/WaitForPause/ResumePause),
	// checked at the exact same safe idle point as the stop signal.
	// ------------------------------------------------------------------
	struct Barrier
	{
		sint32 m_numCores = 1;

		void StopAll()
		{
			// Matches the exact condition CafeSystem.cpp's
			// _LaunchTitleThread() uses for the initial OSSchedulerBegin()
			// call, captured here since it can't be queried after
			// OSSchedulerEnd() tears the scheduler state down.
			m_numCores = coreinit::__CemuIsMulticoreMode() ? 3 : 1;

			Latte_RequestPause();
			coreinit::OSSchedulerEnd();
			Latte_WaitForPause();
		}

		void ResumeAll()
		{
			coreinit::OSSchedulerBegin(m_numCores);
			SavestateSerializer_Thread::RecreateHostThreadsAfterPause();
			Latte_ResumePause();
		}
	};

	// ------------------------------------------------------------------
	// Fixed section ordering. See individual serializer files for why
	// this exact order matters - in short:
	//   RPL before MMU: RPL's reload-by-name step (load path) and any
	//     allocator bookkeeping writes must land in guest memory before
	//     the flat MMU dump is taken/restored.
	//   GPU texture readback before MMU: forces GPU-dirty texture data
	//     into guest RAM so the MMU dump captures it (spec doc §5).
	//   Sync before Thread (load path only): a waiting thread's queue
	//     link must resolve against an already-restored primitive.
	// ------------------------------------------------------------------

	static std::vector<uint8> BuildSaveSections(std::vector<std::pair<SavestateSectionId, std::vector<uint8>>>& outSections)
	{
		// 1. GPU texture readback - writes into guest RAM, must precede MMU.
		SavestateSerializer_GPU::SyncDirtyTexturesToGuestMemory();

		outSections.emplace_back(SavestateSectionId::GPU,
			Savestate_SerializeSection([](Serializer& s) { SavestateSerializer_GPU::DoState(s); }));

		outSections.emplace_back(SavestateSectionId::RPL,
			Savestate_SerializeSection([](Serializer& s) { SavestateSerializer_RPL::DoState_Save(s); }));

		// MMU last among the "writes into guest RAM" producers, first
		// among the pure "read guest RAM" consumers - captures
		// everything the two steps above just wrote.
		outSections.emplace_back(SavestateSectionId::MMU,
			Savestate_SerializeSection([](Serializer& s) { SavestateSerializer_MMU::DoState(s); }));

		outSections.emplace_back(SavestateSectionId::IOSU,
			Savestate_SerializeSection([](Serializer& s) { SavestateSerializer_IOSU::DoState_Save(s); }));

		outSections.emplace_back(SavestateSectionId::SYNC,
			Savestate_SerializeSection([](Serializer& s) { SavestateSerializer_Sync::DoState(s); }));

		outSections.emplace_back(SavestateSectionId::THREAD,
			Savestate_SerializeSection([](Serializer& s) { SavestateSerializer_Thread::DoState(s); }));

		outSections.emplace_back(SavestateSectionId::ALARM,
			Savestate_SerializeSection([](Serializer& s) { SavestateSerializer_Alarm::DoState(s); }));

		return {};
	}

	static Result DoSave(const std::filesystem::path& path)
	{
		// No live "online session active" flag exists in CafeSystem; the
		// closest available signal is whether online features are enabled
		// in settings (config/ActiveSettings.h). This is a config toggle,
		// not a live connection state - documented here as a known
		// limitation rather than a precise live check.
		if (ActiveSettings::IsOnlineEnabled())
			return Result::RefusedOnlineSession;
		if (!SavestateSerializer_RPL::CanSafelySave())
			return Result::RefusedMidModuleLoad;
		if (!SavestateSerializer_Thread::CanSafelySave())
			return Result::RefusedThreadInOSWaitCond;

		bool expected = false;
		if (!s_busy.compare_exchange_strong(expected, true))
			return Result::Busy;

		Barrier barrier;
		barrier.StopAll();

		if (!SavestateSerializer_Alarm::CanSafelySave())
		{
			barrier.ResumeAll();
			s_busy.store(false);
			return Result::RefusedAlarmCallbackRunning;
		}

		if (!SavestateSerializer_IOSU::IsQuiescent())
		{
			barrier.ResumeAll();
			s_busy.store(false);
			return Result::RefusedIOSUBusy;
		}

		std::vector<std::pair<SavestateSectionId, std::vector<uint8>>> sections;
		try
		{
			BuildSaveSections(sections);
		}
		catch (...)
		{
			// never leave the emulation frozen
			barrier.ResumeAll();
			s_busy.store(false);
			throw;
		}

		barrier.ResumeAll();
		s_busy.store(false);

		// Actual file writing happens with the emulator already
		// resumed - the barrier only needs to be held for the duration
		// of copying state out, not for the (potentially slow) disk
		// write. See spec doc's "Sauvegarde en deux temps" note.
		SavestateFile::Writer writer;
		for (auto& [id, bytes] : sections)
			writer.AddSection(id, std::move(bytes));

		std::vector<uint8> thumbnail; // TODO: capture a small PNG of the current frame here
		uint64 titleId = CafeSystem::GetForegroundTitleId();
		uint16 titleVersion = CafeSystem::GetForegroundTitleVersion();

		if (!writer.WriteToFile(path, titleId, titleVersion, thumbnail))
			return Result::FileError;
		return Result::OK;
	}

	static Result DoLoad(const std::filesystem::path& path)
	{
		if (ActiveSettings::IsOnlineEnabled())
			return Result::RefusedOnlineSession;

		bool expected = false;
		if (!s_busy.compare_exchange_strong(expected, true))
			return Result::Busy;

		SavestateFile::Reader reader;
		uint64 expectedTitleId = CafeSystem::GetForegroundTitleId();
		auto error = reader.OpenAndValidate(path, expectedTitleId);
		if (error != SavestateFile::LoadError::OK)
		{
			s_busy.store(false);
			switch (error)
			{
			case SavestateFile::LoadError::WrongTitle: return Result::WrongTitle;
			case SavestateFile::LoadError::IncompatibleBuild:
			case SavestateFile::LoadError::UnsupportedFormatVersion:
			case SavestateFile::LoadError::BadMagic: return Result::IncompatibleFile;
			default: return Result::FileError;
			}
		}

		Barrier barrier;
		barrier.StopAll();

		if (!SavestateSerializer_IOSU::IsQuiescent())
		{
			barrier.ResumeAll();
			s_busy.store(false);
			return Result::RefusedIOSUBusy;
		}

		try
		{
			// Load order: RPL (reloads modules + relinks) -> MMU (flat
			// memory dump, overwrites whatever RPL's reload just set up,
			// which is fine/expected - MMU is authoritative for final byte
			// contents) -> GPU -> Sync -> Thread (needs Sync already
			// restored, see ordering note above) -> Alarm.
			Savestate_DeserializeSection(reader.GetSection(SavestateSectionId::RPL),
				[](Serializer& s) { SavestateSerializer_RPL::DoState_Load(s); });

			Savestate_DeserializeSection(reader.GetSection(SavestateSectionId::MMU),
				[](Serializer& s) { SavestateSerializer_MMU::DoState(s); });

			// IOSU after MMU: it uses the (restored) IPC request pool in guest memory
			Savestate_DeserializeSection(reader.GetSection(SavestateSectionId::IOSU),
				[](Serializer& s) { SavestateSerializer_IOSU::DoState_Load(s); });

			Savestate_DeserializeSection(reader.GetSection(SavestateSectionId::GPU),
				[](Serializer& s) { SavestateSerializer_GPU::DoState(s); });
			SavestateSerializer_GPU::InvalidateHostCachesAfterLoad();

			Savestate_DeserializeSection(reader.GetSection(SavestateSectionId::SYNC),
				[](Serializer& s) { SavestateSerializer_Sync::DoState(s); });

			Savestate_DeserializeSection(reader.GetSection(SavestateSectionId::THREAD),
				[](Serializer& s) { SavestateSerializer_Thread::DoState(s); });

			Savestate_DeserializeSection(reader.GetSection(SavestateSectionId::ALARM),
				[](Serializer& s) { SavestateSerializer_Alarm::DoState(s); });

		}
		catch (...)
		{
			// The emulated state may be inconsistent now, but never leave the
			// emulation frozen / the manager locked: report the error instead.
			barrier.ResumeAll();
			s_busy.store(false);
			throw;
		}

		barrier.ResumeAll();
		s_busy.store(false);
		return Result::OK;
	}

	// ------------------------------------------------------------------
	// Public API
	// ------------------------------------------------------------------

	// Some refusals only mean "the game is in the middle of something right now"
	// (a file access, an alarm callback, a module being linked). They are retried
	// automatically for a short while so the user never has to press the key twice.
	static bool IsTransientRefusal(Result r)
	{
		return r == Result::RefusedMidModuleLoad || r == Result::RefusedThreadInOSWaitCond ||
			   r == Result::RefusedAlarmCallbackRunning || r == Result::RefusedIOSUBusy;
	}

	template<typename Fn>
	static Result RunWithRetry(Fn fn)
	{
		Result result = fn();
		for (int attempt = 0; attempt < 60 && IsTransientRefusal(result); attempt++)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			result = fn();
		}
		return result;
	}

	Result RequestSave(int slotIndex) { return RunWithRetry([&]() { return DoSave(SavestateSlots::GetSlotPath(slotIndex)); }); }
	Result RequestSaveToFile(const std::filesystem::path& path) { return RunWithRetry([&]() { return DoSave(path); }); }
	Result RequestLoad(int slotIndex) { return RunWithRetry([&]() { return DoLoad(SavestateSlots::GetSlotPath(slotIndex)); }); }
	Result RequestLoadFromFile(const std::filesystem::path& path) { return RunWithRetry([&]() { return DoLoad(path); }); }
}

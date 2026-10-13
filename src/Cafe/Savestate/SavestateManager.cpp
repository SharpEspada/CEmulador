#include "Cafe/Savestate/SavestateManager.h"
#include "Cafe/Savestate/SavestateFormat.h"
#include "Cafe/Savestate/SavestateFile.h"
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
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "config/ActiveSettings.h"
#include "Cafe/HW/Latte/Core/Latte.h" // Latte pause API
#include <chrono>

namespace SavestateManager
{
	static std::atomic<bool> s_busy{ false };

	bool IsBusy() { return s_busy.load(); }

	using Clock = std::chrono::steady_clock;
	static long long MsSince(Clock::time_point t)
	{
		return (long long)std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t).count();
	}
	static void Trace(const std::string& text)
	{
		cemuLog_log(LogType::Force, std::string("[Savestate] ") + text);
	}

	// Keeps s_busy set for the whole operation (including the disk write) and always clears it.
	struct BusyGuard
	{
		bool owns = false;
		BusyGuard()
		{
			bool expected = false;
			owns = s_busy.compare_exchange_strong(expected, true);
		}
		~BusyGuard()
		{
			if (owns)
				s_busy.store(false);
		}
	};

	// ------------------------------------------------------------------
	// The barrier. Every host thread that could be mid-execution of
	// guest code must reach a safe, non-mid-syscall point before ANY
	// state is read or written.
	//
	// CPU side: coreinit::OSSchedulerEnd()/OSSchedulerBegin(). After End returns,
	// every guest thread's context is stored in guest memory.
	//
	// GPU side: a lightweight pause (Latte_RequestPause/WaitForPause/ResumePause), reached
	// when the command processor is idle. The wait has a timeout so that a GPU thread that
	// cannot become idle never freezes the emulator: the request is refused and retried.
	//
	// Resume order matters: the host fibers of the guest threads and the run-queue
	// counters must be rebuilt BEFORE the scheduler threads start, otherwise a scheduler
	// thread could pick a guest thread that has no fiber yet.
	// ------------------------------------------------------------------
	struct Barrier
	{
		sint32 m_numCores = 1;
		bool m_stopped = false;

		// returns false if the GPU did not park in time (call ResumeAll() then)
		bool StopAll()
		{
			m_numCores = coreinit::__CemuIsMulticoreMode() ? 3 : 1;
			Latte_RequestPause();
			coreinit::OSSchedulerEnd();
			m_stopped = true;
			return Latte_WaitForPauseWithTimeout(4000);
		}

		void ResumeAll()
		{
			if (!m_stopped)
				return;
			m_stopped = false;
			SavestateSerializer_Thread::RecreateHostThreadsAfterPause();
			coreinit::OSSchedulerBegin(m_numCores);
			Latte_ResumePause();
		}
	};

	struct SectionBuffers
	{
		SavestateBuffer rpl, mmu, iosu, gpu, sync, thread, alarm;
	};

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

		BusyGuard busy;
		if (!busy.owns)
			return Result::Busy;

		auto tStart = Clock::now();
		SavestateSerializer_GPU::SyncDirtyTexturesToGuestMemory();

		Barrier barrier;
		bool gpuParked = barrier.StopAll();
		auto tPaused = Clock::now();
		if (!gpuParked)
		{
			barrier.ResumeAll();
			return Result::RefusedGPUBusy;
		}
		if (!SavestateSerializer_Alarm::CanSafelySave())
		{
			barrier.ResumeAll();
			return Result::RefusedAlarmCallbackRunning;
		}
		if (!SavestateSerializer_IOSU::IsQuiescent())
		{
			barrier.ResumeAll();
			return Result::RefusedIOSUBusy;
		}

		// 1) Copy everything out while the game is paused. This is the only part that
		//    holds the game: no compression and no disk access happen here.
		SavestateFile::Writer writer;
		try
		{
			auto tSection = Clock::now();
			writer.AddSection(SavestateSectionId::GPU,
				Savestate_SerializeSection([](Serializer& s) { SavestateSerializer_GPU::DoState(s); }));
			writer.AddSection(SavestateSectionId::RPL,
				Savestate_SerializeSection([](Serializer& s) { SavestateSerializer_RPL::DoState_Save(s); }));
			Trace(fmt::format("save: modules/GPU state captured ({} ms)", MsSince(tSection)));

			tSection = Clock::now();
			SavestateBuffer mmu = SavestateSerializer_MMU::Capture();
			Trace(fmt::format("save: RAM captured, {} MiB kept ({} ms)", mmu.size / (1024 * 1024), MsSince(tSection)));
			writer.AddSection(SavestateSectionId::MMU, std::move(mmu));

			writer.AddSection(SavestateSectionId::IOSU,
				Savestate_SerializeSection([](Serializer& s) { SavestateSerializer_IOSU::DoState_Save(s); }));
			writer.AddSection(SavestateSectionId::SYNC,
				Savestate_SerializeSection([](Serializer& s) { SavestateSerializer_Sync::DoState(s); }));
			writer.AddSection(SavestateSectionId::THREAD,
				Savestate_SerializeSection([](Serializer& s) { SavestateSerializer_Thread::DoState(s); }));
			writer.AddSection(SavestateSectionId::ALARM,
				Savestate_SerializeSection([](Serializer& s) { SavestateSerializer_Alarm::DoState(s); }));
		}
		catch (...)
		{
			// never leave the emulation frozen
			barrier.ResumeAll();
			throw;
		}

		auto tResume = Clock::now();
		barrier.ResumeAll();
		Trace(fmt::format("save: game was paused {} ms in total (resume took {} ms)", MsSince(tPaused), MsSince(tResume)));

		// 2) Compression and disk write run with the game already running again.
		auto tWrite = Clock::now();
		std::vector<uint8> thumbnail; // TODO: capture a small PNG of the current frame here
		uint64 titleId = CafeSystem::GetForegroundTitleId();
		uint16 titleVersion = CafeSystem::GetForegroundTitleVersion();
		bool ok = writer.WriteToFile(path, titleId, titleVersion, thumbnail);
		Trace(fmt::format("save: file written in background ({} ms), total {} ms", MsSince(tWrite), MsSince(tStart)));
		return ok ? Result::OK : Result::FileError;
	}

	static Result DoLoad(const std::filesystem::path& path)
	{
		if (ActiveSettings::IsOnlineEnabled())
			return Result::RefusedOnlineSession;

		BusyGuard busy;
		if (!busy.owns)
			return Result::Busy;

		auto tStart = Clock::now();

		// Everything that does not need the game to be paused is done first, while the
		// game keeps running: read the file, verify it, decompress all sections.
		SavestateFile::Reader reader;
		uint64 expectedTitleId = CafeSystem::GetForegroundTitleId();
		auto error = reader.OpenAndValidate(path, expectedTitleId);
		if (error != SavestateFile::LoadError::OK)
		{
			switch (error)
			{
			case SavestateFile::LoadError::WrongTitle: return Result::WrongTitle;
			case SavestateFile::LoadError::IncompatibleBuild:
			case SavestateFile::LoadError::UnsupportedFormatVersion:
			case SavestateFile::LoadError::BadMagic: return Result::IncompatibleFile;
			default: return Result::FileError;
			}
		}
		SectionBuffers sec;
		try
		{
			sec.rpl = reader.GetSection(SavestateSectionId::RPL);
			sec.mmu = reader.GetSection(SavestateSectionId::MMU);
			sec.iosu = reader.GetSection(SavestateSectionId::IOSU);
			sec.gpu = reader.GetSection(SavestateSectionId::GPU);
			sec.sync = reader.GetSection(SavestateSectionId::SYNC);
			sec.thread = reader.GetSection(SavestateSectionId::THREAD);
			sec.alarm = reader.GetSection(SavestateSectionId::ALARM);
		}
		catch (const std::exception&)
		{
			return Result::FileError;
		}
		Trace(fmt::format("load: file read and decompressed while the game was still running ({} ms)", MsSince(tStart)));

		Barrier barrier;
		bool gpuParked = barrier.StopAll();
		auto tPaused = Clock::now();
		if (!gpuParked)
		{
			barrier.ResumeAll();
			return Result::RefusedGPUBusy;
		}
		if (!SavestateSerializer_IOSU::IsQuiescent())
		{
			barrier.ResumeAll();
			return Result::RefusedIOSUBusy;
		}

		try
		{
			// RPL first: it verifies that the running game has the same modules and
			// refuses cleanly BEFORE anything is overwritten.
			auto tSection = Clock::now();
			Savestate_DeserializeSection(sec.rpl, [](Serializer& s) { SavestateSerializer_RPL::DoState_Load(s); });
			Trace(fmt::format("load: modules checked ({} ms)", MsSince(tSection)));

			tSection = Clock::now();
			SavestateSerializer_MMU::Restore(sec.mmu);
			Trace(fmt::format("load: RAM restored ({} ms)", MsSince(tSection)));

			// IOSU after MMU: it uses the (restored) IPC request pool in guest memory
			tSection = Clock::now();
			Savestate_DeserializeSection(sec.iosu, [](Serializer& s) { SavestateSerializer_IOSU::DoState_Load(s); });
			Trace(fmt::format("load: IOSU/file handles restored ({} ms)", MsSince(tSection)));

			tSection = Clock::now();
			Savestate_DeserializeSection(sec.gpu, [](Serializer& s) { SavestateSerializer_GPU::DoState(s); });
			SavestateSerializer_GPU::InvalidateHostCachesAfterLoad();
			Trace(fmt::format("load: GPU state restored, caches dropped ({} ms)", MsSince(tSection)));

			Savestate_DeserializeSection(sec.sync, [](Serializer& s) { SavestateSerializer_Sync::DoState(s); });
			Savestate_DeserializeSection(sec.thread, [](Serializer& s) { SavestateSerializer_Thread::DoState(s); });
			Savestate_DeserializeSection(sec.alarm, [](Serializer& s) { SavestateSerializer_Alarm::DoState(s); });
		}
		catch (const std::exception& e)
		{
			// The emulated state may be inconsistent now, but never leave the
			// emulation frozen / the manager locked: report the error instead.
			Trace(std::string("load FAILED: ") + e.what());
			barrier.ResumeAll();
			throw;
		}
		catch (...)
		{
			barrier.ResumeAll();
			throw;
		}

		auto tResume = Clock::now();
		barrier.ResumeAll();
		Trace(fmt::format("load: game was paused {} ms in total (resume took {} ms), total {} ms",
			MsSince(tPaused), MsSince(tResume), MsSince(tStart)));
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
			   r == Result::RefusedAlarmCallbackRunning || r == Result::RefusedIOSUBusy ||
			   r == Result::RefusedGPUBusy;
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

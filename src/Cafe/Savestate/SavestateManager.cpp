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
#include "Cafe/Savestate/SavestateSerializer_HostState.h"
#include "Cafe/Savestate/SavestateSlots.h"
#include "Cafe/CafeSystem.h"
#include "Cafe/HW/Espresso/Recompiler/PPCRecompiler.h"
#include "Cafe/HW/MMU/MMU.h"
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
		SavestateBuffer rpl, mmu, iosu, gpu, sync, thread, alarm, hostState;

		void Assign(SavestateSectionId id, SavestateBuffer&& b)
		{
			switch (id)
			{
			case SavestateSectionId::RPL: rpl = std::move(b); break;
			case SavestateSectionId::MMU: mmu = std::move(b); break;
			case SavestateSectionId::IOSU: iosu = std::move(b); break;
			case SavestateSectionId::GPU: gpu = std::move(b); break;
			case SavestateSectionId::SYNC: sync = std::move(b); break;
			case SavestateSectionId::THREAD: thread = std::move(b); break;
			case SavestateSectionId::ALARM: alarm = std::move(b); break;
			case SavestateSectionId::HOSTSTATE: hostState = std::move(b); break;
			default: break;
			}
		}
	};

	// The most recently saved or loaded state is kept (uncompressed) in RAM, like Dolphin does,
	// so that loading it again does not have to read and decompress the file. It is only used
	// if the file on disk is still exactly the one it was made from (same size and time stamp).
	// Only touched while s_busy is held, so no extra locking is needed.
	struct StateCache
	{
		bool valid = false;
		std::string pathKey;
		uint64 titleId = 0;
		uint64 fileSize = 0;
		std::filesystem::file_time_type fileTime{};
		SectionBuffers sec;
	};
	static StateCache s_cache;

	static std::string PathKey(const std::filesystem::path& p)
	{
		std::error_code ec;
		std::filesystem::path abs = std::filesystem::absolute(p, ec);
		if (ec)
			abs = p;
		auto u8 = abs.generic_u8string();
		return std::string(u8.begin(), u8.end());
	}

	static bool GetFileStamp(const std::filesystem::path& p, uint64& sizeOut, std::filesystem::file_time_type& timeOut)
	{
		std::error_code ec;
		auto size = std::filesystem::file_size(p, ec);
		if (ec)
			return false;
		auto t = std::filesystem::last_write_time(p, ec);
		if (ec)
			return false;
		sizeOut = (uint64)size;
		timeOut = t;
		return true;
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
		if (!SavestateSerializer_Alarm::CanSafelySave() || coreinit::Savestate_WasCallbackNestedAtStop())
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
			writer.AddSection(SavestateSectionId::HOSTSTATE,
				Savestate_SerializeSection([](Serializer& s) { SavestateSerializer_HostState::DoState(s); }));
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
		writer.SetKeepRawSections(true);
		bool ok = writer.WriteToFile(path, titleId, titleVersion, thumbnail);
		if (ok)
		{
			uint64 stampSize = 0;
			std::filesystem::file_time_type stampTime{};
			s_cache.valid = false;
			s_cache.sec = SectionBuffers();
			if (GetFileStamp(path, stampSize, stampTime))
			{
				for (auto& [id, buf] : writer.TakeKeptSections())
					s_cache.sec.Assign(id, std::move(buf));
				s_cache.pathKey = PathKey(path);
				s_cache.titleId = titleId;
				s_cache.fileSize = stampSize;
				s_cache.fileTime = stampTime;
				s_cache.valid = true;
			}
		}
		Trace(fmt::format("save: file written in background ({} ms), total {} ms", MsSince(tWrite), MsSince(tStart)));
		return ok ? Result::OK : Result::FileError;
	}

	static Result ApplySections(const SectionBuffers& sec, Clock::time_point tStart)
	{
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
			if (SavestateSerializer_MMU::Restore(sec.mmu))
			{
				PPCRecompiler_invalidateRange(MEMORY_CODEAREA_ADDR, MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
				PPCRecompiler_invalidateRange(MEMORY_CODE_TRAMPOLINE_AREA_ADDR, MEMORY_CODE_TRAMPOLINE_AREA_ADDR + MEMORY_CODE_TRAMPOLINE_AREA_SIZE);
				Trace("load: code area differed, compiled code dropped");
			}
			Trace(fmt::format("load: RAM restored ({} ms)", MsSince(tSection)));

			// IOSU after MMU: it uses the (restored) IPC request pool in guest memory
			tSection = Clock::now();
			Savestate_DeserializeSection(sec.iosu, [](Serializer& s) { SavestateSerializer_IOSU::DoState_Load(s); });
			Trace(fmt::format("load: IOSU/file handles restored ({} ms)", MsSince(tSection)));

			Savestate_DeserializeSection(sec.hostState, [](Serializer& s) { SavestateSerializer_HostState::DoState(s); });
			Trace("load: HLE host state restored");

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

	static bool IsTransientRefusal(Result r);

	static const char* RefusalName(Result r)
	{
		switch (r)
		{
		case Result::RefusedMidModuleLoad: return "a module is being loaded";
		case Result::RefusedThreadInOSWaitCond: return "a thread is in a condition wait";
		case Result::RefusedAlarmCallbackRunning: return "a callback is running";
		case Result::RefusedIOSUBusy: return "a file/IOSU request is in flight";
		case Result::RefusedGPUBusy: return "the GPU did not become idle in time";
		default: return "other";
		}
	}

	static Result DoLoad(const std::filesystem::path& path)
	{
		if (ActiveSettings::IsOnlineEnabled())
			return Result::RefusedOnlineSession;

		BusyGuard busy;
		if (!busy.owns)
			return Result::Busy;

		auto tStart = Clock::now();
		uint64 expectedTitleId = CafeSystem::GetForegroundTitleId();

		// Fast path: the most recent state that was saved/loaded in this session is still in RAM.
		SectionBuffers decoded;
		const SectionBuffers* sections = nullptr;
		bool fromCache = false;
		uint64 stampSize = 0;
		std::filesystem::file_time_type stampTime{};
		bool haveStamp = GetFileStamp(path, stampSize, stampTime);
		if (haveStamp && s_cache.valid && s_cache.titleId == expectedTitleId && s_cache.pathKey == PathKey(path) &&
			s_cache.fileSize == stampSize && s_cache.fileTime == stampTime)
		{
			sections = &s_cache.sec;
			fromCache = true;
			Trace("load: state found in RAM, no need to read the file");
		}
		else
		{
			// Everything that does not need the game to be paused is done first, while the
			// game keeps running: read the file, verify it, decompress all sections. Done ONCE,
			// even if the pause below has to be retried.
			SavestateFile::Reader reader;
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
			Trace(fmt::format("load: file read and verified ({} ms)", MsSince(tStart)));
			try
			{
				decoded.rpl = reader.GetSection(SavestateSectionId::RPL);
				decoded.mmu = reader.GetSection(SavestateSectionId::MMU);
				decoded.iosu = reader.GetSection(SavestateSectionId::IOSU);
				decoded.gpu = reader.GetSection(SavestateSectionId::GPU);
				decoded.sync = reader.GetSection(SavestateSectionId::SYNC);
				decoded.thread = reader.GetSection(SavestateSectionId::THREAD);
				decoded.alarm = reader.GetSection(SavestateSectionId::ALARM);
				decoded.hostState = reader.GetSection(SavestateSectionId::HOSTSTATE);
			}
			catch (const std::exception&)
			{
				return Result::FileError;
			}
			Trace(fmt::format("load: decompressed while the game was still running ({} ms)", MsSince(tStart)));
			sections = &decoded;
		}

		Result result = Result::OK;
		for (int attempt = 0; attempt < 60; attempt++)
		{
			result = ApplySections(*sections, tStart);
			if (!IsTransientRefusal(result))
				break;
			Trace(fmt::format("load: not possible right now ({}), trying again (attempt {})", RefusalName(result), attempt + 1));
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}

		if (result == Result::OK && !fromCache && haveStamp)
		{
			// keep this state in RAM: loading it again will be almost instant
			s_cache.valid = false;
			s_cache.sec = std::move(decoded);
			s_cache.pathKey = PathKey(path);
			s_cache.titleId = expectedTitleId;
			s_cache.fileSize = stampSize;
			s_cache.fileTime = stampTime;
			s_cache.valid = true;
		}
		return result;
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
	Result RequestLoad(int slotIndex) { return DoLoad(SavestateSlots::GetSlotPath(slotIndex)); }
	Result RequestLoadFromFile(const std::filesystem::path& path) { return DoLoad(path); }
}

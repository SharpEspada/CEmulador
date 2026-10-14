#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "util/helpers/fspinlock.h"
#include "Cafe/Savestate/SavestateSerializer.h"

struct CoreinitAsyncCallback
{
	CoreinitAsyncCallback(MPTR functionMPTR, uint32 numParameters, uint32 r3, uint32 r4, uint32 r5, uint32 r6, uint32 r7, uint32 r8, uint32 r9, uint32 r10) :
		m_functionMPTR(functionMPTR), m_numParameters(numParameters), m_gprParam{ r3, r4, r5, r6, r7, r8, r9, r10 } {};

	static void queue(MPTR functionMPTR, uint32 numParameters, uint32 r3, uint32 r4, uint32 r5, uint32 r6, uint32 r7, uint32 r8, uint32 r9, uint32 r10)
	{
		s_asyncCallbackSpinlock.lock();
		s_asyncCallbackQueue.emplace_back(allocateAndInitFromPool(functionMPTR, numParameters, r3, r4, r5, r6, r7, r8, r9, r10));
		s_asyncCallbackSpinlock.unlock();
	}

	static void callNextFromQueue()
	{
		s_asyncCallbackSpinlock.lock();
		if (s_asyncCallbackQueue.empty())
		{
			cemuLog_log(LogType::Force, "AsyncCallbackQueue is empty. Unexpected behavior");
			s_asyncCallbackSpinlock.unlock();
			return;
		}
		CoreinitAsyncCallback* cb = s_asyncCallbackQueue[0];
		s_asyncCallbackQueue.erase(s_asyncCallbackQueue.begin());
		s_asyncCallbackSpinlock.unlock();
		cb->doCall();
		s_asyncCallbackSpinlock.lock();
		releaseToPool(cb);
		s_asyncCallbackSpinlock.unlock();
	}

	// Savestate support: the queue of pending callbacks is host memory; the semaphore counting
	// them is guest memory. Both have to describe the same moment.
	static void savestateDoState(Serializer& s)
	{
		s_asyncCallbackSpinlock.lock();
		s.DoMarker("coreinitAsyncCb");
		if (!s.IsReading())
		{
			uint32 count = (uint32)s_asyncCallbackQueue.size();
			s.DoPOD(count);
			for (CoreinitAsyncCallback* cb : s_asyncCallbackQueue)
			{
				s.DoPOD(cb->m_functionMPTR);
				s.DoPOD(cb->m_numParameters);
				s.DoPODArray(cb->m_gprParam, 9);
			}
			s_asyncCallbackSpinlock.unlock();
			return;
		}
		uint32 count = 0;
		s.DoPOD(count);
		if (count > 100000)
		{
			s_asyncCallbackSpinlock.unlock();
			throw std::runtime_error("Savestate: invalid callback queue");
		}
		// drop whatever is queued right now
		for (CoreinitAsyncCallback* cb : s_asyncCallbackQueue)
			releaseToPool(cb);
		s_asyncCallbackQueue.clear();
		for (uint32 i = 0; i < count; i++)
		{
			MPTR fn = 0;
			sint32 numParameters = 0;
			uint32 gpr[9] = {};
			s.DoPOD(fn);
			s.DoPOD(numParameters);
			s.DoPODArray(gpr, 9);
			CoreinitAsyncCallback* cb = allocateAndInitFromPool(fn, (uint32)numParameters, gpr[0], gpr[1], gpr[2], gpr[3], gpr[4], gpr[5], gpr[6], gpr[7]);
			cb->m_gprParam[8] = gpr[8];
			s_asyncCallbackQueue.emplace_back(cb);
		}
		s_asyncCallbackSpinlock.unlock();
	}

private:
	void doCall()
	{
		PPCCoreCallback(m_functionMPTR, m_gprParam[0], m_gprParam[1], m_gprParam[2], m_gprParam[3], m_gprParam[4], m_gprParam[5], m_gprParam[6], m_gprParam[7]);
	}
	
	static CoreinitAsyncCallback* allocateAndInitFromPool(MPTR functionMPTR, uint32 numParameters, uint32 r3, uint32 r4, uint32 r5, uint32 r6, uint32 r7, uint32 r8, uint32 r9, uint32 r10)
	{
		cemu_assert_debug(s_asyncCallbackSpinlock.is_locked());
		if (s_asyncCallbackPool.empty())
		{
			CoreinitAsyncCallback* cb = new CoreinitAsyncCallback(functionMPTR, numParameters, r3, r4, r5, r6, r7, r8, r9, r10);
			return cb;
		}
		CoreinitAsyncCallback* cb = s_asyncCallbackPool[0];
		s_asyncCallbackPool.erase(s_asyncCallbackPool.begin());

		*cb = CoreinitAsyncCallback(functionMPTR, numParameters, r3, r4, r5, r6, r7, r8, r9, r10);
		return cb;
	}

	static void releaseToPool(CoreinitAsyncCallback* cb)
	{
		cemu_assert_debug(s_asyncCallbackSpinlock.is_locked());
		s_asyncCallbackPool.emplace_back(cb);
	}

	static std::vector<struct CoreinitAsyncCallback*> s_asyncCallbackPool;
	static std::vector<struct CoreinitAsyncCallback*> s_asyncCallbackQueue;
	static FSpinlock s_asyncCallbackSpinlock;

	sint32	m_numParameters;
	uint32	m_gprParam[9];
	MPTR	m_functionMPTR;
};

std::vector<struct CoreinitAsyncCallback*> CoreinitAsyncCallback::s_asyncCallbackPool;
std::vector<struct CoreinitAsyncCallback*> CoreinitAsyncCallback::s_asyncCallbackQueue;
FSpinlock CoreinitAsyncCallback::s_asyncCallbackSpinlock;

SysAllocator<OSThread_t> g_coreinitCallbackThread;
SysAllocator<uint8, 1024*64> _g_coreinitCallbackThreadStack;
SysAllocator<coreinit::OSSemaphore> g_asyncCallbackAsync;
SysAllocator<char, 32> _g_coreinitCBThreadName;

void _coreinitCallbackThread(PPCInterpreter_t* hCPU)
{
	while (coreinit::OSWaitSemaphore(g_asyncCallbackAsync.GetPtr()))
	{
		CoreinitAsyncCallback::callNextFromQueue();
	}
}

void coreinitAsyncCallback_addWithLock(MPTR functionMPTR, uint32 numParameters, uint32 r3, uint32 r4, uint32 r5, uint32 r6, uint32 r7, uint32 r8, uint32 r9, uint32 r10)
{
	cemu_assert_debug(numParameters <= 8);
	if (functionMPTR >= 0x10000000)
	{
		cemuLog_log(LogType::Force, fmt::format("Suspicious callback address {0:08x} params: {1:08x} {2:08x} {3:08x} {4:08x}", functionMPTR, r3, r4, r5, r6));
		cemuLog_waitForFlush();
	}
	CoreinitAsyncCallback::queue(functionMPTR, numParameters, r3, r4, r5, r6, r7, r8, r9, r10);
	__OSLockScheduler();
	coreinit::OSSignalSemaphoreInternal(g_asyncCallbackAsync.GetPtr(), false);
	__OSUnlockScheduler();
}

void coreinitAsyncCallback_add(MPTR functionMPTR, uint32 numParameters, uint32 r3, uint32 r4, uint32 r5, uint32 r6, uint32 r7, uint32 r8, uint32 r9, uint32 r10)
{
	cemu_assert_debug(__OSHasSchedulerLock() == false); // do not call when holding scheduler lock
	coreinitAsyncCallback_addWithLock(functionMPTR, numParameters, r3, r4, r5, r6, r7, r8, r9, r10);
}

void InitializeAsyncCallback()
{
	coreinit::OSInitSemaphore(g_asyncCallbackAsync.GetPtr(), 0);

	coreinit::OSCreateThreadType(g_coreinitCallbackThread.GetPtr(), PPCInterpreter_makeCallableExportDepr(_coreinitCallbackThread), 0, nullptr, _g_coreinitCallbackThreadStack.GetPtr() + _g_coreinitCallbackThreadStack.GetByteSize(), (sint32)_g_coreinitCallbackThreadStack.GetByteSize(), 0, 7, OSThread_t::THREAD_TYPE::TYPE_IO);
	coreinit::OSResumeThread(g_coreinitCallbackThread.GetPtr());

	strcpy(_g_coreinitCBThreadName.GetPtr(), "Callback Thread");
	coreinit::OSSetThreadName(g_coreinitCallbackThread.GetPtr(), _g_coreinitCBThreadName.GetPtr());
}

void Savestate_DoHostState_AsyncCallbacks(Serializer& s)
{
	CoreinitAsyncCallback::savestateDoState(s);
}

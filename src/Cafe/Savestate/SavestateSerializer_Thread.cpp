#include "Cafe/Savestate/SavestateSerializer_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_Thread.h"
#include "Cafe/OS/libs/coreinit/coreinit_ThreadQueue.h"
#include "Cafe/HW/MMU/MMU.h"

// Requires PATCH_coreinit_Thread.txt applied.

namespace SavestateSerializer_Thread
{
	// Verified against coreinit_Thread.h struct layouts - see header
	// comment for the derivation.
	constexpr uint32 OSCOND_MAGIC = 0x634e6456;
	constexpr uint32 OSCOND_THREADQUEUE_OFFSET = 0x0C;

	static bool IsWaitingOnOSCond(OSThread_t* thread)
	{
		if (thread->state != OSThread_t::THREAD_STATE::STATE_WAITING)
			return false;
		coreinit::OSThreadQueueInternal* queue = thread->currentWaitQueue.GetPtr();
		if (queue == nullptr)
			return false;
		MPTR queueAddr = memory_getVirtualOffsetFromPointer(queue);
		if (queueAddr < OSCOND_THREADQUEUE_OFFSET)
			return false; // underflow guard, should never happen for a real OSCond
		MPTR containerAddr = queueAddr - OSCOND_THREADQUEUE_OFFSET;
		// OSMutex shares the same +0x0C offset - reading magic here is
		// exactly what disambiguates the two. OSEvent/OSSemaphore use
		// +0x10 and so can never produce a false OSCOND_MAGIC match at
		// this offset by construction.
		uint32 magic = memory_readU32(containerAddr);
		return magic == OSCOND_MAGIC;
	}

	bool CanSafelySave()
	{
		if (OSWAITCOND_PATCH_APPLIED)
			return true; // OSWaitCond is now idempotent, nothing to refuse

		for (MPTR threadAddr : coreinit::Savestate_GetActiveThreadList())
		{
			OSThread_t* thread = (OSThread_t*)memory_getPointerFromVirtualOffset(threadAddr);
			if (IsWaitingOnOSCond(thread))
				return false;
		}
		return true;
	}

	void DoState(Serializer& s)
	{
		s.DoMarker("Thread");
		if (s.IsWriting() || s.IsMeasuring())
		{
			uint32 count = (uint32)coreinit::Savestate_GetActiveThreadList().size();
			s.DoPOD(count);
			return;
		}
		uint32 expectedCount = 0;
		s.DoPOD(expectedCount);
		uint32 actualCount = (uint32)coreinit::Savestate_GetActiveThreadList().size();
		if (expectedCount != actualCount)
			throw SavestateDesyncException("Thread:activeCountMismatch", "activeCountMismatch");
	}

	void RecreateHostThreadsAfterPause()
	{
		__OSLockScheduler();

		for (MPTR threadAddr : coreinit::Savestate_GetActiveThreadList())
		{
			OSThread_t* thread = (OSThread_t*)memory_getPointerFromVirtualOffset(threadAddr);
			if (thread->state == OSThread_t::THREAD_STATE::STATE_WAITING)
			{
				// Without the engine patch, CanSafelySave() already
				// refused any file containing a thread parked in
				// OSWaitCond, so this can only be reached for the four
				// verified-safe cases (mutex/event/semaphore/join).
				// WITH the patch applied, OSWaitCond is itself now
				// idempotent (guarded on mutex->owner), so an OSCond
				// wait reaching here is expected and handled identically
				// to the others - no special-casing needed on this side
				// at all.
				cemu_assert_debug(OSWAITCOND_PATCH_APPLIED || !IsWaitingOnOSCond(thread));

				coreinit::OSThreadQueueInternal* queue = thread->currentWaitQueue.GetPtr();
				if (queue != nullptr)
					queue->cancelWait(thread);
				else
					thread->state = OSThread_t::THREAD_STATE::STATE_READY;
				thread->waitingForMutex = nullptr;
				coreinit::__OSAddReadyThreadToRunQueue(thread);
			}
		}

		coreinit::Savestate_RecreateHostThreadsForActiveThreads();

		__OSUnlockScheduler();
	}
}

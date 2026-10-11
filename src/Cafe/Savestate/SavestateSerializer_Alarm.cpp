#include "Cafe/Savestate/SavestateSerializer_Alarm.h"
#include "Cafe/OS/libs/coreinit/coreinit_Alarm.h"
#include "Cafe/HW/Espresso/PPCState.h"

namespace SavestateSerializer_Alarm
{
	bool CanSafelySave()
	{
		// The alarm thread is an HLE host loop (_OSAlarmThread). Recreating its
		// fiber restarts the loop from the top, which is only correct while it
		// is waiting for the alarm event, not while a guest callback runs.
		// Must be called with the scheduler stopped.
		return coreinit::Savestate_IsAlarmThreadIdle();
	}

	// Section content: guest time base + host-tracked active guest alarms.
	// OSAlarm_t itself is guest memory (MMU section). Timeouts of blocked
	// calls (OSSleepTicks, OSWaitEventWithTimeout) are recreated with their
	// full duration when the blocked call is replayed.
	void DoState(Serializer& s)
	{
		s.DoMarker("Alarm");

		uint64 tickSummary = 0;
		std::vector<coreinit::AlarmSavestateRecord> alarms;
		if (!s.IsReading())
		{
			tickSummary = PPCTimer_getFromRDTSC();
			alarms = coreinit::Savestate_GetActiveAlarms();
		}
		s.DoPOD(tickSummary);
		s.DoVector(alarms);
		if (s.IsReading())
		{
			PPCTimer_setTickSummaryForSavestate(tickSummary);
			coreinit::Savestate_ResetAndRestoreAlarms(alarms);
		}
	}
}

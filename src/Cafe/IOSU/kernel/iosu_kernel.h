#pragma once
#include "Cafe/IOSU/iosu_ipc_common.h"
#include "Cafe/IOSU/iosu_types_common.h"

namespace iosu
{
	namespace kernel
	{
		using IOSMessage = uint32;

		IOSMsgQueueId IOS_CreateMessageQueue(IOSMessage* messageArray, uint32 messageCount);
		IOS_ERROR IOS_DestroyMessageQueue(IOSMsgQueueId msgQueueId);
		IOS_ERROR IOS_SendMessage(IOSMsgQueueId msgQueueId, IOSMessage message, uint32 flags);
		IOS_ERROR IOS_ReceiveMessage(IOSMsgQueueId msgQueueId, IOSMessage* messageOut, uint32 flags);

		IOS_ERROR IOS_CreateTimer(uint32 startMicroseconds, uint32 repeatMicroseconds, uint32 queueId, uint32 message);
		IOS_ERROR IOS_StopTimer(IOSTimerId timerId);
		IOS_ERROR IOS_DestroyTimer(IOSTimerId timerId);

		IOS_ERROR IOS_RegisterResourceManager(const char* devicePath, IOSMsgQueueId msgQueueId);
		IOS_ERROR IOS_DeviceAssociateId(const char* devicePath, uint32 id);
		IOS_ERROR IOS_ResourceReply(IPCCommandBody* cmd, IOS_ERROR result);

		void IPCSubmitFromCOS(uint32 ppcCoreIndex, IPCCommandBody* cmd);

		// ---- savestate support (see Cafe/Savestate/SavestateSerializer_IOSU.cpp) ----
		// True if no IPC request is currently being processed by any IOSU module.
		bool Savestate_IsQuiescent();

		struct SavestateHandleRecord
		{
			uint32 index;
			uint32 handleCheckValue;
			uint32 targetHandle; // handle inside the device's resource manager
			uint32 hasTarget;
			char path[64];
		};
		struct SavestateHandleRemap
		{
			std::string path;
			uint32 oldTarget;
			uint32 newTarget;
		};
		void Savestate_GetActiveHandles(std::vector<SavestateHandleRecord>& recordsOut, uint32& handleCounterOut);
		// Closes all open device handles and reopens the saved ones (same handle values the
		// guest holds, new device-side handles, reported in remapOut). Returns false and sets
		// errorOut on failure. Requires quiescence and a stopped PPC side.
		bool Savestate_RestoreActiveHandles(const std::vector<SavestateHandleRecord>& records, uint32 handleCounter, std::vector<SavestateHandleRemap>& remapOut, std::string& errorOut);

		IOSUModule* GetModule();
	}
}
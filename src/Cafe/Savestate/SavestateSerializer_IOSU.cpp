#include "Cafe/Savestate/SavestateSerializer_IOSU.h"
#include "Cafe/IOSU/kernel/iosu_kernel.h"
#include "Cafe/IOSU/fsa/iosu_fsa.h"

namespace SavestateSerializer_IOSU
{
	bool IsQuiescent()
	{
		return iosu::kernel::Savestate_IsQuiescent();
	}

	static void DoFileEntries(Serializer& s, std::vector<iosu::fsa::SavestateFileEntry>& entries)
	{
		uint32 count = (uint32)entries.size();
		s.DoPOD(count);
		if (s.IsReading())
			entries.resize(count);
		for (auto& e : entries)
		{
			s.DoPOD(e.index);
			s.DoPOD(e.checkValue);
			s.DoPOD(e.accessFlags);
			s.DoPOD(e.position);
			s.DoString(e.path);
		}
	}

	static void DoClientEntries(Serializer& s, std::vector<iosu::fsa::SavestateClientEntry>& entries)
	{
		uint32 count = (uint32)entries.size();
		s.DoPOD(count);
		if (s.IsReading())
			entries.resize(count);
		for (auto& e : entries)
		{
			s.DoPOD(e.clientIndex);
			s.DoString(e.workingDirectory);
		}
	}

	// symmetric: used for measuring, writing and reading
	static void DoAll(Serializer& s, std::vector<iosu::kernel::SavestateHandleRecord>& handles, uint32& handleCounter, iosu::fsa::SavestateData& fsaData)
	{
		s.DoMarker("IOSU");
		s.DoVector(handles);
		s.DoPOD(handleCounter);
		s.DoPOD(fsaData.fileCounter);
		s.DoPOD(fsaData.dirCounter);
		DoFileEntries(s, fsaData.files);
		DoFileEntries(s, fsaData.dirs);
		DoClientEntries(s, fsaData.clients);
	}

	void DoState_Save(Serializer& s)
	{
		std::vector<iosu::kernel::SavestateHandleRecord> handles;
		uint32 handleCounter = 0;
		iosu::kernel::Savestate_GetActiveHandles(handles, handleCounter);
		iosu::fsa::SavestateData fsaData = iosu::fsa::Savestate_Get();
		DoAll(s, handles, handleCounter, fsaData);
	}

	void DoState_Load(Serializer& s)
	{
		std::vector<iosu::kernel::SavestateHandleRecord> handles;
		uint32 handleCounter = 0;
		iosu::fsa::SavestateData fsaData;
		DoAll(s, handles, handleCounter, fsaData);

		std::vector<iosu::kernel::SavestateHandleRemap> remap;
		std::string error;
		if (!iosu::kernel::Savestate_RestoreActiveHandles(handles, handleCounter, remap, error))
			throw std::runtime_error("Savestate load failed: " + error);

		std::unordered_map<uint32, uint32> fsaClientRemap;
		for (const auto& r : remap)
		{
			if (r.path == "/dev/fsa")
				fsaClientRemap[r.oldTarget] = r.newTarget;
		}
		if (!iosu::fsa::Savestate_Restore(fsaData, fsaClientRemap, error))
			throw std::runtime_error("Savestate load failed: " + error);
	}
}

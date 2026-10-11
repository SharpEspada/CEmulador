#include "Cafe/Savestate/SavestateSerializer_MMU.h"
#include "Cafe/HW/MMU/MMU.h"

extern MPTR sysAreaAllocatorOffset; // coreinit_MEM.cpp (global scope)

namespace SavestateSerializer_MMU
{
	// Per-range record. Order matches memory_getMMURanges() at save
	// time; on load we match by areaId rather than by vector index, in
	// case a future Cemu version reorders the global range list.
	struct RangeRecord
	{
		MMU_MEM_AREA_ID areaId;
		bool wasMapped;
		uint32 size; // current size at save time (may differ from
					 // initSize() due to a graphic pack's setEnd() -
					 // see MMU.cpp memory_mapForCurrentTitle())
	};

	void DoState(Serializer& s)
	{
		s.DoMarker("MMU");
		// host-side bump pointer of the system-area allocator (the area's
		// content is part of the dump below, the cursor is not)

		std::vector<MMURange*> ranges = memory_getMMURanges();

		if (s.IsWriting() || s.IsMeasuring())
		{
			uint32 count = (uint32)ranges.size();
			s.DoPOD(count);

			for (MMURange* range : ranges)
			{
				RangeRecord record{};
				record.areaId = range->areaId;
				record.wasMapped = range->isMapped();
				record.size = range->getSize();
				s.DoPOD(record);

				if (record.wasMapped)
				{
					// Written as raw bytes straight out of the 4GB
					// reservation - memory_base + baseAddress, exactly
					// what MMU.cpp's memory_createDump() already does
					// per-range for the ramDump debug feature, just
					// captured into the savestate stream instead of a
					// separate file.
					s.DoBytes(range->getPtr(), record.size);
				}
			}
			MPTR sysAreaOffset = sysAreaAllocatorOffset;
			s.DoPOD(sysAreaOffset);
			return;
		}

		// --- Reading -----------------------------------------------

		uint32 count = 0;
		s.DoPOD(count);

		// Build a lookup so we can match saved records to the current
		// range list by areaId regardless of vector ordering.
		std::unordered_map<int, MMURange*> rangeByAreaId;
		for (MMURange* range : ranges)
			rangeByAreaId[(int)range->areaId] = range;

		for (uint32 i = 0; i < count; i++)
		{
			RangeRecord record{};
			s.DoPOD(record);

			auto it = rangeByAreaId.find((int)record.areaId);
			if (it == rangeByAreaId.end())
			{
				// A range that existed in the file but not in this
				// build. This should already have been caught by the
				// compat hash (SavestateCompat.cpp includes build id),
				// but fail loudly rather than silently drop bytes.
				throw SavestateDesyncException("MMU:unknownAreaId", "unknownAreaId");
			}
			MMURange* range = it->second;

			// OVERLAY_AREA (and any other FLAG_OPTIONAL region) is not
			// mapped by default - re-enable it if the savestate needs it.
			if (record.wasMapped && !range->isMapped())
			{
				if (range->areaId == MMU_MEM_AREA_ID::OVERLAY)
					memory_enableOverlayArena();
				else if (range->isOptional())
					range->mapMem();
				// FLAG_MAP_EARLY ranges (CEMU_AREA, SHARED_AREA) are
				// always mapped well before a savestate can be loaded,
				// so they fall through here already mapped.
			}
			else if (!record.wasMapped && range->isMapped() && range->isOptional())
			{
				// Savestate was made without this optional region -
				// unmap it so behavior matches exactly.
				range->unmapMem();
			}

			if (record.wasMapped)
			{
				if (record.size != range->getSize())
				{
					// A graphic pack changed a range's size between
					// save and load (setEnd() in memory_mapForCurrentTitle()).
					// SavestateCompat.cpp's hash should already prevent
					// this, but guard here too rather than corrupt
					// memory on a partial mismatch.
					throw SavestateDesyncException("MMU:sizeMismatch", "sizeMismatch");
				}
				s.DoBytes(range->getPtr(), record.size);
			}
		}

		MPTR sysAreaOffset = 0;
		s.DoPOD(sysAreaOffset);
		sysAreaAllocatorOffset = sysAreaOffset;
	}
}

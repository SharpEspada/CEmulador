#include "Cafe/Savestate/SavestateSerializer_MMU.h"
#include "Cafe/Savestate/SavestateParallel.h"
#include "Cafe/HW/MMU/MMU.h"
#include <unordered_map>

extern MPTR sysAreaAllocatorOffset; // coreinit_MEM.cpp (global scope)

// Section layout (all values little endian, as stored in host memory):
//   SectionHeader
//   RangeRecord[rangeCount]
//   for every range with wasMapped: bitmap of ceil(blockCount/8) bytes, padded to 8 bytes
//   packed data: the non-zero blocks, in range order then block order. Every block is
//                blockSize bytes, except the last block of a range which may be shorter.
namespace SavestateSerializer_MMU
{
	static constexpr uint32 BLOCK_SIZE = 64 * 1024;
	static constexpr char SECTION_MAGIC[8] = { 'C', 'E', 'M', 'M', 'U', '0', '0', '2' };

	struct SectionHeader
	{
		char magic[8];
		uint32 blockSize;
		uint32 rangeCount;
		uint32 sysAreaOffset; // host-side bump pointer of the system-area allocator
		uint32 reserved;
		uint64 totalSize;	  // size of the whole section, for validation
	};

	struct RangeRecord
	{
		sint32 areaId;
		uint32 wasMapped;
		uint32 size;		// size at save time (a graphic pack may have changed it)
		uint32 blockCount;
		uint32 nonZeroBlocks;
		uint32 reserved;
	};

	static inline uint64 Align8(uint64 v) { return (v + 7) & ~7ull; }

	static bool IsZeroBlock(const uint8* p, size_t n)
	{
		size_t words = n / 8;
		for (size_t i = 0; i + 8 <= words; i += 8)
		{
			uint64 w[8];
			memcpy(w, p + i * 8, 64);
			if ((w[0] | w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7]) != 0)
				return false;
		}
		// remainder (less than 64 bytes)
		for (size_t i = (words / 8) * 64; i < n; i++)
			if (p[i] != 0)
				return false;
		return true;
	}

	struct BlockTask
	{
		uint32 rangeIndex;
		uint32 blockIndex;
	};

	SavestateBuffer Capture()
	{
		std::vector<MMURange*> ranges = memory_getMMURanges();

		struct Info
		{
			MMURange* range;
			bool mapped;
			uint32 size;
			uint32 blockCount;
			std::vector<uint8> nonZero; // one byte per block during capture
			uint32 nonZeroCount = 0;
		};
		std::vector<Info> infos;
		infos.reserve(ranges.size());
		std::vector<BlockTask> tasks;
		for (MMURange* range : ranges)
		{
			Info info{};
			info.range = range;
			info.mapped = range->isMapped();
			info.size = info.mapped ? range->getSize() : 0;
			info.blockCount = (info.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
			info.nonZero.assign(info.blockCount, 0);
			for (uint32 b = 0; b < info.blockCount; b++)
				tasks.push_back({ (uint32)infos.size(), b });
			infos.push_back(std::move(info));
		}

		auto blockLength = [&](const Info& info, uint32 b) -> size_t
		{
			return std::min<uint64>(BLOCK_SIZE, (uint64)info.size - (uint64)b * BLOCK_SIZE);
		};

		// pass 1: find out which blocks contain data
		SavestateParallel::For(tasks.size(), [&](size_t i)
		{
			Info& info = infos[tasks[i].rangeIndex];
			uint32 b = tasks[i].blockIndex;
			const uint8* p = info.range->getPtr() + (uint64)b * BLOCK_SIZE;
			info.nonZero[b] = IsZeroBlock(p, blockLength(info, b)) ? 0 : 1;
		});

		// layout
		uint64 headerBytes = sizeof(SectionHeader) + sizeof(RangeRecord) * infos.size();
		for (auto& info : infos)
		{
			for (uint32 b = 0; b < info.blockCount; b++)
				info.nonZeroCount += info.nonZero[b];
			if (info.mapped)
				headerBytes += Align8((info.blockCount + 7) / 8);
		}
		std::vector<uint64> taskOffset(tasks.size(), 0);
		uint64 cursor = headerBytes;
		for (size_t i = 0; i < tasks.size(); i++)
		{
			Info& info = infos[tasks[i].rangeIndex];
			if (info.nonZero[tasks[i].blockIndex])
			{
				taskOffset[i] = cursor;
				cursor += blockLength(info, tasks[i].blockIndex);
			}
		}

		SavestateBuffer out = SavestateBuffer::Allocate((size_t)cursor);
		uint8* base = out.data.get();

		SectionHeader header{};
		memcpy(header.magic, SECTION_MAGIC, 8);
		header.blockSize = BLOCK_SIZE;
		header.rangeCount = (uint32)infos.size();
		header.sysAreaOffset = (uint32)sysAreaAllocatorOffset;
		header.totalSize = cursor;
		memcpy(base, &header, sizeof(header));

		uint64 pos = sizeof(SectionHeader);
		for (auto& info : infos)
		{
			RangeRecord rec{};
			rec.areaId = (sint32)info.range->areaId;
			rec.wasMapped = info.mapped ? 1 : 0;
			rec.size = info.size;
			rec.blockCount = info.blockCount;
			rec.nonZeroBlocks = info.nonZeroCount;
			memcpy(base + pos, &rec, sizeof(rec));
			pos += sizeof(rec);
		}
		for (auto& info : infos)
		{
			if (!info.mapped)
				continue;
			uint64 bitmapBytes = Align8((info.blockCount + 7) / 8);
			memset(base + pos, 0, (size_t)bitmapBytes);
			for (uint32 b = 0; b < info.blockCount; b++)
				if (info.nonZero[b])
					base[pos + b / 8] |= (uint8)(1u << (b % 8));
			pos += bitmapBytes;
		}

		// pass 2: copy the blocks that contain data
		SavestateParallel::For(tasks.size(), [&](size_t i)
		{
			Info& info = infos[tasks[i].rangeIndex];
			uint32 b = tasks[i].blockIndex;
			if (!info.nonZero[b])
				return;
			memcpy(base + taskOffset[i], info.range->getPtr() + (uint64)b * BLOCK_SIZE, blockLength(info, b));
		});
		return out;
	}

	bool Restore(const SavestateBuffer& section)
	{
		const uint8* base = section.data.get();
		const uint64 totalSize = section.size;
		auto fail = [](const char* what) { throw SavestateDesyncException("MMU", what); };

		if (totalSize < sizeof(SectionHeader))
			fail("MMU:truncated");
		SectionHeader header{};
		memcpy(&header, base, sizeof(header));
		if (memcmp(header.magic, SECTION_MAGIC, 8) != 0)
			fail("MMU:badMagic");
		if (header.blockSize != BLOCK_SIZE || header.totalSize != totalSize)
			fail("MMU:badHeader");
		uint64 pos = sizeof(SectionHeader);
		if (header.rangeCount > 1024 || pos + sizeof(RangeRecord) * (uint64)header.rangeCount > totalSize)
			fail("MMU:truncated");

		std::vector<RangeRecord> records(header.rangeCount);
		if (header.rangeCount)
			memcpy(records.data(), base + pos, sizeof(RangeRecord) * header.rangeCount);
		pos += sizeof(RangeRecord) * (uint64)header.rangeCount;

		std::vector<MMURange*> ranges = memory_getMMURanges();
		std::unordered_map<int, MMURange*> rangeByAreaId;
		for (MMURange* range : ranges)
			rangeByAreaId[(int)range->areaId] = range;

		struct Target
		{
			MMURange* range = nullptr;
			const RangeRecord* rec = nullptr;
			const uint8* bitmap = nullptr;
			std::vector<uint64> blockOffset; // offset of each stored block inside the section, ~0 for a zero block
		};
		std::vector<Target> targets(records.size());

		// 1) validate everything and fix up optional regions BEFORE touching any memory content
		for (size_t r = 0; r < records.size(); r++)
		{
			const RangeRecord& rec = records[r];
			auto it = rangeByAreaId.find(rec.areaId);
			if (it == rangeByAreaId.end())
				fail("MMU:unknownAreaId");
			MMURange* range = it->second;

			// OVERLAY_AREA (and any other FLAG_OPTIONAL region) is not mapped by default
			if (rec.wasMapped && !range->isMapped())
			{
				if (range->areaId == MMU_MEM_AREA_ID::OVERLAY)
					memory_enableOverlayArena();
				else if (range->isOptional())
					range->mapMem();
			}
			else if (!rec.wasMapped && range->isMapped() && range->isOptional())
			{
				range->unmapMem();
			}
			if (!rec.wasMapped)
				continue;
			if (!range->isMapped())
				fail("MMU:rangeNotMapped");
			if (rec.size != range->getSize())
				fail("MMU:sizeMismatch");
			if (rec.blockCount != (rec.size + BLOCK_SIZE - 1) / BLOCK_SIZE)
				fail("MMU:badBlockCount");
			uint64 bitmapBytes = Align8((rec.blockCount + 7) / 8);
			if (pos + bitmapBytes > totalSize)
				fail("MMU:truncated");
			targets[r].range = range;
			targets[r].rec = &rec;
			targets[r].bitmap = base + pos;
			pos += bitmapBytes;
		}
		uint64 dataCursor = pos;
		std::vector<BlockTask> tasks;
		for (size_t r = 0; r < records.size(); r++)
		{
			Target& t = targets[r];
			if (!t.range)
				continue;
			t.blockOffset.assign(t.rec->blockCount, ~0ull);
			uint32 stored = 0;
			for (uint32 b = 0; b < t.rec->blockCount; b++)
			{
				if (t.bitmap[b / 8] & (1u << (b % 8)))
				{
					uint64 len = std::min<uint64>(BLOCK_SIZE, (uint64)t.rec->size - (uint64)b * BLOCK_SIZE);
					t.blockOffset[b] = dataCursor;
					dataCursor += len;
					stored++;
				}
				tasks.push_back({ (uint32)r, b });
			}
			if (stored != t.rec->nonZeroBlocks)
				fail("MMU:badBlockCount");
		}
		if (dataCursor != totalSize)
			fail("MMU:badSize");

		// 2) restore, in parallel. Blocks that are zero both in the file and in live memory are left alone
		std::atomic<bool> codeChanged{ false };
		SavestateParallel::For(tasks.size(), [&](size_t i)
		{
			Target& t = targets[tasks[i].rangeIndex];
			const MMU_MEM_AREA_ID areaId = t.range->areaId;
			const bool isCodeArea = areaId == MMU_MEM_AREA_ID::CODE_LOW0 || areaId == MMU_MEM_AREA_ID::CODE_TRAMPOLINE ||
									areaId == MMU_MEM_AREA_ID::CODE_CAVE || areaId == MMU_MEM_AREA_ID::CODE_MAIN;
			uint32 b = tasks[i].blockIndex;
			uint64 len = std::min<uint64>(BLOCK_SIZE, (uint64)t.rec->size - (uint64)b * BLOCK_SIZE);
			uint8* live = t.range->getPtr() + (uint64)b * BLOCK_SIZE;
			if (t.blockOffset[b] != ~0ull)
			{
				const uint8* saved = base + t.blockOffset[b];
				// only write blocks that really differ (less memory traffic, and tells us whether code changed)
				if (memcmp(live, saved, (size_t)len) != 0)
				{
					memcpy(live, saved, (size_t)len);
					if (isCodeArea)
						codeChanged = true;
				}
			}
			else if (!IsZeroBlock(live, (size_t)len))
			{
				memset(live, 0, (size_t)len);
				if (isCodeArea)
					codeChanged = true;
			}
		});

		sysAreaAllocatorOffset = header.sysAreaOffset;
		return codeChanged.load();
	}
}

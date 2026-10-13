#include "Cafe/Savestate/SavestateFile.h"
#include "Cafe/Savestate/SavestateSerializer.h"
#include "Cafe/Savestate/SavestateFormat.h"
#include "Cafe/Savestate/SavestateCompat.h"
#include "util/crypto/crc32.h" // reused from rpl.cpp's include of crypto helpers; swap for a 64-bit hash if preferred
#include <zstd.h>
#include "Cafe/Savestate/SavestateParallel.h"
#include "Common/FileStream.h" // same FileStream class already used by MMU.cpp's memory_writeDumpFile

namespace SavestateFile
{
	// crc32 update over an arbitrarily large range (crc32_calc() takes a 32 bit length)
	static uint32 ChainChecksum(uint32 crc, const uint8* data, uint64 size)
	{
		uint64 pos = 0;
		while (pos < size)
		{
			uint32 chunk = (uint32)std::min<uint64>(size - pos, 0x40000000ull);
			crc = crc32_calc(crc, data + pos, chunk);
			pos += chunk;
		}
		return crc;
	}

	static uint64 CalcSectionChecksum(const uint8* data, uint64 size)
	{
		// crc32 extended to 64 bits by hashing twice with different
		// seeds is sufficient here - this is corruption detection, not
		// cryptographic integrity. Computed over the bytes as STORED in the
		// file (i.e. after compression), in chunks because crc32_calc() takes a
		// 32 bit length.
		uint32 lo = 0;
		uint32 hi = 0x811C9DC5u;
		uint64 pos = 0;
		while (pos < size)
		{
			uint32 chunk = (uint32)std::min<uint64>(size - pos, 0x40000000ull);
			lo = crc32_calc(lo, data + pos, chunk);
			hi = crc32_calc(hi, data + pos, chunk);
			pos += chunk;
		}
		return ((uint64)hi << 32) | (uint64)lo;
	}

	// writes in chunks of at most 1 GiB (the stream API takes a 32 bit length)
	static bool WriteChunked(FileStream* out, const void* data, uint64 size)
	{
		const uint8* p = (const uint8*)data;
		while (size > 0)
		{
			uint32 chunk = (uint32)std::min<uint64>(size, 0x40000000ull);
			if ((uint32)out->writeData(p, (sint32)chunk) != chunk)
				return false;
			p += chunk;
			size -= chunk;
		}
		return true;
	}

	// A section as it will appear in the file: either the raw bytes, or a
	// header + independently compressed blocks.
	struct StoredSection
	{
		SavestateSectionId id;
		SavestateBuffer raw;				// kept alive only while compression == NONE
		uint64 rawSize = 0;
		uint32 compression = SAVESTATE_COMPRESSION_NONE;
		std::vector<uint8> head;			// block table (compressed sections)
		std::vector<std::vector<uint8>> blocks;

		uint64 storedSize() const
		{
			if (compression == SAVESTATE_COMPRESSION_NONE)
				return rawSize;
			uint64 total = head.size();
			for (auto& b : blocks)
				total += b.size();
			return total;
		}

		template <typename Fn>
		void forEachSegment(Fn&& fn) const
		{
			if (compression == SAVESTATE_COMPRESSION_NONE)
			{
				fn(raw.data.get(), (uint64)rawSize);
				return;
			}
			fn(head.data(), (uint64)head.size());
			for (auto& b : blocks)
				fn(b.data(), (uint64)b.size());
		}
	};

	// Compresses a section into independent blocks, in parallel.
	static void CompressSection(StoredSection& st)
	{
		const uint64 blockRaw = SAVESTATE_COMPRESSION_BLOCK_SIZE;
		uint64 blockCount = (st.rawSize + blockRaw - 1) / blockRaw;
		std::vector<std::vector<uint8>> blocks((size_t)blockCount);
		const uint8* src = st.raw.data.get();
		uint64 rawSize = st.rawSize;
		SavestateParallel::For((size_t)blockCount, [&](size_t i)
		{
			uint64 off = (uint64)i * blockRaw;
			size_t len = (size_t)std::min<uint64>(blockRaw, rawSize - off);
			// scratch buffer reused by this worker for all of its blocks, so only
			// the real compressed size is ever kept (never the worst-case bound)
			thread_local std::vector<uint8> scratch;
			size_t bound = ZSTD_compressBound(len);
			if (scratch.size() < bound)
				scratch.resize(bound);
			size_t cs = ZSTD_compress(scratch.data(), bound, src + off, len, 1);
			if (ZSTD_isError(cs))
				throw std::runtime_error("Savestate compression failed");
			blocks[i].assign(scratch.begin(), scratch.begin() + cs);
		}, std::max(2u, SavestateParallel::GetWorkerCount() / 2)); // game is running again: leave it some CPU

		std::vector<uint8> head(sizeof(uint32) * 2 + sizeof(uint64) * (size_t)blockCount);
		uint32 blockRaw32 = (uint32)blockRaw;
		uint32 blockCount32 = (uint32)blockCount;
		memcpy(head.data(), &blockRaw32, 4);
		memcpy(head.data() + 4, &blockCount32, 4);
		uint64 total = head.size();
		for (size_t i = 0; i < blocks.size(); i++)
		{
			uint64 cs = blocks[i].size();
			memcpy(head.data() + 8 + i * 8, &cs, 8);
			total += cs;
		}
		if (total >= st.rawSize)
			return; // incompressible: keep raw
		st.head = std::move(head);
		st.blocks = std::move(blocks);
		st.compression = SAVESTATE_COMPRESSION_ZSTD_BLOCKS;
		st.raw = SavestateBuffer(); // release the raw bytes early
	}

	// ------------------------------------------------------------------
	// Writing
	// ------------------------------------------------------------------

	void Writer::AddSection(SavestateSectionId id, SavestateBuffer&& bytes)
	{
		m_sections.push_back(PendingSection{ id, std::move(bytes) });
	}
	void Writer::AddSection(SavestateSectionId id, std::vector<uint8>&& bytes)
	{
		m_sections.push_back(PendingSection{ id, SavestateBuffer::CopyOf(bytes) });
	}

	bool Writer::WriteToFile(const std::filesystem::path& path, uint64 titleId, uint16 titleVersion,
		const std::vector<uint8>& thumbnailPng)
	{
		namespace fs = std::filesystem;
		{
			SavestateFileHeader header{};
			header.magic1 = SAVESTATE_MAGIC1;
			header.magic2 = SAVESTATE_MAGIC2;
			header.formatVersion = SAVESTATE_FORMAT_VERSION;
			header.compatBreakVersion = SAVESTATE_COMPAT_BREAK_VERSION;
			strncpy(header.buildIdString, SavestateCompat_GetBuildIdString().c_str(), sizeof(header.buildIdString) - 1);
			header.titleId = titleId;
			header.titleVersion = titleVersion;
			header.compatHash = SavestateCompat_ComputeCurrentHash();
			header.creationTimestamp = (uint64)time(nullptr);

			std::vector<StoredSection> stored;
			stored.reserve(m_sections.size());
			for (auto& section : m_sections)
			{
				StoredSection st;
				st.id = section.id;
				st.rawSize = section.raw.size;
				st.raw = std::move(section.raw);
				if (st.rawSize >= 4096)
					CompressSection(st);
				stored.push_back(std::move(st));
			}
			m_sections.clear();

			// layout: header | section table | thumbnail | section data...
			header.sectionCount = (uint32)stored.size();
			uint64 cursor = sizeof(SavestateFileHeader);
			header.sectionTableOffset = cursor;
			cursor += sizeof(SavestateSectionTableEntry) * stored.size();

			if (!thumbnailPng.empty())
			{
				header.thumbnailOffset = cursor;
				header.thumbnailSize = (uint32)thumbnailPng.size();
				cursor += thumbnailPng.size();
			}
			else
			{
				header.thumbnailOffset = 0;
				header.thumbnailSize = 0;
			}

			std::vector<SavestateSectionTableEntry> tableEntries(stored.size());
			SavestateParallel::For(stored.size(), [&](size_t i)
			{
				const StoredSection& section = stored[i];
				uint32 lo = 0;
				uint32 hi = 0x811C9DC5u;
				section.forEachSegment([&](const uint8* p, uint64 size)
				{
					lo = ChainChecksum(lo, p, size);
					hi = ChainChecksum(hi, p, size);
				});
				SavestateSectionTableEntry entry{};
				entry.id = section.id;
				entry.sizeBytes = section.storedSize();
				entry.checksum = ((uint64)hi << 32) | (uint64)lo;
				entry.rawSize = section.rawSize;
				entry.compression = section.compression;
				tableEntries[i] = entry;
			}, 4);
			for (auto& entry : tableEntries)
			{
				entry.fileOffset = cursor;
				cursor += entry.sizeBytes;
			}

			// Write to a temporary file first and rename it at the end, so a failure
			// (disk full, ...) never destroys an existing good savestate.
			std::error_code ec;
			if (path.has_parent_path())
				fs::create_directories(path.parent_path(), ec);
			fs::path tmpPath = path;
			tmpPath += ".tmp";
			FileStream* out = FileStream::createFile2(tmpPath);
			if (!out)
				return false;

			bool ok = true;
			ok &= WriteChunked(out, &header, sizeof(header));
			ok &= WriteChunked(out, tableEntries.data(), sizeof(SavestateSectionTableEntry) * tableEntries.size());
			if (!thumbnailPng.empty())
				ok &= WriteChunked(out, thumbnailPng.data(), thumbnailPng.size());
			for (auto& section : stored)
			{
				section.forEachSegment([&](const uint8* p, uint64 size)
				{
					if (ok && size > 0)
						ok &= WriteChunked(out, p, size);
				});
			}

			delete out;
			if (!ok)
			{
				fs::remove(tmpPath, ec);
				return false;
			}
			fs::rename(tmpPath, path, ec);
			if (ec)
			{
				fs::remove(tmpPath, ec);
				return false;
			}
			return true;
		}
	}

	// ------------------------------------------------------------------
	// Reading
	// ------------------------------------------------------------------

	LoadError Reader::OpenAndValidate(const std::filesystem::path& path, uint64 expectedTitleId)
	{
		{
			auto loaded = FileStream::LoadIntoMemory(path);
			if (!loaded.has_value())
				return LoadError::CannotOpenFile;
			m_fileBytes = std::move(loaded.value());

			if (m_fileBytes.size() < sizeof(SavestateFileHeader))
				return LoadError::TruncatedOrCorrupt;

			memcpy(&m_header, m_fileBytes.data(), sizeof(SavestateFileHeader));

			if (m_header.magic1 != SAVESTATE_MAGIC1 || m_header.magic2 != SAVESTATE_MAGIC2)
				return LoadError::BadMagic;
			if (m_header.formatVersion != SAVESTATE_FORMAT_VERSION ||
				m_header.compatBreakVersion != SAVESTATE_COMPAT_BREAK_VERSION)
				return LoadError::UnsupportedFormatVersion;
			if (m_header.titleId != expectedTitleId)
				return LoadError::WrongTitle;
			if (m_header.compatHash != SavestateCompat_ComputeCurrentHash())
				return LoadError::IncompatibleBuild;

			uint64 tableBytes = sizeof(SavestateSectionTableEntry) * (uint64)m_header.sectionCount;
			if (m_header.sectionTableOffset + tableBytes > m_fileBytes.size())
				return LoadError::TruncatedOrCorrupt;

			m_sectionTable.resize(m_header.sectionCount);
			memcpy(m_sectionTable.data(), m_fileBytes.data() + m_header.sectionTableOffset, tableBytes);

			// validate every section is fully in-bounds and checksums
			// match BEFORE any of them are handed out. This is what
			// makes "refuse cleanly, never half-apply state" possible.
			for (auto& entry : m_sectionTable)
			{
				if (entry.fileOffset > m_fileBytes.size() || entry.sizeBytes > m_fileBytes.size() - entry.fileOffset)
					return LoadError::TruncatedOrCorrupt;
				if (entry.compression != SAVESTATE_COMPRESSION_NONE && entry.compression != SAVESTATE_COMPRESSION_ZSTD_BLOCKS)
					return LoadError::TruncatedOrCorrupt;
				if (entry.compression == SAVESTATE_COMPRESSION_NONE && entry.rawSize != entry.sizeBytes)
					return LoadError::TruncatedOrCorrupt;
			}
			std::atomic<bool> checksumOk{ true };
			SavestateParallel::For(m_sectionTable.size(), [&](size_t i)
			{
				auto& entry = m_sectionTable[i];
				if (CalcSectionChecksum(m_fileBytes.data() + entry.fileOffset, entry.sizeBytes) != entry.checksum)
					checksumOk = false;
			}, 4);
			if (!checksumOk)
				return LoadError::SectionChecksumMismatch;

			return LoadError::OK;
		}
	}

	// Compressed sections are decompressed block-parallel. Safe to call while the game is still running.
	SavestateBuffer Reader::GetSection(SavestateSectionId id) const
	{
		{
			for (auto& entry : m_sectionTable)
			{
				if (entry.id != id)
					continue;
				const uint8* stored = m_fileBytes.data() + entry.fileOffset;
				if (entry.compression == SAVESTATE_COMPRESSION_NONE)
				{
					SavestateBuffer buf = SavestateBuffer::Allocate((size_t)entry.sizeBytes);
					if (entry.sizeBytes)
						memcpy(buf.data.get(), stored, (size_t)entry.sizeBytes);
					return buf;
				}
				const char* corrupt = "Savestate file is corrupted (section could not be decompressed)";
				if (entry.sizeBytes < 8)
					throw std::runtime_error(corrupt);
				uint32 blockRaw = 0, blockCount = 0;
				memcpy(&blockRaw, stored, 4);
				memcpy(&blockCount, stored + 4, 4);
				if (blockRaw == 0 || (uint64)blockCount * blockRaw < entry.rawSize ||
					(uint64)(blockCount ? blockCount - 1 : 0) * blockRaw >= std::max<uint64>(entry.rawSize, 1) ||
					8 + (uint64)blockCount * 8 > entry.sizeBytes)
					throw std::runtime_error(corrupt);
				std::vector<uint64> compOffset(blockCount), compSize(blockCount);
				uint64 cursor = 8 + (uint64)blockCount * 8;
				for (uint32 i = 0; i < blockCount; i++)
				{
					memcpy(&compSize[i], stored + 8 + (size_t)i * 8, 8);
					compOffset[i] = cursor;
					if (compSize[i] > entry.sizeBytes || cursor > entry.sizeBytes - compSize[i])
						throw std::runtime_error(corrupt);
					cursor += compSize[i];
				}
				SavestateBuffer raw = SavestateBuffer::Allocate((size_t)entry.rawSize);
				uint8* dst = raw.data.get();
				uint64 rawSize = entry.rawSize;
				SavestateParallel::For(blockCount, [&](size_t i)
				{
					uint64 off = (uint64)i * blockRaw;
					size_t len = (size_t)std::min<uint64>(blockRaw, rawSize - off);
					size_t r = ZSTD_decompress(dst + off, len, stored + compOffset[i], (size_t)compSize[i]);
					if (ZSTD_isError(r) || r != len)
						throw std::runtime_error(corrupt);
				});
				return raw;
			}
			return SavestateBuffer::Allocate(0);
		}
	}

	std::vector<uint8> Reader::GetThumbnail() const
	{
		if (m_header.thumbnailSize == 0)
			return {};
		return std::vector<uint8>(
			m_fileBytes.begin() + m_header.thumbnailOffset,
			m_fileBytes.begin() + m_header.thumbnailOffset + m_header.thumbnailSize);
	}

} // namespace SavestateFile


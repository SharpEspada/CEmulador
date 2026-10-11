#include "Cafe/Savestate/SavestateSerializer.h"
#include "Cafe/Savestate/SavestateFormat.h"
#include "Cafe/Savestate/SavestateCompat.h"
#include "util/crypto/crc32.h" // reused from rpl.cpp's include of crypto helpers; swap for a 64-bit hash if preferred
#include <zstd.h>
#include "Common/FileStream.h" // same FileStream class already used by MMU.cpp's memory_writeDumpFile

namespace SavestateFile
{
	static uint64 CalcSectionChecksum(const uint8* data, uint64 size)
	{
		// crc32 extended to 64 bits by hashing twice with different
		// seeds is sufficient here - this is corruption detection, not
		// cryptographic integrity. Computed over the bytes as STORED in the
		// file (i.e. after compression), in chunks because crc32() takes a
		// 32 bit length.
		uint32 lo = 0;
		uint32 hi = 0x811C9DC5u;
		uint64 pos = 0;
		while (pos < size)
		{
			uint32 chunk = (uint32)std::min<uint64>(size - pos, 0x40000000ull);
			lo = crc32(lo, data + pos, chunk);
			hi = crc32(hi, data + pos, chunk);
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

	struct PendingSection
	{
		SavestateSectionId id;
		std::vector<uint8> bytes;
	};

	// ------------------------------------------------------------------
	// Writing
	// ------------------------------------------------------------------

	class Writer
	{
	public:
		void AddSection(SavestateSectionId id, std::vector<uint8>&& bytes)
		{
			m_sections.push_back({ id, std::move(bytes) });
		}

		// thumbnailPng may be empty - then no thumbnail is written.
		bool WriteToFile(const fs::path& path, uint64 titleId, uint16 titleVersion,
			const std::vector<uint8>& thumbnailPng)
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

			// Compress every section (zstd, fast level). Guest memory is mostly
			// empty or repetitive, so this typically shrinks a savestate a lot.
			struct StoredSection
			{
				SavestateSectionId id;
				std::vector<uint8> data;
				uint64 rawSize;
				uint32 compression;
			};
			std::vector<StoredSection> stored;
			stored.reserve(m_sections.size());
			ZSTD_CCtx* cctx = ZSTD_createCCtx();
			ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, 1);
			ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 4); // ignored (error code) if zstd was built without threads
			for (auto& section : m_sections)
			{
				StoredSection st{};
				st.id = section.id;
				st.rawSize = section.bytes.size();
				st.compression = SAVESTATE_COMPRESSION_NONE;
				if (section.bytes.size() >= 4096 && cctx)
				{
					size_t bound = ZSTD_compressBound(section.bytes.size());
					std::unique_ptr<uint8[]> tmp(new uint8[bound]); // intentionally not zero-initialized
					size_t compressedSize = ZSTD_compress2(cctx, tmp.get(), bound, section.bytes.data(), section.bytes.size());
					if (!ZSTD_isError(compressedSize) && compressedSize < section.bytes.size())
					{
						st.data.assign(tmp.get(), tmp.get() + compressedSize);
						st.compression = SAVESTATE_COMPRESSION_ZSTD;
						std::vector<uint8>().swap(section.bytes); // release the raw bytes early
					}
				}
				if (st.compression == SAVESTATE_COMPRESSION_NONE)
					st.data = std::move(section.bytes);
				stored.push_back(std::move(st));
			}
			if (cctx)
				ZSTD_freeCCtx(cctx);
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

			std::vector<SavestateSectionTableEntry> tableEntries;
			tableEntries.reserve(stored.size());
			for (auto& section : stored)
			{
				SavestateSectionTableEntry entry{};
				entry.id = section.id;
				entry.fileOffset = cursor;
				entry.sizeBytes = section.data.size();
				entry.checksum = CalcSectionChecksum(section.data.data(), section.data.size());
				entry.rawSize = section.rawSize;
				entry.compression = section.compression;
				tableEntries.push_back(entry);
				cursor += section.data.size();
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
				ok &= WriteChunked(out, section.data.data(), section.data.size());

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

	private:
		std::vector<PendingSection> m_sections;
	};

	// ------------------------------------------------------------------
	// Reading
	// ------------------------------------------------------------------

	enum class LoadError
	{
		OK,
		CannotOpenFile,
		BadMagic,
		UnsupportedFormatVersion,
		IncompatibleBuild,		// compatHash mismatch
		WrongTitle,
		TruncatedOrCorrupt,
		SectionChecksumMismatch,
	};

	class Reader
	{
	public:
		// Validates the ENTIRE header and section table before
		// returning success. On success, GetSection() below can be
		// called freely - no emulation state should be touched until
		// this returns LoadError::OK.
		LoadError OpenAndValidate(const fs::path& path, uint64 expectedTitleId)
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
				if (entry.compression != SAVESTATE_COMPRESSION_NONE && entry.compression != SAVESTATE_COMPRESSION_ZSTD)
					return LoadError::TruncatedOrCorrupt;
				if (entry.compression == SAVESTATE_COMPRESSION_NONE && entry.rawSize != entry.sizeBytes)
					return LoadError::TruncatedOrCorrupt;
				if (CalcSectionChecksum(m_fileBytes.data() + entry.fileOffset, entry.sizeBytes) != entry.checksum)
					return LoadError::SectionChecksumMismatch;
			}

			return LoadError::OK;
		}

		// Returns the raw bytes of a section, or an empty vector if
		// the section is not present in this file (older savestates
		// missing a section added later - callers should treat missing
		// optional sections as "use defaults", not as an error).
		std::vector<uint8> GetSection(SavestateSectionId id) const
		{
			for (auto& entry : m_sectionTable)
			{
				if (entry.id == id)
				{
					const uint8* stored = m_fileBytes.data() + entry.fileOffset;
					if (entry.compression == SAVESTATE_COMPRESSION_ZSTD)
					{
						std::vector<uint8> raw(entry.rawSize);
						size_t r = ZSTD_decompress(raw.data(), raw.size(), stored, entry.sizeBytes);
						if (ZSTD_isError(r) || r != entry.rawSize)
							throw std::runtime_error("Savestate file is corrupted (section could not be decompressed)");
						return raw;
					}
					return std::vector<uint8>(stored, stored + entry.sizeBytes);
				}
			}
			return {};
		}

		const SavestateFileHeader& GetHeader() const { return m_header; }

		std::vector<uint8> GetThumbnail() const
		{
			if (m_header.thumbnailSize == 0)
				return {};
			return std::vector<uint8>(
				m_fileBytes.begin() + m_header.thumbnailOffset,
				m_fileBytes.begin() + m_header.thumbnailOffset + m_header.thumbnailSize);
		}

	private:
		std::vector<uint8> m_fileBytes;
		SavestateFileHeader m_header{};
		std::vector<SavestateSectionTableEntry> m_sectionTable;
	};

} // namespace SavestateFile


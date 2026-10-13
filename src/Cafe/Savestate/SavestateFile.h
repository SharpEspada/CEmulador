#pragma once
// SavestateFile.h
// Reading / writing of the savestate container file (see SavestateFormat.h for the layout).
// Implemented in SavestateSerializer.cpp.

#include <filesystem>
#include <vector>
#include "Cafe/Savestate/SavestateFormat.h"
#include "Cafe/Savestate/SavestateSerializer.h"

namespace SavestateFile
{
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

	class Writer
	{
	public:
		void AddSection(SavestateSectionId id, SavestateBuffer&& bytes);
		void AddSection(SavestateSectionId id, std::vector<uint8>&& bytes);

		// Compresses (block-parallel) and writes the file atomically (temp file + rename).
		// thumbnailPng may be empty - then no thumbnail is written.
		bool WriteToFile(const std::filesystem::path& path, uint64 titleId, uint16 titleVersion,
			const std::vector<uint8>& thumbnailPng);

	private:
		struct PendingSection
		{
			SavestateSectionId id;
			SavestateBuffer raw;
		};
		std::vector<PendingSection> m_sections;
	};

	class Reader
	{
	public:
		// Reads the file and validates the ENTIRE header, section table and all checksums
		// before returning success. No emulation state is touched.
		LoadError OpenAndValidate(const std::filesystem::path& path, uint64 expectedTitleId);

		// Raw (decompressed) bytes of a section; an empty buffer if the section is absent.
		SavestateBuffer GetSection(SavestateSectionId id) const;

		const SavestateFileHeader& GetHeader() const { return m_header; }
		std::vector<uint8> GetThumbnail() const;

	private:
		std::vector<uint8> m_fileBytes;
		SavestateFileHeader m_header{};
		std::vector<SavestateSectionTableEntry> m_sectionTable;
	};
}

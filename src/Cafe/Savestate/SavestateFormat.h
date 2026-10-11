#pragma once
// SavestateFormat.h
// Defines the on-disk layout of a CEmulador savestate file.
//
// Design notes (see CEmulador_Savestates_Specification.md for the full rationale):
// - The header is validated IN FULL before any emulation state is touched.
// - Sections are self-describing (id, size, checksum) so a corrupt or
//   partial file can be rejected without half-applying state.
// - Compatibility hashes cover everything that can silently change the
//   meaning of the recorded state: build id, active title, CPU mode,
//   active graphic packs. A mismatch is a hard refusal, not a warning,
//   for the initial version.

#define SAVESTATE_MAGIC1			0x53534345u	// 'ESSC' ("CEmu Save State")
#define SAVESTATE_MAGIC2			0x53746174u	// "tatS"
#define SAVESTATE_FORMAT_VERSION	1

// bump this any time SavestateFormat.h or any DoState() layout changes
// in a way that is not backwards compatible
#define SAVESTATE_COMPAT_BREAK_VERSION 3

enum class SavestateSectionId : uint32
{
	// id 1 (formerly CPU) retired - verified unnecessary, CPU registers
	// live entirely in guest OSContext_t, already covered by MMU. Never
	// reused per the rule below.
	MMU			= 2,
	RPL			= 3,
	THREAD		= 4,
	SYNC		= 5,
	ALARM		= 6,
	GPU			= 7,
	IOSU		= 8,
	// New sections must be appended at the end. Never reuse or reorder
	// existing ids - old savestates (within the same compat version)
	// must keep resolving to the same section.
};

#pragma pack(push, 1)

// Fixed-size header. Always the first bytes of the file, always read
// and fully validated before anything else, regardless of size.
struct SavestateFileHeader
{
	uint32 magic1;					// must equal SAVESTATE_MAGIC1
	uint32 magic2;					// must equal SAVESTATE_MAGIC2
	uint32 formatVersion;			// must equal SAVESTATE_FORMAT_VERSION
	uint32 compatBreakVersion;		// must equal SAVESTATE_COMPAT_BREAK_VERSION

	// identifies the build that wrote this file. Purely informational,
	// does not gate loading by itself (compatHash below does that).
	char buildIdString[64];

	// Title this savestate belongs to. A load request is refused if
	// this does not match the currently running title.
	uint64 titleId;
	uint16 titleVersion;			// CafeSystem::GetForegroundTitleVersion()

	// Hash over: active graphic packs (name + version list), CPU mode
	// (single/multicore), and any other option that changes emulated
	// behavior. See SavestateCompat.cpp for exactly what feeds this.
	uint64 compatHash;

	// Section table: how many sections follow, and where the section
	// table itself starts (right after this header).
	uint32 sectionCount;
	uint64 sectionTableOffset;

	// Wall clock time the state was created (informational, used for
	// slot listings), Unix timestamp.
	uint64 creationTimestamp;

	// PNG thumbnail: 0 length means no thumbnail. Written directly
	// after the section table.
	uint64 thumbnailOffset;
	uint32 thumbnailSize;

	uint8 reserved[128]; // room to extend the header without another format bump
};

struct SavestateSectionTableEntry
{
	SavestateSectionId id;
	uint64 fileOffset;	// absolute offset into the file
	uint64 sizeBytes;	// number of bytes stored in the file (compressed size if compression != 0)
	uint64 checksum;	// checksum of the STORED bytes, see SavestateSerializer.cpp
	uint64 rawSize;		// size of the section after decompression
	uint32 compression;	// SAVESTATE_COMPRESSION_*
	uint32 reserved;
};
#define SAVESTATE_COMPRESSION_NONE	0
#define SAVESTATE_COMPRESSION_ZSTD	1

#pragma pack(pop)

static_assert(sizeof(SavestateFileHeader) % 8 == 0);
static_assert(offsetof(SavestateFileHeader, compatHash) != 0);

#pragma once

// AviWriter
// ---------
// Writes a classic (RIFF/AVI 1.0) container with:
//   - one uncompressed video stream (BITMAPINFOHEADER, BI_RGB, 32bpp BGRA, top-down)
//   - one PCM16 audio stream (WAVEFORMATEX)
// interleaved in the 'movi' list, in the exact order frames/blocks are pushed in.
//
// This format was chosen deliberately over inventing a custom container:
//  - it can be opened directly by ffmpeg/most players for verification
//  - it requires no external encoding library (no ffmpeg/libav build dependency)
//  - audio/video interleaving is handled by the container itself, so sync
//    is preserved as long as the caller pushes frames/audio in the order
//    they actually occurred in the emulated timeline (see FrameDumper.cpp)
//
// Known limitation: classic AVI uses 32-bit chunk/RIFF sizes (~4GiB ceiling).
// FrameDumper is expected to start a new AviWriter file if this limit is
// approached (see FrameDumper::RotateIfNeeded). This class does not do this
// on its own; it only refuses to write past the limit.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

class AviWriter
{
public:
	struct VideoParams
	{
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t fpsNumerator = 60;
		uint32_t fpsDenominator = 1;
	};

	struct AudioParams
	{
		uint32_t sampleRate = 48000;
		uint16_t channels = 2;
		uint16_t bitsPerSample = 16;
	};

	AviWriter() = default;
	~AviWriter();

	AviWriter(const AviWriter&) = delete;
	AviWriter& operator=(const AviWriter&) = delete;

	// Opens 'path' and writes a placeholder header. Returns false on I/O failure.
	bool Open(const std::filesystem::path& path, const VideoParams& videoParams, const AudioParams& audioParams);

	// Finalizes the file: patches header fields (frame counts, sizes) and
	// writes the idx1 index. Safe to call multiple times; a no-op if not open.
	void Close();

	bool IsOpen() const { return m_file.is_open(); }

	// Returns false (and leaves the file untouched) if writing this frame
	// would push the file past the classic AVI 4GiB limit; the caller
	// (FrameDumper) is expected to close this writer and open a new segment.
	bool WriteVideoFrame(const uint8_t* frameBGRA, uint32_t width, uint32_t height, uint32_t rowStrideBytes);

	bool WriteAudioSamples(const int16_t* interleaved, uint32_t frameCount);

	uint32_t GetVideoFrameCount() const { return m_videoFrameCount; }
	uint64_t GetApproximateFileSize() const { return m_approxFileSize; }

private:
	struct IndexEntry
	{
		char id[4];
		uint32_t flags;
		uint32_t offset; // relative to start of 'movi' data (i.e. first byte after the 'movi' fourCC)
		uint32_t size;
	};

	// low level helpers
	void WriteRaw(const void* data, size_t size);
	void WriteFourCC(const char* fourCC);
	void WriteU16(uint16_t v);
	void WriteU32(uint32_t v);
	std::streampos ReserveU32(); // writes a 0 placeholder, returns its position for later patch
	void PatchU32At(std::streampos pos, uint32_t value);

	// writes [fourCC][size][data][pad?] ; if addToIndex, records offset/size relative to movi start
	void WriteChunk(const char* fourCC, const void* data, uint32_t size, bool addToIndex);

	void WriteMainHeaderPlaceholder();
	void WriteStreamHeadersAndFormats();

	// Actual close logic, assumes m_writeMutex is already held by the caller.
	// Open() and the public Close() both funnel through this to avoid
	// self-deadlocking on the non-recursive m_writeMutex.
	void CloseLocked();

	static constexpr uint64_t kMaxFileSize = 0xFFFFFFFFull - (64ull * 1024 * 1024); // leave headroom for idx1 + header patch

	std::ofstream m_file;
	std::mutex m_writeMutex;

	VideoParams m_video{};
	AudioParams m_audio{};

	uint32_t m_videoFrameCount = 0;
	uint32_t m_audioTotalFrames = 0; // audio frames (samples per channel), used for strh->dwLength of audio stream

	std::streampos m_riffSizeField{};
	std::streampos m_aviMainHeaderFramesField{};
	std::streampos m_videoStreamLengthField{};
	std::streampos m_audioStreamLengthField{};
	std::streampos m_moviSizeField{};
	std::streampos m_moviDataStart{}; // first byte after 'movi' fourCC; index offsets are relative to this

	uint64_t m_approxFileSize = 0;

	std::vector<IndexEntry> m_index;
	std::vector<uint8_t> m_scratchBuffer; // used to compact rows when rowStrideBytes != width*4
};

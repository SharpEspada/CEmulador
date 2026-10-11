#include "FrameDump/AviWriter.h"

#include <cstring>

namespace
{
	constexpr uint32_t kAVIF_HASINDEX = 0x00000010;
	constexpr uint32_t kAVIF_ISINTERLEAVED = 0x00000100;

	constexpr uint32_t kBI_RGB = 0;

	constexpr uint16_t kWAVE_FORMAT_PCM = 1;

	constexpr uint32_t kAVIIF_KEYFRAME = 0x00000010;
}

AviWriter::~AviWriter()
{
	Close();
}

void AviWriter::WriteRaw(const void* data, size_t size)
{
	m_file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
	m_approxFileSize += size;
}

void AviWriter::WriteFourCC(const char* fourCC)
{
	WriteRaw(fourCC, 4);
}

void AviWriter::WriteU16(uint16_t v)
{
	WriteRaw(&v, sizeof(v));
}

void AviWriter::WriteU32(uint32_t v)
{
	WriteRaw(&v, sizeof(v));
}

std::streampos AviWriter::ReserveU32()
{
	std::streampos pos = m_file.tellp();
	WriteU32(0);
	return pos;
}

void AviWriter::PatchU32At(std::streampos pos, uint32_t value)
{
	std::streampos cur = m_file.tellp();
	m_file.seekp(pos);
	WriteRaw(&value, sizeof(value)); // note: WriteRaw() also bumps m_approxFileSize, harmless since we seek back
	m_approxFileSize -= sizeof(value);
	m_file.seekp(cur);
}

void AviWriter::WriteChunk(const char* fourCC, const void* data, uint32_t size, bool addToIndex)
{
	if (addToIndex)
	{
		IndexEntry entry{};
		std::memcpy(entry.id, fourCC, 4);
		entry.flags = kAVIIF_KEYFRAME; // every video frame and every audio block is independently "complete" here
		entry.offset = static_cast<uint32_t>(static_cast<int64_t>(m_file.tellp()) - static_cast<int64_t>(m_moviDataStart));
		entry.size = size;
		m_index.push_back(entry);
	}

	WriteFourCC(fourCC);
	WriteU32(size);
	if (size)
		WriteRaw(data, size);
	if (size & 1)
	{
		const char pad = 0;
		WriteRaw(&pad, 1); // RIFF chunks are word-aligned
	}
}

void AviWriter::WriteMainHeaderPlaceholder()
{
	// LIST 'hdrl'
	WriteFourCC("LIST");
	std::streampos hdrlSizeField = ReserveU32();
	std::streampos hdrlStart = m_file.tellp();
	WriteFourCC("hdrl");

	// 'avih' - AVIMAINHEADER (56 bytes payload)
	WriteFourCC("avih");
	WriteU32(56);
	const uint32_t usecPerFrame = static_cast<uint32_t>(
		(1000000ull * m_video.fpsDenominator) / std::max<uint32_t>(1, m_video.fpsNumerator));
	WriteU32(usecPerFrame);					// dwMicroSecPerFrame
	WriteU32(0);								// dwMaxBytesPerSec (unknown up front, not required by readers)
	WriteU32(0);								// dwPaddingGranularity
	WriteU32(kAVIF_HASINDEX | kAVIF_ISINTERLEAVED); // dwFlags
	m_aviMainHeaderFramesField = m_file.tellp();
	WriteU32(0);								// dwTotalFrames (patched on Close)
	WriteU32(0);								// dwInitialFrames
	WriteU32(2);								// dwStreams (video + audio)
	WriteU32(1 * 1024 * 1024);					// dwSuggestedBufferSize
	WriteU32(m_video.width);					// dwWidth
	WriteU32(m_video.height);					// dwHeight
	WriteU32(0); WriteU32(0); WriteU32(0); WriteU32(0); // dwReserved[4]

	// ---- video stream ----
	WriteFourCC("LIST");
	std::streampos strlVideoSizeField = ReserveU32();
	std::streampos strlVideoStart = m_file.tellp();
	WriteFourCC("strl");

	WriteFourCC("strh");
	WriteU32(56);
	WriteFourCC("vids");						// fccType
	WriteFourCC("DIB ");						// fccHandler (uncompressed)
	WriteU32(0);								// dwFlags
	WriteU16(0);								// wPriority
	WriteU16(0);								// wLanguage
	WriteU32(0);								// dwInitialFrames
	WriteU32(m_video.fpsDenominator);			// dwScale
	WriteU32(m_video.fpsNumerator);			// dwRate  (Rate/Scale = fps)
	WriteU32(0);								// dwStart
	m_videoStreamLengthField = m_file.tellp();
	WriteU32(0);								// dwLength (patched on Close, = frame count)
	WriteU32(1 * 1024 * 1024);					// dwSuggestedBufferSize
	WriteU32(static_cast<uint32_t>(-1));		// dwQuality
	WriteU32(0);								// dwSampleSize (0 = each sample -- i.e. frame -- can have a different size)
	WriteU16(0); WriteU16(0);					// rcFrame.left/top
	WriteU16(static_cast<uint16_t>(m_video.width));
	WriteU16(static_cast<uint16_t>(m_video.height)); // rcFrame.right/bottom

	// 'strf' - BITMAPINFOHEADER (40 bytes)
	WriteFourCC("strf");
	WriteU32(40);
	WriteU32(40);								// biSize
	WriteU32(m_video.width);					// biWidth
	WriteU32(m_video.height);					// biHeight (positive = bottom-up; we store bottom-up, see WriteVideoFrame)
	WriteU16(1);								// biPlanes
	WriteU16(32);								// biBitCount (BGRA)
	WriteU32(kBI_RGB);							// biCompression
	WriteU32(m_video.width * m_video.height * 4); // biSizeImage
	WriteU32(0);								// biXPelsPerMeter
	WriteU32(0);								// biYPelsPerMeter
	WriteU32(0);								// biClrUsed
	WriteU32(0);								// biClrImportant

	PatchU32At(strlVideoSizeField, static_cast<uint32_t>(static_cast<int64_t>(m_file.tellp()) - static_cast<int64_t>(strlVideoStart)));

	// ---- audio stream ----
	WriteFourCC("LIST");
	std::streampos strlAudioSizeField = ReserveU32();
	std::streampos strlAudioStart = m_file.tellp();
	WriteFourCC("strl");

	const uint16_t blockAlign = static_cast<uint16_t>(m_audio.channels * (m_audio.bitsPerSample / 8));
	const uint32_t avgBytesPerSec = m_audio.sampleRate * blockAlign;

	WriteFourCC("strh");
	WriteU32(56);
	WriteFourCC("auds");						// fccType
	WriteU32(0);								// fccHandler (0 = PCM, no specific handler)
	WriteU32(0);								// dwFlags
	WriteU16(0);								// wPriority
	WriteU16(0);								// wLanguage
	WriteU32(0);								// dwInitialFrames
	WriteU32(blockAlign);						// dwScale     (Scale = bytes per sample-block)
	WriteU32(avgBytesPerSec);					// dwRate      (Rate/Scale = samples-blocks per sec)
	WriteU32(0);								// dwStart
	m_audioStreamLengthField = m_file.tellp();
	WriteU32(0);								// dwLength (patched on Close, = total sample-blocks)
	WriteU32(static_cast<uint32_t>(avgBytesPerSec / 4)); // dwSuggestedBufferSize (~250ms)
	WriteU32(static_cast<uint32_t>(-1));		// dwQuality
	WriteU32(blockAlign);						// dwSampleSize (audio: fixed size per sample-block)
	WriteU16(0); WriteU16(0); WriteU16(0); WriteU16(0); // rcFrame (unused for audio)

	// 'strf' - WAVEFORMATEX (18 bytes, cbSize = 0)
	WriteFourCC("strf");
	WriteU32(18);
	WriteU16(kWAVE_FORMAT_PCM);				// wFormatTag
	WriteU16(m_audio.channels);				// nChannels
	WriteU32(m_audio.sampleRate);				// nSamplesPerSec
	WriteU32(avgBytesPerSec);					// nAvgBytesPerSec
	WriteU16(blockAlign);						// nBlockAlign
	WriteU16(m_audio.bitsPerSample);			// wBitsPerSample
	WriteU16(0);								// cbSize

	PatchU32At(strlAudioSizeField, static_cast<uint32_t>(static_cast<int64_t>(m_file.tellp()) - static_cast<int64_t>(strlAudioStart)));

	PatchU32At(hdrlSizeField, static_cast<uint32_t>(static_cast<int64_t>(m_file.tellp()) - static_cast<int64_t>(hdrlStart)));
}

bool AviWriter::Open(const std::filesystem::path& path, const VideoParams& videoParams, const AudioParams& audioParams)
{
	std::lock_guard<std::mutex> lock(m_writeMutex);
	CloseLocked();

	m_file.open(path, std::ios::binary | std::ios::trunc);
	if (!m_file.is_open())
		return false;

	m_video = videoParams;
	m_audio = audioParams;
	m_videoFrameCount = 0;
	m_audioTotalFrames = 0;
	m_approxFileSize = 0;
	m_index.clear();

	WriteFourCC("RIFF");
	m_riffSizeField = ReserveU32();
	WriteFourCC("AVI ");

	WriteMainHeaderPlaceholder();

	WriteFourCC("LIST");
	m_moviSizeField = ReserveU32();
	WriteFourCC("movi");
	m_moviDataStart = m_file.tellp();

	return m_file.good();
}

bool AviWriter::WriteVideoFrame(const uint8_t* frameBGRA, uint32_t width, uint32_t height, uint32_t rowStrideBytes)
{
	std::lock_guard<std::mutex> lock(m_writeMutex);
	if (!m_file.is_open())
		return false;
	if (width != m_video.width || height != m_video.height)
		return false; // resolution changes mid-recording are not supported; caller must rotate to a new segment

	const uint32_t tightRowBytes = width * 4;
	const uint32_t frameSize = tightRowBytes * height;
	if (m_approxFileSize + frameSize > kMaxFileSize)
		return false; // caller should Close() this segment and Open() a new one

	// BITMAPINFOHEADER with a positive biHeight expects rows stored bottom-up.
	// Our source buffer is top-down, so we flip while (optionally) compacting stride.
	if (m_scratchBuffer.size() < frameSize)
		m_scratchBuffer.resize(frameSize);

	for (uint32_t y = 0; y < height; ++y)
	{
		const uint8_t* srcRow = frameBGRA + static_cast<size_t>(y) * rowStrideBytes;
		uint8_t* dstRow = m_scratchBuffer.data() + static_cast<size_t>(height - 1 - y) * tightRowBytes;
		std::memcpy(dstRow, srcRow, tightRowBytes);
	}

	WriteChunk("00dc", m_scratchBuffer.data(), frameSize, true);
	m_videoFrameCount++;
	return true;
}

bool AviWriter::WriteAudioSamples(const int16_t* interleaved, uint32_t frameCount)
{
	std::lock_guard<std::mutex> lock(m_writeMutex);
	if (!m_file.is_open())
		return false;

	const uint32_t byteSize = frameCount * m_audio.channels * sizeof(int16_t);
	if (m_approxFileSize + byteSize > kMaxFileSize)
		return false;

	WriteChunk("01wb", interleaved, byteSize, true);
	m_audioTotalFrames += frameCount;
	return true;
}

void AviWriter::Close()
{
	std::lock_guard<std::mutex> lock(m_writeMutex);
	CloseLocked();
}

void AviWriter::CloseLocked()
{
	if (!m_file.is_open())
		return;

	// patch 'movi' LIST size
	const uint32_t moviDataSize = static_cast<uint32_t>(static_cast<int64_t>(m_file.tellp()) - static_cast<int64_t>(m_moviDataStart));
	PatchU32At(m_moviSizeField, moviDataSize + 4 /* 'movi' fourCC itself counts towards the LIST size */);

	// write idx1
	WriteFourCC("idx1");
	WriteU32(static_cast<uint32_t>(m_index.size() * sizeof(IndexEntry)));
	for (const IndexEntry& e : m_index)
	{
		WriteRaw(e.id, 4);
		WriteU32(e.flags);
		WriteU32(e.offset);
		WriteU32(e.size);
	}

	// patch frame counts
	PatchU32At(m_aviMainHeaderFramesField, m_videoFrameCount);
	PatchU32At(m_videoStreamLengthField, m_videoFrameCount);
	PatchU32At(m_audioStreamLengthField, m_audioTotalFrames);

	// patch RIFF size (everything after the 'RIFF' fourCC + size field itself)
	const uint32_t riffSize = static_cast<uint32_t>(static_cast<int64_t>(m_file.tellp()) - static_cast<int64_t>(m_riffSizeField) - 4);
	PatchU32At(m_riffSizeField, riffSize);

	m_file.close();
}

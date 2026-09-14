#include "FrameDump/FrameDumper.h"
#include "FrameDump/AviWriter.h"

#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cemu/Logging/CemuLogging.h"

std::unique_ptr<FrameDumper> g_frameDumper;

FrameDumper::FrameDumper() = default;

FrameDumper::~FrameDumper()
{
	Stop();
}

bool FrameDumper::Start(const std::filesystem::path& outputDir, uint32_t audioSampleRate, uint16_t audioChannels)
{
	if (m_active.load())
		return false; // already running; caller should Stop() first

	std::error_code ec;
	std::filesystem::create_directories(outputDir, ec);
	if (ec)
	{
		cemuLog_log(LogType::Force, "FrameDumper: failed to create output directory: {}", ec.message());
		return false;
	}

	m_outputDir = outputDir;
	m_audioSampleRate = audioSampleRate;
	m_audioChannels = audioChannels;
	m_segmentIndex = 0;
	m_stopRequested.store(false);

	m_workerThread = std::thread(&FrameDumper::WorkerThreadFunc, this);

	m_active.store(true, std::memory_order_release);
	cemuLog_log(LogType::Force, "FrameDumper: started, writing to {}", outputDir.string());
	return true;
}

void FrameDumper::Stop()
{
	if (!m_active.load())
		return;

	m_active.store(false, std::memory_order_release);
	m_stopRequested.store(true);
	m_queueWakeup.notify_all();

	if (m_workerThread.joinable())
		m_workerThread.join();

	cemuLog_log(LogType::Force, "FrameDumper: stopped");
}

void FrameDumper::NotifyPresentedFrame(LatteTextureView* texView)
{
	if (!IsActive())
		return;

	VideoItem item;
	// See Renderer::CaptureFrameForDump (Renderer.h/.cpp + one implementation
	// per backend). Returns false if this backend doesn't implement frame
	// dumping yet, or if the capture failed this particular frame (in which
	// case we simply skip the frame rather than corrupt the stream -- a
	// dropped frame is recoverable in post, a corrupted container is not).
	if (!g_renderer->CaptureFrameForDump(texView, item.bgra, item.width, item.height))
		return;

	{
		std::lock_guard<std::mutex> lock(m_queueMutex);
		m_queue.push_back(std::move(item));
	}
	m_queueWakeup.notify_one();
}

void FrameDumper::PushAudioBlock(const int16_t* interleaved, uint32_t frameCount, uint16_t channels)
{
	if (!IsActive())
		return;
	if (channels != m_audioChannels)
	{
		// Device channel count changed mid-recording (e.g. user changed
		// audio settings while dumping). Rather than silently corrupt the
		// audio stream we drop the block; consider calling Stop() from the
		// audio-device-changed callback instead if this matters to you.
		return;
	}

	AudioItem item;
	item.frameCount = frameCount;
	item.samples.assign(interleaved, interleaved + static_cast<size_t>(frameCount) * channels);

	{
		std::lock_guard<std::mutex> lock(m_queueMutex);
		m_queue.push_back(std::move(item));
	}
	m_queueWakeup.notify_one();
}

void FrameDumper::OpenNewSegment(uint32_t width, uint32_t height)
{
	CloseCurrentSegment();

	char nameBuf[64];
	std::snprintf(nameBuf, sizeof(nameBuf), "framedump_%03u.avi", m_segmentIndex++);
	std::filesystem::path segmentPath = m_outputDir / nameBuf;

	AviWriter::VideoParams videoParams{};
	videoParams.width = width;
	videoParams.height = height;
	// NOTE: Cemu does not currently expose a single authoritative "TV output
	// fps" value (games can vary; most Wii U titles target 59.94 or 29.97).
	// 60000/1001 is used as a reasonable default matching most titles.
	// If you want per-title accuracy, wire this up to whatever the graphic
	// pack / title-specific frame limiter already knows, and pass it into
	// Start() instead of hardcoding it here.
	videoParams.fpsNumerator = 60000;
	videoParams.fpsDenominator = 1001;

	AviWriter::AudioParams audioParams{};
	audioParams.sampleRate = m_audioSampleRate;
	audioParams.channels = m_audioChannels;
	audioParams.bitsPerSample = 16;

	m_currentWriter = std::make_unique<AviWriter>();
	if (!m_currentWriter->Open(segmentPath, videoParams, audioParams))
	{
		cemuLog_log(LogType::Force, "FrameDumper: failed to open {}", segmentPath.string());
		m_currentWriter.reset();
	}
}

void FrameDumper::CloseCurrentSegment()
{
	if (m_currentWriter)
	{
		m_currentWriter->Close();
		m_currentWriter.reset();
	}
}

void FrameDumper::WorkerThreadFunc()
{
	while (true)
	{
		QueueItem item;
		{
			std::unique_lock<std::mutex> lock(m_queueMutex);
			m_queueWakeup.wait(lock, [this] { return !m_queue.empty() || m_stopRequested.load(); });
			if (m_queue.empty())
			{
				if (m_stopRequested.load())
					break;
				continue;
			}
			item = std::move(m_queue.front());
			m_queue.pop_front();
		}

		if (std::holds_alternative<VideoItem>(item))
		{
			const VideoItem& v = std::get<VideoItem>(item);

			if (!m_currentWriter)
				OpenNewSegment(v.width, v.height);

			if (m_currentWriter)
			{
				const uint32_t rowStride = v.width * 4;
				if (!m_currentWriter->WriteVideoFrame(v.bgra.data(), v.width, v.height, rowStride))
				{
					// either a resolution change or the 4GiB ceiling was hit;
					// either way, start a fresh segment and retry once.
					OpenNewSegment(v.width, v.height);
					if (m_currentWriter)
						m_currentWriter->WriteVideoFrame(v.bgra.data(), v.width, v.height, rowStride);
				}
			}
		}
		else
		{
			const AudioItem& a = std::get<AudioItem>(item);
			if (m_currentWriter)
				m_currentWriter->WriteAudioSamples(a.samples.data(), a.frameCount);
			// If no writer exists yet (first queue items happened to be audio
			// before the first video frame arrived), the audio is intentionally
			// dropped: an AVI segment cannot be opened without knowing the
			// video resolution first, and desyncing at start would defeat the
			// entire point of this feature. In practice the TV backbuffer
			// resolution is known essentially immediately, so this only
			// discards a handful of milliseconds of audio at worst.
		}
	}

	// drain whatever is left without blocking further (best-effort on stop)
	{
		std::lock_guard<std::mutex> lock(m_queueMutex);
		while (!m_queue.empty())
		{
			QueueItem item = std::move(m_queue.front());
			m_queue.pop_front();
			if (std::holds_alternative<VideoItem>(item) && m_currentWriter)
			{
				const VideoItem& v = std::get<VideoItem>(item);
				m_currentWriter->WriteVideoFrame(v.bgra.data(), v.width, v.height, v.width * 4);
			}
			else if (std::holds_alternative<AudioItem>(item) && m_currentWriter)
			{
				const AudioItem& a = std::get<AudioItem>(item);
				m_currentWriter->WriteAudioSamples(a.samples.data(), a.frameCount);
			}
		}
	}

	CloseCurrentSegment();
}

#pragma once

// FrameDumper
// -----------
// Orchestrates lossless-ish video+audio dumping of the TV output, entirely
// decoupled from real (wall-clock) time: video frames are only pushed when
// the emulator actually swaps the TV backbuffer, and audio blocks are only
// pushed when the emulator actually feeds a block to the TV audio device.
// Both therefore stay on the *emulated* timeline, which is exactly why this
// avoids the desync that capturing with OBS in real time causes: nothing
// here is time-based, everything is event-based.
//
// Threading model:
//  - PushVideoFrame() is called from the render thread. It must not block on
//    disk I/O, so it only kicks off an async GPU->CPU readback (reusing the
//    engine's existing LatteTextureReadbackInfo abstraction) and returns.
//  - A dedicated worker thread polls pending readbacks, and once a readback
//    is finished, converts it to BGRA and hands it to AviWriter, which does
//    the (potentially slow) disk write off the render thread.
//  - PushAudioBlock() is called from the audio-mixing thread (ax_out.cpp).
//    Audio blocks are small and infrequent enough (one per ~3ms of emulated
//    audio) that they are written to a small lock-protected queue and
//    drained by the same worker thread, immediately before/after video
//    frames as they arrive, preserving relative ordering.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>
#include <vector>

class LatteTextureView;
class AviWriter;

class FrameDumper
{
public:
	FrameDumper();
	~FrameDumper();

	FrameDumper(const FrameDumper&) = delete;
	FrameDumper& operator=(const FrameDumper&) = delete;

	// outputDir: directory the .avi segment(s) will be written into.
	// audioSampleRate/channels: must match what ax_out.cpp actually feeds
	// (currently always 48000Hz; channel count comes from IAudioAPI::GetChannels()).
	bool Start(const std::filesystem::path& outputDir, uint32_t audioSampleRate, uint16_t audioChannels);
	void Stop();
	bool IsActive() const { return m_active.load(std::memory_order_acquire); }

	// Called from the render thread (see LatteRenderTarget_copyToBackbuffer),
	// for the TV view only. Performs a *synchronous* GPU->CPU capture via
	// Renderer::CaptureFrameForDump() (same cost class as taking a manual
	// screenshot every frame -- see the accompanying notes on why this is
	// v1 and what an async v2 would require per backend). The resulting
	// buffer is then handed off to the worker thread, so disk I/O itself
	// never blocks the render thread.
	void NotifyPresentedFrame(LatteTextureView* texView);

	// Called from the audio thread (see ax_out.cpp, right before/after
	// g_tvAudio->FeedBlock()). No-op if IsActive() is false. Copies the
	// block into the queue (cheap, ~288 samples/block) and returns.
	void PushAudioBlock(const int16_t* interleaved, uint32_t frameCount, uint16_t channels);

private:
	struct VideoItem
	{
		std::vector<uint8_t> bgra;
		uint32_t width;
		uint32_t height;
	};

	struct AudioItem
	{
		std::vector<int16_t> samples; // interleaved
		uint32_t frameCount;
	};

	using QueueItem = std::variant<VideoItem, AudioItem>;

	void WorkerThreadFunc();
	void OpenNewSegment(uint32_t width, uint32_t height); // worker-thread only
	void CloseCurrentSegment(); // worker-thread only

	std::atomic<bool> m_active{false};
	std::atomic<bool> m_stopRequested{false};

	std::filesystem::path m_outputDir;
	uint32_t m_audioSampleRate = 48000;
	uint16_t m_audioChannels = 2;

	// single ordered queue: since video items are only ever pushed from the
	// render thread and audio items only from the audio thread, and both
	// producers push in the order their events actually occurred, draining
	// this queue in FIFO order on the worker thread reconstructs the
	// original interleaving faithfully enough for AviWriter's chunk order
	// to preserve A/V sync.
	std::deque<QueueItem> m_queue;
	std::mutex m_queueMutex;
	std::condition_variable m_queueWakeup;

	std::thread m_workerThread;

	std::unique_ptr<AviWriter> m_currentWriter; // worker-thread-only
	uint32_t m_segmentIndex = 0;
};

extern std::unique_ptr<FrameDumper> g_frameDumper;

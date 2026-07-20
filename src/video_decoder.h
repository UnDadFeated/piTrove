#pragma once // PITROVE_VIDEO_DECODER_H
#include <string>
#include <pthread.h>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <SDL3/SDL_audio.h>

struct VideoFrame {
    int width = 0;
    int height = 0;
    int format = 0;          // AVPixelFormat (e.g. AV_PIX_FMT_NV12)
    int linesize_y = 0;
    int linesize_uv = 0;
    int data_size = 0;       // Total bytes in data buffer
    uint8_t* data = nullptr;
    double pts = 0.0;

    VideoFrame() = default;
    ~VideoFrame() { delete[] data; }
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    VideoFrame(VideoFrame&& o) noexcept
        : width(o.width), height(o.height), format(o.format),
          linesize_y(o.linesize_y), linesize_uv(o.linesize_uv),
          data_size(o.data_size), data(o.data), pts(o.pts) {
        o.data = nullptr;
    }
    VideoFrame& operator=(VideoFrame&& o) noexcept {
        delete[] data;
        width = o.width; height = o.height; format = o.format;
        linesize_y = o.linesize_y; linesize_uv = o.linesize_uv;
        data_size = o.data_size; data = o.data; pts = o.pts;
        o.data = nullptr;
        return *this;
    }
};


static constexpr size_t MAX_QUEUED_FRAMES = 8;
static constexpr size_t FRAME_POOL_SIZE = 10;

struct FramePool {
    std::vector<uint8_t*> buffers;
    std::queue<uint8_t*> free_list;
    std::mutex pool_mtx;
    size_t frame_size = 0;

    void init(size_t num_buffers, size_t size) {
        frame_size = size;
        for (size_t i = 0; i < num_buffers; i++) {
            uint8_t* buf = new uint8_t[size];
            buffers.push_back(buf);
            free_list.push(buf);
        }
    }

    uint8_t* acquire() {
        std::lock_guard<std::mutex> lk(pool_mtx);
        if (free_list.empty()) return nullptr;
        uint8_t* buf = free_list.front();
        free_list.pop();
        return buf;
    }

    void release(uint8_t* buf) {
        if (!buf) return;
        std::lock_guard<std::mutex> lk(pool_mtx);
        free_list.push(buf);
    }

    ~FramePool() {
        for (auto* b : buffers) delete[] b;
    }
};

class VideoDecoder {
public:
 VideoDecoder();
 ~VideoDecoder();

 bool start(const std::string& path, int target_width, int target_height);
 void stop();
 bool is_running() const;
 bool is_eof() const;
 bool get_frame(VideoFrame& out);
 bool has_frames() const;
 double get_frame_duration() const;
 double get_video_remaining(double fallback_duration = 0.0) const;
 double get_video_duration() const { return m_video_total_duration.load(std::memory_order_relaxed); }
 double get_fps() const { return m_frame_duration > 0 ? 1.0 / m_frame_duration : 0; }

private:
 std::string m_path;
 int m_target_width;
 int m_target_height;
 pthread_t m_thread;
 std::atomic<bool> m_running;
 std::atomic<bool> m_eof{false};

 std::queue<VideoFrame> m_frame_queue;
 mutable std::mutex m_queue_mtx;
 std::condition_variable m_queue_cv;

 double m_frame_duration{0.04}; // seconds per frame from stream metadata
 std::atomic<double> m_video_start_pts{0.0};
 std::atomic<double> m_video_total_duration{0.0};
 std::atomic<double> m_last_frame_pts{0.0}; // PTS of last decoded frame for accurate countdown
    std::atomic<int> m_decoded_frames{0}; // frame count for duration estimation
 std::atomic<double> decode_start_time{0.0};
 static constexpr size_t MAX_QUEUED_FRAMES = 8; // wall-clock at first frame

 // Audio
 SDL_AudioStream* m_audio_stream{nullptr};
 int m_audio_device{-1};
 bool m_audio_initialized{false};
 std::mutex m_audio_mtx;

 void init_audio();
 void shutdown_audio();
 void push_audio_samples(const int16_t* samples, int num_frames);

 void decode_loop();
 static void* decode_thread_entry(void* arg);
};

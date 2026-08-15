#pragma once // PITROVE_VIDEO_DECODER_H
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <span>
#include <queue>
#include <condition_variable>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_timer.h>

struct VideoFrame {
    int width = 0;
    int height = 0;
    int format = 0;          // AVPixelFormat (e.g. AV_PIX_FMT_NV12)
    int linesize_y = 0;
    int linesize_uv = 0;
    int data_size = 0;       // Total bytes in data buffer
    uint8_t* data = nullptr; // Y-plane for NV12 or RGBA buffer
    uint8_t* data_uv = nullptr; // UV-plane for NV12
    bool is_nv12 = false;
    double pts = 0.0;

    VideoFrame() = default;
    ~VideoFrame() {
        delete[] data;
        delete[] data_uv;
    }
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    VideoFrame(VideoFrame&& o) noexcept
        : width(o.width), height(o.height), format(o.format),
          linesize_y(o.linesize_y), linesize_uv(o.linesize_uv),
          data_size(o.data_size), data(o.data), data_uv(o.data_uv),
          is_nv12(o.is_nv12), pts(o.pts) {
        o.data = nullptr;
        o.data_uv = nullptr;
    }
    VideoFrame& operator=(VideoFrame&& o) noexcept {
        if (this != &o) {
            delete[] data;
            delete[] data_uv;
            width = o.width; height = o.height; format = o.format;
            linesize_y = o.linesize_y; linesize_uv = o.linesize_uv;
            data_size = o.data_size; data = o.data; data_uv = o.data_uv;
            is_nv12 = o.is_nv12; pts = o.pts;
            o.data = nullptr;
            o.data_uv = nullptr;
        }
        return *this;
    }
};


class VideoDecoder {
public:
 VideoDecoder();
 ~VideoDecoder();

 bool start(const std::string& path, int target_width, int target_height);
 bool prewarm(const std::string& path, int target_width, int target_height);
 std::string get_current_path() const;
 void stop();
 bool is_running() const;
 bool is_eof() const;
 bool consume_start_failed();
 bool get_frame(VideoFrame& out);
 bool has_frames() const;
 size_t frame_queue_size() const;
 double get_frame_duration() const;
 double get_video_remaining(double fallback_duration = 0.0) const;

    // --- A/V sync (PTS-based presentation clock) ---
    void set_av_sync(bool on);
    bool av_sync_ready() const;
    double get_anchor_wall_ms() const;
    double get_anchor_pts() const;
    void note_frame_anchor(double pts_s, bool has_pts, bool stream_eof = false);
    void note_audio_anchor(double pts_s);
    void note_presentation_start(double pts_s);
    void set_displayed_pts(double pts_s);
 double get_video_duration() const { return m_video_total_duration.load(std::memory_order_relaxed); }
 double get_fps() const { return m_frame_duration > 0 ? 1.0 / m_frame_duration : 0; }
 static constexpr size_t MAX_QUEUED_FRAMES = 128; // ~4.3s at 30fps / ~2.1s at 60fps (NV12 1080p ~3.1MB -> ~396MB)

private:
 std::string m_path;
 mutable std::mutex m_path_mtx;
 int m_target_width;
 int m_target_height;
 std::jthread m_thread;
 std::atomic<bool> m_running;
 std::atomic<bool> m_eof{false};
 std::atomic<bool> m_start_failed{false}; // latched when start-phase (open/probe) fails

 std::queue<VideoFrame> m_frame_queue;
 mutable std::mutex m_queue_mtx;
 std::condition_variable m_queue_cv;

 double m_frame_duration{0.04}; // seconds per frame from stream metadata
 std::atomic<double> m_video_start_pts{0.0};
 std::atomic<double> m_video_total_duration{0.0};
 std::atomic<double> m_last_frame_pts{0.0}; // PTS of last decoded frame for accurate countdown
    std::atomic<int> m_decoded_frames{0}; // frame count for duration estimation

    // --- A/V presentation clock (av_sync): video and audio share one wall-clock anchor
    std::atomic<bool>   m_av_sync{true};
    std::atomic<bool>   m_anchor_set{false};
    std::atomic<double> m_anchor_wall_ms{0.0};  // SDL_GetTicks() when anchor event was pushed
    std::atomic<double> m_anchor_pts{0.0};      // stream pts (seconds) of the anchor event
    std::atomic<bool>   m_v0_set{false};
    std::atomic<double> m_v0_pts{0.0};
    std::atomic<double> m_current_displayed_pts{0.0};         // first video frame pts (seconds)
    std::atomic<bool>   m_pts_valid{true};     // false once a frame pushes without valid pts
 std::atomic<double> decode_start_time{0.0};

 // Audio
 SDL_AudioStream* m_audio_stream{nullptr};
 int m_audio_device{-1};
 bool m_audio_initialized{false};
 std::mutex m_audio_mtx;

 void init_audio();
 void shutdown_audio();
 void push_audio_samples(std::span<const int16_t> samples, double pts_s = -1.0);

 void decode_loop();
};

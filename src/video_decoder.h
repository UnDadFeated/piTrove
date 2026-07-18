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
 uint8_t* data = nullptr;
 double pts = 0.0;
 VideoFrame() = default;
 ~VideoFrame() { delete[] data; }
 VideoFrame(const VideoFrame&) = delete;
 VideoFrame& operator=(const VideoFrame&) = delete;
 VideoFrame(VideoFrame&& o) noexcept
  : width(o.width), height(o.height), data(o.data), pts(o.pts) { o.data = nullptr; }
 VideoFrame& operator=(VideoFrame&& o) noexcept {
  delete[] data;
  width = o.width; height = o.height; data = o.data; pts = o.pts;
  o.data = nullptr;
  return *this;
 }
};

class VideoDecoder {
public:
 VideoDecoder();
 ~VideoDecoder();

 bool start(const std::string& path, int target_width, int target_height);
 void stop();
 bool is_running() const;
 bool get_frame(VideoFrame& out);
 double get_frame_duration() const;
 double get_video_remaining() const;
 double get_fps() const { return m_frame_duration > 0 ? 1.0 / m_frame_duration : 0; }

private:
 std::string m_path;
 int m_target_width;
 int m_target_height;
 pthread_t m_thread;
 std::atomic<bool> m_running;

 std::queue<VideoFrame> m_frame_queue;
 std::mutex m_queue_mtx;
 std::condition_variable m_queue_cv;

 double m_frame_duration{0.04}; // seconds per frame from stream metadata
 std::atomic<double> m_video_start_pts{0.0};
 std::atomic<double> m_video_total_duration{0.0};
 double decode_start_time{0.0}; // wall-clock at first frame

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

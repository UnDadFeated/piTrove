#pragma once // PITROVE_VIDEO_DECODER_H
#include <string>
#include <pthread.h>
#include <atomic>
#include <mutex>
#include <queue>
#include <condition_variable>

struct VideoFrame {
    int width = 0;
    int height = 0;
    uint8_t* data = nullptr;
    VideoFrame() = default;
    ~VideoFrame() {
        if (data) {
            delete[] data;
            data = nullptr;
        }
    }
    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    VideoFrame(VideoFrame&& other) noexcept
        : width(other.width), height(other.height), data(other.data) {
        other.width = 0;
        other.height = 0;
        other.data = nullptr;
    }
    VideoFrame& operator=(VideoFrame&& other) noexcept {
        if (this != &other) {
            if (data) delete[] data;
            width = other.width;
            height = other.height;
            data = other.data;
            other.width = 0;
            other.height = 0;
            other.data = nullptr;
        }
        return *this;
    }
};

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    // Start decoding the video file in a background thread
    bool start(const std::string& path, int target_width, int target_height);
    // Stop decoding and clean up
    void stop();
    // Check if decoder thread is still running
    bool is_running() const;
    // Poll for the next decoded frame (non-blocking)
    bool get_frame(VideoFrame& out);

    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

private:
    static void* decode_thread_entry(void* arg);
    void decode_loop();

    pthread_t m_thread;
    std::atomic<bool> m_running{false};

    std::mutex m_queue_mtx;
    std::queue<VideoFrame> m_frame_queue;
    std::condition_variable m_queue_cv;

    int m_target_width = 0;
    int m_target_height = 0;
    std::string m_path;
};

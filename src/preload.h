#ifndef PITROVE_PRELOAD_H
#define PITROVE_PRELOAD_H

#include <queue>
#include <unordered_set>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <memory>
#include <string>
#include <vector>
#include <SDL3/SDL.h>
#include "image_loader.h"

// Preloaded item: raw pixels + path from worker, surface created on main thread
struct PreloadedItem {
    RawImage raw;
    RawImage blur_raw;  // Blurred background (move-only)
    std::string path;
    bool valid = false;
    int exif_rotation = 1;
    uint8_t matte_r = 0;
    uint8_t matte_g = 0;
    uint8_t matte_b = 0;
    uint8_t avg_r = 220;
    uint8_t avg_g = 210;
    uint8_t avg_b = 195;
    // Per-edge averaged colors (computed in worker thread)
    uint8_t edge_r[4] = {0, 0, 0, 0};
    uint8_t edge_g[4] = {0, 0, 0, 0};
    uint8_t edge_b[4] = {0, 0, 0, 0};
    // Per-pixel edge strips (computed in worker thread)
    std::vector<uint8_t> edge_top_rgb, edge_bot_rgb;
    std::vector<uint8_t> edge_lft_rgb, edge_rgt_rgb;

    PreloadedItem() = default;
    PreloadedItem(PreloadedItem&&) noexcept = default;
    PreloadedItem& operator=(PreloadedItem&&) noexcept = default;
    PreloadedItem(const PreloadedItem&) = delete;
    PreloadedItem& operator=(const PreloadedItem&) = delete;
};

struct PreloadState {
    std::vector<PreloadedItem> loaded_items;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;

    std::queue<std::string> work_queue;
    std::unordered_set<std::string> active_preloads;
    std::mutex work_mutex;
    std::condition_variable work_cv;

    int max_size;
    std::atomic<bool> running{false};
    uint64_t current_epoch = 0;

    PreloadState(int max_size) : max_size(max_size) {}
};

class PreloadQueue {
private:
    std::shared_ptr<PreloadState> state;
    int num_threads;
    std::vector<std::thread> threads;
    SDL_Renderer* sdl_renderer;

public:
    PreloadQueue(int max_size, int num_threads, SDL_Renderer* sdl_renderer);
    ~PreloadQueue();

    void start();
    void shutdown();

    // Main thread calls this to queue a path
    void enqueue(const std::string& path);

    // Main thread calls this to retrieve the next preloaded image (uploads surface to VRAM)
    std::shared_ptr<ImageData> try_dequeue(const std::string& target_path);

    // Cancel all pending preloads
    void cancel_all();

private:
    static void worker_thread(std::shared_ptr<PreloadState> state, int thread_id);
};

#endif // PITROVE_PRELOAD_H

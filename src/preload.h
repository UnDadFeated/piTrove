#ifndef PITROVE_PRELOAD_H
#define PITROVE_PRELOAD_H

#include <queue>
#include <unordered_set>
#include <unordered_map>
#include <cstdint>
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
    PreparedImage image;
    std::string path;

    PreloadedItem() = default;
    PreloadedItem(PreloadedItem&&) noexcept = default;
    PreloadedItem& operator=(PreloadedItem&&) noexcept = default;
    PreloadedItem(const PreloadedItem&) = delete;
    PreloadedItem& operator=(const PreloadedItem&) = delete;
};

struct PreloadState {
    std::vector<PreloadedItem> loaded_items;
    std::mutex queue_mutex;
    std::atomic<int> loaded_count{0};

    std::queue<std::string> work_queue;
    std::unordered_set<std::string> active_preloads;
    // path -> steady-clock ms of last stat/read failure; enqueue skips paths
    // still in cooldown (prevents the 5s re-enqueue livelock on dead NAS files)
    std::unordered_map<std::string, int64_t> stat_failed_ts;
    std::mutex work_mutex;
    std::condition_variable_any work_cv;

    int max_size;
    std::atomic<bool> running{false};
    uint64_t current_epoch = 0;

    PreloadState(int max_size) : max_size(max_size) {}
};

class PreloadQueue {
private:
    std::shared_ptr<PreloadState> state;
    int num_threads;
    std::vector<std::jthread> threads;
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
    static void worker_thread(std::stop_token stop, std::shared_ptr<PreloadState> state, int thread_id);
};

#endif // PITROVE_PRELOAD_H

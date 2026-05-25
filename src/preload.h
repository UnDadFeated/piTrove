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

    PreloadedItem() = default;
    PreloadedItem(PreloadedItem&&) noexcept = default;
    PreloadedItem& operator=(PreloadedItem&&) noexcept = default;
    PreloadedItem(const PreloadedItem&) = delete;
    PreloadedItem& operator=(const PreloadedItem&) = delete;
};

class PreloadQueue {
private:
    // Queue of preloaded items waiting for VRAM upload on main thread
    std::queue<PreloadedItem> loaded_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;

    // Queue of paths that need to be preloaded
    std::queue<std::string> work_queue;
    std::unordered_set<std::string> active_preloads;
    std::mutex work_mutex;
    std::condition_variable work_cv;

    int max_size;
    std::atomic<bool> running{false};
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

    int pending_count();
    int ready_count();

private:
    void worker_thread(int thread_id);
};

#endif // PITROVE_PRELOAD_H

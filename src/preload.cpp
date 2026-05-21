#include "preload.h"
#include "util.h"
#include <unordered_set>

PreloadQueue::PreloadQueue(int max_size, int num_threads, SDL_Renderer* sdl_renderer)
    : max_size(max_size), num_threads(num_threads), sdl_renderer(sdl_renderer) {
    running.store(false);
}

PreloadQueue::~PreloadQueue() {
    shutdown();
}

void PreloadQueue::start() {
    if (running.load()) return;
    running.store(true);

    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(&PreloadQueue::worker_thread, this, i);
    }
    g_logger.info("PreloadQueue started with %d worker threads (capacity=%d)", num_threads, max_size);
}

void PreloadQueue::shutdown() {
    if (!running.load()) return;
    running.store(false);

    work_cv.notify_all();
    queue_cv.notify_all();

    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }
    threads.clear();

    cancel_all();
    g_logger.info("PreloadQueue shut down successfully");
}

void PreloadQueue::enqueue(const std::string& path) {
    if (path.empty()) return;
    {
        std::lock_guard<std::mutex> lock(work_mutex);
        
        if (queued_paths.count(path)) return;

        work_queue.push(path);
        queued_paths.insert(path);
    }
    work_cv.notify_one();
}

std::shared_ptr<ImageData> PreloadQueue::try_dequeue() {
    std::shared_ptr<ImageData> data = nullptr;
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (!loaded_queue.empty()) {
            data = loaded_queue.front();
            loaded_queue.pop();
        }
    }

    if (data) {
        // We are on the main thread, so it is safe to upload the surface to VRAM
        if (data->valid && data->surface && !data->texture) {
            ImageLoader::load_texture(data.get(), sdl_renderer);
        }
        // Wake up workers because we just freed a slot in the loaded queue
        work_cv.notify_all();
    }
    return data;
}

void PreloadQueue::cancel_all() {
    {
        std::lock_guard<std::mutex> lock(work_mutex);
        std::queue<std::string> empty;
        std::swap(work_queue, empty);
        queued_paths.clear();
    }
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        while (!loaded_queue.empty()) {
            auto data = loaded_queue.front();
            ImageLoader::unload(data.get());
            loaded_queue.pop();
        }
    }
    work_cv.notify_all();
}

int PreloadQueue::pending_count() {
    std::lock_guard<std::mutex> lock(work_mutex);
    return work_queue.size();
}

int PreloadQueue::ready_count() {
    std::lock_guard<std::mutex> lock(queue_mutex);
    return loaded_queue.size();
}

void PreloadQueue::worker_thread(int thread_id) {
    g_logger.debug("Preload worker thread %d starting", thread_id);
    while (running.load()) {
        std::string path;
        {
            std::unique_lock<std::mutex> lock(work_mutex);
            work_cv.wait(lock, [this] {
                bool has_work = !work_queue.empty();
                bool space_available = false;
                {
                    std::lock_guard<std::mutex> qlk(queue_mutex);
                    space_available = (int)loaded_queue.size() < max_size;
                }
                return (has_work && space_available) || !running.load();
            });

            if (!running.load() && work_queue.empty()) break;
            if (work_queue.empty()) continue;

            path = work_queue.front();
            work_queue.pop();
            queued_paths.erase(path);
        }

        g_logger.debug("[Worker %d] preloading: %s", thread_id, path.c_str());
        auto data = ImageLoader::load(path);

        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            loaded_queue.push(data);
        }
        queue_cv.notify_one();
    }
    g_logger.debug("Preload worker thread %d exiting", thread_id);
}

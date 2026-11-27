#include <thread>
#include "preload.h"
#include "renderer.h"
#include "util.h"
#include "image_loader.h"
#include "scanner.h"
#include <sys/stat.h>
#include <algorithm>
#include <chrono>

PreloadQueue::PreloadQueue(int max_size, int num_threads, SDL_Renderer* sdl_renderer)
    : num_threads(num_threads), sdl_renderer(sdl_renderer) {
    state = std::make_shared<PreloadState>(max_size);
}

PreloadQueue::~PreloadQueue() {
    shutdown();
}

void PreloadQueue::start() {
    {
        std::lock_guard lock(state->work_mutex);
        if (state->running.exchange(true)) return;
    }

    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; i++) {
        std::jthread th;
        if (spawn_thread_safe(th, "preload_worker", &PreloadQueue::worker_thread, state, i)) {
            threads.emplace_back(std::move(th));
        } else {
            // Degrade rather than let std::system_error escape start() and abort the process.
            g_logger.error("PreloadQueue: failed to spawn worker {} (running degraded)", i);
        }
    }
    g_logger.info("PreloadQueue started with {} worker threads (capacity={})", threads.size(), state->max_size);
}

void PreloadQueue::shutdown() {
    {
        std::lock_guard lock(state->work_mutex);
        if (!state->running.exchange(false)) return;
    }
    for (auto& th : threads) th.request_stop();

    state->work_cv.notify_all();
    threads.clear();

    cancel_all();
    g_logger.info("PreloadQueue shut down successfully");
}

static int64_t now_steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
// How long a failed path is skipped by enqueue() before a retry is attempted.
static constexpr int64_t STAT_FAIL_COOLDOWN_MS = 60000;
static constexpr size_t  STAT_FAIL_MAP_MAX = 1000;

void PreloadQueue::enqueue(const std::string& path) {
    if (path.empty()) return;
    {
        std::lock_guard<std::mutex> lock(state->work_mutex);
        if (!state->running.load() || state->active_preloads.count(path)) return;
        auto it = state->stat_failed_ts.find(path);
        if (it != state->stat_failed_ts.end()) {
            if (now_steady_ms() - it->second < STAT_FAIL_COOLDOWN_MS) return;
            state->stat_failed_ts.erase(it); // cooldown expired: retry
        }
        g_logger.debug("PreloadQueue::enqueue '{}'", path.c_str());
        state->work_queue.push(path);
        state->active_preloads.insert(path);
    }
    state->work_cv.notify_one();
}

std::shared_ptr<ImageData> PreloadQueue::try_dequeue(const std::string& target_path) {
    std::shared_ptr<ImageData> data = nullptr;
    PreloadedItem item;
    {
        std::scoped_lock lk(state->work_mutex, state->queue_mutex);
        g_logger.debug("PreloadQueue::try_dequeue queue_size={} target={}", state->loaded_items.size(), target_path);
        auto it = std::find_if(state->loaded_items.begin(), state->loaded_items.end(),
            [&](const PreloadedItem& item) { return item.path == target_path; });

        if (it != state->loaded_items.end()) {
            item = std::move(*it);
            state->loaded_items.erase(it);
            state->loaded_count.store(std::ssize(state->loaded_items));
            state->active_preloads.erase(target_path);
        }
    }
    if (item.image.raw.valid) {
        data = ImageLoader::make_surface(std::move(item.image), item.path);
        ImageLoader::load_texture(data.get(), sdl_renderer);
    }

    state->work_cv.notify_all();
    return data;
}

void PreloadQueue::cancel_all() {
    {
        std::scoped_lock lock(state->work_mutex, state->queue_mutex);
        std::queue<std::string> empty;
        std::swap(state->work_queue, empty);
        state->active_preloads.clear();
        state->current_epoch++;
        state->loaded_items.clear();
        state->loaded_count.store(0);
    }
    state->work_cv.notify_all();
}

void PreloadQueue::worker_thread(std::stop_token stop, std::shared_ptr<PreloadState> state, int thread_id) {
    g_logger.debug("Preload worker thread {} starting", thread_id);
    while (!stop.stop_requested() && state->running.load()) {
        std::string path;
        uint64_t task_epoch = 0;
        {
            std::unique_lock<std::mutex> lock(state->work_mutex);
            state->work_cv.wait(lock, stop, [state] {
                bool has_work = !state->work_queue.empty();
                bool space_available = state->loaded_count.load() < state->max_size;
                return (has_work && space_available) || !state->running.load();
            });

            if (stop.stop_requested() || !state->running.load()) break;
            if (state->work_queue.empty()) continue;

            path = state->work_queue.front();
            state->work_queue.pop();
            task_epoch = state->current_epoch;
        }

        g_logger.debug("[Worker {}] preloading: {}", thread_id, path.c_str());
        auto note_failure = [&] {
            std::lock_guard lock(state->work_mutex);
            if (task_epoch == state->current_epoch) {
                state->active_preloads.erase(path);
                if (state->stat_failed_ts.size() >= STAT_FAIL_MAP_MAX) state->stat_failed_ts.clear();
                state->stat_failed_ts[path] = now_steady_ms();
            }
        };

        struct stat st;
        if (!stat_timeout(path, st, 5000, stop)) {
            if (stop.stop_requested()) break;
            g_logger.warn("[Worker {}] stat_timeout: '{}' inaccessible, skipping.", thread_id, path.c_str());
            note_failure();
            continue;
        }

        std::vector<uint8_t> buffer = ImageLoader::read_file_to_buffer(path, stop);
        if (stop.stop_requested()) break;
        if (buffer.empty()) {
            g_logger.warn("[Worker {}] Failed to read file to buffer: {}", thread_id, path.c_str());
            note_failure();
            continue;
        }

        auto image = ImageLoader::prepare(buffer, stop);
        if (stop.stop_requested()) break;
        if (!image.raw.valid) {
            g_logger.warn("[Worker {}] Failed to decode: {}", thread_id, path);
            note_failure();
            continue;
        }

        {
            std::scoped_lock lock(state->work_mutex, state->queue_mutex);
            if (task_epoch != state->current_epoch) {
                g_logger.debug("[Worker {}] Discarding stale preload item (epoch mismatch: {} vs {}) for {}",
                    thread_id, task_epoch, state->current_epoch, path);
                continue;
            }
            PreloadedItem item;
            item.image = std::move(image);
            item.path = path;
            state->loaded_items.push_back(std::move(item));
            // Keep capacity under max_size by popping/discarding oldest items
            while (std::ssize(state->loaded_items) > state->max_size) {
                state->active_preloads.erase(state->loaded_items.front().path);
                state->loaded_items.erase(state->loaded_items.begin());
            }
            state->loaded_count.store(std::ssize(state->loaded_items));
        }
    }
    g_logger.debug("Preload worker thread {} exiting", thread_id);
}

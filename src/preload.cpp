#include "preload.h"
#include "renderer.h"
#include "util.h"
#include "image_loader.h"
#include <algorithm>
#include <cstring>
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
    g_logger.info("TRACE: PreloadQueue::enqueue '%s'", path.c_str());
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
    g_logger.info("TRACE: PreloadQueue::try_dequeue queue_size=%d", (int)loaded_queue.size());
    std::shared_ptr<ImageData> data = nullptr;
    {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (!loaded_queue.empty()) {
            PreloadedItem item = std::move(loaded_queue.front());
            loaded_queue.pop();

            // Build ImageData from raw pixels (main thread — SDL context is thread-local)
            data = std::make_shared<ImageData>();
            data->valid = false;
            
            if (item.raw.valid) {
                // Read EXIF (file I/O, no SDL)
                data->exif_rotation = ImageLoader::read_exif_rotation(item.path.c_str());
                data->width = item.raw.width;
                data->height = item.raw.height;
                data->valid = true;

                // Create SDL surface from raw pixels
                data->surface = SDL_CreateSurface(item.raw.width, item.raw.height, SDL_PIXELFORMAT_RGBA32);
                if (data->surface) {
                    memcpy(data->surface->pixels, item.raw.pixels, (size_t)item.raw.width * item.raw.height * 4);
                    free(item.raw.pixels);
                    item.raw.pixels = nullptr;

                    if (data->exif_rotation >= 2 && data->exif_rotation <= 8) {
                        SDL_Surface* rotated = ImageLoader::apply_exif_rotation(data->surface, data->exif_rotation);
                        if (rotated) {
                            SDL_DestroySurface(data->surface);
                            data->surface = rotated;
                        }
                    }

                    // Extract average color
                    GpuColor avg = Renderer::get_average_color(data->surface);
                    data->avg_r = avg.r;
                    data->avg_g = avg.g;
                    data->avg_b = avg.b;

                    // Sample 4 edge colors for bias gradient
                    for (int e = 0; e < 4; e++) {
                        GpuColor ec = Renderer::get_edge_average_color(data->surface, 8, e);
                        data->edge_r[e] = ec.r;
                        data->edge_g[e] = ec.g;
                        data->edge_b[e] = ec.b;
                    }

                    // Per-pixel edge strips: average 3px deep per position
                    {
                        uint8_t* px = (uint8_t*)data->surface->pixels;
                        int bpp = SDL_BYTESPERPIXEL(data->surface->format);
                        int sw = data->surface->w, sh = data->surface->h;
                        int pitch = data->surface->pitch;

                        data->edge_top_rgb.resize(sw * 3);
                        for (int x = 0; x < sw; x++) {
                            int ar = 0, ag = 0, ab = 0, ac = 0;
                            for (int d = 0; d < 3; d++) {
                                const uint8_t* dp = px + x * bpp + d * pitch;
                                ar += dp[0]; ag += dp[1]; ab += dp[2]; ac++;
                            }
                            data->edge_top_rgb[x * 3 + 0] = (uint8_t)(ar / ac);
                            data->edge_top_rgb[x * 3 + 1] = (uint8_t)(ag / ac);
                            data->edge_top_rgb[x * 3 + 2] = (uint8_t)(ab / ac);
                        }

                        data->edge_bot_rgb.resize(sw * 3);
                        for (int x = 0; x < sw; x++) {
                            int ar = 0, ag = 0, ab = 0, ac = 0;
                            for (int d = -1; d <= 1; d++) {
                                int ry = sh - 1 + d;
                                if (ry >= 0 && ry < sh) {
                                    const uint8_t* dp = px + x * bpp + ry * pitch;
                                    ar += dp[0]; ag += dp[1]; ab += dp[2]; ac++;
                                }
                            }
                            data->edge_bot_rgb[x * 3 + 0] = (uint8_t)(ar / ac);
                            data->edge_bot_rgb[x * 3 + 1] = (uint8_t)(ag / ac);
                            data->edge_bot_rgb[x * 3 + 2] = (uint8_t)(ab / ac);
                        }

                        data->edge_lft_rgb.resize(sh * 3);
                        for (int y = 0; y < sh; y++) {
                            const uint8_t* p = px + y * pitch;
                            int ar = 0, ag = 0, ab = 0, ac = 0;
                            for (int w = 0; w < 3 && w < sw; w++) {
                                ar += p[w * bpp + 0]; ag += p[w * bpp + 1]; ab += p[w * bpp + 2]; ac++;
                            }
                            data->edge_lft_rgb[y * 3 + 0] = (uint8_t)(ar / ac);
                            data->edge_lft_rgb[y * 3 + 1] = (uint8_t)(ag / ac);
                            data->edge_lft_rgb[y * 3 + 2] = (uint8_t)(ab / ac);
                        }

                        data->edge_rgt_rgb.resize(sh * 3);
                        for (int y = 0; y < sh; y++) {
                            const uint8_t* p = px + y * pitch;
                            int ar = 0, ag = 0, ab = 0, ac = 0;
                            for (int w = 0; w < 3; w++) {
                                int wc = sw - 1 - w;
                                if (wc >= 0) { ar += p[wc * bpp + 0]; ag += p[wc * bpp + 1]; ab += p[wc * bpp + 2]; ac++; }
                            }
                            data->edge_rgt_rgb[y * 3 + 0] = (uint8_t)(ar / ac);
                            data->edge_rgt_rgb[y * 3 + 1] = (uint8_t)(ag / ac);
                            data->edge_rgt_rgb[y * 3 + 2] = (uint8_t)(ab / ac);
                        }
                    }
                }
            }
        }
    }

    if (data && data->valid && data->surface && !data->texture) {
        ImageLoader::load_texture(data.get(), sdl_renderer);
    }

    work_cv.notify_all();
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
            // The destructor of PreloadedItem automatically destroys its RawImage member,
            // which safely releases the pixels memory. No manual free is needed here.
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
                    std::scoped_lock qlk(queue_mutex);
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
        
        // Decode to raw pixels ONLY — no SDL calls, safe for worker threads
        RawImage raw = ImageLoader::load_raw(path);
        
        if (!raw.valid) {
            g_logger.warn("[Worker %d] Failed to decode: %s", thread_id, path.c_str());
            continue;
        }

        g_logger.debug("[Worker %d] Decoded %dx%d: %s", thread_id, raw.width, raw.height, path.c_str());

        // Push raw data to loaded queue — main thread creates SDL surface
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            PreloadedItem item;
            item.raw = std::move(raw);
            item.path = path;
            item.valid = true;
            loaded_queue.push(std::move(item));
        }
        queue_cv.notify_one();
    }
    g_logger.debug("Preload worker thread %d exiting", thread_id);
}

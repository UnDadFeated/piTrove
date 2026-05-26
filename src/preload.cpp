#include "preload.h"
#include "renderer.h"
#include "util.h"
#include "image_loader.h"
#include "blur.h"
#include "config.h"
#include <algorithm>
#include <cstring>
#include <unordered_set>

static void compute_average_color(const RawImage& src, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (!src.valid || !src.pixels || src.width <= 0 || src.height <= 0) {
        r = 220; g = 210; b = 195;
        return;
    }
    const int MAX_STEPS = 64;
    int step_x = std::max(1, src.width / MAX_STEPS);
    int step_y = std::max(1, src.height / MAX_STEPS);
    long sum_r = 0, sum_g = 0, sum_b = 0, samples = 0;
    for (int y = 0; y < src.height; y += step_y) {
        for (int x = 0; x < src.width; x += step_x) {
            const uint8_t* px = src.pixels + y * 4 * src.width + x * 4;
            sum_r += px[0]; sum_g += px[1]; sum_b += px[2];
            ++samples;
        }
    }
    if (samples == 0) {
        r = 220; g = 210; b = 195;
    } else {
        r = (uint8_t)(sum_r / samples);
        g = (uint8_t)(sum_g / samples);
        b = (uint8_t)(sum_b / samples);
    }
}

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
        
        if (active_preloads.count(path)) return;

        g_logger.info("TRACE: PreloadQueue::enqueue '%s'", path.c_str());
        work_queue.push(path);
        active_preloads.insert(path);
    }
    work_cv.notify_one();
}

std::shared_ptr<ImageData> PreloadQueue::try_dequeue(const std::string& target_path) {
    g_logger.info("TRACE: PreloadQueue::try_dequeue queue_size=%d target=%s", (int)loaded_queue.size(), target_path.c_str());
    std::shared_ptr<ImageData> data = nullptr;
    {
        std::lock_guard<std::mutex> work_lk(work_mutex);
        std::lock_guard<std::mutex> lock(queue_mutex);
        while (!loaded_queue.empty()) {
            if (loaded_queue.front().path == target_path) {
                PreloadedItem item = std::move(loaded_queue.front());
                loaded_queue.pop();
                active_preloads.erase(target_path);

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
                        if (data->surface) {
                            data->width = data->surface->w;
                            data->height = data->surface->h;
                        }

                        // Extract average color from preloaded background calculations
                        data->avg_r = item.avg_r;
                        data->avg_g = item.avg_g;
                        data->avg_b = item.avg_b;

                        // Extract matte color from preloaded background calculations
                        data->matte_r = item.matte_r;
                        data->matte_g = item.matte_g;
                        data->matte_b = item.matte_b;

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

                            // Guard: skip edge sampling for images too small to sample
                            if (sw > 0 && sh > 0) {
                                data->edge_top_rgb.resize(sw * 3);
                                for (int x = 0; x < sw; x++) {
                                    int ar = 0, ag = 0, ab = 0, ac = 0;
                                    int samples = sh < 3 ? sh : 3;
                                    for (int d = 0; d < samples; d++) {
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
                                    int ar = 0, ag = 0, ab = 0, ac = 0;
                                    const uint8_t* p = px + y * pitch;
                                    int samples = sw < 3 ? sw : 3;
                                    for (int w = 0; w < samples; w++) {
                                        ar += p[w * bpp + 0]; ag += p[w * bpp + 1]; ab += p[w * bpp + 2]; ac++;
                                    }
                                    data->edge_lft_rgb[y * 3 + 0] = (uint8_t)(ar / ac);
                                    data->edge_lft_rgb[y * 3 + 1] = (uint8_t)(ag / ac);
                                    data->edge_lft_rgb[y * 3 + 2] = (uint8_t)(ab / ac);
                                }

                                data->edge_rgt_rgb.resize(sh * 3);
                                for (int y = 0; y < sh; y++) {
                                    int ar = 0, ag = 0, ab = 0, ac = 0;
                                    const uint8_t* p = px + y * pitch;
                                    int samples = sw < 3 ? sw : 3;
                                    for (int w = 0; w < samples; w++) {
                                        int wc = sw - 1 - w;
                                        if (wc >= 0 && wc < sw) { ar += p[wc * bpp + 0]; ag += p[wc * bpp + 1]; ab += p[wc * bpp + 2]; ac++; }
                                    }
                                    data->edge_rgt_rgb[y * 3 + 0] = (uint8_t)(ar / ac);
                                    data->edge_rgt_rgb[y * 3 + 1] = (uint8_t)(ag / ac);
                                    data->edge_rgt_rgb[y * 3 + 2] = (uint8_t)(ab / ac);
                                }
                            }
                        }
                    }

                    // Move blur_raw to ImageData (no SDL needed — renderer creates texture on demand)
                    if (item.blur_raw.valid && item.blur_raw.pixels) {
                        data->blur_raw = std::move(item.blur_raw);
                    }
                }
                break;
            } else {
                g_logger.warn("Preload mismatch: front is '%s', target is '%s'. Discarding front stale item.",
                    loaded_queue.front().path.c_str(), target_path.c_str());
                active_preloads.erase(loaded_queue.front().path);
                loaded_queue.pop();
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
        active_preloads.clear();
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

            if (!running.load()) break;
            if (work_queue.empty()) continue;

            path = work_queue.front();
            work_queue.pop();
            // Keep path in active_preloads while decoding is actively in progress to prevent duplicate preloads
        }

        g_logger.debug("[Worker %d] preloading: %s", thread_id, path.c_str());
        
        // Decode to raw pixels ONLY — no SDL calls, safe for worker threads
        RawImage raw = ImageLoader::load_raw(path);
        
        if (!raw.valid) {
            g_logger.warn("[Worker %d] Failed to decode: %s", thread_id, path.c_str());
            {
                std::lock_guard<std::mutex> lock(work_mutex);
                active_preloads.erase(path);
            }
            continue;
        }

        g_logger.debug("[Worker %d] Decoded %dx%d: %s", thread_id, raw.width, raw.height, path.c_str());

        // Compute blurred background and matte color in worker thread
        int blur_radius = 14;
        {
            std::lock_guard<std::mutex> lock(g_config_mtx);
            blur_radius = g_renderer.scale_px(g_cfg.blur_radius);
        }
        RawImage blur = box_blur(raw, blur_radius);

        uint8_t mr = 0, mg = 0, mb = 0;
        if (blur.valid) {
            compute_matte_color(raw, mr, mg, mb);
        }

        uint8_t ar = 220, ag = 210, ab = 195;
        compute_average_color(raw, ar, ag, ab);

        // Push raw data to loaded queue — main thread creates SDL surface
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            PreloadedItem item;
            item.raw = std::move(raw);
            item.blur_raw = std::move(blur);
            item.path = path;
            item.valid = true;
            item.matte_r = mr;
            item.matte_g = mg;
            item.matte_b = mb;
            item.avg_r = ar;
            item.avg_g = ag;
            item.avg_b = ab;
            loaded_queue.push(std::move(item));
        }
        queue_cv.notify_one();
    }
    g_logger.debug("Preload worker thread %d exiting", thread_id);
}

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
                        uint8_t* dst = (uint8_t*)data->surface->pixels;
                        const uint8_t* src = item.raw.pixels;
                        for (int y = 0; y < item.raw.height; y++) {
                            memcpy(dst + y * data->surface->pitch, src + y * item.raw.width * 4, item.raw.width * 4);
                        }
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

                        // Copy precomputed matte color from worker thread
                        data->matte_r = item.matte_r;
                        data->matte_g = item.matte_g;
                        data->matte_b = item.matte_b;

                        // Copy precomputed edge colors from worker thread
                        for (int e = 0; e < 4; e++) {
                            data->edge_r[e] = item.edge_r[e];
                            data->edge_g[e] = item.edge_g[e];
                            data->edge_b[e] = item.edge_b[e];
                        }

                        // Copy precomputed per-pixel edge strips from worker thread
                        data->edge_top_rgb = std::move(item.edge_top_rgb);
                        data->edge_bot_rgb = std::move(item.edge_bot_rgb);
                        data->edge_lft_rgb = std::move(item.edge_lft_rgb);
                        data->edge_rgt_rgb = std::move(item.edge_rgt_rgb);
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
        std::scoped_lock lock(work_mutex, queue_mutex);
        std::queue<std::string> empty;
        std::swap(work_queue, empty);
        active_preloads.clear();
        current_epoch++;
        while (!loaded_queue.empty()) {
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

static void compute_edge_data(const RawImage& raw,
    uint8_t edge_r[4], uint8_t edge_g[4], uint8_t edge_b[4],
    std::vector<uint8_t>& edge_top_rgb, std::vector<uint8_t>& edge_bot_rgb,
    std::vector<uint8_t>& edge_lft_rgb, std::vector<uint8_t>& edge_rgt_rgb)
{
    if (!raw.valid || raw.width <= 0 || raw.height <= 0) return;
    int w = raw.width, h = raw.height;
    const uint8_t* px = raw.pixels;
    int stride = w * 4;
    const int STEPS = 32;

    // Edge average colors (sampled, matching get_edge_average_color)
    auto sample_edge = [&](int x0, int y0, int x1, int y1) -> GpuColor {
        int sx = std::max(1, (x1 - x0) / STEPS);
        int sy = std::max(1, (y1 - y0) / STEPS);
        long r = 0, g = 0, b = 0, n = 0;
        for (int y = y0; y < y1; y += sy)
            for (int x = x0; x < x1; x += sx) {
                const uint8_t* p = px + y * stride + x * 4;
                r += p[0]; g += p[1]; b += p[2]; n++;
            }
        if (n == 0) return GpuColor{0,0,0,255};
        return GpuColor{(uint8_t)(r/n), (uint8_t)(g/n), (uint8_t)(b/n), 255};
    };
    int depth = 8;
    GpuColor ec = sample_edge(0, 0, w, std::min(depth, h));
    edge_r[0] = ec.r; edge_g[0] = ec.g; edge_b[0] = ec.b;
    ec = sample_edge(0, std::max(0, h - depth), w, h);
    edge_r[1] = ec.r; edge_g[1] = ec.g; edge_b[1] = ec.b;
    ec = sample_edge(0, 0, std::min(depth, w), h);
    edge_r[2] = ec.r; edge_g[2] = ec.g; edge_b[2] = ec.b;
    ec = sample_edge(std::max(0, w - depth), 0, w, h);
    edge_r[3] = ec.r; edge_g[3] = ec.g; edge_b[3] = ec.b;

    // Per-pixel edge strips (3px deep average per position)
    int ns = h < 3 ? h : 3;
    edge_top_rgb.resize(w * 3);
    for (int x = 0; x < w; x++) {
        int ar = 0, ag = 0, ab = 0;
        for (int d = 0; d < ns; d++) {
            const uint8_t* p = px + x * 4 + d * stride;
            ar += p[0]; ag += p[1]; ab += p[2];
        }
        edge_top_rgb[x * 3 + 0] = (uint8_t)(ar / ns);
        edge_top_rgb[x * 3 + 1] = (uint8_t)(ag / ns);
        edge_top_rgb[x * 3 + 2] = (uint8_t)(ab / ns);
    }

    edge_bot_rgb.resize(w * 3);
    for (int x = 0; x < w; x++) {
        int ar = 0, ag = 0, ab = 0;
        for (int d = -1; d <= 1; d++) {
            int ry = h - 1 + d;
            if (ry >= 0 && ry < h) {
                const uint8_t* p = px + x * 4 + ry * stride;
                ar += p[0]; ag += p[1]; ab += p[2];
            }
        }
        edge_bot_rgb[x * 3 + 0] = (uint8_t)(ar / ns);
        edge_bot_rgb[x * 3 + 1] = (uint8_t)(ag / ns);
        edge_bot_rgb[x * 3 + 2] = (uint8_t)(ab / ns);
    }

    ns = w < 3 ? w : 3;
    edge_lft_rgb.resize(h * 3);
    for (int y = 0; y < h; y++) {
        int ar = 0, ag = 0, ab = 0;
        for (int ww = 0; ww < ns; ww++) {
            const uint8_t* p = px + y * stride + ww * 4;
            ar += p[0]; ag += p[1]; ab += p[2];
        }
        edge_lft_rgb[y * 3 + 0] = (uint8_t)(ar / ns);
        edge_lft_rgb[y * 3 + 1] = (uint8_t)(ag / ns);
        edge_lft_rgb[y * 3 + 2] = (uint8_t)(ab / ns);
    }

    edge_rgt_rgb.resize(h * 3);
    for (int y = 0; y < h; y++) {
        int ar = 0, ag = 0, ab = 0;
        int sample_count = 0;
        for (int ww = 0; ww < ns; ww++) {
            int wc = w - 1 - ww;
            if (wc >= 0 && wc < w) {
                const uint8_t* p = px + y * stride + wc * 4;
                ar += p[0]; ag += p[1]; ab += p[2];
                sample_count++;
            }
        }
        if (sample_count == 0) sample_count = 1;
        edge_rgt_rgb[y * 3 + 0] = (uint8_t)(ar / sample_count);
        edge_rgt_rgb[y * 3 + 1] = (uint8_t)(ag / sample_count);
        edge_rgt_rgb[y * 3 + 2] = (uint8_t)(ab / sample_count);
    }
}

void PreloadQueue::worker_thread(int thread_id) {
    g_logger.debug("Preload worker thread %d starting", thread_id);
    while (running.load()) {
        std::string path;
        uint64_t task_epoch = 0;
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
            task_epoch = current_epoch;
        }

        g_logger.debug("[Worker %d] preloading: %s", thread_id, path.c_str());

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

        // Edge strip data (precomputed in worker to avoid main-thread stalls)
        uint8_t edge_r[4] = {0,0,0,0}, edge_g[4] = {0,0,0,0}, edge_b[4] = {0,0,0,0};
        std::vector<uint8_t> edge_top_rgb, edge_bot_rgb, edge_lft_rgb, edge_rgt_rgb;
        compute_edge_data(raw, edge_r, edge_g, edge_b,
            edge_top_rgb, edge_bot_rgb, edge_lft_rgb, edge_rgt_rgb);

        {
            std::scoped_lock lock(work_mutex, queue_mutex);
            if (task_epoch != current_epoch) {
                g_logger.debug("[Worker %d] Discarding stale preload item (epoch mismatch: %llu vs %llu) for %s",
                    thread_id, (unsigned long long)task_epoch, (unsigned long long)current_epoch, path.c_str());
                continue;
            }
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
            item.edge_r[0] = edge_r[0]; item.edge_r[1] = edge_r[1]; item.edge_r[2] = edge_r[2]; item.edge_r[3] = edge_r[3];
            item.edge_g[0] = edge_g[0]; item.edge_g[1] = edge_g[1]; item.edge_g[2] = edge_g[2]; item.edge_g[3] = edge_g[3];
            item.edge_b[0] = edge_b[0]; item.edge_b[1] = edge_b[1]; item.edge_b[2] = edge_b[2]; item.edge_b[3] = edge_b[3];
            item.edge_top_rgb = std::move(edge_top_rgb);
            item.edge_bot_rgb = std::move(edge_bot_rgb);
            item.edge_lft_rgb = std::move(edge_lft_rgb);
            item.edge_rgt_rgb = std::move(edge_rgt_rgb);
            loaded_queue.push(std::move(item));
        }
        queue_cv.notify_one();
    }
    g_logger.debug("Preload worker thread %d exiting", thread_id);
}

#include "preload.h"
#include "renderer.h"
#include "util.h"
#include "image_loader.h"
#include "blur.h"
#include "config.h"
#include "scanner.h"
#include <sys/stat.h>
#include <shared_mutex>
#include <future>
#include <stb_image.h>
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
    long long sum_r = 0, sum_g = 0, sum_b = 0, samples = 0;
    for (int y = 0; y < src.height; y += step_y) {
        for (int x = 0; x < src.width; x += step_x) {
            long long offset = (long long)y * 4 * src.width + (long long)x * 4;
            if (offset >= 0 && offset + 3 < (long long)src.width * src.height * 4) {
                const uint8_t* px = src.pixels + offset;
                sum_r += px[0]; sum_g += px[1]; sum_b += px[2];
                ++samples;
            }
        }
    }
    if (samples <= 0) {
        r = 220; g = 210; b = 195;
    } else {
        r = (uint8_t)(sum_r / samples);
        g = (uint8_t)(sum_g / samples);
        b = (uint8_t)(sum_b / samples);
    }
}

PreloadQueue::PreloadQueue(int max_size, int num_threads, SDL_Renderer* sdl_renderer)
    : num_threads(num_threads), sdl_renderer(sdl_renderer) {
    state = std::make_shared<PreloadState>(max_size);
}

PreloadQueue::~PreloadQueue() {
    shutdown();
}

void PreloadQueue::start() {
    if (state->running.load()) return;
    state->running.store(true);

    threads.reserve(num_threads);
    for (int i = 0; i < num_threads; i++) {
        threads.emplace_back(&PreloadQueue::worker_thread, state, i);
    }
    g_logger.info("PreloadQueue started with %d worker threads (capacity=%d)", num_threads, state->max_size);
    g_logger.info("Health check caching enabled (TTL=5s, NAS monitor=10s)");
}

void PreloadQueue::shutdown() {
    if (!state->running.load()) return;
    state->running.store(false);

    state->work_cv.notify_all();
    state->queue_cv.notify_all();

    for (auto& t : threads) {
        if (t.joinable()) {
            std::thread([&t] { t.join(); }).detach();
        }
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    threads.clear();

    cancel_all();
    g_logger.info("PreloadQueue shut down successfully");
}

void PreloadQueue::enqueue(const std::string& path) {
    if (path.empty()) return;
    {
        std::lock_guard<std::mutex> lock(state->work_mutex);
        
        if (state->active_preloads.count(path)) return;

        g_logger.info("TRACE: PreloadQueue::enqueue '%s'", path.c_str());
        state->work_queue.push(path);
        state->active_preloads.insert(path);
    }
    state->work_cv.notify_one();
}

std::shared_ptr<ImageData> PreloadQueue::try_dequeue(const std::string& target_path) {
    g_logger.info("TRACE: PreloadQueue::try_dequeue queue_size=%d target=%s", (int)state->loaded_items.size(), target_path.c_str());
    std::shared_ptr<ImageData> data = nullptr;
    {
        std::scoped_lock lk(state->work_mutex, state->queue_mutex);
        auto it = std::find_if(state->loaded_items.begin(), state->loaded_items.end(),
            [&](const PreloadedItem& item) { return item.path == target_path; });

        if (it != state->loaded_items.end()) {
            PreloadedItem item = std::move(*it);
            state->loaded_items.erase(it);
            state->loaded_count.store((int)state->loaded_items.size());
            state->active_preloads.erase(target_path);

            // Build ImageData from raw pixels (main thread — SDL context is thread-local)
            data = std::make_shared<ImageData>();
            data->valid = false;

            if (item.raw.valid) {
                // Retrieve pre-computed EXIF rotation from worker thread
                data->exif_rotation = item.exif_rotation;
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
                }
                if (item.raw.pixels) {
                    free(item.raw.pixels);
                    item.raw.pixels = nullptr;
                }

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

                // Move blur_raw to ImageData (no SDL needed — renderer creates texture on demand)
                if (item.blur_raw.valid && item.blur_raw.pixels) {
                    data->blur_raw = std::move(item.blur_raw);
                }
            }
        }
    }

    if (data && data->valid && data->surface && !data->texture) {
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
        long long r = 0, g = 0, b = 0, n = 0;
        for (int y = y0; y < y1; y += sy)
            for (int x = x0; x < x1; x += sx) {
                long long offset = (long long)y * stride + (long long)x * 4;
                if (offset >= 0) {
                    const uint8_t* p = px + offset;
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            }
        if (n <= 0) return GpuColor{0,0,0,255};
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
        edge_top_rgb[x * 3 + 0] = (uint8_t)(ns > 0 ? (ar / ns) : 0);
        edge_top_rgb[x * 3 + 1] = (uint8_t)(ns > 0 ? (ag / ns) : 0);
        edge_top_rgb[x * 3 + 2] = (uint8_t)(ns > 0 ? (ab / ns) : 0);
    }

    edge_bot_rgb.resize(w * 3);
    for (int x = 0; x < w; x++) {
        int ar = 0, ag = 0, ab = 0;
        int sample_count = 0;
        for (int d = -1; d <= 1; d++) {
            int ry = h - 1 + d;
            if (ry >= 0 && ry < h) {
                const uint8_t* p = px + x * 4 + ry * stride;
                ar += p[0]; ag += p[1]; ab += p[2];
                sample_count++;
            }
        }
        if (sample_count > 0) {
            edge_bot_rgb[x * 3 + 0] = (uint8_t)(ar / sample_count);
            edge_bot_rgb[x * 3 + 1] = (uint8_t)(ag / sample_count);
            edge_bot_rgb[x * 3 + 2] = (uint8_t)(ab / sample_count);
        } else {
            edge_bot_rgb[x * 3 + 0] = 0;
            edge_bot_rgb[x * 3 + 1] = 0;
            edge_bot_rgb[x * 3 + 2] = 0;
        }
    }

    ns = w < 3 ? w : 3;
    edge_lft_rgb.resize(h * 3);
    for (int y = 0; y < h; y++) {
        int ar = 0, ag = 0, ab = 0;
        for (int ww = 0; ww < ns; ww++) {
            const uint8_t* p = px + y * stride + ww * 4;
            ar += p[0]; ag += p[1]; ab += p[2];
        }
        edge_lft_rgb[y * 3 + 0] = (uint8_t)(ns > 0 ? (ar / ns) : 0);
        edge_lft_rgb[y * 3 + 1] = (uint8_t)(ns > 0 ? (ag / ns) : 0);
        edge_lft_rgb[y * 3 + 2] = (uint8_t)(ns > 0 ? (ab / ns) : 0);
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
        edge_rgt_rgb[y * 3 + 0] = (uint8_t)(sample_count > 0 ? (ar / sample_count) : 0);
        edge_rgt_rgb[y * 3 + 1] = (uint8_t)(sample_count > 0 ? (ag / sample_count) : 0);
        edge_rgt_rgb[y * 3 + 2] = (uint8_t)(sample_count > 0 ? (ab / sample_count) : 0);
    }
}

void PreloadQueue::worker_thread(std::shared_ptr<PreloadState> state, int thread_id) {
    g_logger.info("Preload worker %d: I/O throttle enabled (50ms between reads, capacity=%d)", thread_id, state->max_size);
    g_logger.info("  Throttle ACTIVE: I/O spread to protect CIFS session");
    g_logger.debug("Preload worker thread %d starting", thread_id);
    while (state->running.load()) {
        std::string path;
        uint64_t task_epoch = 0;
        {
            std::unique_lock<std::mutex> lock(state->work_mutex);
            state->work_cv.wait(lock, [state] {
                bool has_work = !state->work_queue.empty();
                bool space_available = state->loaded_count.load() < state->max_size;
                return (has_work && space_available) || !state->running.load();
            });

            if (!state->running.load()) break;
            if (state->work_queue.empty()) continue;

            path = state->work_queue.front();
            state->work_queue.pop();
            task_epoch = state->current_epoch;
        }

        g_logger.debug("[Worker %d] preloading: %s", thread_id, path.c_str());

        struct stat st;
        if (!stat_timeout(path, st, 5000)) {
            g_logger.warn("[Worker %d] stat_timeout: '%s' inaccessible, skipping.", thread_id, path.c_str());
            {
                std::lock_guard<std::mutex> lock(state->work_mutex);
                state->active_preloads.erase(path);
            }
            continue;
        }

        std::vector<uint8_t> buffer = ImageLoader::read_file_to_buffer(path);
        if (buffer.empty()) {
            g_logger.warn("[Worker %d] Failed to read file to buffer: %s", thread_id, path.c_str());
            {
                std::lock_guard<std::mutex> lock(state->work_mutex);
                state->active_preloads.erase(path);
            }
            continue;
        }

        int w = 0, h = 0, ch = 0;
        uint8_t* pixels = stbi_load_from_memory(buffer.data(), (int)buffer.size(), &w, &h, &ch, 4);
        if (!pixels || w <= 0 || h <= 0) {
            g_logger.warn("[Worker %d] Failed to decode: %s", thread_id, path.c_str());
            {
                std::lock_guard<std::mutex> lock(state->work_mutex);
                state->active_preloads.erase(path);
            }
            continue;
        }

        RawImage raw;
        raw.width = w;
        raw.height = h;
        raw.channels = 4;
        raw.format = ImageFormat::RGBA32;
        raw.valid = true;

        size_t buf_size = (size_t)w * h * 4;
        raw.pixels = (uint8_t*)malloc(buf_size);
        if (!raw.pixels) {
            stbi_image_free(pixels);
            {
                std::lock_guard<std::mutex> lock(state->work_mutex);
                state->active_preloads.erase(path);
            }
            continue;
        }
        memcpy(raw.pixels, pixels, buf_size);
        stbi_image_free(pixels);

        int exif_rotation = ImageLoader::read_exif_rotation_from_memory(buffer.data(), (unsigned int)buffer.size());

        g_logger.debug("[Worker %d] Decoded %dx%d (EXIF: %d): %s", thread_id, raw.width, raw.height, exif_rotation, path.c_str());

        int blur_radius = 14;
        {
            std::shared_lock<std::shared_mutex> lock(g_config_mtx);
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
            std::scoped_lock lock(state->work_mutex, state->queue_mutex);
            if (task_epoch != state->current_epoch) {
                g_logger.debug("[Worker %d] Discarding stale preload item (epoch mismatch: %llu vs %llu) for %s",
                    thread_id, (unsigned long long)task_epoch, (unsigned long long)state->current_epoch, path.c_str());
                if (raw.pixels) { free(raw.pixels); raw.pixels = nullptr; }
                if (blur.pixels) { free(blur.pixels); blur.pixels = nullptr; }
                continue;
            }
            PreloadedItem item;
            item.raw = std::move(raw);
            item.blur_raw = std::move(blur);
            item.path = path;
            item.valid = true;
            item.exif_rotation = exif_rotation;
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
            state->loaded_items.push_back(std::move(item));
            // Keep capacity under max_size by popping/discarding oldest items
            while ((int)state->loaded_items.size() > state->max_size) {
                state->active_preloads.erase(state->loaded_items.front().path);
                state->loaded_items.erase(state->loaded_items.begin());
            }
            state->loaded_count.store((int)state->loaded_items.size());
        }
        state->queue_cv.notify_one();

        // I/O throttle: spread out file reads to protect CIFS session
        std::this_thread::sleep_for(std::chrono::microseconds(50000));  // 50ms between reads
    }
    g_logger.debug("Preload worker thread %d exiting", thread_id);
}

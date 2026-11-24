#include "preprocess.h"
#include "cache.h"
#include "util.h"
#include "config.h"
#include "media_item.h"
#include "image_loader.h"
#include "scanner.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <mutex>
#include <cstdio>
#include <memory>
#include <fstream>
#include "video_input.h"

static std::jthread g_preprocess_thread;
static std::atomic<bool> g_preprocess_running{false};
static std::atomic<bool> g_preprocess_finished{true};
static std::atomic<int> g_metadata_workers{0};
static constexpr int MAX_METADATA_WORKERS = 2;
struct MetadataSlot { ~MetadataSlot() { g_metadata_workers.fetch_sub(1); } };

static bool get_image_metadata_fast(const std::string& path, int& out_w, int& out_h, int& out_exif, int& out_is_camera, int64_t& out_creation_time, double& out_lat, double& out_lon, bool& out_has_gps) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return false;
    }
    std::vector<uint8_t> buffer(256 * 1024);
    size_t got = 0;
    while (got < buffer.size()) {
        const ssize_t n = ::read(fd, buffer.data() + got, buffer.size() - got);
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        got += static_cast<size_t>(n);
    }
    ::close(fd);
    buffer.resize(got);
    if (buffer.empty()) return false;

    int w = 0, h = 0;
    if (!ImageLoader::image_dimensions(buffer, w, h)) {
        return false;
    }
    ImageMetadata meta = ImageLoader::read_metadata_from_memory(buffer);
    const bool swap_dimensions = meta.rotation >= 5 && meta.rotation <= 8;
    out_w = swap_dimensions ? h : w;
    out_h = swap_dimensions ? w : h;
    out_exif = meta.rotation;
    out_is_camera = meta.is_camera ? 1 : 0;
    out_creation_time = meta.creation_time;
    out_lat = meta.latitude;
    out_lon = meta.longitude;
    out_has_gps = meta.has_gps;
    return true;
}

static bool get_video_metadata(const std::string& path, int& out_w, int& out_h, double& out_duration) {
    AVFormatContext* context = avformat_alloc_context();
    if (!context) return false;
    struct Input {
        AVFormatContext*& context;
        ~Input() { avformat_close_input(&context); }
    } input{context};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    context->interrupt_callback = {
        [](void* data) -> int {
            return std::chrono::steady_clock::now() >=
                   *static_cast<const std::chrono::steady_clock::time_point*>(data);
        }, &deadline
    };
    const int opened = open_local_video_input(&context, path.c_str());
    if (opened == AVERROR_INVALIDDATA) out_w = -1;
    if (opened < 0 || avformat_find_stream_info(context, nullptr) < 0) return false;
    const int video = av_find_best_stream(context, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (video < 0) return false;
    const AVCodecParameters* stream = context->streams[video]->codecpar;
    out_w = stream->width;
    out_h = stream->height;
    out_duration = context->duration > 0 ? static_cast<double>(context->duration) / AV_TIME_BASE : 0.0;
    return out_w > 0 && out_h > 0;
}

static std::vector<std::pair<std::string, std::string>> fetch_preprocess_batch() {
    std::vector<std::pair<std::string, std::string>> batch;
    if (!g_cache) return batch;
    std::lock_guard<std::mutex> lk(g_cache->db_mutex);
    if (!g_cache->db) return batch;

    int batch_size = std::clamp(static_cast<int>(std::thread::hardware_concurrency() * 5), 10, 50);
    std::string sql = std::format("SELECT path, type FROM cache WHERE bad = 0 AND preprocessed = 0 LIMIT {};", batch_size);
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(g_cache->db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (p && t) {
                batch.push_back({p, t});
            }
        }
        sqlite3_finalize(stmt);
    }
    return batch;
}

static void preprocess_loop(std::stop_token stop) {
    g_logger.info("Preprocess: Background preprocessing thread started.");
    g_preprocess_finished.store(false);

    while (g_preprocess_running.load() && g_running.load()) {
        auto batch = fetch_preprocess_batch();
        if (batch.empty()) {
            for (int i = 0; i < 50 && g_preprocess_running.load() && g_running.load(); i++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }

        if (g_metadata_workers.load() >= MAX_METADATA_WORKERS) {
            for (int i = 0; i < 50 && g_preprocess_running.load() && g_running.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        g_logger.info("Preprocess: Found {} items needing preprocessing. Processing...", batch.size());

        std::vector<MediaItem> processed;
        std::vector<std::string> completed_without_metadata;
        bool stat_failed = false;
        for (const auto& [path, type] : batch) {
            if (!g_preprocess_running.load() || !g_running.load()) break;

            struct stat st;
            if (!stat_timeout(path, st, 5000, stop)) {
                g_logger.debug("Preprocess: File '{}' inaccessible; deferring metadata.", path);
                stat_failed = true;
                continue;
            }

            MediaItem mi;
            mi.path = path;
            mi.filename = std::filesystem::path(path).filename().string();
            mi.ext = std::filesystem::path(path).extension().string();
            mi.type = type;
            mi.file_size = st.st_size;
            mi.modified_time = st.st_mtime;

            struct PreprocessResult {
                std::atomic<bool> done{false};
                bool success{false};
                MediaItem item;
            };
            auto pr = std::make_shared<PreprocessResult>();
            pr->item = mi;

            if (g_metadata_workers.load() >= MAX_METADATA_WORKERS) {
                // Only this coordinator reserves slots; blocked workers keep their slots.
                // Do not mark unprocessed items complete just because the NAS is hung.
                break;
            }
            g_metadata_workers.fetch_add(1);
            std::jthread t;
            if (spawn_thread_safe(t, "preprocess_item", [pr, path, type]() {
                MetadataSlot slot;
                try {
                if (type == "image") {
                    int w = 0, h = 0, exif = 1, is_camera = 0;
                    int64_t creation = 0;
                    double lat = 0.0, lon = 0.0;
                    bool has_gps = false;
                    if (get_image_metadata_fast(path, w, h, exif, is_camera, creation, lat, lon, has_gps)) {
                        pr->item.width = w;
                        pr->item.height = h;
                        pr->item.exif_rotation = exif;
                        pr->item.is_camera = is_camera;
                        pr->item.creation_time = creation;
                        pr->item.latitude = lat;
                        pr->item.longitude = lon;
                        pr->item.has_gps = has_gps;
                        pr->success = true;
                    }
                } else if (type == "video") {
                    int w = 0, h = 0;
                    double dur = 0.0;
                    if (get_video_metadata(path, w, h, dur)) {
                        pr->item.width = w;
                        pr->item.height = h;
                        pr->item.duration = dur;
                        pr->success = true;
                    } else {
                        pr->item.width = w;
                    }
                }
                } catch (...) { pr->success = false; }
                pr->done.store(true);
            })) {
                t.detach();
            } else {
                g_metadata_workers.fetch_sub(1);
                // Leave metadata pending when resources are temporarily exhausted.
                continue;
            }

            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10000); // 10s timeout
            while (!pr->done.load() && std::chrono::steady_clock::now() < deadline && g_preprocess_running.load() && g_running.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (pr->done.load()) {
                if (pr->success) {
                    processed.push_back(std::move(pr->item));
                } else {
                    g_logger.warn("Preprocess: Metadata probe failed for '{}'.", path);
                    completed_without_metadata.push_back(path);
                }
            } else if (g_preprocess_running.load() && g_running.load()) {
                g_logger.warn("Preprocess: Metadata probe timed out for '{}'.", path);
                completed_without_metadata.push_back(path);
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        const bool saved = g_cache && g_cache->upsert_batch(processed, 1, completed_without_metadata);
        if (stat_failed || !saved) {
            if (stat_failed) g_logger.warn("Preprocess: NAS metadata reads deferred; retrying after backoff.");
            for (int i = 0; i < 50 && g_preprocess_running.load() && g_running.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    g_preprocess_finished.store(true);
    g_logger.info("Preprocess: Background preprocessing thread exiting.");
}

void start_preprocess_worker() {
    if (g_preprocess_running.load()) return;
    g_preprocess_running.store(true);
    g_preprocess_finished.store(false);
    if (spawn_thread_safe(g_preprocess_thread, "preprocess", preprocess_loop)) {
        g_logger.info("Preprocess: Background preprocessing thread spawned successfully.");
    } else {
        g_preprocess_running.store(false);
        g_preprocess_finished.store(true);
    }
}

void stop_preprocess_worker() {
    g_preprocess_running.store(false);
    g_preprocess_thread.request_stop();
    if (g_preprocess_thread.joinable()) {
        int timeout_ms = 2500;
        while (timeout_ms > 0 && !g_preprocess_finished.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            timeout_ms -= 10;
        }
        if (g_preprocess_thread.joinable()) {
            g_preprocess_thread.join();
            g_logger.info("Preprocess: Background preprocessing thread joined cleanly.");
        }
    }
}

#include "preprocess.h"
#include "cache.h"
#include "util.h"
#include "config.h"
#include "media_item.h"
#include "image_loader.h"
#include <stb_image.h>
#include <unistd.h>
#include <sys/stat.h>
#include <filesystem>
#include <thread>
#include <chrono>
#include <vector>
#include <string>
#include <mutex>
#include <cstdio>
#include <memory>

static std::thread g_preprocess_thread;
static std::atomic<bool> g_preprocess_running{false};
static std::atomic<bool> g_preprocess_finished{true};

static bool get_image_metadata_fast(const std::string& path, int& out_w, int& out_h, int& out_exif, int& out_is_camera, int64_t& out_creation_time) {
    std::vector<uint8_t> buffer = ImageLoader::read_file_to_buffer(path);
    if (buffer.empty()) return false;

    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(buffer.data(), (int)buffer.size(), &w, &h, &comp) || w <= 0 || h <= 0) {
        return false;
    }
    out_w = w;
    out_h = h;

    ImageMetadata meta = ImageLoader::read_metadata_from_memory(buffer.data(), (unsigned int)buffer.size());
    out_exif = meta.rotation;
    out_is_camera = meta.is_camera ? 1 : 0;
    out_creation_time = meta.creation_time;
    return true;
}

static bool get_video_metadata_ffprobe(const std::string& path, int& out_w, int& out_h, double& out_duration) {
    std::string cmd = "ffprobe -v error -select_streams v:0 -show_entries stream=width,height -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 '" + escape_shell_arg(path) + "'";
    std::shared_ptr<FILE> pipe(popen((cmd + " 2>/dev/null").c_str(), "r"), pclose);
    if (!pipe) return false;
    char buffer[128];
    std::vector<std::string> lines;
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        lines.push_back(trim(buffer));
    }
    if (lines.size() < 3) return false;
    try {
        out_w = std::stoi(lines[0]);
        out_h = std::stoi(lines[1]);
        out_duration = std::stod(lines[2]);
        return true;
    } catch (...) {
        return false;
    }
}

static std::vector<std::pair<std::string, std::string>> fetch_preprocess_batch() {
    std::vector<std::pair<std::string, std::string>> batch;
    if (!g_cache) return batch;
    std::lock_guard<std::mutex> lk(g_cache->db_mutex);
    if (!g_cache->db) return batch;

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT path, type FROM cache WHERE bad = 0 AND preprocessed = 0 LIMIT 20;";
    if (sqlite3_prepare_v2(g_cache->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
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

static void preprocess_loop() {
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

        g_logger.info("Preprocess: Found %zu items needing preprocessing. Processing...", batch.size());

        for (const auto& [path, type] : batch) {
            if (!g_preprocess_running.load() || !g_running.load()) break;

            struct stat st;
            if (stat(path.c_str(), &st) != 0) {
                g_logger.warn("Preprocess: File '%s' not found or inaccessible. Marking bad.", path.c_str());
                if (g_cache) g_cache->mark_bad(path);
                continue;
            }

            MediaItem mi;
            mi.path = path;
            mi.filename = std::filesystem::path(path).filename().string();
            mi.ext = std::filesystem::path(path).extension().string();
            mi.type = type;
            mi.file_size = st.st_size;
            mi.modified_time = st.st_mtime;

            bool success = false;
            if (type == "image") {
                int w = 0, h = 0, exif = 1, is_camera = 0;
                int64_t creation = 0;
                if (get_image_metadata_fast(path, w, h, exif, is_camera, creation)) {
                    mi.width = w;
                    mi.height = h;
                    mi.exif_rotation = exif;
                    mi.is_camera = is_camera;
                    mi.creation_time = creation;
                    success = true;
                }
            } else if (type == "video") {
                int w = 0, h = 0;
                double dur = 0.0;
                if (get_video_metadata_ffprobe(path, w, h, dur)) {
                    mi.width = w;
                    mi.height = h;
                    mi.duration = dur;
                    success = true;
                }
            }

            if (success) {
                if (g_cache) {
                    g_cache->upsert(mi, 0, 1);
                }
            } else {
                g_logger.warn("Preprocess: Failed to extract metadata for '%s'. Marking bad.", path.c_str());
                if (g_cache) g_cache->mark_bad(path);
            }

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
    g_preprocess_thread = std::thread(preprocess_loop);
    g_logger.info("Preprocess: Background preprocessing thread spawned successfully.");
}

void stop_preprocess_worker() {
    g_preprocess_running.store(false);
    if (g_preprocess_thread.joinable()) {
        int timeout_ms = 500;
        while (timeout_ms > 0 && !g_preprocess_finished.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            timeout_ms -= 10;
        }
        if (g_preprocess_finished.load()) {
            g_preprocess_thread.join();
            g_logger.info("Preprocess: Background preprocessing thread stopped successfully.");
        } else {
            g_preprocess_thread.detach();
            g_logger.warn("Preprocess: Preprocessor thread did not exit cleanly. Detached.");
        }
    }
}

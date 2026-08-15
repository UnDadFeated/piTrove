#include "preprocess.h"
#include "cache.h"
#include "util.h"
#include "config.h"
#include "media_item.h"
#include "image_loader.h"
#include "scanner.h"
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

static std::jthread g_preprocess_thread;
static std::atomic<bool> g_preprocess_running{false};
static std::atomic<bool> g_preprocess_finished{true};

static bool get_image_metadata_fast(const std::string& path, int& out_w, int& out_h, int& out_exif, int& out_is_camera, int64_t& out_creation_time) {
    std::vector<uint8_t> buffer = ImageLoader::read_file_to_buffer(path);
    if (buffer.empty()) return false;

    int w = 0, h = 0, comp = 0;
    if (!stbi_info_from_memory(buffer.data(), std::ssize(buffer), &w, &h, &comp) || w <= 0 || h <= 0) {
        return false;
    }
    out_w = w;
    out_h = h;

    ImageMetadata meta = ImageLoader::read_metadata_from_memory(buffer);
    out_exif = meta.rotation;
    out_is_camera = meta.is_camera ? 1 : 0;
    out_creation_time = meta.creation_time;
    return true;
}

static bool get_video_metadata_ffprobe(const std::string& path, int& out_w, int& out_h, double& out_duration) {
    std::string cmd = "ffprobe -v error -select_streams v:0 -show_entries stream=width,height -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 '" + escape_shell_arg(path) + "'";
    FILE* raw_pipe = popen((cmd + " 2>&1").c_str(), "r");
    if (!raw_pipe) return false;
    std::shared_ptr<FILE> pipe(raw_pipe, pclose);
    char buffer[1024];
    std::vector<std::string> lines;
    while (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
        std::string line = trim(buffer);
        if (line.find("moov atom not found") != std::string::npos ||
            line.find("Invalid data found") != std::string::npos) {
            out_w = -1;
            return false;
        }
        lines.push_back(line);
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

        g_logger.info("Preprocess: Found {} items needing preprocessing. Processing...", batch.size());

        for (const auto& [path, type] : batch) {
            if (!g_preprocess_running.load() || !g_running.load()) break;

            struct stat st;
            if (!stat_timeout(path, st, 5000)) {
                g_logger.warn("Preprocess: File '{}' not found or inaccessible (stat timeout). Marking bad.", path.c_str());
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

            std::jthread t;
            if (spawn_thread_safe(t, "preprocess_item", [pr, path, type]() {
                if (type == "image") {
                    int w = 0, h = 0, exif = 1, is_camera = 0;
                    int64_t creation = 0;
                    if (get_image_metadata_fast(path, w, h, exif, is_camera, creation)) {
                        pr->item.width = w;
                        pr->item.height = h;
                        pr->item.exif_rotation = exif;
                        pr->item.is_camera = is_camera;
                        pr->item.creation_time = creation;
                        pr->success = true;
                    }
                } else if (type == "video") {
                    int w = 0, h = 0;
                    double dur = 0.0;
                    if (get_video_metadata_ffprobe(path, w, h, dur)) {
                        pr->item.width = w;
                        pr->item.height = h;
                        pr->item.duration = dur;
                        pr->success = true;
                    } else {
                        pr->item.width = w;
                    }
                }
                pr->done.store(true);
            })) {
                t.detach();
            } else {
                // Spawn failed: mark done so the waiter proceeds with no metadata.
                pr->done.store(true);
            }

            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10000); // 10s timeout
            while (!pr->done.load() && std::chrono::steady_clock::now() < deadline && g_preprocess_running.load() && g_running.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (pr->done.load()) {
                if (pr->success) {
                    if (g_cache) {
                        g_cache->upsert(pr->item, 0, 1);
                    }
                } else {
                    if (g_cache) {
                        if (pr->item.width == -1) {
                            pr->item.width = 1920;
                            pr->item.height = 1080;
                        }
                        g_logger.warn("Preprocess: Failed/corrupted metadata for '{}'. Marking preprocessed.", path.c_str());
                        g_cache->upsert(pr->item, 0, 1);
                    }
                }
            } else {
                g_logger.warn("Preprocess: Timeout (10s) extracting metadata for '{}' -- marking preprocessed.", path.c_str());
                if (g_cache) {
                    g_cache->upsert(pr->item, 0, 1);
                }
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
    if (spawn_thread_safe(g_preprocess_thread, "preprocess", preprocess_loop)) {
        g_logger.info("Preprocess: Background preprocessing thread spawned successfully.");
    } else {
        g_preprocess_running.store(false);
        g_preprocess_finished.store(true);
    }
}

void stop_preprocess_worker() {
    g_preprocess_running.store(false);
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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "scanner.h"
#include "util.h"
#include "config.h"
#include "cache.h"
#include <signal.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <thread>
#include <future>
#include <cstdlib>
#include <cstring>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <random>

static const char* IMAGE_EXTS[] = {"jpg", "jpeg", "png", "bmp", "tga", "gif", "webp", "tiff", "tif", "heic", "heif"};


// Compares only the final extension component (e.g. "jpeg", case-insensitive).
// Both arguments must be extension components, not full file paths.
static bool ext_matches(std::string_view ext, std::string_view target) {
    if (ext.size() != target.size()) return false;
    for (size_t i = 0; i < target.size(); ++i) {
        if (tolower((unsigned char)ext[i]) != tolower((unsigned char)target[i])) return false;
    }
    return true;
}

std::vector<std::string> read_dir(const std::string& path) {
    std::vector<std::string> entries;
    try {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
            if (ec) break;
            std::string name = entry.path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            entries.push_back(std::move(name));
        }
    } catch (...) {
        // Safe catch-all fallback
    }
    return entries;
}

std::vector<std::string> read_dir_timeout(const std::string& path, [[maybe_unused]] int timeout_ms) {
    // Timeout parameter is currently unused because directory listing
    // should not block indefinitely on a standard local/mounted FS.
    return read_dir(path);
}

 bool stat_timeout(const std::string& path, struct stat& st, [[maybe_unused]] int timeout_ms) {
    // alarm() is process-global and races between threads.
    // Revert to plain stat() with a comment that CIFS should not block
    // indefinitely in practice.
    return stat(path.c_str(), &st) == 0;
}





bool is_image(std::string_view ext_or_path) {
    std::string_view ext = ext_or_path;
    auto dot = ext_or_path.find_last_of('.');
    if (dot != std::string_view::npos) {
        ext = ext_or_path.substr(dot + 1);
    }
    for (auto e : IMAGE_EXTS) {
        if (ext_matches(ext, e)) return true;
    }
    return false;
}





bool is_in_seasonal_window(const std::string& filename, int window_days) {
    if (window_days <= 0) return true;

    int file_y = 0, file_m = 0, file_d = 0;
    if (!parse_filename_date(filename, file_y, file_m, file_d)) {
        return true; // Non-date files always scanned
    }

    time_t t = std::time(nullptr);
    tm tm_buf;
    tm* now = localtime_r(&t, &tm_buf);
    if (!now) return true;
    int curr_m = now->tm_mon + 1;
    int curr_d = now->tm_mday;

    auto get_day_of_year = [](int month, int day) {
        static const int days_before_month[] = { 0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
        if (month < 1 || month > 12) return 0;
        return days_before_month[month] + day;
    };

    int file_doy = get_day_of_year(file_m, file_d);
    int curr_doy = get_day_of_year(curr_m, curr_d);

    int diff = std::abs(curr_doy - file_doy);
    if (diff > 365 / 2) diff = 365 - diff;

    return diff <= window_days;
}









bool MediaScanner::is_month_in_window(const std::string& dirname, int window_days) {
    if (window_days <= 0) return true;

    int groups[2] = {0, 0};
    int gc = 0;
    size_t i = 0;
    while (i < dirname.size() && gc < 2) {
        while (i < dirname.size() && !isdigit(dirname[i])) i++;
        if (i >= dirname.size()) break;
        int val = 0;
        int digit_count = 0;
        while (i < dirname.size() && isdigit(dirname[i])) {
            if (digit_count < 9) {
                val = val * 10 + (dirname[i] - '0');
            }
            digit_count++;
            i++;
        }
        if (gc == 0 && digit_count >= 6 && digit_count <= 8) {
            int year = 0, month = 0;
            if (digit_count == 6) {
                year = val / 100;
                month = val % 100;
            } else {
                year = val / 10000;
                month = (val % 10000) / 100;
            }
            if (year >= 1900 && year <= 2100 && month >= 1 && month <= 12) {
                groups[0] = year;
                groups[1] = month;
                gc = 2;
                break;
            }
        }
        groups[gc++] = val;
        if (gc == 1 && i < dirname.size() && (dirname[i] == '-' || dirname[i] == '_')) i++;
    }

    if (gc < 2) return true;  // Non-date folders always scanned (per legacy)

    int folder_m = groups[1];
    if (folder_m < 1 || folder_m > 12) return true;

    time_t t = std::time(nullptr);
    tm tm_buf;
    tm* now = localtime_r(&t, &tm_buf);
    int curr_m = now->tm_mon + 1;
    int month_diff = std::abs(curr_m - folder_m);
    if (month_diff > 6) month_diff = 12 - month_diff;
    // With window_days=15, ±1 month passes. With 30+, ±2 months.
    int max_spread = (window_days + 29) / 30;
    return month_diff <= max_spread;
}

std::vector<MediaItem> MediaScanner::scan(const std::string& directory,
                                         const std::vector<std::string>& exts,
                                         int window_days,
                                         int max_depth,
                                         const std::vector<std::string>& ignore_folders,
                                         std::function<void(int)> progress) {
    live_found_count.store(0);
    std::vector<MediaItem> all_items;
    std::mutex list_mutex;

    g_logger.info("TRACE: scan start dir='%s' exts=%d window=%d depth=%d ignore=%d", directory.c_str(), (int)exts.size(), window_days, max_depth, (int)ignore_folders.size());
    std::vector<std::string> subdirs;
    std::vector<std::pair<std::string, struct stat>> root_files;

    std::function<void(const std::string&, int)> gather_dirs;
    gather_dirs = [&](const std::string& dir, int d) {
        if (d > 2) return;
        std::vector<std::string> entries = read_dir_timeout(dir, 15000);
        for (const auto& name : entries) {
            std::string p = dir + "/" + name;
            struct stat st;
            if (!stat_timeout(p, st, 5000)) continue;
            if (S_ISDIR(st.st_mode)) {
                if (name[0] == '.') continue;
                bool ignored = false;
                for (const auto& ign : ignore_folders) {
                    if (name == ign) { ignored = true; break; }
                }
                if (ignored) continue;
                if (!is_month_in_window(name, window_days)) continue;
                
                if (name == "Photos" || name == "Videos") {
                    gather_dirs(p, d + 1);
                } else {
                    subdirs.push_back(p);
                    live_found_count.fetch_add(1, std::memory_order_relaxed);
                    if (progress) progress(live_found_count.load());
                }
            } else if (S_ISREG(st.st_mode)) {
                root_files.push_back({p, st});
                live_found_count.fetch_add(1, std::memory_order_relaxed);
                if (progress) progress(live_found_count.load());
            }
        }
    };
    gather_dirs(directory, 0);

    auto worker = [&](int start_idx, int end_idx) {
        try {
            for (int i = start_idx; i < end_idx; i++) {
                std::string target_dir = subdirs[i];
                std::function<void(const std::string&, int)> rec;
                rec = [&](const std::string& dir, int d) {
                    if (d > max_depth) return;
                    std::vector<std::string> entries = read_dir_timeout(dir, 15000);
                    for (const auto& name : entries) {
                        std::string p = dir + "/" + name;
                        struct stat st;
                        if (!stat_timeout(p, st, 5000)) continue;
                        if (S_ISDIR(st.st_mode)) {
                            if (name[0] == '.') continue;
                            bool ignored = false;
                            for (const auto& ign : ignore_folders) {
                                if (name == ign) { ignored = true; break; }
                            }
                            if (ignored) continue;
                            if (!is_month_in_window(name, window_days)) continue;
                            rec(p, d + 1);
                        } else if (S_ISREG(st.st_mode)) {
                            process_entry(p, st, exts, list_mutex, all_items);
                            if (progress) progress(live_found_count.load());
                        }
                    }
                };
                rec(target_dir, 2);
            }
        } catch (...) {}
    };

    // Use hardware threads (like legacy) for max throughput
    int hw_cores = std::max(1, (int)std::thread::hardware_concurrency());
    int num_threads = std::min(hw_cores - 1, (int)subdirs.size());
    if (num_threads < 1) num_threads = 1;

    int chunk = std::max(1, (int)subdirs.size() / num_threads);
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; t++) {
        int start = t * chunk;
        int end = (t == num_threads - 1) ? (int)subdirs.size() : start + chunk;
        threads.emplace_back(worker, start, end);
    }
    for (auto& th : threads) {
        if (th.joinable()) th.join();
    }

    for (const auto& [p, st] : root_files) {
        process_entry(p, st, exts, list_mutex, all_items);
        if (progress) progress(live_found_count.load());
    }

    g_logger.info("TRACE: scan threads joined, total=%d", (int)all_items.size());
    if (progress) progress(live_found_count.load());
    return all_items;
}



void MediaScanner::process_entry(const std::string& path_str,
                                 const struct stat& st,
                                 const std::vector<std::string>& exts,
                                 std::mutex& list_mutex,
                                 std::vector<MediaItem>& all_items) {
    std::filesystem::path path_obj(path_str);
    if (path_obj.stem().string().empty()) return;

    std::string ext = path_obj.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (std::find(exts.begin(), exts.end(), ext) != exts.end()) {
        MediaItem mi;
        mi.path = path_str;
        mi.filename = path_obj.filename().string();
        mi.ext = ext;
        mi.file_size = st.st_size;
        mi.modified_time = st.st_mtime;
        mi.type = is_image(ext) ? "image" : "video";

        std::lock_guard<std::mutex> lock(list_mutex);
        all_items.push_back(std::move(mi));
        live_found_count.fetch_add(1, std::memory_order_relaxed);
    }
}

void scan_directory(const std::string& dir, int depth,
                    std::vector<MediaItem>& items,
                    std::function<void(int)> progress) {
    g_logger.info("TRACE: scan_directory dir='%s' depth=%d", dir.c_str(), depth);
    MediaScanner scanner;
    std::vector<std::string> exts = {".jpg", ".jpeg", ".png", ".webp", ".heic", ".heif", ".gif", ".bmp", ".tiff", ".mp4", ".mov", ".mkv", ".avi", ".webm"};
    int scan_days;
    std::vector<std::string> ignore_f;
    {
        std::lock_guard lk(g_config_mtx);
        scan_days = g_cfg.scan_window_days;
        ignore_f = g_cfg.ignore_folders;
    }

    auto media_files = scanner.scan(dir, exts, scan_days, depth, ignore_f, progress);
    g_logger.info("TRACE: scan returned %d media files", (int)media_files.size());

    items = std::move(media_files);
}

unsigned long long make_entropy_seed() {
    unsigned long long seed = 0;
    try {
        std::random_device rd;
        seed = rd();
        seed = (seed << 32) | rd();
    } catch (...) {
        seed = 0;
    }
    {
        FILE* f = fopen("/dev/urandom", "r");
        if (f) {
            unsigned char buf[8];
            if (fread(buf, 1, 8, f) == 8) {
                unsigned long long file_seed = 0;
                for (int i = 0; i < 8; i++) file_seed = (file_seed << 8) | buf[i];
                seed ^= file_seed;
            }
            fclose(f);
        }
    }
    seed ^= (unsigned long long)0xCAFEBABE;
    seed ^= (unsigned long long)getpid() << 32;
    
    auto now = std::chrono::high_resolution_clock::now();
    seed ^= (unsigned long long)now.time_since_epoch().count();
    
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    seed ^= ((unsigned long long)ts.tv_sec << 32) ^ ts.tv_nsec;
    
    return seed ? seed : (unsigned long long)time(nullptr);
}

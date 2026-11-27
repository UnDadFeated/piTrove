#include <atomic>
std::atomic<int> g_scanner_detached_threads{0};
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
#include <utility>
#include <memory>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <random>
#include <dirent.h>
#include <deque>
#include <set>
#include <sys/stat.h>


#include <ctime>

static inline int get_max_detached_scanner_threads() {
    unsigned int cores = std::thread::hardware_concurrency();
    if (cores == 0) cores = 4;
    return std::clamp(static_cast<int>(cores * 4), 8, 64);
}

static bool reserve_scanner_thread() {
    int active = g_scanner_detached_threads.load();
    const int limit = get_max_detached_scanner_threads();
    while (active < limit) {
        if (g_scanner_detached_threads.compare_exchange_weak(active, active + 1)) return true;
    }
    return false;
}

struct ScannerThreadSlot {
    ~ScannerThreadSlot() { g_scanner_detached_threads.fetch_sub(1); }
};

static constexpr std::string_view IMAGE_EXTS[] = {"jpg", "jpeg", "png", "bmp", "tga", "gif", "webp", "tiff", "tif", "heic", "heif"};


// Compares only the final extension component (e.g. "jpeg", case-insensitive).
// Both arguments must be extension components, not full file paths.
static bool ext_matches(std::string_view ext, std::string_view target) {
    if (ext.size() != target.size()) return false;
    for (size_t i = 0; i < target.size(); ++i) {
        if (tolower((unsigned char)ext[i]) != tolower((unsigned char)target[i])) return false;
    }
    return true;
}

static std::optional<std::vector<std::string>> read_dir(const std::string& path) {
    std::error_code ec;
    std::filesystem::directory_iterator entry(path, ec), end;
    std::vector<std::string> entries;
    for (; !ec && entry != end; entry.increment(ec)) {
        auto name = entry->path().filename().string();
        if (!name.empty() && name.front() != '.') entries.push_back(std::move(name));
    }
    if (ec) return std::nullopt;
    return entries;
}

std::optional<std::vector<std::string>> read_dir_timeout(const std::string& path, int timeout_ms,
                                                        std::stop_token stop) {
    if (stop.stop_requested()) return std::nullopt;
    struct SharedResult {
        std::mutex mutex;
        std::condition_variable_any cv;
        bool done = false;
        std::optional<std::vector<std::string>> entries;
    };
    auto sr = std::make_shared<SharedResult>();
    std::string p(path);
    if (!reserve_scanner_thread()) {
        g_logger.warn("Scanner: max outstanding threads limit reached, skipping read_dir for {}", path);
        return std::nullopt;
    }
    std::jthread t;
    if (!spawn_thread_safe(t, "scanner_read_dir", [p = std::move(p), sr]() {
        ScannerThreadSlot slot;
        std::optional<std::vector<std::string>> entries;
        try { entries = read_dir(p); } catch (...) {}
        {
            std::lock_guard lock(sr->mutex);
            sr->entries = std::move(entries);
            sr->done = true;
        }
        sr->cv.notify_one();
    })) {
        g_scanner_detached_threads.fetch_sub(1);
        return std::nullopt;
    }
    t.detach();

    std::unique_lock lock(sr->mutex);
    if (!sr->cv.wait_for(lock, stop, std::chrono::milliseconds(timeout_ms), [&] { return sr->done; })) {
        if (!stop.stop_requested()) g_logger.warn("Scanner: read_dir timed out for path {}", path);
        return std::nullopt;
    }
    if (!sr->entries) g_logger.debug("Scanner: failed to list {}", path);
    return std::move(sr->entries);
}

bool stat_timeout(const std::string& path, struct stat& st, int timeout_ms, std::stop_token stop) {
    if (stop.stop_requested()) return false;
    struct SharedResult {
        std::mutex mutex;
        std::condition_variable_any cv;
        bool done = false;
        bool ok = false;
        struct stat data{};
    };
    auto sr = std::make_shared<SharedResult>();
    std::string p(path);
    if (!reserve_scanner_thread()) return false;
    std::jthread t;
    if (!spawn_thread_safe(t, "scanner_stat", [p = std::move(p), sr]() {
        struct stat data{};
        bool ok;
        {
            ScannerThreadSlot slot;
            ok = stat(p.c_str(), &data) == 0;
        }
        {
            std::lock_guard lock(sr->mutex);
            sr->data = data;
            sr->ok = ok;
            sr->done = true;
        }
        sr->cv.notify_one();
    })) {
        g_scanner_detached_threads.fetch_sub(1);
        return false;
    }
    t.detach();

    std::unique_lock lock(sr->mutex);
    if (!sr->cv.wait_for(lock, stop, std::chrono::milliseconds(timeout_ms), [&] { return sr->done; }) || !sr->ok) {
        return false;
    }
    st = sr->data;
    return true;
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





bool is_in_seasonal_window(std::string_view filename, int window_days) {
    if (window_days <= 0) return true;

    auto date = parse_filename_date(filename);
    if (!date) {
        return true; // Non-date files always scanned
    }
    auto [file_y, file_m, file_d] = *date;

    time_t t = std::time(nullptr);
    tm tm_buf;
    tm* now = localtime_r(&t, &tm_buf);
    if (!now) return true;
    using namespace std::chrono;
    constexpr auto reference = year{2000};
    const auto today = sys_days{reference / unsigned(now->tm_mon + 1) / unsigned(now->tm_mday)};
    const auto candidate = sys_days{reference / unsigned(file_m) / unsigned(file_d)};
    const auto distance = std::abs((candidate - today).count());
    return std::min(distance, 366 - distance) <= window_days;
}

bool MediaScanner::is_month_in_window(std::string_view dirname, int window_days) {
    if (window_days <= 0) return true;
    std::string prefix;
    if (dirname.size() == 7) prefix = std::string(dirname) + "-01";
    else if (dirname.size() == 6) prefix = std::string(dirname) + "01";
    else if (dirname.size() == 8 || dirname.size() == 10) prefix = dirname;
    else return true;
    if (dirname.size() == 7 && dirname[4] != '-' && dirname[4] != '_') return true;
    if (dirname.size() == 7) prefix[7] = dirname[4];
    auto date = parse_filename_date(prefix);
    if (!date) return true;
    const auto [y, m, d] = *date;
    using namespace std::chrono;
    for (unsigned day_number = 1; (year{2000}/unsigned(m)/day_number).ok(); ++day_number) {
        if (is_in_seasonal_window(std::format("2000-{:02d}-{:02d}", m, day_number), window_days))
            return true;
    }
    return false;
}

ScanResult MediaScanner::scan(const std::string& directory, const std::vector<std::string>& exts,
                              int window_days, int max_depth,
                              const std::vector<std::string>& ignore_folders,
                              std::function<void(int)> progress, std::stop_token stop) {
    if (!g_running || stop.stop_requested() || !is_nas_online()) return {};
    live_found_count = 0;
    struct Work {
        std::mutex mutex;
        std::condition_variable_any cv;
        std::deque<std::pair<std::string, int>> dirs;
        std::set<std::pair<dev_t, ino_t>> visited;
        std::vector<MediaItem> items;
        unsigned working = 0;
        size_t steps = 0;
        bool cancelled = false;
        bool complete = true;
    };
    auto state = std::make_shared<Work>();
    state->dirs.emplace_back(directory, 0);

    // Copy all inputs into the worker so a stuck filesystem call can unwind
    // after its coordinator returns the partial results.
    auto worker = [state, exts, window_days, max_depth, ignore_folders] {
        ScannerThreadSlot slot;
        MediaScanner dates;
        auto& work = *state;
        for (;;) {
            std::pair<std::string, int> job;
            {
                std::unique_lock lock(work.mutex);
                work.cv.wait(lock, [&] { return work.cancelled || !work.dirs.empty() || !work.working; });
                if (work.cancelled || work.dirs.empty()) return;
                job = std::move(work.dirs.front());
                work.dirs.pop_front();
                ++work.working;
            }
            bool ok = true;
            std::vector<MediaItem> local_items;
            std::vector<std::pair<std::string, int>> local_dirs;
            size_t local_steps = 0;
            try {
                // glibc readdir buffers getdents64 results. Only possible media,
                // directories, symlinks, and unknown types need a metadata lookup.
                const auto close_dir = [](DIR* handle) { closedir(handle); };
                std::unique_ptr<DIR, decltype(close_dir)> dir(opendir(job.first.c_str()), close_dir);
                struct stat root{};
                ok = dir && fstat(dirfd(dir.get()), &root) == 0;
                bool first = false;
                if (ok) {
                    std::lock_guard lock(work.mutex);
                    first = work.visited.emplace(root.st_dev, root.st_ino).second;
                }
                while (first) {
                    errno = 0;
                    const auto* entry = readdir(dir.get());
                    if (!entry) { ok &= errno == 0; break; }
                    std::string_view name(entry->d_name);
                    if (name.starts_with('.') || std::ranges::find(ignore_folders, name) != ignore_folders.end()) continue;
                    const auto dot = name.find_last_of('.');
                    std::string ext(dot == name.npos ? "" : name.substr(dot));
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
                    const bool media = std::ranges::find(exts, ext) != exts.end();
                    if (entry->d_type == DT_LNK) continue;
                    if (!media && entry->d_type != DT_UNKNOWN && entry->d_type != DT_DIR) continue;
                    struct stat item{};
                    if (fstatat(dirfd(dir.get()), entry->d_name, &item, AT_SYMLINK_NOFOLLOW) != 0) {
                        // A file vanishing between readdir and fstatat is benign; do not fail the whole scan.
                        if (errno != ENOENT) ok = false;
                        continue;
                    }
                    if (S_ISLNK(item.st_mode)) continue;
                    bool recurse = S_ISDIR(item.st_mode) && job.second < max_depth &&
                                   dates.is_month_in_window(name, window_days);
                    ++local_steps;
                    if (recurse) {
                        local_dirs.emplace_back(job.first + "/" + std::string(name), job.second + 1);
                    } else if (media && S_ISREG(item.st_mode)) {
                        MediaItem file;
                        file.path = job.first + "/" + std::string(name);
                        file.filename = name;
                        file.ext = ext;
                        file.type = is_image(ext) ? "image" : "video";
                        file.file_size = item.st_size;
                        file.modified_time = item.st_mtime;
                        local_items.push_back(std::move(file));
                    }
                    // Batch flush on very large directories so cancellation and coordinator progress stay responsive.
                    if (local_items.size() >= 256 || local_dirs.size() >= 32) {
                        bool cancelled = false;
                        {
                            std::lock_guard lock(work.mutex);
                            cancelled = work.cancelled;
                            if (!cancelled) {
                                for (auto& subdir : local_dirs) work.dirs.push_back(std::move(subdir));
                                if (!local_items.empty()) {
                                    work.items.insert(work.items.end(),
                                                      std::make_move_iterator(local_items.begin()),
                                                      std::make_move_iterator(local_items.end()));
                                }
                                work.steps += local_steps;
                            }
                        }
                        local_dirs.clear();
                        local_items.clear();
                        local_steps = 0;
                        work.cv.notify_all();
                        if (cancelled) break;
                    }
                }
            } catch (...) { ok = false; }
            {
                std::lock_guard lock(work.mutex);
                if (!work.cancelled) {
                    for (auto& subdir : local_dirs) work.dirs.push_back(std::move(subdir));
                    if (!local_items.empty()) {
                        work.items.insert(work.items.end(),
                                          std::make_move_iterator(local_items.begin()),
                                          std::make_move_iterator(local_items.end()));
                    }
                    work.steps += local_steps;
                }
                --work.working;
                work.complete &= ok;
                ++work.steps;
            }
            work.cv.notify_all();
        }
    };

    unsigned started = 0;
    for (unsigned i = 0; i < std::clamp(std::thread::hardware_concurrency(), 1u, 4u); ++i) {
        if (!reserve_scanner_thread()) break;
        std::jthread thread;
        if (spawn_thread_safe(thread, "scanner_pool", worker)) {
            ++started;
            thread.detach();
        } else {
            g_scanner_detached_threads.fetch_sub(1);
        }
    }
    if (!started) return {};
    std::unique_lock lock(state->mutex);
    while (!state->dirs.empty() || state->working) {
        const auto steps = state->steps;
        if (!state->cv.wait_for(lock, stop, std::chrono::seconds(15), [&] { return state->steps != steps; }) ||
            stop.stop_requested() || !g_running) break;
        live_found_count = static_cast<int>(state->items.size());
        if (progress) {
            const int count = live_found_count;
            lock.unlock();
            progress(count);
            lock.lock();
        }
    }
    const bool completed = state->complete && state->dirs.empty() && !state->working && !stop.stop_requested() && g_running;
    state->cancelled = true;
    live_found_count = static_cast<int>(state->items.size());
    auto results = std::move(state->items);
    lock.unlock();
    state->cv.notify_all();
    return {std::move(results), completed};
}

ScanResult scan_directory(const std::string& dir, int depth,
                          std::function<void(int)> progress, std::stop_token stop) {
    g_logger.info("TRACE: scan_directory dir='{}' depth={}", dir.c_str(), depth);
    MediaScanner scanner;
    std::vector<std::string> exts = {".jpg", ".jpeg", ".png", ".webp", ".heic", ".heif", ".gif", ".bmp", ".tiff", ".tif", ".mp4", ".mov", ".mkv", ".avi", ".webm"};
    int scan_days;
    std::vector<std::string> ignore_f;
    {
        std::shared_lock lk(g_config_mtx);
        scan_days = g_cfg.scan_window_days;
        ignore_f = g_cfg.ignore_folders;
    }

    auto result = scanner.scan(dir, exts, scan_days, depth, ignore_f, std::move(progress), stop);
    g_logger.info("Scanner: returned {} media files (complete={})", result.items.size(), result.complete);
    return result;
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

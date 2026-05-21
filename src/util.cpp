#include "util.h"
#include <iostream>
#include <chrono>
#include <filesystem>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>

std::atomic<bool> g_running{true};
std::atomic<int> g_remote_command{0};
std::atomic<float> g_weather_temp{0.0f};
std::atomic<int> g_weather_code{0};
std::atomic<bool> g_config_changed{false};
int g_http_server_fd = -1;
std::atomic<bool> g_database_complete{false};
std::string g_crash_cache_dir = "";

Logger g_logger;

// Math, string parsing, and files helpers
int safe_stoi(const std::string& s, int def) {
    try {
        return std::stoi(s);
    } catch (...) {
        return def;
    }
}

float safe_stof(const std::string& s, float def) {
    try {
        return std::stof(s);
    } catch (...) {
        return def;
    }
}

double safe_stod(const std::string& s, double def) {
    try {
        return std::stod(s);
    } catch (...) {
        return def;
    }
}

long long safe_stoll(const std::string& s, long long def) {
    try {
        return std::stoll(s);
    } catch (...) {
        return def;
    }
}

std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Signal / crash handling
void crash_handler(int sig) {
    const char* msg = "\n[CRITICAL ERROR] piTrove intercepted a terminal fault / crash signal.\n";
    write(STDERR_FILENO, msg, strlen(msg));
    if (!g_database_complete.load() && !g_crash_cache_dir.empty()) {
        const char* purge_msg = "[CRITICAL] Database compilation was incomplete. Purging partial database records to protect state integrity...\n";
        write(STDERR_FILENO, purge_msg, strlen(purge_msg));
        std::string db_file = g_crash_cache_dir + "/cache.db";
        std::remove(db_file.c_str());
        std::remove((db_file + "-wal").c_str());
        std::remove((db_file + "-shm").c_str());
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

void terminate_handler() {
    fprintf(stderr, "\n[CRITICAL ERROR] piTrove exited due to an unhandled C++ runtime exception.\n");
    if (!g_database_complete.load() && !g_crash_cache_dir.empty()) {
        fprintf(stderr, "[CRITICAL] Database compilation incomplete. Purging partial database records...\n");
        std::string db_file = g_crash_cache_dir + "/cache.db";
        std::remove(db_file.c_str());
        std::remove((db_file + "-wal").c_str());
        std::remove((db_file + "-shm").c_str());
    }
    std::abort();
}

// System diagnostics and file path helpers
std::string get_exe_dir() {
    char exe_buf[4096];
    ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (len > 0) {
        exe_buf[len] = '\0';
        std::string real_exe = exe_buf;
        return std::filesystem::path(real_exe).parent_path().string();
    }
    return ".";
}

bool file_exists(const std::string& path) {
    return std::filesystem::exists(path);
}

std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char buf[64];
    struct tm tm_buf;
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime_r(&t, &tm_buf));
    return std::string(buf);
}

// Logger Implementation
void Logger::flush_loop() {
    while (flush_running.load() || !front_queue.empty()) {
        {
            std::unique_lock<std::mutex> lock(queue_mtx);
            cv.wait(lock, [this] { return !front_queue.empty() || !flush_running.load(); });
            std::swap(front_queue, back_queue);
        }
        if (!back_queue.empty()) {
            FILE* f = fopen(log_file_path.c_str(), "a");
            for (const auto& msg : back_queue) {
                write(STDOUT_FILENO, msg.c_str(), msg.size());
                if (f) fprintf(f, "%s", msg.c_str());
            }
            if (f) {
                fclose(f);
                fflush(stdout);
            }
            back_queue.clear();
        }
    }
}

void Logger::init(const std::string& path, LogLevel lvl) {
    level = lvl;
    log_dir = path;
    std::filesystem::create_directories(path);

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char fname[128];
    struct tm tm_buf;
    std::strftime(fname, sizeof(fname), "piTrove_%Y%m%d_%H%M%S.log", localtime_r(&t, &tm_buf));
    log_file_path = path + "/" + fname;

    // Rotate: keep only last 5 log files
    rotate_logs(path, 5);

    // Write version header to new log file
    {
        FILE* f = fopen(log_file_path.c_str(), "w");
        if (f) {
            char timebuf[64];
            struct tm htm = *localtime_r(&t, &tm_buf);
            std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &htm);
            fprintf(f, "=== piTrove v%s started %s ===\n", VERSION, timebuf);
            fclose(f);
        }
    }

    flush_thread = std::thread(&Logger::flush_loop, this);
}

Logger::~Logger() {
    flush_running.store(false);
    cv.notify_one();
    if (flush_thread.joinable()) flush_thread.join();
}

void Logger::rotate_logs(const std::string& dir, int keep) {
    std::vector<std::string> files;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            std::string fn = entry.path().filename().string();
            if (fn.find("piTrove_") == 0 && fn.find(".log") != std::string::npos) {
                files.push_back(entry.path().string());
            }
        }
        std::sort(files.begin(), files.end());
        while ((int)files.size() > keep) {
            std::filesystem::remove(files.front());
            files.erase(files.begin());
        }
    } catch (...) {}
}

void Logger::log(LogLevel lvl, const char* fmt, ...) {
    if (lvl < level) return;

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    const char* tag = (lvl == LogLevel::WARN)  ? "WARN"
                      : (lvl == LogLevel::ERROR) ? "ERROR"
                      : (lvl == LogLevel::DEBUG) ? "DEBUG"
                                                 : "INFO";

    char header[64];
    struct tm tm_buf2;
    std::strftime(header, sizeof(header), "%Y-%m-%d %H:%M:%S", localtime_r(&t, &tm_buf2));

    char line[512];
    int n = std::snprintf(line, sizeof(line), "v%s %s.%03ld [%s] ", VERSION, header, (long)ms.count(), tag);

    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
    va_end(ap);

    std::string final_line;
    final_line.reserve(n + 512);
    final_line += line;
    final_line += '\n';

    {
        std::lock_guard<std::mutex> lock(queue_mtx);
        front_queue.push_back(std::move(final_line));
    }
    cv.notify_one();
}

void Logger::info(const char* fmt, ...) {
    char buf[4096];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log(LogLevel::INFO, "%s", buf);
}

void Logger::warn(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log(LogLevel::WARN, "%s", buf);
}

void Logger::error(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log(LogLevel::ERROR, "%s", buf);
}

void Logger::debug(const char* fmt, ...) {
    if (level < LogLevel::DEBUG) return;
    va_list ap; va_start(ap, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log(LogLevel::DEBUG, "%s", buf);
}

// slide_debug utilities
static std::mutex __slide_debug_mtx;
static FILE* __slide_debug_f = nullptr;
static std::atomic<bool> __slide_debug_of{false};
static time_t __slide_debug_last_rotate = 0;
static bool __slide_debug_first = true;
static std::string __slide_debug_fname;

static std::string _slide_log_dir() {
    if (!g_logger.log_dir.empty()) return g_logger.log_dir;
    std::string h = getenv("HOME") ? getenv("HOME") : "/home/pi";
    return h + "/piTrove/logs";
}

void slide_debug(const char* fmt, ...) {
    {
        std::lock_guard<std::mutex> lk(__slide_debug_mtx);
        if (!__slide_debug_of) {
            auto now = std::chrono::system_clock::now();
            auto ts = std::chrono::system_clock::to_time_t(now);
            struct tm tmb;
            char datestr[32];
            strftime(datestr, sizeof(datestr), "%Y%m%d_%H%M%S", localtime_r(&ts, &tmb));
            __slide_debug_fname = _slide_log_dir() + "/slide_debug_" + std::string(datestr) + ".log";
            
            // Create directories if needed
            std::filesystem::create_directories(_slide_log_dir());
            
            __slide_debug_f = fopen(__slide_debug_fname.c_str(), "a");
            if (__slide_debug_f) {
                __slide_debug_of = true;
            } else {
                __slide_debug_of = false;
            }
        }
    }
    if (!__slide_debug_of.load()) return;

    bool do_rotate = false;
    {
        std::lock_guard<std::mutex> lk(__slide_debug_mtx);
        time_t now = time(nullptr);
        struct stat szWcheck;
        if (__slide_debug_f && fstat(fileno(__slide_debug_f), &szWcheck) == 0 && szWcheck.st_size > 5 * 1024 * 1024) {
            do_rotate = true;
        } else if (!__slide_debug_first && now - __slide_debug_last_rotate > 300) {
            __slide_debug_last_rotate = now;
            do_rotate = true;
        }
        if (__slide_debug_first) __slide_debug_first = false;
    }

    if (do_rotate) {
        std::lock_guard<std::mutex> lk(__slide_debug_mtx);
        try {
            std::vector<std::string> files;
            std::string logdir = _slide_log_dir();
            for (const auto& entry : std::filesystem::directory_iterator(logdir)) {
                std::string fn = entry.path().filename().string();
                if (fn.find("slide_debug_") == 0 && fn.find(".log") != std::string::npos) {
                    files.push_back(entry.path().string());
                }
            }
            std::sort(files.begin(), files.end());
            while ((int)files.size() > 3) {
                std::filesystem::remove(files.front());
                files.erase(files.begin());
            }
        } catch (...) {}
        if (__slide_debug_f) {
            fclose(__slide_debug_f);
            __slide_debug_f = nullptr;
        }
        __slide_debug_fname.clear();
        __slide_debug_of = false;
    }

    {
        std::lock_guard<std::mutex> lk(__slide_debug_mtx);
        if (!__slide_debug_f) {
            __slide_debug_f = fopen(__slide_debug_fname.empty() ? (_slide_log_dir() + "/slide_debug.log").c_str() : __slide_debug_fname.c_str(), "a");
            if (__slide_debug_f) {
                ftruncate(fileno(__slide_debug_f), 0);
                __slide_debug_of = true;
            }
            else return;
        }
    }

    va_list ap;
    va_start(ap, fmt);
    char line[1024];
    int n = std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    char tb[64];
    {
        std::lock_guard<std::mutex> lk(__slide_debug_mtx);
        static time_t cached_sec = -1;
        static struct tm cached_tm;
        static char cached_tb[64];
        time_t tv = time(nullptr);
        struct tm* tm = localtime_r(&tv, &cached_tm);
        if (!tm) return;
        if (tv != cached_sec) {
            cached_sec = tv;
            snprintf(cached_tb, sizeof(cached_tb), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
        }
        snprintf(tb, sizeof(tb), "%s", cached_tb);
        fprintf(__slide_debug_f, "[%s] %s\n", tb, line);
        fflush(__slide_debug_f);
    }
}

void slide_debug_close() {
    std::lock_guard<std::mutex> lk(__slide_debug_mtx);
    if (__slide_debug_f) {
        fclose(__slide_debug_f);
        __slide_debug_f = nullptr;
        __slide_debug_of = false;
    }
}

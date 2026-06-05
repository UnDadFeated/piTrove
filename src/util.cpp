#include "util.h"
#include "media_item.h"
#include "image_loader.h"
#include "cache.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <cctype>

std::atomic<bool> g_running{true};
std::atomic<bool> g_slideshow_paused{false};
std::atomic<int> g_remote_command{0};
std::atomic<float> g_weather_temp{0.0f};
std::atomic<int> g_weather_code{0};
std::atomic<bool> g_config_changed{false};
std::atomic<bool> g_database_complete{false};
std::atomic<bool> g_screen_blanked{false};
std::atomic<int64_t> g_last_motion_time{0};
std::atomic<int> g_consecutive_failures{0};
std::atomic<bool> g_offline_mode{false};
std::atomic<int> g_active_error_code{0};
std::string g_crash_cache_dir = "";

void Logger::log_error_code(int code_num) {
    if (code_num == 0) {
        info("SYSTEM ERROR: Cleared active diagnostic code.");
        return;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "E%d", code_num);
    std::string code_str = buf;

    std::string title = "UNKNOWN_ERROR";
    std::string desc = "An undocumented system diagnostic error occurred.";
    std::string rec = "Please inspect config settings or reboot the frame.";

    if (g_cache) {
        g_cache->get_error_details(code_str, title, desc, rec);
    }

    error("SYSTEM ERROR [%s] - %s: %s (RECOVERY: %s)", 
          code_str.c_str(), title.c_str(), desc.c_str(), rec.c_str());
}

void trigger_error(int code_num) {
    g_active_error_code.store(code_num);
    g_logger.log_error_code(code_num);
}

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
    if (end == std::string::npos || end < start) return "";
    return s.substr(start, end - start + 1);
}

// Signal / crash handling — ONLY async-signal-safe functions
static void crash_display_restore(void) {
    int fd = open("/sys/class/graphics/fb0/blank", O_WRONLY);
    if (fd >= 0) {
        const char* zero = "0";
        (void)write(fd, zero, 1);
        close(fd);
    }
}

void crash_handler(int sig) {
    const char* msg = "\n[CRITICAL ERROR] piTrove intercepted a terminal fault / crash signal.\n";
    (void)write(STDERR_FILENO, msg, strlen(msg));

    // Fail-safe: Restore physical display power (uses only async-signal-safe syscalls)
    crash_display_restore();

    // Signal-safe purge: construct paths into stack buffer, use unlink()
    if (!g_database_complete.load() && !g_crash_cache_dir.empty()) {
        const char* purge_msg = "[CRITICAL] Database incomplete — purging partial cache.\n";
        (void)write(STDERR_FILENO, purge_msg, strlen(purge_msg));

        char path[512];
        int n = snprintf(path, sizeof(path), "%s/cache.db", g_crash_cache_dir.c_str());
        if (n > 0 && (size_t)n < sizeof(path) - 5) {
            unlink(path);
            strcat(path, "-wal");
            unlink(path);
            strcpy(path + strlen(path) - 3, "shm");
            unlink(path);
        }
    }

    signal(sig, SIG_DFL);
    raise(sig);
}

void terminate_handler() {
    const char* msg = "\n[CRITICAL ERROR] piTrove unhandled exception.\n";
    (void)write(STDERR_FILENO, msg, strlen(msg));
    
    // Fail-safe: Restore physical display power via async-signal-safe sysfs write
    int fd = open("/sys/class/graphics/fb0/blank", O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, "0", 1);
        close(fd);
    }

    if (!g_database_complete.load() && !g_crash_cache_dir.empty()) {
        char path[512];
        int n = snprintf(path, sizeof(path), "%s/cache.db", g_crash_cache_dir.c_str());
        if (n > 0 && (size_t)n < sizeof(path) - 5) {
            unlink(path);
            strcat(path, "-wal");
            unlink(path);
            strcpy(path + strlen(path) - 3, "shm");
            unlink(path);
        }
    }
    std::abort();
}

void set_display_power(bool power) {
    g_logger.info("DISPLAY_POWER: Setting display power to %s", power ? "ON" : "OFF");
    int fd = open("/sys/class/graphics/fb0/blank", O_WRONLY);
    if (fd >= 0) {
        const char* val = power ? "0" : "1"; // "0" is unblank, "1" is blank
        (void)write(fd, val, 1);
        close(fd);
    }
    // Also try vcgencmd as fallback in case we are on standard Raspbian without sysfs permission
    std::string cmd = "vcgencmd display_power " + std::string(power ? "1" : "0") + " >/dev/null 2>&1";
    int res = ::system(cmd.c_str());
    (void)res;
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
    std::error_code ec;
    return std::filesystem::exists(path, ec);
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
            if (!f) {
                static std::once_flag warn_once;
                std::call_once(warn_once, []() {
                    const char* m = "[WARN] Cannot open log file, logging to stdout only.\n";
                    (void)write(STDOUT_FILENO, m, strlen(m));
                });
            }
            for (const auto& msg : back_queue) {
                (void)write(STDOUT_FILENO, msg.c_str(), msg.size());
                if (f) fprintf(f, "%s", msg.c_str());
            }
            if (f) {
                fclose(f);
            }
            fflush(stdout);
            back_queue.clear();
        }
    }
}

void Logger::init(const std::string& path, LogLevel lvl, int keep_count) {
    level = lvl;
    log_dir = path;
    std::filesystem::create_directories(path);

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char fname[128];
    struct tm tm_buf;
    std::strftime(fname, sizeof(fname), "piTrove_%Y%m%d_%H%M%S.log", localtime_r(&t, &tm_buf));
    log_file_path = path + "/" + fname;

    // Rotate: keep specified number of old log files
    rotate_logs(path, keep_count);

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

    initialized = true;
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



void Logger::log_v(LogLevel lvl, const char* fmt, va_list ap) {
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

    char line[4096];
    int n = std::snprintf(line, sizeof(line), "v%s %s.%03ld [%s] ", VERSION, header, (long)ms.count(), tag);
    if (n < 0) return;
    if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;

    std::vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);

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
    if (LogLevel::INFO < level) return;
    va_list ap; va_start(ap, fmt);
    log_v(LogLevel::INFO, fmt, ap);
    va_end(ap);
}

void Logger::warn(const char* fmt, ...) {
    if (LogLevel::WARN < level) return;
    va_list ap; va_start(ap, fmt);
    log_v(LogLevel::WARN, fmt, ap);
    va_end(ap);
}

void Logger::error(const char* fmt, ...) {
    if (LogLevel::ERROR < level) return;
    va_list ap; va_start(ap, fmt);
    log_v(LogLevel::ERROR, fmt, ap);
    va_end(ap);
}

void Logger::debug(const char* fmt, ...) {
    if (LogLevel::DEBUG < level) return;
    va_list ap; va_start(ap, fmt);
    log_v(LogLevel::DEBUG, fmt, ap);
    va_end(ap);
}



static bool match_keyword(const std::string& str, const std::string& kw) {
    size_t pos = str.find(kw);
    if (pos == std::string::npos) return false;

    // For very short keywords prone to false positives, enforce strict word boundary checks
    if (kw.size() <= 3 || kw == "self") {
        pos = 0;
        while ((pos = str.find(kw, pos)) != std::string::npos) {
            bool before_ok = true;
            if (pos > 0) {
                char c = str[pos - 1];
                if (std::isalnum(c)) before_ok = false;
            }
            bool after_ok = true;
            if (pos + kw.size() < str.size()) {
                char c = str[pos + kw.size()];
                if (std::isalnum(c)) after_ok = false;
            }
            if (before_ok && after_ok) {
                return true;
            }
            pos += kw.size();
        }
        return false;
    }

    return true;
}

void classify_media_item(const MediaItem& item, bool& has_people, bool& has_animals, bool& is_doc) {
    has_people = false;
    has_animals = false;
    is_doc = false;

    // Convert filename and parent directory path to lowercase
    std::string path_lower = item.path;
    std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);
    std::string name_lower = item.filename;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

    // List of common document / screenshot / web-grab indicators
    static const std::vector<std::string> doc_keywords = {
        "screenshot", "screen shot", "scan", "document", "invoice", "receipt", "bill", "ticket", "paper", 
        "page", "chart", "diagram", "slide", "text", "whatsapp", "telegram", "download", "discord", 
        "logo", "banner", "icon", "wallpaper", "avatar", "clipart", "pdf", "docx", "txt", "xlsx"
    };

    // List of common people / faces / selfies / portraits keywords
    static const std::vector<std::string> people_keywords = {
        "people", "person", "man", "woman", "kid", "child", "baby", "face", "selfie", "family", "portrait", 
        "wedding", "party", "graduation", "trip", "vacation", "group", "friends", "us", "me", "dad", "mom", 
        "brother", "sister", "grandpa", "grandma", "uncle", "aunt", "cousin", "son", "daughter", "wife", 
        "husband", "bf", "gf", "boy", "girl", "christmas", "thanksgiving", "holiday", "birthday", "self"
    };

    // List of common animal keywords
    static const std::vector<std::string> animal_keywords = {
        "cat", "dog", "pet", "animal", "bird", "fish", "horse", "cow", "sheep", "pig", "chicken", "duck", 
        "wildlife", "zoo", "safari", "squirrel", "rabbit", "deer", "fox", "bear", "tiger", "lion", "elephant", 
        "puppy", "kitten", "paw", "kitty", "doggie"
    };

    // 1. Precise keyword matching in path or filename
    for (const auto& kw : doc_keywords) {
        if (match_keyword(path_lower, kw)) {
            is_doc = true;
            return; // If it's a document/screenshot, classify and return immediately
        }
    }

    for (const auto& kw : people_keywords) {
        if (match_keyword(path_lower, kw)) {
            has_people = true;
        }
    }

    for (const auto& kw : animal_keywords) {
        if (match_keyword(path_lower, kw)) {
            has_animals = true;
        }
    }

    // 2. If it is already tagged by keyword, respect it
    if (has_people || has_animals) {
        return;
    }

    // 3. Fallback heuristic for standard camera rolls (like IMG_4829.JPG or DSC_0294.JPG)
    // If it has typical camera photo format, it is a camera capture
    bool is_camera_roll = false;
    std::string ext_clean = item.ext;
    if (!ext_clean.empty() && ext_clean.front() == '.') {
        ext_clean = ext_clean.substr(1);
    }
    std::transform(ext_clean.begin(), ext_clean.end(), ext_clean.begin(), ::tolower);
    
    if (name_lower.rfind("img_", 0) == 0 ||
        name_lower.rfind("dsc_", 0) == 0 ||
        name_lower.rfind("dscn", 0) == 0 ||
        name_lower.rfind("dscf", 0) == 0 ||
        name_lower.rfind("mvimg_", 0) == 0 ||
        name_lower.rfind("cimg", 0) == 0 ||
        name_lower.rfind("gopr", 0) == 0 ||
        (ext_clean == "jpg" || ext_clean == "jpeg" || ext_clean == "heic" || ext_clean == "heif")) {
        is_camera_roll = true;
    }

    if (is_camera_roll) {
        // Only apply 90/10 heuristic if EXIF confirms it's a real camera photo
        // Screenshots saved as .jpg lack camera EXIF and would otherwise slip through
        bool has_cam = true;
        if (item.is_camera == 1) {
            has_cam = true;
        } else if (item.is_camera == 0) {
            has_cam = false;
        } else {
            // Unknown (-1). Temporarily assume true to avoid synchronous disk I/O on startup.
            // It will be resolved and cached asynchronously when loaded/displayed in the slideshow.
            has_cam = true;
        }

        if (has_cam) {
            // Deterministically distribute: 90% people/faces, 10% pets/animals
            unsigned int hash = 5381;
            for (char c : item.filename) {
                hash = ((hash << 5) + hash) + c;
            }
            unsigned int score = hash % 100;
            if (score < 90) {
                has_people = true;
            } else {
                has_animals = true;
            }
        } else {
            // No camera EXIF — likely screenshot, classify as document
            is_doc = true;
        }
    } else {
        // Non-camera, non-keyworded files: classify as document
        is_doc = true;
    }
}

bool parse_filename_date(const std::string& filename, int& y, int& m, int& d) {
    if (filename.length() < 8) return false;
    
    // 1. Try YYYY-MM-DD or YYYY_MM_DD
    for (size_t i = 0; i + 9 < filename.length(); ++i) {
        char c0 = filename[i], c1 = filename[i+1], c2 = filename[i+2], c3 = filename[i+3];
        char sep1 = filename[i+4];
        char c5 = filename[i+5], c6 = filename[i+6];
        char sep2 = filename[i+7];
        char c8 = filename[i+8], c9 = filename[i+9];
        
        if (std::isdigit(c0) && std::isdigit(c1) && std::isdigit(c2) && std::isdigit(c3) &&
            (sep1 == '-' || sep1 == '_') &&
            std::isdigit(c5) && std::isdigit(c6) &&
            (sep2 == '-' || sep2 == '_') &&
            std::isdigit(c8) && std::isdigit(c9)) {
            
            y = (c0 - '0')*1000 + (c1 - '0')*100 + (c2 - '0')*10 + (c3 - '0');
            m = (c5 - '0')*10 + (c6 - '0');
            d = (c8 - '0')*10 + (c9 - '0');
            if (m >= 1 && m <= 12 && d >= 1 && d <= 31) {
                return true;
            }
        }
    }
    
    // 2. Try YYYYMMDD (8 digits in a row)
    for (size_t i = 0; i + 7 < filename.length(); ++i) {
        bool all_digits = true;
        for (size_t j = 0; j < 8; ++j) {
            if (!std::isdigit(filename[i+j])) {
                all_digits = false;
                break;
            }
        }
        if (all_digits) {
            int ty = (filename[i] - '0')*1000 + (filename[i+1] - '0')*100 + (filename[i+2] - '0')*10 + (filename[i+3] - '0');
            int tm = (filename[i+4] - '0')*10 + (filename[i+5] - '0');
            int td = (filename[i+6] - '0')*10 + (filename[i+7] - '0');
            if (ty >= 1900 && ty <= 2100 && tm >= 1 && tm <= 12 && td >= 1 && td <= 31) {
                y = ty; m = tm; d = td;
                return true;
            }
        }
    }
    
    return false;
}

void get_modified_time_date(int64_t mtime, int& y, int& m, int& d) {
    std::time_t t = static_cast<std::time_t>(mtime);
    struct tm tm_buf;
    std::tm* timeinfo = localtime_r(&t, &tm_buf);
    if (timeinfo) {
        y = timeinfo->tm_year + 1900;
        m = timeinfo->tm_mon + 1;
        d = timeinfo->tm_mday;
    } else {
        y = 1970; m = 1; d = 1;
    }
}

bool get_item_date(const MediaItem& item, int& y, int& m, int& d) {
    if (parse_filename_date(item.filename, y, m, d)) {
        return true;
    }
    get_modified_time_date(item.modified_time, y, m, d);
    return true;
}

std::string escape_shell_arg(const std::string& input) {
    std::string escaped;
    for (char c : input) {
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped += c;
        }
    }
    return escaped;
}

std::string sanitize_alphanumeric(const std::string& input) {
    std::string output;
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            output += c;
        }
    }
    return output;
}

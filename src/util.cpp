#include "scanner.h"
#include "safe_mode.h"
#include "error_db.h"
#include <arpa/inet.h>
#include <string>
#include <format>
#include <sys/sysinfo.h>
#include <fstream>
#include <memory>
#include <mntent.h>

uint64_t get_available_ram_bytes() {
    uint64_t available = 0;
    std::ifstream meminfo("/proc/meminfo");
    for (std::string line; std::getline(meminfo, line);) {
        unsigned long long kb = 0;
        if (std::sscanf(line.c_str(), "MemAvailable: %llu kB", &kb) == 1) {
            available = kb * 1024ULL;
            break;
        }
    }
    if (!available) {
        struct sysinfo si;
        available = sysinfo(&si) == 0 ? static_cast<uint64_t>(si.freeram) * si.mem_unit
                                     : 1024ULL * 1024 * 1024;
    }

    // MemAvailable describes the host, including inside a Docker container.
    const std::filesystem::path root("/sys/fs/cgroup");
    std::ifstream membership("/proc/self/cgroup");
    for (std::string line; std::getline(membership, line);) {
        if (!line.starts_with("0::/")) continue;
        auto group = line.size() == 4 ? root : root / line.substr(4);
        while (true) {
            uint64_t limit, used;
            std::ifstream maximum(group / "memory.max");
            std::ifstream current(group / "memory.current");
            if (maximum >> limit && current >> used) {
                available = std::min(available, limit > used ? limit - used : uint64_t{0});
            }
            if (group == root) break;
            group = group.parent_path();
        }
        break;
    }
    return available;
}

uint64_t get_total_ram_bytes() {
    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                uint64_t kb = 0;
                if (std::sscanf(line.c_str(), "MemTotal: %lu kB", &kb) == 1) {
                    return kb * 1024ULL;
                }
            }
        }
    }
    struct sysinfo si;
    if (sysinfo(&si) == 0) {
        return (uint64_t)si.totalram * si.mem_unit;
    }
    return 2048ULL * 1024 * 1024;
}

#include "util.h"
#include "media_item.h"
#include "image_loader.h"
#include "cache.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <cctype>
#include <net/if.h>
#include <mutex>
#include <dlfcn.h>




std::string redact_secrets(const std::string& message) {
    static constexpr std::string_view keys[]{"bearer", "api_key", "password", "pass", "client_secret",
                                             "refresh_token", "ical_url", "pin", "token"};
    static constexpr std::string_view key_starts = "bBaApPcCrRiItT";
    static constexpr std::string_view spaces = " \t\r\n\v\f";
    static constexpr std::string_view bearer_chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._~+/=-";
    const std::string_view text(message);
    const auto matches = [](std::string_view value, std::string_view term) {
        if (value.size() < term.size()) return false;
        return std::ranges::equal(value.substr(0, term.size()), term, {},
            [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; });
    };
    const auto is_alpha_char = [](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) != 0;
    };
    std::string redacted;
    size_t copied = 0;
    size_t offset = text.find_first_of(key_starts);
    while (offset != text.npos) {
        if (offset > 0 && is_alpha_char(text[offset - 1])) {
            offset = text.find_first_of(key_starts, offset + 1);
            continue;
        }
        const auto key = std::ranges::find_if(keys, [&](auto term) { return matches(text.substr(offset), term); });
        if (key == std::end(keys)) {
            offset = text.find_first_of(key_starts, offset + 1);
            continue;
        }
        const bool bearer = *key == "bearer";
        size_t after_key = offset + key->size();
        if (!bearer && after_key < text.size() && (text[after_key] == '"' || text[after_key] == '\'')) {
            ++after_key;
        }
        size_t begin = text.find_first_not_of(spaces, after_key);
        if (begin == text.npos) break;
        if (bearer) {
            if (begin == offset + key->size() || !bearer_chars.contains(text[begin])) {
                offset = text.find_first_of(key_starts, offset + 1);
                continue;
            }
        } else {
            if (text[begin] != '=' && text[begin] != ':') {
                offset = text.find_first_of(key_starts, offset + 1);
                continue;
            }
            begin = text.find_first_not_of(spaces, begin + 1);
            if (begin == text.npos) break;
        }
        size_t end = text.size();
        if (bearer) {
            end = text.find_first_not_of(bearer_chars, begin);
            if (end == text.npos) end = text.size();
        } else if (text[begin] == '"' || text[begin] == '\'') {
            const char quote = text[begin];
            size_t pos = begin + 1;
            while (pos < text.size() && text[pos] != '\n' && text[pos] != '\r') {
                if (text[pos] == '\\' && pos + 1 < text.size()) {
                    pos += 2;
                    continue;
                }
                if (text[pos] == quote) {
                    ++pos;
                    break;
                }
                ++pos;
            }
            end = pos;
        } else {
            end = text.find_first_of(" \t\r\n\v\f,;", begin);
            if (end == text.npos) end = text.size();
        }
        if (end == begin) {
            offset = text.find_first_of(key_starts, offset + 1);
            continue;
        }
        redacted.append(text.substr(copied, (bearer ? offset : begin) - copied));
        redacted.append(bearer ? "Bearer [REDACTED]" : "[REDACTED]");
        copied = end;
        offset = text.find_first_of(key_starts, end);
    }
    if (copied == 0) return message;
    redacted.append(text.substr(copied));
    return redacted;
}

void Logger::log_error_code(int code_num) {
    if (code_num == 0) {
        info("SYSTEM ERROR: Cleared active diagnostic code.");
        return;
    }

    std::string code_str = std::format("E{}", code_num);

    std::string title = "UNKNOWN_ERROR";
    std::string desc = "An undocumented system diagnostic error occurred.";
    std::string rec = "Please inspect config settings or reboot the frame.";

    // Diagnostics can originate while the media cache transaction is locked.
    static const auto errors = get_all_error_seeds();
    const auto entry = std::find_if(errors.begin(), errors.end(), [&](const ErrorSeed& e) { return e.code == code_str; });
    if (entry != errors.end()) {
        title = entry->title;
        desc = entry->desc;
        rec = entry->rec;
    }

    error("SYSTEM ERROR [{}] - {}: {} (RECOVERY: {})",
          code_str, title, desc, rec);
}

#include <set>

static std::set<int> g_active_errors;
static std::mutex g_errors_mtx;

void trigger_error(int code_num) {
    if (code_num <= 0) return;
    {
        std::lock_guard lock(g_errors_mtx);
        if (!g_active_errors.insert(code_num).second) return;
        g_active_error_code.store(code_num);
    }
    g_logger.log_error_code(code_num);
}

void clear_error(int code_num) {
    std::lock_guard<std::mutex> lk(g_errors_mtx);
    g_active_errors.erase(code_num);
    if (g_active_error_code.load() == code_num) {
        if (g_active_errors.empty()) {
            g_active_error_code.store(0);
        } else {
            g_active_error_code.store(*g_active_errors.rbegin());
        }
    }
}

bool is_error_active(int code_num) {
    std::lock_guard<std::mutex> lk(g_errors_mtx);
    return g_active_errors.find(code_num) != g_active_errors.end();
}



// Math, string parsing, and files helpers
// NOTE: safe_stoi/safe_stof/safe_stod/safe_stoll are now inline wrappers
// in util.h using the safe_parse<T>() template with std::from_chars.


std::string trim(std::string_view s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    if (end == std::string_view::npos || end < start) return "";
    return std::string(s.substr(start, end - start + 1));
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
    constexpr char message[] = "\n[CRITICAL ERROR] piTrove received a fatal signal.\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    pitrove::safe_mode::record_crash();
    crash_display_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

void terminate_handler() {
    constexpr char message[] = "\n[CRITICAL ERROR] piTrove unhandled exception.\n";
    (void)write(STDERR_FILENO, message, sizeof(message) - 1);
    std::abort();
}

WifiStats read_wifi_stats(const std::string& iface) {
    WifiStats stats;
    std::string path = "/proc/net/wireless";
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return stats;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (std::strstr(line, iface.c_str())) {
            // Format: <iface>: <status> <link> <level> <noise> ...
            int link = -1, level = -129, noise = -129;
            unsigned long missed = 0;
            if (sscanf(line, "%*s %*x %d %d %d %*u %*u %*u %*u %*u %*u %lu",
                       &link, &level, &noise, &missed) >= 3) {
                stats.quality = link;
                stats.signal_dbm = level;
                stats.noise_dbm = noise;
                stats.missed_beacons = missed;
                stats.has_data = true;
            }
            break;
        }
    }
    fclose(fp);
    return stats;
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
    struct tm* tm_ptr = localtime_r(&t, &tm_buf);
    if (tm_ptr) {
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_ptr);
    } else {
        std::strcpy(buf, "0000-00-00 00:00:00");
    }
    return std::string(buf);
}

// Bound pending log storage even when a disk or stdout consumer stops draining.
void Logger::enqueue(LogMessage msg) {
    if (msg.body.size() > MAX_MESSAGE_BYTES) {
        msg.body.resize(MAX_MESSAGE_BYTES - 16);
        msg.body += " [truncated]";
    }
    {
        std::lock_guard lock(queue_mtx);
        if (flush_running.load()) {
            if (front_queue.size() >= MAX_QUEUED_MESSAGES ||
                msg.body.size() > MAX_QUEUED_BYTES - queued_bytes) {
                ++dropped_messages;
                return;
            }
            const size_t bytes = msg.body.size();
            front_queue.push_back(std::move(msg));
            queued_bytes += bytes;
            cv.notify_one();
            return;
        }
    }
    // A failed logger-thread spawn must neither lose all diagnostics nor leave
    // messages queued forever. This path also works during late shutdown.
    std::fprintf(stderr, "[LOGGER FALLBACK] %s\n", msg.body.c_str());
}

// Logger Implementation
void Logger::flush_loop() {
    const auto close_log = [](FILE* file) { fclose(file); };
    std::unique_ptr<FILE, decltype(close_log)> log_file(nullptr, close_log);
    std::string open_path;
    size_t current_file_bytes = 0;
    unsigned rotation_seq = 0;

    auto open_current = [&](const std::string& path) {
        log_file.reset(path.empty() ? nullptr : fopen(path.c_str(), "ae"));
        open_path = path;
        current_file_bytes = 0;
        if (log_file) {
            struct stat st{};
            if (fstat(fileno(log_file.get()), &st) == 0 && st.st_size > 0) {
                current_file_bytes = static_cast<size_t>(st.st_size);
            }
        }
    };

    auto rotate_active_file = [&]() {
        if (log_dir.empty()) return;
        if (log_file) {
            (void)fflush(log_file.get());
            log_file.reset();
        }
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        char fname[128];
        char timebuf[64];
        struct tm tm_buf{};
        struct tm* tm_ptr = localtime_r(&t, &tm_buf);
        if (tm_ptr) {
            std::strftime(fname, sizeof(fname), "piTrove_%Y%m%d_%H%M%S.log", tm_ptr);
            std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", tm_ptr);
        } else {
            std::strcpy(fname, "piTrove_00000000_000000.log");
            std::strcpy(timebuf, "0000-00-00 00:00:00");
        }
        std::string next_path = log_dir + "/" + fname;
        std::error_code ec;
        if (next_path == open_path || std::filesystem::exists(next_path, ec)) {
            ++rotation_seq;
            const std::string base(fname, std::strlen(fname) - 4);
            next_path = std::format("{}/{}_{}.log", log_dir, base, rotation_seq);
        }
        rotate_logs(log_dir, std::max(0, keep_count_ - 1));
        {
            std::lock_guard<std::mutex> lk(log_path_mtx);
            log_file_path = next_path;
        }
        open_current(next_path);
        if (log_file) {
            std::string hdr = std::format("=== piTrove v{} log rotated {} ===\n", VERSION, timebuf);
            if (fputs(hdr.c_str(), log_file.get()) != EOF) {
                current_file_bytes += hdr.size();
            }
        }
    };

    while (true) {
        size_t dropped = 0;
        {
            std::unique_lock<std::mutex> lock(queue_mtx);
            cv.wait(lock, [this] { return !front_queue.empty() || !flush_running.load(); });
            if (front_queue.empty() && !flush_running.load()) break;
            std::swap(front_queue, back_queue);
            queued_bytes = 0;
            dropped = std::exchange(dropped_messages, 0);
        }
        if (!back_queue.empty()) {
            std::string current_log_path;
            { std::lock_guard<std::mutex> lk(log_path_mtx); current_log_path = log_file_path; }
            if (!log_file || open_path != current_log_path) {
                open_current(current_log_path);
            }
            FILE* f = log_file.get();
            if (!f) {
                static std::once_flag warn_once;
                std::call_once(warn_once, []() {
                    const char* m = "[WARN] Cannot open log file, logging to stdout only.\n";
                    (void)write(STDOUT_FILENO, m, strlen(m));
                });
            }
            if (dropped) {
                std::string warning = std::format("[WARN] Logger dropped {} messages while its queue was full.\n", dropped);
                (void)write(STDOUT_FILENO, warning.data(), warning.size());
                if (f && fputs(warning.c_str(), f) != EOF) current_file_bytes += warning.size();
            }
            for (const auto& msg : back_queue) {
                auto t = std::chrono::system_clock::to_time_t(msg.time);
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(msg.time.time_since_epoch()) % 1000;
                const char* tag = (msg.level == LogLevel::WARN)  ? "WARN"
                                  : (msg.level == LogLevel::ERROR) ? "ERROR"
                                  : (msg.level == LogLevel::DEBUG) ? "DEBUG"
                                  : "INFO";
                char header[64];
                struct tm tm_buf;
                struct tm* time_ptr = localtime_r(&t, &tm_buf);
                if (time_ptr) {
                    std::strftime(header, sizeof(header), "%Y-%m-%d %H:%M:%S", time_ptr);
                } else {
                    std::strcpy(header, "0000-00-00 00:00:00");
                }
                std::string line = std::format("v{} {}.{:03d} [{}] {}\n",
                    VERSION, header, (int)ms.count(), tag, msg.body);
                if (!line.empty()) {
                    (void)write(STDOUT_FILENO, line.c_str(), line.size());
                    if (f) {
                        if (current_file_bytes + line.size() > MAX_LOG_FILE_BYTES && current_file_bytes > 0) {
                            rotate_active_file();
                            f = log_file.get();
                        }
                        if (f && fputs(line.c_str(), f) != EOF) {
                            current_file_bytes += line.size();
                        }
                    }
                }
            }
            if (f && fflush(f) != 0) log_file.reset();
            back_queue.clear();
        }
    }
}

void Logger::init(const std::string& path, LogLevel lvl, int keep_count) {
    level = lvl;
    log_dir = path;
    keep_count_ = std::max(1, keep_count);
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        (void)std::fputs(std::format("[ERROR] Logger: Failed to create directories {}\n: {}\n", path, ec.message()).c_str(), stderr);
    }

    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    char fname[128];
    struct tm tm_buf;
    struct tm* tm_ptr = localtime_r(&t, &tm_buf);
    if (tm_ptr) {
        std::strftime(fname, sizeof(fname), "piTrove_%Y%m%d_%H%M%S.log", tm_ptr);
    } else {
        std::strcpy(fname, "piTrove_00000000_000000.log");
    }
    std::string new_log_path = path + "/" + fname;
    {
        std::lock_guard<std::mutex> lk(log_path_mtx);
        log_file_path = new_log_path;
    }

    // Rotate: keep specified number of old log files
    // The new log file is created below, so retain keep_count-1 previous logs.
    rotate_logs(path, std::max(0, keep_count - 1));

    // Write version header to new log file
    {
        FILE* f = fopen(new_log_path.c_str(), "w");
        if (f) {
            char timebuf[64];
            struct tm tm_buf_htm;
            struct tm* htm_ptr = localtime_r(&t, &tm_buf_htm);
            struct tm htm = {};
            if (htm_ptr) {
                htm = *htm_ptr;
            }
            std::strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &htm);
            (void)std::fputs(std::format("=== piTrove v{} started {} ===\n", VERSION, timebuf).c_str(), f);
            fclose(f);
        }
    }

    initialized = true;
    if (!spawn_thread_safe(flush_thread, "logger_flush", &Logger::flush_loop, this)) {
        flush_running.store(false);
        flush_loop(); // Drain startup diagnostics once; later messages use stderr.
    }
}

Logger::~Logger() {
    {
        std::lock_guard lock(queue_mtx);
        flush_running.store(false);
    }
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
        while (std::ssize(files) > keep) {
            std::filesystem::remove(files.front());
            files.erase(files.begin());
        }
    } catch (...) {}
}



static bool match_keyword(std::string_view str, std::string_view kw) {
    if (auto pos = str.find(kw); pos == std::string_view::npos) return false;

    // For very short keywords prone to false positives, enforce strict word boundary checks
    if (kw.size() <= 3 || kw == "self") {
        size_t pos = 0;
        while ((pos = str.find(kw, pos)) != std::string_view::npos) {
            bool before_ok = true;
            if (pos > 0) {
                unsigned char c = static_cast<unsigned char>(str[pos - 1]);
                if (std::isalnum(c)) before_ok = false;
            }
            bool after_ok = true;
            if (pos + kw.size() < str.size()) {
                unsigned char c = static_cast<unsigned char>(str[pos + kw.size()]);
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
    std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string name_lower = item.filename;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    // List of common document / screenshot / web-grab indicators
    static constexpr std::string_view doc_keywords[] = {
        "screenshot", "screen shot", "scan", "document", "invoice", "receipt", "bill", "ticket", "paper", 
        "page", "chart", "diagram", "slide", "text", "whatsapp", "telegram", "download", "discord", 
        "logo", "banner", "icon", "wallpaper", "avatar", "clipart", "pdf", "docx", "txt", "xlsx"
    };

    // List of common people / faces / selfies / portraits keywords
    static constexpr std::string_view people_keywords[] = {
        "people", "person", "man", "woman", "kid", "child", "baby", "face", "selfie", "family", "portrait", 
        "wedding", "party", "graduation", "trip", "vacation", "group", "friends", "us", "me", "dad", "mom", 
        "brother", "sister", "grandpa", "grandma", "uncle", "aunt", "cousin", "son", "daughter", "wife", 
        "husband", "bf", "gf", "boy", "girl", "christmas", "thanksgiving", "holiday", "birthday", "self"
    };

    // List of common animal keywords
    static constexpr std::string_view animal_keywords[] = {
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
    std::transform(ext_clean.begin(), ext_clean.end(), ext_clean.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    
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

std::optional<std::tuple<int,int,int>> get_item_date(const MediaItem& item) {
    if (auto date = parse_filename_date(item.filename)) {
        return date;
    }
    int y, m, d;
    get_modified_time_date(item.modified_time, y, m, d);
    return std::tuple{y, m, d};
}

std::string escape_shell_arg(std::string_view input) {
    std::string escaped;
    for (char c : input) {
        if (c == '\0' || c == '\n' || c == '\r') continue;
        if (c == '\'') {
            escaped += "'\\''";
        } else {
            escaped += c;
        }
    }
    return escaped;
}

std::string sanitize_alphanumeric(std::string_view input) {
    std::string output;
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') {
            output += c;
        }
    }
    return output;
}

// Health check cache state
static std::string g_last_health_check_path;
static std::chrono::steady_clock::time_point g_last_health_check_time{};
static bool g_has_health_check{false};
static bool g_last_health_check_result{false};
static bool g_last_known_healthy{true};  // Track previous health state for E910
static constexpr auto HEALTH_CHECK_CACHE_TTL = std::chrono::seconds(5);
static std::mutex g_health_check_mutex;

bool is_media_dir_healthy(const std::string& media_dir) {
    if (media_dir.empty()) return false;

    // Check cache -- monotonic steady_clock TTL (immune to NTP wall-clock jumps)
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(g_health_check_mutex);
        if (g_has_health_check && media_dir == g_last_health_check_path &&
            (now - g_last_health_check_time) <= HEALTH_CHECK_CACHE_TTL) {
            return g_last_health_check_result;  // Return STATIC cached result
        }
    }

    // CIFS directory listing can stall despite a reachable host. Reuse the
    // scanner's deadline and bounded pool of outstanding filesystem calls.
    const auto entries = read_dir_timeout(media_dir, 2000);
    const bool healthy = entries && !entries->empty();

    // Store cache result (statics -- survives across calls)
    {
        std::lock_guard<std::mutex> lock(g_health_check_mutex);
        g_last_health_check_path = media_dir;
        g_last_health_check_time = std::chrono::steady_clock::now();
        g_has_health_check = true;
        g_last_health_check_result = healthy;

        // Burst detection: if health state changed (healthy -> unhealthy)
        if (!healthy && g_last_known_healthy) {
            g_logger.warn("IO_BURST: Media dir went unhealthy -- check NAS connectivity");
        }
        g_last_known_healthy = healthy;
    }

    return healthy;
}

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <fstream>
#include <memory>
#include <mntent.h>
#include <shared_mutex>
#include "config.h"
#include <ifaddrs.h>
#include <net/if.h>

namespace {
    struct AddrInfoGuard {
        struct addrinfo* res;
        ~AddrInfoGuard() { if (res) freeaddrinfo(res); }
    };
    struct SocketGuard {
        int fd;
        ~SocketGuard() { if (fd >= 0) close(fd); }
    };
}

static std::atomic<bool> g_nas_online{true};
static std::atomic<bool> g_nas_check_in_progress{false};
static std::atomic<int64_t> g_last_nas_check_ms{0};
static std::atomic<bool> g_has_nas_check{false};
static std::atomic<bool> g_nas_shutdown{false};

static int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static bool perform_nas_online_check() {
    std::string media_dir;
    {
        std::shared_lock<std::shared_mutex> lk(g_config_mtx);
        media_dir = g_cfg.media_dir;
    }
    if (media_dir.empty()) return true;

    // Standardize path (remove trailing slash)
    if (media_dir.length() > 1 && media_dir.back() == '/') {
        media_dir.pop_back();
    }

    FILE* mounts = setmntent("/proc/mounts", "re");
    if (!mounts) return true;

    std::string nas_host;
    int port = 0;
    size_t longest = 0;
    struct mntent entry{};
    char line[8192];
    while (getmntent_r(mounts, &entry, line, sizeof(line))) {
        const std::string_view mount(entry.mnt_dir);
        if (mount.size() < longest ||
            !(mount == "/" || media_dir == mount || (media_dir.starts_with(mount) &&
                 media_dir.size() > mount.size() && media_dir[mount.size()] == '/'))) continue;
        longest = mount.size();
        nas_host.clear();
        const std::string_view type(entry.mnt_type);
        const std::string_view device(entry.mnt_fsname);
        if ((type == "cifs" || type == "smb3") && device.starts_with("//")) {
            nas_host = device.substr(2, device.find('/', 2) - 2);
            port = 445;
        } else if (type == "nfs" || type == "nfs4") {
            const auto end = device.rfind(":/");
            if (end != std::string_view::npos) nas_host = device.substr(0, end);
            port = 2049;
        }
    }
    endmntent(mounts);
    if (nas_host.size() > 2 && nas_host.front() == '[' && nas_host.back() == ']')
        nas_host = nas_host.substr(1, nas_host.size() - 2);

    if (nas_host.empty()) {
        // No network mount found, assume it is local/always online
        return true;
    }

    // Perform non-blocking TCP socket connection check to nas_host on port 445 (SMB) or 2049 (NFS)

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(nas_host.c_str(), std::format("{}", port).c_str(), &hints, &res) != 0) {
        return false; // Hostname resolution failed
    }
    AddrInfoGuard addr_guard{res};

    int fd = socket(res->ai_family, res->ai_socktype | SOCK_CLOEXEC, res->ai_protocol);
    [[unlikely]]
    if (fd < 0) {
        return false;
    }
    SocketGuard sock_guard{fd};

    // Set non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int conn_rc = connect(fd, res->ai_addr, res->ai_addrlen);
    bool online = false;

    if (conn_rc == 0) {
        online = true;
    } else if (errno == EINPROGRESS) {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLOUT;
        int poll_rc = poll(&pfd, 1, 1500); // 1.5 second timeout
        if (poll_rc > 0) {
            int valopt = 0;
            socklen_t lon = sizeof(valopt);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &valopt, &lon) == 0) {
                if (valopt == 0) {
                    online = true;
                }
            }
        }
    }

    return online;
}

static std::jthread g_nas_check_thread;
static std::mutex g_nas_thread_mtx;

bool is_nas_online() {
    check_network_status();
    if (is_error_active(102) || is_error_active(103)) {
        return false;
    }

    const int64_t now_ms = steady_now_ms();
    if (!g_nas_shutdown.load() &&
        (!g_has_nas_check.load() || (now_ms - g_last_nas_check_ms.load()) > 10000)) {
        if (!g_nas_check_in_progress.exchange(true)) {
            std::lock_guard lk(g_nas_thread_mtx);
            if (g_nas_shutdown.load()) {
                g_nas_check_in_progress.store(false);
            } else {
                if (g_nas_check_thread.joinable()) {
                    g_nas_check_thread.join();
                }
                if (!spawn_thread_safe(g_nas_check_thread, "nas_check", []() {
                    bool online = perform_nas_online_check();
                    g_nas_online.store(online);
                    g_last_nas_check_ms.store(steady_now_ms());
                    g_has_nas_check.store(true);
                    g_nas_check_in_progress.store(false);
                })) {
                    // Spawn failed: reset the in-progress flag so the next
                    // call can retry.
                    g_nas_check_in_progress.store(false);
                }
            }
        }
    }
    return g_nas_online.load();
}

void cleanup_nas_thread() {
    std::lock_guard lk(g_nas_thread_mtx);
    g_nas_shutdown.store(true);
    g_nas_check_in_progress.store(false);
    if (g_nas_check_thread.joinable()) {
        g_nas_check_thread.join();
    }
}

void check_network_status() {
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return;
    }
    bool has_active_interface = false;
    bool has_valid_ip = false;
    bool has_apipa = false;

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (std::strcmp(ifa->ifa_name, "lo") == 0) continue;

        if ((ifa->ifa_flags & IFF_UP) && (ifa->ifa_flags & IFF_RUNNING)) {
            if (ifa->ifa_addr->sa_family == AF_INET) {
                has_active_interface = true;
                struct sockaddr_in* addr = (struct sockaddr_in*)ifa->ifa_addr;
                uint32_t ip = ntohl(addr->sin_addr.s_addr);
                if ((ip & 0xFFFF0000) == 0xA9FE0000) {
                    has_apipa = true;
                } else {
                    has_valid_ip = true;
                }
            } else if (ifa->ifa_addr->sa_family == AF_INET6) {
                has_active_interface = true;
                struct sockaddr_in6* addr6 = (struct sockaddr_in6*)ifa->ifa_addr;
                if (IN6_IS_ADDR_LINKLOCAL(&addr6->sin6_addr)) {
                    has_apipa = true;
                } else if (!IN6_IS_ADDR_LOOPBACK(&addr6->sin6_addr)) {
                    has_valid_ip = true;
                }
            }
        }
    }
    freeifaddrs(ifaddr);

    if (!has_active_interface || (!has_valid_ip && !has_apipa)) {
    [[unlikely]]
        trigger_error(102); // E102: WIFI_DISCONNECTED
        clear_error(103);
    } else if (has_apipa && !has_valid_ip) {
        clear_error(102);
    [[unlikely]]
        trigger_error(103); // E103: IP_CONFIGURATION_ERROR
    } else {
        clear_error(102);
        clear_error(103);
    }
}

namespace {
auto display_time(time_t epoch_sec, const std::string& zone) {
    const auto time = std::chrono::floor<std::chrono::seconds>(
        std::chrono::system_clock::from_time_t(epoch_sec == 0 ? std::time(nullptr) : epoch_sec));
    try {
        return std::chrono::zoned_time{zone.empty() ? "UTC" : zone, time};
    } catch (const std::runtime_error&) {
        return std::chrono::zoned_time{"UTC", time};
    }
}
}

std::string format_epoch_tz(time_t epoch_sec, const std::string& tz_name, const std::string& fmt_str) {
    try {
        const auto time = display_time(epoch_sec, tz_name);
        if (fmt_str == "%I:%M %p") return std::format("{:%I:%M %p}", time);
        if (fmt_str == "%I:%M %p %Z") return std::format("{:%I:%M %p %Z}", time);
        if (fmt_str == "%Y-%m-%d %H:%M") return std::format("{:%Y-%m-%d %H:%M}", time);
        if (fmt_str == "%Y-%m-%d") return std::format("{:%Y-%m-%d}", time);
        if (fmt_str == "%m/%d/%Y") return std::format("{:%m/%d/%Y}", time);
        if (fmt_str == "%a, %b %d") return std::format("{:%a, %b %d}", time);
        if (fmt_str == "%A") return std::format("{:%A}", time);
        if (fmt_str == "%B %d") return std::format("{:%B %d}", time);
        return std::format("{:%Y-%m-%d %H:%M:%S}", time);
    } catch (const std::runtime_error&) {
        return {};
    }
}

bool display_local_hm(time_t epoch_sec, const std::string& tz_name, int& out_hour, int& out_min) {
    try {
        const auto local = display_time(epoch_sec, tz_name).get_local_time();
        const auto day = std::chrono::floor<std::chrono::days>(local);
        const std::chrono::hh_mm_ss hms{local - day};
        out_hour = static_cast<int>(hms.hours().count());
        out_min = static_cast<int>(hms.minutes().count());
        return true;
    } catch (const std::runtime_error&) {
        return false;
    }
}

bool is_valid_host_or_ip(std::string_view value) {
    if (value.empty() || value.size() > 253 || value.front() == '-') return false;
    for (char ch : value) {
        if (static_cast<unsigned char>(ch) <= 32 || static_cast<unsigned char>(ch) >= 127) return false;
    }
    const std::string host(value);
    in_addr v4{};
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) return true;
    in6_addr v6{};
    if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) return true;
    // Strict RFC 1123 hostname (no leading '-', no empty labels, labels 1..63 chars of [A-Za-z0-9-])
    size_t pos = 0;
    while (pos < value.size()) {
        const size_t dot = value.find('.', pos);
        const size_t end = (dot == std::string_view::npos) ? value.size() : dot;
        const size_t len = end - pos;
        if (len == 0 || len > 63) return false;
        if (value[pos] == '-' || value[end - 1] == '-') return false;
        for (size_t i = pos; i < end; ++i) {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            if (!std::isalnum(c) && c != '-') return false;
        }
        if (dot == std::string_view::npos) break;
        pos = dot + 1;
        if (pos == value.size()) return false;
    }
    return true;
}

std::string format_tomorrow_tz(time_t epoch_sec, const std::string& tz_name) {
    try {
        const auto today = std::chrono::floor<std::chrono::days>(display_time(epoch_sec, tz_name).get_local_time());
        const std::chrono::year_month_day tomorrow{today + std::chrono::days{1}};
        return std::format("{:%Y-%m-%d}", tomorrow);
    } catch (const std::runtime_error&) {
        return {};
    }
}

std::vector<std::string> get_supported_timezones() {
    return {
        "UTC",
        "America/Los_Angeles",
        "America/Denver",
        "America/Chicago",
        "America/New_York",
        "America/Anchorage",
        "America/Phoenix",
        "America/Toronto",
        "America/Vancouver",
        "Europe/London",
        "Europe/Paris",
        "Europe/Berlin",
        "Europe/Rome",
        "Europe/Madrid",
        "Asia/Tokyo",
        "Asia/Shanghai",
        "Asia/Singapore",
        "Asia/Kolkata",
        "Asia/Dubai",
        "Australia/Sydney",
        "Australia/Melbourne",
        "Pacific/Auckland",
        "Pacific/Honolulu"
    };
}


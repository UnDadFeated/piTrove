#ifndef PITROVE_UTIL_H
#define PITROVE_UTIL_H

#define VERSION "12.3.0"
#define APP_NAME "piTrove"

#include <atomic>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cstdarg>

struct MediaItem; // Forward declaration

// Global atomics and sharing handles
inline std::atomic<bool> g_running{true};
inline std::atomic<bool> g_slideshow_paused{false};
inline std::atomic<int> g_remote_command{0}; // 1=Next, 2=Prev, 3=PauseToggle
inline std::atomic<float> g_item_timer{0.0f}; // Shared slideshow timer progress
inline std::atomic<float> g_weather_temp{0.0f};
inline std::atomic<int> g_weather_code{0};
inline std::atomic<bool> g_config_changed{false};
inline std::atomic<bool> g_screen_blanked{false};
inline std::atomic<int64_t> g_last_motion_time{0};
inline std::atomic<int> g_consecutive_failures{0};
inline std::atomic<bool> g_offline_mode{false};
inline std::atomic<int> g_active_error_code{0};
void trigger_error(int code_num);

inline std::atomic<bool> g_database_complete{false};
inline std::string g_crash_cache_dir = "";

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

struct Logger {
    LogLevel level{LogLevel::INFO};
    std::string log_dir;
    std::string log_file_path;

    // Async queue
    std::mutex queue_mtx;
    std::condition_variable cv;
    std::vector<std::string> front_queue;
    std::vector<std::string> back_queue;
    std::thread flush_thread;
    std::atomic<bool> flush_running{true};
    bool initialized{false};

    void flush_loop();
    void init(const std::string& path, LogLevel lvl, int keep_count = 5);
    void rotate_logs(const std::string& dir, int keep);

    void log_v(LogLevel lvl, const char* fmt, va_list ap);
    void info(const char* fmt, ...);
    void warn(const char* fmt, ...);
    void error(const char* fmt, ...);
    void debug(const char* fmt, ...);
    void log_error_code(int code_num);
    bool is_initialized() const { return initialized; }

    Logger() = default;
    ~Logger();
};

inline Logger g_logger;

// Math, string parsing, and files helpers
[[nodiscard]] int safe_stoi(const std::string& s, int def);
[[nodiscard]] float safe_stof(const std::string& s, float def);
[[nodiscard]] double safe_stod(const std::string& s, double def);
[[nodiscard]] long long safe_stoll(const std::string& s, long long def);
[[nodiscard]] std::string trim(const std::string& s);
[[nodiscard]] std::string escape_shell_arg(const std::string& input);
[[nodiscard]] std::string sanitize_alphanumeric(const std::string& input);

// Signal / crash handling
void crash_handler(int sig);
void terminate_handler();
void set_display_power(bool power);

// System diagnostics and file path helpers
[[nodiscard]] std::string get_exe_dir();
[[nodiscard]] bool file_exists(const std::string& path);
[[nodiscard]] std::string get_timestamp();
[[nodiscard]] bool is_media_dir_healthy(const std::string& media_dir);



// Media Classification & Date parsing utilities
void classify_media_item(const MediaItem& item, bool& has_people, bool& has_animals, bool& is_doc);
bool parse_filename_date(const std::string& filename, int& y, int& m, int& d);
void get_modified_time_date(int64_t mtime, int& y, int& m, int& d);
bool get_item_date(const MediaItem& item, int& y, int& m, int& d);

#endif // PITROVE_UTIL_H


#ifndef PITROVE_UTIL_H
#define PITROVE_UTIL_H

#define VERSION "14.7.9"
#define APP_NAME "piTrove"

#include <atomic>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cstdarg>
#include <charconv>
#include <type_traits>
#include <optional>
#include <tuple>

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
void clear_error(int code_num);
bool is_error_active(int code_num);

inline std::atomic<bool> g_database_complete{false};
inline std::string g_crash_cache_dir = "";
inline char g_crash_cache_dir_safe[512] = "";

enum class LogLevel {
    DEBUG = 0,
    INFO = 1,
    WARN = 2,
    ERROR = 3
};

struct LogMessage {
    std::chrono::system_clock::time_point time;
    LogLevel level;
    std::string body;
};

struct Logger {
    LogLevel level{LogLevel::INFO};
    std::string log_dir;
    std::string log_file_path;

    // Async queue
    std::mutex queue_mtx;
    std::mutex log_path_mtx;
    std::condition_variable cv;
    std::vector<LogMessage> front_queue;
    std::vector<LogMessage> back_queue;
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
// Unified safe number parser using std::from_chars (noexcept, zero-allocation)
template<typename T>
[[nodiscard]] inline T safe_parse(std::string_view s, T def) {
    // Trim leading whitespace (from_chars doesn't skip it)
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return def;
    s.remove_prefix(start);
    T value{};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    return (ec == std::errc{}) ? value : def;
}

// Legacy wrappers — call sites unchanged
[[nodiscard]] inline int safe_stoi(const std::string& s, int def) { return safe_parse<int>(s, def); }
[[nodiscard]] inline float safe_stof(const std::string& s, float def) { return safe_parse<float>(s, def); }
[[nodiscard]] inline double safe_stod(const std::string& s, double def) { return safe_parse<double>(s, def); }
[[nodiscard]] inline long long safe_stoll(const std::string& s, long long def) { return safe_parse<long long>(s, def); }

[[nodiscard]] std::string trim(std::string_view s);
[[nodiscard]] std::string escape_shell_arg(std::string_view input);
[[nodiscard]] std::string sanitize_alphanumeric(std::string_view input);

// Signal / crash handling
void crash_handler(int sig);
void terminate_handler();
void set_display_power(bool power);
bool set_interface_status(const std::string& iface, bool up);

// WiFi diagnostics
struct WifiStats {
    int quality = -1;       // 0-255 scale (link quality)
    int signal_dbm = -129;  // Signal level in dBm
    int noise_dbm = -129;   // Noise level in dBm
    unsigned long missed_beacons = 0;
    bool has_data = false;
};
[[nodiscard]] WifiStats read_wifi_stats(const std::string& iface = "wlan0");

// System diagnostics and file path helpers
[[nodiscard]] std::string get_exe_dir();
[[nodiscard]] bool file_exists(const std::string& path);
[[nodiscard]] std::string get_timestamp();
[[nodiscard]] bool is_media_dir_healthy(const std::string& media_dir);
[[nodiscard]] bool is_nas_online();
void check_network_status();
void prefetch_video(const std::string& path);
void cleanup_nas_thread();
void cleanup_prefetch_thread();



// Media Classification & Date parsing utilities
void classify_media_item(const MediaItem& item, bool& has_people, bool& has_animals, bool& is_doc);
[[nodiscard]] std::optional<std::tuple<int,int,int>> parse_filename_date(std::string_view filename);
void get_modified_time_date(int64_t mtime, int& y, int& m, int& d);
[[nodiscard]] std::optional<std::tuple<int,int,int>> get_item_date(const MediaItem& item);

#endif // PITROVE_UTIL_H


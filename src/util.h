#ifndef PITROVE_UTIL_H
#define PITROVE_UTIL_H

#define VERSION "9.0.12"
#define APP_NAME "piTrove"

#include <atomic>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <cstdarg>

// Global atomics and sharing handles
extern std::atomic<bool> g_running;
extern std::atomic<int> g_remote_command; // 1=Next, 2=Prev, 3=PauseToggle
extern std::atomic<float> g_weather_temp;
extern std::atomic<int> g_weather_code;
extern std::atomic<bool> g_config_changed;
extern int g_http_server_fd;
extern std::atomic<bool> g_database_complete;
extern std::string g_crash_cache_dir;

enum class LogLevel {
    INFO,
    WARN,
    ERROR,
    DEBUG
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

    void flush_loop();
    void init(const std::string& path, LogLevel lvl);
    void rotate_logs(const std::string& dir, int keep);
    void log(LogLevel lvl, const char* fmt, ...);
    void info(const char* fmt, ...);
    void warn(const char* fmt, ...);
    void error(const char* fmt, ...);
    void debug(const char* fmt, ...);

    Logger() = default;
    ~Logger();
};

extern Logger g_logger;

// Math, string parsing, and files helpers
int safe_stoi(const std::string& s, int def);
float safe_stof(const std::string& s, float def);
double safe_stod(const std::string& s, double def);
long long safe_stoll(const std::string& s, long long def);
std::string trim(const std::string& s);

// Signal / crash handling
void crash_handler(int sig);
void terminate_handler();

// System diagnostics and file path helpers
std::string get_exe_dir();
bool file_exists(const std::string& path);
std::string get_timestamp();

// slide_debug utilities
void slide_debug(const char* fmt, ...);
void slide_debug_close();

#endif // PITROVE_UTIL_H

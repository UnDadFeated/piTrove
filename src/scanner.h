#ifndef PITROVE_SCANNER_H
#define PITROVE_SCANNER_H

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <poll.h>
#include "media_item.h"

// CIFS getdents64 directory read wrappers
ssize_t get_dents64(int fd, char* buf, size_t bufsz);
std::vector<std::string> read_dir(const std::string& path);
std::vector<std::string> read_dir_timeout(const std::string& path, int timeout_ms = 15000);
bool stat_timeout(const std::string& path, struct stat& st, int timeout_ms = 5000);

// Filename and type validation helpers
std::string file_ext(const std::string& path);
std::string file_name(const std::string& path);
bool is_image(std::string_view ext_or_path);
bool is_video(std::string_view ext_or_path);
bool is_media(std::string_view ext_or_path);

// Seasonal date checkers
bool is_in_seasonal_window(const std::string& filename, int window_days);

// Video metadata probing
bool probe_video_meta(const std::string& filepath, int& width, int& height, float& duration, time_t& creation_time);
double probe_video_duration(const std::string& path, int timeout_ms);

// MediaScanner class
class MediaScanner {
public:
    std::atomic<int> live_found_count{0};

    MediaScanner() = default;

    bool is_month_in_window(const std::string& dirname, int window_days);
    std::vector<std::string> scan(const std::string& directory,
                                 const std::vector<std::string>& exts,
                                 int window_days,
                                 int max_depth);
    int get_count();

private:
    void process_entry(const std::string& path_str,
                       const std::vector<std::string>& exts,
                       int window_days,
                       std::mutex& list_mutex,
                       std::vector<std::string>& all_files);
};

// Global scanner interface
void scan_directory(const std::string& dir, int depth,
                    std::vector<MediaItem>& items, std::atomic<int64_t>& count);

// Triple-entropy/playlist helpers
unsigned long long make_entropy_seed();

#endif // PITROVE_SCANNER_H

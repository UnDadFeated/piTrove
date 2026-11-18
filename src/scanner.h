#ifndef PITROVE_SCANNER_H
#define PITROVE_SCANNER_H

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <optional>
#include <stop_token>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <functional>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <poll.h>
#include "media_item.h"

// A disengaged listing means a failed or interrupted read. An empty directory
// is a successful listing. Workers may outlive the wait if CIFS is blocked.
std::optional<std::vector<std::string>> read_dir_timeout(const std::string& path,
                                                         int timeout_ms = 15000,
                                                         std::stop_token stop = {});
bool stat_timeout(const std::string& path, struct stat& st, int timeout_ms = 5000,
                  std::stop_token stop = {});

struct ScanResult {
    std::vector<MediaItem> items;
    bool complete = false;
};

bool is_image(std::string_view ext_or_path);

// Seasonal date checkers
bool is_in_seasonal_window(std::string_view filename, int window_days);



// MediaScanner class
class MediaScanner {
public:
    std::atomic<int> live_found_count{0};

    MediaScanner() = default;

  bool is_month_in_window(std::string_view dirname, int window_days);
    ScanResult scan(const std::string& directory,
                    const std::vector<std::string>& exts, int window_days, int max_depth,
                    const std::vector<std::string>& ignore_folders,
                    std::function<void(int)> progress, std::stop_token stop);


private:
    void process_entry(const std::string& path_str,
                       const struct stat& st,
                       const std::vector<std::string>& exts,
                       std::mutex& list_mutex,
                       std::vector<MediaItem>& all_items);
};

// Global scanner interface
ScanResult scan_directory(const std::string& dir, int depth,
                          std::function<void(int)> progress = nullptr, std::stop_token stop = {});

// Triple-entropy/playlist helpers
unsigned long long make_entropy_seed();

#endif // PITROVE_SCANNER_H

extern std::atomic<int> g_scanner_detached_threads;

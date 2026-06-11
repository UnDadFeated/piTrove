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

// CIFS getdents64 directory read wrappers
std::vector<std::string> read_dir(const std::string& path);
std::vector<std::string> read_dir_timeout(const std::string& path, int timeout_ms = 15000);
bool stat_timeout(const std::string& path, struct stat& st, int timeout_ms = 5000);

bool is_image(std::string_view ext_or_path);

// Seasonal date checkers
bool is_in_seasonal_window(std::string_view filename, int window_days);



// MediaScanner class
class MediaScanner {
public:
    std::atomic<int> live_found_count{0};

    MediaScanner() = default;

  bool is_month_in_window(std::string_view dirname, int window_days);
    std::vector<MediaItem> scan(const std::string& directory,
                                   const std::vector<std::string>& exts,
                                   int window_days,
                                   int max_depth,
                                   const std::vector<std::string>& ignore_folders,
                                   std::function<void(int)> progress = nullptr);


private:
    void process_entry(const std::string& path_str,
                       const struct stat& st,
                       const std::vector<std::string>& exts,
                       std::mutex& list_mutex,
                       std::vector<MediaItem>& all_items);
};

// Global scanner interface
void scan_directory(const std::string& dir, int depth,
                    std::vector<MediaItem>& items,
                    std::function<void(int)> progress = nullptr);

// Triple-entropy/playlist helpers
unsigned long long make_entropy_seed();

#endif // PITROVE_SCANNER_H

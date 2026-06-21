#include "organizer.h"
#include "util.h"
#include "image_loader.h"
#include <filesystem>
#include <iostream>
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <regex>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

static std::string format_date(int y, int m, int d) {
    std::ostringstream oss;
    oss << y << "-" << std::setw(2) << std::setfill('0') << m << "-" << std::setw(2) << std::setfill('0') << d;
    return oss.str();
}

static std::string get_exif_date_str(const fs::path& p) {
    int64_t t_val = ImageLoader::get_creation_time(p.string());
    if (t_val > 0) {
        std::time_t t = static_cast<std::time_t>(t_val);
        struct tm tm_buf;
        struct tm* timeinfo = localtime_r(&t, &tm_buf);
        if (timeinfo) {
            return format_date(timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday);
        }
    }
    return "";
}

static std::string get_mtime_date_str(const fs::path& p) {
    std::error_code ec;
    auto ftime = fs::last_write_time(p, ec);
    if (!ec) {
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
        std::time_t t = std::chrono::system_clock::to_time_t(sctp);
        struct tm tm_buf;
        struct tm* timeinfo = localtime_r(&t, &tm_buf);
        if (timeinfo) {
            return format_date(timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday);
        }
    }
    return "";
}

struct MoveOperation {
    fs::path src;
    fs::path dest_dir;
    fs::path dest;
};

bool organize_media_archive(const std::string& root_dir, bool in_place) {
    std::error_code ec;
    if (!fs::is_directory(root_dir, ec)) {
        std::cerr << "ERROR: Target directory does not exist: " << root_dir << "\n";
        return false;
    }

    size_t scanned_count = 0;
    size_t organized_count = 0;
    size_t error_count = 0;

    static const std::unordered_set<std::string> image_exts = {".jpg", ".jpeg", ".png", ".tiff", ".tif", ".webp", ".heic", ".heif", ".bmp"};
    static const std::unordered_set<std::string> video_exts = {".mp4", ".m4v", ".mov", ".avi", ".mkv", ".hevc"};

    std::vector<MoveOperation> moves_to_perform;
    std::regex org_pattern("/(Photos|Videos)/[0-9]{4}-[0-9]{2}/[0-9]{4}-[0-9]{2}-[0-9]{2}_");
    std::regex prefix_pattern("^[0-9]{4}-[0-9]{2}-[0-9]{2}_");

    std::cout << "Scanning media files in: " << root_dir << "...\n";

    // 1. Traverse and schedule moves
    try {
        for (const auto& entry : fs::recursive_directory_iterator(root_dir)) {
            if (!entry.is_regular_file()) continue;

            fs::path src_path = entry.path();
            std::string ext = src_path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            bool is_img = image_exts.count(ext) > 0;
            bool is_vid = video_exts.count(ext) > 0;
            if (!is_img && !is_vid) continue;

            scanned_count++;

            // Skip if already in organized out-of-place path
            std::string rel_path_str = fs::relative(src_path, root_dir).generic_string();
            if (!in_place && std::regex_search("/" + rel_path_str, org_pattern)) {
                continue;
            }

            // A. Resolve Date
            std::string date_str = "";
            if (is_img && (ext == ".jpg" || ext == ".jpeg")) {
                date_str = get_exif_date_str(src_path);
            }

            if (date_str.empty()) {
                std::string filename = src_path.filename().string();
                if (auto parsed = parse_filename_date(filename)) {
                    auto [y, m, d] = *parsed;
                    date_str = format_date(y, m, d);
                }
            }

            if (date_str.empty()) {
                date_str = get_mtime_date_str(src_path);
            }

            if (date_str.empty()) {
                std::cerr << "WARNING: Could not retrieve timestamp for: " << src_path << "\n";
                error_count++;
                continue;
            }

            // Determine parts for grouping folders
            std::string year_month = date_str.substr(0, 7); // "YYYY-MM"

            // B. Determine Destination directory
            fs::path dest_dir;
            if (in_place) {
                dest_dir = src_path.parent_path();
            } else {
                std::string subfolder = is_img ? "Photos" : "Videos";
                dest_dir = fs::path(root_dir) / subfolder / year_month;
            }

            // C. Determine Destination filename & handle prefix cleaning
            std::string base_name = src_path.stem().string();
            std::string clean_base = std::regex_replace(base_name, prefix_pattern, "");
            std::string new_filename = date_str + "_" + clean_base + ext;
            fs::path dest_path = dest_dir / new_filename;

            // Skip if source is already same as destination
            if (fs::equivalent(src_path, dest_path, ec)) {
                continue;
            }

            int counter = 1;
            std::error_code exists_ec;
            while (fs::exists(dest_path, exists_ec) && !exists_ec) {
                new_filename = date_str + "_" + clean_base + "_" + std::to_string(counter) + ext;
                dest_path = dest_dir / new_filename;
                counter++;
            }
            if (exists_ec) {
                std::cerr << "ERROR: Filesystem error checking destination existence for " << dest_path << ": " << exists_ec.message() << "\n";
                error_count++;
                continue;
            }

            moves_to_perform.push_back({src_path, dest_dir, dest_path});
        }
    } catch (const std::exception& e) {
        std::cerr << "ERROR: Failed during filesystem scan: " << e.what() << "\n";
        return false;
    }

    // 2. Perform scheduled moves
    std::cout << "Scheduled " << moves_to_perform.size() << " file operations. Executing...\n";

    for (const auto& op : moves_to_perform) {
        try {
            std::error_code dest_dir_ec;
            if (!fs::exists(op.dest_dir, dest_dir_ec) && !dest_dir_ec) {
                fs::create_directories(op.dest_dir, dest_dir_ec);
            }
            if (dest_dir_ec) {
                std::cerr << "ERROR: Failed to verify/create destination directory " << op.dest_dir << ": " << dest_dir_ec.message() << "\n";
                error_count++;
                continue;
            }

            // Preserve original timestamps
            std::error_code mtime_ec;
            auto orig_mtime = fs::last_write_time(op.src, mtime_ec);
            if (mtime_ec) {
                std::cerr << "ERROR: Failed to read modification time of " << op.src << ": " << mtime_ec.message() << "\n";
                error_count++;
                continue;
            }

            // Move the file
            std::error_code rename_ec;
            fs::rename(op.src, op.dest, rename_ec);
            if (rename_ec) {
                std::cerr << "ERROR: Failed to rename " << op.src << " to " << op.dest << ": " << rename_ec.message() << "\n";
                error_count++;
                continue;
            }

            // Restore timestamps on destination
            std::error_code set_mtime_ec;
            fs::last_write_time(op.dest, orig_mtime, set_mtime_ec);
            organized_count++;
        } catch (const std::exception& e) {
            std::cerr << "ERROR: Failed to move " << op.src << " to " << op.dest << ": " << e.what() << "\n";
            error_count++;
        }
    }

    std::cout << "\n[✓] Reorganization complete.\n";
    std::cout << "    - Total files scanned: " << scanned_count << "\n";
    std::cout << "    - Files successfully reorganized: " << organized_count << "\n";
    std::cout << "    - Errors encountered: " << error_count << "\n";

    return error_count == 0;
}

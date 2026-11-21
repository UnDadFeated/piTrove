#include "organizer.h"
#include "file_ops.h"
#include <sqlite3.h>
#include <filesystem>
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
#include <format>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <memory>

namespace fs = std::filesystem;

static bool archive_path(const fs::path& name, const fs::path& root) {
    const auto relative = name.lexically_relative(root);
    if (relative.empty() || relative == "." || *relative.begin() == "..") return false;
    std::error_code error;
    return fs::weakly_canonical(name.parent_path(), error) == name.parent_path() && !error;
}

static std::string format_date(int y, int m, int d) {
    return std::format("{}-{:02d}-{:02d}", y, m, d);
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

bool organize_media_archive(const std::string& root_dir, bool in_place,
                            const std::vector<std::string>& ignore_folders) {
    std::error_code ec;
    if (!fs::is_directory(root_dir, ec)) {
        std::cerr << "ERROR: Target directory does not exist: " << root_dir << "\n";
        return false;
    }

    struct statvfs storage{};
    if (statvfs(root_dir.c_str(), &storage) != 0 || (storage.f_flag & ST_RDONLY)) {
        std::cerr << "Media archive requires a writable mount.\n";
        return false;
    }

    const auto root = fs::canonical(root_dir, ec);
    if (ec) return false;
    const auto journal_path = root / ".pitrove-organizer.sqlite3";
    int journal_fd = open(journal_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (journal_fd < 0) return false;
    struct JournalLock { int fd; ~JournalLock() { close(fd); } } lock{journal_fd};
    if (flock(journal_fd, LOCK_EX | LOCK_NB) != 0) {
        std::cerr << "Another organizer is using this archive.\n";
        return false;
    }
    sqlite3* database = nullptr;
    const int opened = sqlite3_open_v2(journal_path.c_str(), &database, SQLITE_OPEN_READWRITE, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> journal(database, sqlite3_close);
    if (opened != SQLITE_OK || sqlite3_exec(database,
        "PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS moves(id INTEGER PRIMARY KEY, old_path TEXT NOT NULL, new_path TEXT NOT NULL, dev INTEGER NOT NULL, inode INTEGER NOT NULL);",
        nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "Could not open the archive undo journal.\n";
        return false;
    }
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, "INSERT INTO moves(old_path,new_path,dev,inode) VALUES(?,?,?,?)", -1, &statement, nullptr) != SQLITE_OK)
        return false;
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> record(statement, sqlite3_finalize);

    size_t scanned_count = 0;
    size_t organized_count = 0;
    size_t error_count = 0;

    static const std::unordered_set<std::string> image_exts = {".jpg", ".jpeg", ".png", ".tiff", ".tif", ".webp", ".heic", ".heif", ".bmp"};
    static const std::unordered_set<std::string> video_exts = {".mp4", ".m4v", ".mov", ".avi", ".mkv", ".hevc"};

    std::vector<MoveOperation> moves_to_perform;
    std::unordered_set<std::string> reserved_destinations;
    std::regex org_pattern("^/(Photos|Videos)/[0-9]{4}-[0-9]{2}/[0-9]{4}-[0-9]{2}-[0-9]{2}_");
    std::regex prefix_pattern("^[0-9]{4}-[0-9]{2}-[0-9]{2}_");

    std::cout << "Scanning media files in: " << root_dir << "...\n";

    // 1. Traverse and schedule moves
    try {
        for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
            const auto& entry = *it;
            auto name = entry.path().filename().string();
            if (name.starts_with('.') || std::find(ignore_folders.begin(), ignore_folders.end(), name) != ignore_folders.end()) {
                it.disable_recursion_pending();
                continue;
            }
            if (!fs::is_regular_file(entry.symlink_status())) continue;

            fs::path src_path = entry.path();
            std::string ext = src_path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });

            bool is_img = image_exts.count(ext) > 0;
            bool is_vid = video_exts.count(ext) > 0;
            if (!is_img && !is_vid) continue;

            scanned_count++;

            // Skip if already in organized out-of-place path
            std::error_code ec;
            std::string rel_path_str = fs::relative(src_path, root, ec).generic_string();
            if (ec) { g_logger.warn("Organizer: skipping {}: {}", src_path.c_str(), ec.message().c_str()); continue; }
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
                dest_dir = root / subfolder / year_month;
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
            while (!exists_ec && (reserved_destinations.contains(dest_path.lexically_normal().string()) || fs::exists(dest_path, exists_ec))) {
                new_filename = std::format("{}_{}_{}{}", date_str, clean_base, counter, ext);
                dest_path = dest_dir / new_filename;
                counter++;
            }
            if (exists_ec) {
                std::cerr << "ERROR: Filesystem error checking destination existence for " << dest_path << ": " << exists_ec.message() << "\n";
                error_count++;
                continue;
            }

            reserved_destinations.insert(dest_path.lexically_normal().string());
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
            if (!archive_path(op.dest, root) || !archive_path(op.src, root)) {
                ++error_count;
                continue;
            }
            std::error_code dest_dir_ec;
            if (!fs::exists(op.dest_dir, dest_dir_ec) && !dest_dir_ec) {
                fs::create_directories(op.dest_dir, dest_dir_ec);
            }
            if (dest_dir_ec) {
                std::cerr << "ERROR: Failed to verify/create destination directory " << op.dest_dir << ": " << dest_dir_ec.message() << "\n";
                error_count++;
                continue;
            }

            struct stat identity{};
            if (lstat(op.src.c_str(), &identity) != 0 || !S_ISREG(identity.st_mode)) {
                ++error_count;
                continue;
            }
            const auto source = op.src.lexically_relative(root);
            const auto destination = op.dest.lexically_relative(root);
            sqlite3_reset(record.get());
            if (sqlite3_bind_text(record.get(), 1, source.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                sqlite3_bind_text(record.get(), 2, destination.c_str(), -1, SQLITE_TRANSIENT) != SQLITE_OK ||
                sqlite3_bind_int64(record.get(), 3, identity.st_dev) != SQLITE_OK ||
                sqlite3_bind_int64(record.get(), 4, identity.st_ino) != SQLITE_OK ||
                sqlite3_step(record.get()) != SQLITE_DONE) {
                std::cerr << "Could not record an undo entry. Stopped organizing.\n";
                return false;
            }
            const auto journal_id = sqlite3_last_insert_rowid(database);
            std::error_code rename_ec;
            if (!move_without_replace(op.src, op.dest, rename_ec)) {
                if (fs::exists(op.src, ec) && !ec) {
                    sqlite3_stmt* remove = nullptr;
                    if (sqlite3_prepare_v2(database, "DELETE FROM moves WHERE id=?", -1, &remove, nullptr) == SQLITE_OK) {
                        sqlite3_bind_int64(remove, 1, journal_id);
                        sqlite3_step(remove);
                    }
                    sqlite3_finalize(remove);
                }
                std::cerr << "ERROR: Failed to rename " << op.src << " to " << op.dest << ": " << rename_ec.message() << "\n";
                error_count++;
                continue;
            }

            struct stat moved{};
            if (lstat(op.dest.c_str(), &moved) != 0) return false;
            if (moved.st_dev != identity.st_dev || moved.st_ino != identity.st_ino) {
                // Until this update is durable, undo leaves a cross-device copy alone.
                sqlite3_stmt* update = nullptr;
                const bool saved = sqlite3_prepare_v2(database, "UPDATE moves SET dev=?, inode=? WHERE id=?", -1, &update, nullptr) == SQLITE_OK &&
                    sqlite3_bind_int64(update, 1, moved.st_dev) == SQLITE_OK &&
                    sqlite3_bind_int64(update, 2, moved.st_ino) == SQLITE_OK &&
                    sqlite3_bind_int64(update, 3, journal_id) == SQLITE_OK &&
                    sqlite3_step(update) == SQLITE_DONE;
                sqlite3_finalize(update);
                if (!saved) return false;
            }
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


bool undo_organize(const std::string& journal_path) {
    std::error_code error;
    const auto root = fs::canonical(fs::absolute(journal_path).parent_path(), error);
    if (error) return false;
    const int fd = open(journal_path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    struct JournalLock { int fd; ~JournalLock() { close(fd); } } lock{fd};
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) return false;
    sqlite3* db = nullptr;
    const auto result = sqlite3_open_v2(journal_path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> journal(db, sqlite3_close);
    if (result != SQLITE_OK) return false;
    struct UndoRow { sqlite3_int64 id; fs::path old_path, new_path; dev_t dev; ino_t inode; };
    std::vector<UndoRow> rows;
    sqlite3_stmt* stmt = nullptr;
    const bool has_identity = sqlite3_prepare_v2(db,
        "SELECT id, old_path, new_path, dev, inode FROM moves ORDER BY id DESC", -1, &stmt, nullptr) == SQLITE_OK;
    if (!has_identity && sqlite3_prepare_v2(db,
        "SELECT id, old_path, new_path FROM moves ORDER BY id DESC", -1, &stmt, nullptr) != SQLITE_OK) return false;
    int next;
    while ((next = sqlite3_step(stmt)) == SQLITE_ROW) {
        const auto* old_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const auto* new_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        if (old_path && new_path) rows.push_back({sqlite3_column_int64(stmt, 0), old_path, new_path,
            has_identity ? static_cast<dev_t>(sqlite3_column_int64(stmt, 3)) : 0,
            has_identity ? static_cast<ino_t>(sqlite3_column_int64(stmt, 4)) : 0});
    }
    sqlite3_finalize(stmt);
    if (next != SQLITE_DONE) return false;
    sqlite3_stmt* remove = nullptr;
    if (sqlite3_prepare_v2(db, "DELETE FROM moves WHERE id=?", -1, &remove, nullptr) != SQLITE_OK) return false;
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> deletion(remove, sqlite3_finalize);
    bool complete = true;
    for (const auto& row : rows) {
        const auto dest = (root / row.new_path).lexically_normal();
        const auto source = (root / row.old_path).lexically_normal();
        struct stat current{};
        if (!archive_path(dest, root) || !archive_path(source, root) ||
            lstat(dest.c_str(), &current) != 0 || !S_ISREG(current.st_mode) ||
            (has_identity && (current.st_dev != row.dev || current.st_ino != row.inode)) ||
            !move_without_replace(dest, source, error)) {
            g_logger.warn("Organizer: retained undo entry for {}", row.new_path.string());
            complete = false;
            continue;
        }
        sqlite3_reset(remove);
        if (sqlite3_bind_int64(remove, 1, row.id) != SQLITE_OK || sqlite3_step(remove) != SQLITE_DONE) return false;
    }
    return complete;
}

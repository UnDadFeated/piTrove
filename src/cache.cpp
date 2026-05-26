#include "cache.h"
#include "config.h"
#include "util.h"
#include "image_loader.h"
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cstdio>

CacheManager* g_cache = nullptr;

bool CacheManager::open(const std::string& dir) {
    g_logger.info("[TRACE] CacheManager::open dir=%s", dir.c_str());
    std::filesystem::create_directories(dir);

    std::string path = dir + "/cache.db";
    if (std::filesystem::exists(path) && !verify_database(path)) {
        g_logger.warn("Cache database at %s has an outdated schema or is corrupt. Purging to rebuild...", path.c_str());
        std::filesystem::remove(path);
        std::filesystem::remove(path + "-wal");
        std::filesystem::remove(path + "-shm");
    }

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
        if (db) { sqlite3_close(db); db = nullptr; }
        return false;
    }
    sqlite3_busy_timeout(db, 5000);

    // Proactive SQLite integrity check
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (result && std::string(result) != "ok") {
                g_logger.error("SQLite integrity check failed (%s). Purging corrupted cache...", result);
                sqlite3_finalize(stmt);
                stmt = nullptr;
                sqlite3_close(db);
                db = nullptr;
                std::remove(path.c_str());
                std::string wal = path + "-wal";
                std::string shm = path + "-shm";
                std::remove(wal.c_str());
                std::remove(shm.c_str());
                if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
                    if (db) { sqlite3_close(db); db = nullptr; }
                    return false;
                }
            }
        }
        if (stmt) {
            sqlite3_finalize(stmt);
        }
    }

    sqlite3_busy_timeout(db, 5000);

    char* err = nullptr;
    if (sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err) != SQLITE_OK) {
        g_logger.warn("Failed to set WAL mode: %s", err ? err : "unknown");
        if (err) sqlite3_free(err);
    }
    if (sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, &err) != SQLITE_OK) {
        g_logger.warn("Failed to set synchronous=NORMAL: %s", err ? err : "unknown");
        if (err) sqlite3_free(err);
    }
    char mmap_sql[64];
    {
        std::lock_guard<std::mutex> lk(g_config_mtx);
        snprintf(mmap_sql, sizeof(mmap_sql), "PRAGMA mmap_size=%lld", (long long)g_cfg.cache_mmap_size);
    }
    if (sqlite3_exec(db, mmap_sql, nullptr, nullptr, &err) != SQLITE_OK) {
        g_logger.warn("Failed to set mmap_size=%lld: %s", g_cfg.cache_mmap_size, err ? err : "unknown");
        if (err) sqlite3_free(err);
    }

    if (sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS cache ("
        "path TEXT PRIMARY KEY, type TEXT, w INT, h INT, duration REAL, "
        "exif INT, bad INT DEFAULT 0, last_shown INTEGER DEFAULT 0, timestamp INTEGER DEFAULT 0, is_camera INT DEFAULT -1"
        ")", nullptr, nullptr, &err) != SQLITE_OK) {
        g_logger.error("Failed to create cache table: %s", err ? err : "unknown");
        if (err) sqlite3_free(err);
        close();
        return false;
    }

    // Safe migration for existing databases
    if (sqlite3_exec(db, "ALTER TABLE cache ADD COLUMN last_shown INTEGER DEFAULT 0",
                  nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
    }
    if (sqlite3_exec(db, "ALTER TABLE cache ADD COLUMN bad INT DEFAULT 0",
                  nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
    }
    if (sqlite3_exec(db, "ALTER TABLE cache ADD COLUMN is_camera INT DEFAULT -1",
                  nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
    }

    sqlite3_finalize(stmt_upsert); stmt_upsert = nullptr;
    sqlite3_finalize(stmt_load); stmt_load = nullptr;
    sqlite3_finalize(stmt_mark); stmt_mark = nullptr;

    if (sqlite3_prepare_v2(db,
        "INSERT INTO cache (path, type, w, h, exif, duration, bad, last_shown, timestamp, is_camera) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(path) DO UPDATE SET "
        "w=excluded.w, h=excluded.h, exif=excluded.exif, "
        "duration=excluded.duration, bad=excluded.bad, "
        "last_shown=excluded.last_shown, timestamp=excluded.timestamp, is_camera=excluded.is_camera",
        -1, &stmt_upsert, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare upsert statement.");
        close();
        return false;
    }

    if (sqlite3_prepare_v2(db,
        "SELECT w, h, duration, exif, bad, last_shown, timestamp, is_camera FROM cache WHERE path = ?",
        -1, &stmt_load, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare load statement.");
        close();
        return false;
    }

    if (sqlite3_prepare_v2(db,
        "UPDATE cache SET last_shown = ? WHERE path = ?",
        -1, &stmt_mark, nullptr) != SQLITE_OK) {
        g_logger.error("Failed to prepare mark statement.");
        close();
        return false;
    }

    return true;
}

void CacheManager::close() {
    g_logger.info("[TRACE] CacheManager::close");
    if (stmt_upsert) { sqlite3_finalize(stmt_upsert); stmt_upsert = nullptr; }
    if (stmt_load) { sqlite3_finalize(stmt_load); stmt_load = nullptr; }
    if (stmt_mark) { sqlite3_finalize(stmt_mark); stmt_mark = nullptr; }
    if (db) { sqlite3_close(db); db = nullptr; }
}

CacheManager::~CacheManager() {
    close();
}

bool CacheManager::load_cached(MediaItem& mi) {
    if (!stmt_load) return false;
    std::lock_guard<std::mutex> lk(db_mutex);
    bool found = false;
    sqlite3_bind_text(stmt_load, 1, mi.path.c_str(), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt_load) == SQLITE_ROW) {
        mi.width      = sqlite3_column_int64(stmt_load, 0);
        mi.height     = sqlite3_column_int64(stmt_load, 1);
        mi.duration   = sqlite3_column_double(stmt_load, 2);
        mi.exif_rotation = sqlite3_column_int(stmt_load, 3);
        int bad = sqlite3_column_int(stmt_load, 4);
        mi.last_shown = sqlite3_column_int64(stmt_load, 5);
        mi.modified_time = sqlite3_column_int64(stmt_load, 6);
        mi.is_camera  = sqlite3_column_int(stmt_load, 7);
        if (bad == 0) found = true;
    }
    sqlite3_reset(stmt_load);
    return found;
}

void CacheManager::upsert(const MediaItem& mi, int bad) {
    if (!stmt_upsert) return;

    // Cast to mutable reference to allow caching the camera EXIF check dynamically outside bulk transactions
    MediaItem& mutable_mi = const_cast<MediaItem&>(mi);
    if (mutable_mi.is_camera == -1 && !in_transaction && mutable_mi.type == "image" && bad == 0) {
        mutable_mi.is_camera = ImageLoader::has_camera_exif(mutable_mi.path.c_str()) ? 1 : 0;
    }

    std::lock_guard<std::mutex> lk(db_mutex);
    sqlite3_bind_text(stmt_upsert, 1, mutable_mi.path.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt_upsert, 2, mutable_mi.type.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt_upsert, 3, mutable_mi.width);
    sqlite3_bind_int64(stmt_upsert, 4, mutable_mi.height);
    sqlite3_bind_int(stmt_upsert, 5, mutable_mi.exif_rotation);
    sqlite3_bind_double(stmt_upsert, 6, mutable_mi.duration);
    sqlite3_bind_int(stmt_upsert, 7, bad);
    sqlite3_bind_int64(stmt_upsert, 8, mutable_mi.last_shown);
    sqlite3_bind_int64(stmt_upsert, 9, mutable_mi.modified_time);
    sqlite3_bind_int(stmt_upsert, 10, mutable_mi.is_camera);
    int step_ret = sqlite3_step(stmt_upsert);
    if (step_ret != SQLITE_DONE) {
        g_logger.error("Failed to execute upsert for: %s", mutable_mi.path.c_str());
    }
    sqlite3_reset(stmt_upsert);
}

void CacheManager::mark_shown(const std::string& path) {
    g_logger.info("[TRACE] CacheManager::mark_shown path=%s", path.c_str());
    if (!stmt_mark) return;
    std::lock_guard<std::mutex> lk(db_mutex);
    sqlite3_bind_int64(stmt_mark, 1, time(nullptr));
    sqlite3_bind_text(stmt_mark, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    int step_ret = sqlite3_step(stmt_mark);
    if (step_ret != SQLITE_DONE) {
        g_logger.error("Failed to execute mark_shown for: %s", path.c_str());
    }
    sqlite3_reset(stmt_mark);
}

void CacheManager::mark_bad(const std::string& filepath) {
    g_logger.info("[TRACE] CacheManager::mark_bad path=%s", filepath.c_str());
    if (!db) return;
    std::lock_guard<std::mutex> lk(db_mutex);
    const char* sql = "UPDATE cache SET bad = 1 WHERE path = ?;";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, filepath.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

void CacheManager::begin_transaction() {
    if (!db) return;
    std::lock_guard<std::mutex> lk(db_mutex);
    char* err = nullptr;
    int rc = sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        g_logger.error("Failed to BEGIN TRANSACTION: %s", err ? err : "unknown error");
        if (err) sqlite3_free(err);
    } else {
        in_transaction.store(true);
    }
}

void CacheManager::commit_transaction() {
    if (!db) return;
    std::lock_guard<std::mutex> lk(db_mutex);
    char* err = nullptr;
    int rc = sqlite3_exec(db, "COMMIT;", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        g_logger.error("Failed to COMMIT transaction: %s. Performing ROLLBACK...", err ? err : "unknown error");
        if (err) sqlite3_free(err);
        sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    in_transaction.store(false);
}

void CacheManager::reset_all_cooldowns() {
    if (!db) return;
    std::lock_guard<std::mutex> lk(db_mutex);
    char* err = nullptr;
    int rc = sqlite3_exec(db, "UPDATE cache SET last_shown = 0;", nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        g_logger.error("Failed to reset shown history cooldowns: %s", err ? err : "unknown error");
        if (err) sqlite3_free(err);
    } else {
        g_logger.info("Successfully reset shown history cooldowns inside database.");
    }
}

bool verify_database(const std::string& path) {
    sqlite3* db = nullptr;
    sqlite3_stmt* stmt = nullptr;
    bool ok = false;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) { sqlite3_close(db); }
        return false;
    }
    
    // Check if the table 'cache' exists and has all the required columns
    const char* sql = "SELECT path, type, w, h, duration, exif, bad, last_shown, timestamp, is_camera FROM cache LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        ok = true;
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return ok;
}

#include "cache.h"
#include "config.h"
#include "util.h"
#include "image_loader.h"
#include "error_db.h"
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <climits>


bool CacheManager::open(const std::string& dir) {
    g_logger.info("[TRACE] CacheManager::open dir=%s", dir.c_str());
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
        g_logger.error("CacheManager: Failed to create directories %s: %s", dir.c_str(), ec.message().c_str());
    }

    std::string path = dir + "/cache.db";
    if (std::filesystem::exists(path, ec) && !ec && !verify_database(path)) {
        g_logger.warn("Cache database at %s has an outdated schema or is corrupt. Purging to rebuild...", path.c_str());
        std::filesystem::remove(path, ec);
        std::filesystem::remove(path + "-wal", ec);
        std::filesystem::remove(path + "-shm", ec);
    }

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
        if (db) { sqlite3_close(db); db = nullptr; }
        trigger_error(413); // E413: SQLITE_OPEN_FAILED
        return false;
    }
    sqlite3_busy_timeout(db, 5000);

    // Proactive SQLite integrity check
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (result && std::string(result) != "ok") {
                trigger_error(401); // E401: SQLITE_DB_CORRUPTED
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
                    trigger_error(413); // E413: SQLITE_OPEN_FAILED
                    return false;
                }
                sqlite3_busy_timeout(db, 5000);
            }
        }
        if (stmt) {
            sqlite3_finalize(stmt);
        }
    }

    char* err = nullptr;
    if (sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err) != SQLITE_OK) {
        g_logger.warn("Failed to set WAL mode: %s", err ? err : "unknown");
        if (err) sqlite3_free(err);
        close();
        return false;
    }
    if (sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, &err) != SQLITE_OK) {
        g_logger.warn("Failed to set synchronous=NORMAL: %s", err ? err : "unknown");
        if (err) sqlite3_free(err);
        close();
        return false;
    }
    char mmap_sql[64];
    long long mmap_val = 0;
    {
        std::shared_lock lk(g_config_mtx);
        mmap_val = (long long)g_cfg.cache_mmap_size;
    }
    snprintf(mmap_sql, sizeof(mmap_sql), "PRAGMA mmap_size=%lld", mmap_val);
    if (sqlite3_exec(db, mmap_sql, nullptr, nullptr, &err) != SQLITE_OK) {
        g_logger.warn("Failed to set mmap_size=%lld: %s", mmap_val, err ? err : "unknown");
        if (err) sqlite3_free(err);
    }

    if (sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS cache ("
        "path TEXT PRIMARY KEY, type TEXT, w INT, h INT, duration REAL, "
        "exif INT, bad INT DEFAULT 0, last_shown INTEGER DEFAULT 0, timestamp INTEGER DEFAULT 0, is_camera INT DEFAULT -1, creation_time INTEGER DEFAULT 0, preprocessed INT DEFAULT 0"
        ")", nullptr, nullptr, &err) != SQLITE_OK) {
        trigger_error(407); // E407: SQLITE_MIGRATION_FAILED
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
    if (sqlite3_exec(db, "ALTER TABLE cache ADD COLUMN creation_time INTEGER DEFAULT 0",
                  nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
    }
    if (sqlite3_exec(db, "ALTER TABLE cache ADD COLUMN preprocessed INTEGER DEFAULT 0",
                  nullptr, nullptr, &err) != SQLITE_OK) {
        if (err) sqlite3_free(err);
    }

    sqlite3_finalize(stmt_upsert); stmt_upsert = nullptr;
    sqlite3_finalize(stmt_load); stmt_load = nullptr;
    sqlite3_finalize(stmt_mark); stmt_mark = nullptr;

    if (sqlite3_prepare_v2(db,
        "INSERT INTO cache (path, type, w, h, exif, duration, bad, last_shown, timestamp, is_camera, creation_time, preprocessed) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(path) DO UPDATE SET "
        "w=excluded.w, h=excluded.h, exif=excluded.exif, "
        "duration=excluded.duration, bad=excluded.bad, "
        "last_shown=excluded.last_shown, timestamp=excluded.timestamp, is_camera=excluded.is_camera, creation_time=excluded.creation_time, preprocessed=excluded.preprocessed",
        -1, &stmt_upsert, nullptr) != SQLITE_OK) {
        trigger_error(410); // E410: SQLITE_PREPARE_STMT_FAIL
        close();
        return false;
    }

    if (sqlite3_prepare_v2(db,
        "SELECT w, h, duration, exif, bad, last_shown, timestamp, is_camera, creation_time FROM cache WHERE path = ?",
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

    seed_error_catalog();

    return true;
}

void CacheManager::close() {
    g_logger.info("[TRACE] CacheManager::close");
    if (stmt_upsert) { sqlite3_finalize(stmt_upsert); stmt_upsert = nullptr; }
    if (stmt_load) { sqlite3_finalize(stmt_load); stmt_load = nullptr; }
    if (stmt_mark) { sqlite3_finalize(stmt_mark); stmt_mark = nullptr; }
    if (db) { sqlite3_close_v2(db); db = nullptr; }
}

CacheManager::~CacheManager() {
    close();
}

bool CacheManager::load_cached(MediaItem& mi) {
    if (!stmt_load) return false;
    std::lock_guard<std::mutex> lk(db_mutex);
    bool found = false;
    sqlite3_bind_text(stmt_load, 1, mi.path.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt_load) == SQLITE_ROW) {
        int64_t col_w = sqlite3_column_int64(stmt_load, 0);
        int64_t col_h = sqlite3_column_int64(stmt_load, 1);
        mi.width      = (int)std::min(col_w, (int64_t)INT_MAX);
        mi.height     = (int)std::min(col_h, (int64_t)INT_MAX);
        mi.duration   = sqlite3_column_double(stmt_load, 2);
        mi.exif_rotation = sqlite3_column_int(stmt_load, 3);
        int bad = sqlite3_column_int(stmt_load, 4);
        mi.last_shown = sqlite3_column_int64(stmt_load, 5);
        mi.modified_time = sqlite3_column_int64(stmt_load, 6);
        mi.is_camera  = sqlite3_column_int(stmt_load, 7);
        mi.creation_time = sqlite3_column_int64(stmt_load, 8);
        if (bad == 0) found = true;
    }
    sqlite3_reset(stmt_load);
    sqlite3_clear_bindings(stmt_load);
    return found;
}

void CacheManager::upsert(const MediaItem& mi, int bad, int preprocessed) {
    if (!stmt_upsert) return;

    if ((mi.is_camera == -1 || mi.creation_time == 0) && mi.type == "image" && bad == 0) {
        int db_is_cam = -1;
        int64_t db_creat = 0;
        bool db_found = false;
        {
            std::lock_guard<std::mutex> lk(db_mutex);
            sqlite3_stmt* stmt_chk = nullptr;
            const char* sql_chk = "SELECT is_camera, creation_time FROM cache WHERE path = ?;";
            if (sqlite3_prepare_v2(db, sql_chk, -1, &stmt_chk, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt_chk, 1, mi.path.c_str(), -1, SQLITE_TRANSIENT);
                if (sqlite3_step(stmt_chk) == SQLITE_ROW) {
                    db_is_cam = sqlite3_column_int(stmt_chk, 0);
                    db_creat = sqlite3_column_int64(stmt_chk, 1);
                    db_found = true;
                }
                sqlite3_finalize(stmt_chk);
            }
        }
        if (db_found && db_is_cam != -1) {
            mi.is_camera = db_is_cam;
            mi.creation_time = db_creat;
        } else if (!in_transaction) {
            std::vector<uint8_t> buffer = ImageLoader::read_file_to_buffer(mi.path);
            if (!buffer.empty()) {
                ImageMetadata meta = ImageLoader::read_metadata_from_memory(buffer.data(), (unsigned int)buffer.size());
                mi.is_camera = meta.is_camera ? 1 : 0;
                mi.creation_time = meta.creation_time;
            } else {
                mi.is_camera = 0;
            }
        }
    }

    std::lock_guard<std::mutex> lk(db_mutex);
    sqlite3_bind_text(stmt_upsert, 1, mi.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt_upsert, 2, mi.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt_upsert, 3, mi.width);
    sqlite3_bind_int64(stmt_upsert, 4, mi.height);
    sqlite3_bind_int(stmt_upsert, 5, mi.exif_rotation);
    sqlite3_bind_double(stmt_upsert, 6, mi.duration);
    sqlite3_bind_int(stmt_upsert, 7, bad);
    sqlite3_bind_int64(stmt_upsert, 8, mi.last_shown);
    sqlite3_bind_int64(stmt_upsert, 9, mi.modified_time);
    sqlite3_bind_int(stmt_upsert, 10, mi.is_camera);
    sqlite3_bind_int64(stmt_upsert, 11, mi.creation_time);
    sqlite3_bind_int(stmt_upsert, 12, preprocessed);
    int step_ret = sqlite3_step(stmt_upsert);
    if (step_ret != SQLITE_DONE) {
        if (step_ret == SQLITE_BUSY || step_ret == SQLITE_LOCKED) {
            trigger_error(402); // E402: SQLITE_LOCK_TIMEOUT
        } else {
            trigger_error(408); // E408: DISK_WRITE_FAIL
        }
        g_logger.error("Failed to execute upsert for: %s (error code: %d)", mi.path.c_str(), step_ret);
    } else {
        if (g_active_error_code.load() == 402 || g_active_error_code.load() == 408) {
            trigger_error(0);
        }
    }
    sqlite3_reset(stmt_upsert);
    sqlite3_clear_bindings(stmt_upsert);
}

void CacheManager::mark_shown(const std::string& path) {
    g_logger.info("[TRACE] CacheManager::mark_shown path=%s", path.c_str());
    if (!stmt_mark) return;
    std::lock_guard<std::mutex> lk(db_mutex);
    sqlite3_bind_int64(stmt_mark, 1, time(nullptr));
    sqlite3_bind_text(stmt_mark, 2, path.c_str(), -1, SQLITE_TRANSIENT);
    int step_ret = sqlite3_step(stmt_mark);
    if (step_ret != SQLITE_DONE) {
        if (step_ret == SQLITE_BUSY || step_ret == SQLITE_LOCKED) {
            trigger_error(402); // E402: SQLITE_LOCK_TIMEOUT
        } else {
            trigger_error(408); // E408: DISK_WRITE_FAIL
        }
        g_logger.error("Failed to execute mark_shown for: %s (error code: %d)", path.c_str(), step_ret);
    } else {
        if (g_active_error_code.load() == 402 || g_active_error_code.load() == 408) {
            trigger_error(0);
        }
    }
    sqlite3_reset(stmt_mark);
    sqlite3_clear_bindings(stmt_mark);
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
    if (in_transaction.load()) {
        g_logger.warn("CacheManager: begin_transaction called but in_transaction is already true.");
        return;
    }
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
    if (!in_transaction.load()) {
        g_logger.warn("CacheManager: commit_transaction called but in_transaction is false.");
        return;
    }
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
    const char* sql = "SELECT path, type, w, h, duration, exif, bad, last_shown, timestamp, is_camera, creation_time, preprocessed FROM cache LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        ok = true;
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    return ok;
}

void CacheManager::seed_error_catalog() {
    if (!db) return;
    std::lock_guard<std::mutex> lk(db_mutex);

    char* err = nullptr;
    if (sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS error_catalog ("
        "code TEXT PRIMARY KEY, title TEXT, description TEXT, recovery TEXT"
        ");", nullptr, nullptr, &err) != SQLITE_OK) {
        g_logger.error("Failed to create error_catalog table: %s", err ? err : "unknown");
        if (err) sqlite3_free(err);
        return;
    }

    auto seeds = get_all_error_seeds();

    for (const auto& seed : seeds) {
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "INSERT OR REPLACE INTO error_catalog (code, title, description, recovery) VALUES (?, ?, ?, ?);";
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, seed.code.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, seed.title.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 3, seed.desc.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 4, seed.rec.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
}

bool CacheManager::get_error_details(const std::string& code, std::string& title, std::string& desc, std::string& recovery) {
    if (!db) return false;
    std::lock_guard<std::mutex> lk(db_mutex);

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT title, description, recovery FROM error_catalog WHERE code = ?;";
    bool found = false;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, code.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* d = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* r = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            if (t) title = t;
            if (d) desc = d;
            if (r) recovery = r;
            found = true;
        }
        sqlite3_finalize(stmt);
    }
    return found;
}

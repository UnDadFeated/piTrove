#ifndef PITROVE_CACHE_H
#define PITROVE_CACHE_H

#include <string>
#include <mutex>
#include <atomic>
#include <sqlite3.h>
#include "media_item.h"

struct CacheManager {
    sqlite3* db{nullptr};
    std::mutex db_mutex;
    sqlite3_stmt* stmt_upsert{nullptr};
    sqlite3_stmt* stmt_load{nullptr};
    sqlite3_stmt* stmt_mark{nullptr};
    std::atomic<bool> in_transaction{false};

    bool open(const std::string& dir);
    void close();
    bool load_cached(MediaItem& mi);
    void upsert(const MediaItem& mi, int bad);
    void mark_shown(const std::string& path);
    void mark_bad(const std::string& filepath);
    void begin_transaction();
    void commit_transaction();
    void reset_all_cooldowns();
    void seed_error_catalog();
    bool get_error_details(const std::string& code, std::string& title, std::string& desc, std::string& recovery);
    ~CacheManager();
};

// Verify database integrity before use (legacy pattern)
bool verify_database(const std::string& path);

extern CacheManager* g_cache;

#endif // PITROVE_CACHE_H

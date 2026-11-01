#include "font_render.h"
#include "cache.h"
#include "scanner.h"
#include "util.h"
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <latch>
#include <stdexcept>
#include <cstring>

static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

// Model blocked CIFS syscalls without touching the network or waiting 120 seconds.
static std::mutex io_mutex;
static std::condition_variable io_cv;
static bool release_io = false;
static int entered_io = 0;
extern "C" int __real_stat(const char*, struct stat*);
extern "C" int __wrap_stat(const char* path, struct stat* result) {
    if (std::strcmp(path, "/blocked-cifs-test") != 0) return __real_stat(path, result);
    std::unique_lock lock(io_mutex);
    ++entered_io;
    io_cv.notify_all();
    io_cv.wait(lock, [] { return release_io; });
    std::memset(result, 0, sizeof(*result));
    result->st_mode = S_IFREG | 0644;
    return 0;
}

static void test_fonts(const std::string& path) {
    FontRenderer fonts(nullptr);
    auto pinned = fonts.load_font(path, 14);
    std::weak_ptr<FontHandle> retired = pinned;
    check(fonts.load_font(path, 14) == pinned, "font cache did not reuse active key");
    // Exceed both the former 32-entry cache and 64-entry retired-handle buffer.
    for (int size = 20; size < 180; ++size) {
        auto transient = fonts.load_font(path, size);
        int w = 0, h = 0;
        fonts.measure(*pinned, "Still alive", w, h);
        check(w > 0 && h > 0, "font stopped measuring after cache eviction");
        check(pinned->path == path && pinned->font, "pinned font was invalidated");
    }
    pinned.reset();
    check(retired.expired(), "evicted font leaked after its last owner released it");
    // Several local fonts must remain usable while more loads evict their keys.
    auto title = fonts.load_font(path, 13);
    auto body = fonts.load_font(path, 15);
    for (int size = 180; size < 220; ++size) fonts.load_font(path, size);
    int w = 0, h = 0;
    fonts.measure(*title, "Title", w, h);
    check(w > 0, "local title font was invalidated by another load");
    fonts.measure(*body, "Body", w, h);
    check(w > 0, "local body font was invalidated by another load");
}

static void test_scanner() {
    struct stat st{};
    check(stat_timeout(".", st, 1000) && S_ISDIR(st.st_mode), "normal stat failed");
    check(!stat_timeout("/pitrove-no-such-test-path", st, 1000), "missing stat succeeded");
    check(!read_dir_timeout(".", 1000).empty(), "normal directory read failed");
    const int cores = std::max(1U, std::thread::hardware_concurrency());
    const int limit = std::clamp(cores * 4, 8, 64);
    std::latch start(1);
    std::atomic<int> unexpectedly_completed{0};
    std::vector<std::jthread> callers;
    for (int i = 0; i < limit * 2; ++i) {
        callers.emplace_back([&] {
            start.wait();
            struct stat result{};
            if (stat_timeout("/blocked-cifs-test", result, 30)) ++unexpectedly_completed;
        });
    }
    start.count_down();
    callers.clear();
    check(unexpectedly_completed == 0, "blocked I/O unexpectedly completed");
    check(g_scanner_detached_threads == limit, "thread admission limit was exceeded or lost");
    check(!stat_timeout(".", st, 1000), "stat admitted work despite exhausted slots");
    check(read_dir_timeout(".", 1000).empty(), "directory read bypassed shared limit");
    {
        std::unique_lock lock(io_mutex);
        check(io_cv.wait_for(lock, std::chrono::seconds(5), [&] { return entered_io == limit; }),
              "reserved workers failed to reach blocked syscall");
        release_io = true;
    }
    io_cv.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (g_scanner_detached_threads != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check(g_scanner_detached_threads == 0, "completed workers did not return their slots");
    check(stat_timeout(".", st, 1000), "scanner did not recover when blocked I/O returned");
}

static void sql(sqlite3* db, const char* statement) {
    check(sqlite3_exec(db, statement, nullptr, nullptr, nullptr) == SQLITE_OK, sqlite3_errmsg(db));
}
static int row_count(sqlite3* db) {
    sqlite3_stmt* query = nullptr;
    check(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM cache", -1, &query, nullptr) == SQLITE_OK,
          "count prepare failed");
    check(sqlite3_step(query) == SQLITE_ROW, "count query failed");
    int count = sqlite3_column_int(query, 0);
    sqlite3_finalize(query);
    return count;
}
static void test_cache() {
    CacheManager cache;
    check(sqlite3_open(":memory:", &cache.db) == SQLITE_OK, "in-memory cache open failed");
    sql(cache.db, "CREATE TABLE cache(path TEXT PRIMARY KEY); INSERT INTO cache VALUES('keep'),('old')");
    MediaItem item;
    item.path = "keep";
    const std::vector<MediaItem> live{item};
    check(cache.prune_missing({}) == 0 && row_count(cache.db) == 2, "empty scan pruned cache");
    // Abort an INSERT into the temporary live set. No cache row may be deleted.
    sql(cache.db, "CREATE TEMP TABLE live_paths(path TEXT PRIMARY KEY); "
                  "CREATE TEMP TRIGGER fail_live BEFORE INSERT ON live_paths "
                  "BEGIN SELECT RAISE(ABORT, 'injected insertion failure'); END");
    check(cache.prune_missing(live) == 0 && row_count(cache.db) == 2, "failed insertion lost cache rows");
    check(sqlite3_get_autocommit(cache.db), "failed prune left a transaction open");
    sql(cache.db, "DROP TRIGGER fail_live; BEGIN");
    check(cache.prune_missing(live) == 1 && row_count(cache.db) == 1, "normal pruning failed");
    check(!sqlite3_get_autocommit(cache.db), "prune committed its caller's transaction");
    sql(cache.db, "ROLLBACK");
    check(row_count(cache.db) == 2, "outer rollback did not restore pruned rows");
    check(cache.prune_missing(live) == 1 && row_count(cache.db) == 1, "standalone pruning failed");
}

static void test_preprocessed_marker() {
    CacheManager cache;
    check(sqlite3_open(":memory:", &cache.db) == SQLITE_OK, "marker cache open failed");
    sql(cache.db, "CREATE TABLE cache(path TEXT PRIMARY KEY, w INT, h INT, preprocessed INT); "
                  "INSERT INTO cache VALUES('known',1920,1080,0)");
    check(cache.mark_preprocessed("known"), "metadata completion marker failed");
    sqlite3_stmt* query = nullptr;
    check(sqlite3_prepare_v2(cache.db, "SELECT w,h,preprocessed FROM cache WHERE path='known'", -1, &query, nullptr) == SQLITE_OK, "marker query failed");
    check(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 1920 &&
          sqlite3_column_int(query, 1) == 1080 && sqlite3_column_int(query, 2) == 1,
          "failed metadata extraction erased known dimensions");
    sqlite3_finalize(query);
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected font path");
        g_logger.level = LogLevel::ERROR;
        check(SDL_Init(0), SDL_GetError());
        test_fonts(argv[1]);
        test_scanner();
        test_cache();
        test_preprocessed_marker();
        SDL_Quit();
        std::cout << "font eviction, scanner admission, and cache rollback regressions passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

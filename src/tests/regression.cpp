#include "font_render.h"
#include "infopanel_render.h"
#include "renderer.h"
#include "cache.h"
#include "scanner.h"
#include "mqtt.h"
#include "config.h"
#include "util.h"
#include <condition_variable>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <latch>
#include <stdexcept>
#include <cstring>
#include <mosquitto.h>
#include <arpa/inet.h>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
extern char** environ;

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

static size_t rasterized_textures = 0;
extern "C" SDL_Surface* __real_TTF_RenderText_Blended(TTF_Font*, const char*, size_t, SDL_Color);
extern "C" SDL_Surface* __wrap_TTF_RenderText_Blended(TTF_Font* font, const char* text, size_t length, SDL_Color color) {
    ++rasterized_textures;
    return __real_TTF_RenderText_Blended(font, text, length, color);
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

static void test_fitted_panel_text(const std::string& path) {
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> surface(
        SDL_CreateSurface(360, 180, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
    check(bool(surface), SDL_GetError());
    std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
        SDL_CreateSoftwareRenderer(surface.get()), SDL_DestroyRenderer);
    check(bool(renderer), SDL_GetError());
    Renderer render_target;
    render_target.sdl_renderer = renderer.get();
    FontRenderer fonts(&render_target);
    const auto clock = fonts.load_font(path, 88);
    const auto body = fonts.load_font(path, panel_body_size);

    for (const std::string time : {"9:59 AM", "10:00 AM", "12:00 PM"}) {
        SDL_SetRenderDrawColor(renderer.get(), 0, 0, 0, 255);
        SDL_RenderClear(renderer.get());
        const SDL_Rect box{37, 15, 259, 130};
        const int height = fonts.draw_text_fitted(box, *clock, time, {255, 255, 255, 255});
        check(height > 28 && height <= box.h, "clock did not use the available space");
        SDL_RenderPresent(renderer.get());

        int left = surface->w, right = -1;
        for (int y = 0; y < surface->h; ++y) {
            for (int x = 0; x < surface->w; ++x) {
                Uint8 r, g, b, a;
                check(SDL_ReadSurfacePixel(surface.get(), x, y, &r, &g, &b, &a), SDL_GetError());
                if (r < 128) continue;
                check(x >= box.x && x < box.x + box.w && y >= box.y && y < box.y + box.h,
                      "fitted clock drew outside the column");
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
        check(right - left > box.w * 9 / 10, "clock did not fill the column width");
        check(std::abs(left + right + 1 - (2 * box.x + box.w)) <= 9,
              "clock ink is visibly off center");
    }

    const auto draw = [&](const std::string& text) {
        fonts.draw_text(0, 0, *body, text, 255, 255, 255, 255);
    };
    const size_t before = rasterized_textures;
    for (int i = 0; i < 256; ++i) draw("Ticker " + std::to_string(i));
    check(rasterized_textures == before + 256, "new ticker text was not rasterized");
    draw("Ticker 0");  // Keep the oldest inserted entry in current use.
    draw("Ticker 256");
    draw("Ticker 0");
    check(rasterized_textures == before + 257, "active ticker text was evicted");
    draw("Ticker 1");
    check(rasterized_textures == before + 258, "old ticker text was not evicted");

    const int short_width = 57;
    const auto shortened = fonts.ellipsize(*body, "ZoëZoëZoëZoëZoë", short_width);
    int measured, height;
    fonts.measure(*body, shortened, measured, height);
    check(shortened.ends_with("…") && measured <= short_width, "long Unicode label did not fit");
    check(fonts.ellipsize(*body, "Zoë", 100) == "Zoë", "short label was changed");

    const SDL_Rect original{30, 10, 80, 80};
    SDL_SetRenderClipRect(renderer.get(), &original);
    {
        const InfopanelClip no_overlap(renderer.get(), {150, 10, 30, 50});
        check(!no_overlap, "non-overlapping clip entered panel renderer");
        const InfopanelClip overlap(renderer.get(), {40, 20, 100, 40});
        check(bool(overlap), "overlap clip was not set");
        SDL_Rect clipped{};
        SDL_GetRenderClipRect(renderer.get(), &clipped);
        check(clipped.x == 40 && clipped.y == 20 && clipped.w == 70 && clipped.h == 40,
              "panel clip ignored caller bounds");
    }
    SDL_Rect restored{};
    SDL_GetRenderClipRect(renderer.get(), &restored);
    check(SDL_RectsEqual(&original, &restored), "panel failed to restore caller clip");
}

static void test_scanner() {
    struct stat st{};
    check(stat_timeout(".", st, 1000) && S_ISDIR(st.st_mode), "normal stat failed");
    check(!stat_timeout("/pitrove-no-such-test-path", st, 1000), "missing stat succeeded");
    const auto entries = read_dir_timeout(".", 1000);
    check(entries && !entries->empty(), "normal directory read failed");
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
    check(!read_dir_timeout(".", 1000), "directory read bypassed shared limit");
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

static void test_scan_result() {
    namespace fs = std::filesystem;
    char path[] = "/tmp/pitrove-scan-XXXXXX";
    check(mkdtemp(path) != nullptr, "scan fixture directory creation failed");
    const fs::path root(path);
    struct Cleanup { fs::path root; ~Cleanup() { fs::remove_all(root); } } cleanup{root};
    const auto empty = scan_directory(path, 10);
    check(empty.complete && empty.items.empty(), "readable empty directory is a complete scan");
    const auto missing = scan_directory((root / "absent").string(), 10);
    check(!missing.complete, "missing directory reported a complete scan");
    std::ofstream(root / "good.JPG").put('x');
    std::ofstream(root / "secret.txt") << "secret";
    fs::create_symlink(root / "secret.txt", root / "escape.jpg");
    fs::create_symlink(root / "absent", root / "disappeared");
    const auto filtered = scan_directory(path, 10);
    check(filtered.complete && filtered.items.size() == 1 && filtered.items[0].filename == "good.JPG",
          "scanner must ignore symlinks without failing the scan");
    fs::remove(root / "escape.jpg");
    fs::remove(root / "disappeared");
    const auto recovered = scan_directory(path, 10);
    check(recovered.complete && recovered.items.size() == 1, "recovered scan did not complete");
}

static void test_scan_cancel() {
    {
        std::lock_guard lock(io_mutex);
        entered_io = 0;
        release_io = false;
    }
    bool stat_succeeded = false;
    std::jthread caller([&](std::stop_token stop) {
        struct stat result{};
        stat_succeeded = stat_timeout("/blocked-cifs-test", result, 4000, stop);
    });
    bool entered;
    {
        std::unique_lock lock(io_mutex);
        entered = io_cv.wait_for(lock, std::chrono::seconds(1), [] { return entered_io == 1; });
    }
    const auto started = std::chrono::steady_clock::now();
    caller.request_stop();
    caller.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const int still_blocked = g_scanner_detached_threads.load();
    {
        std::lock_guard lock(io_mutex);
        release_io = true;
    }
    io_cv.notify_all();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (g_scanner_detached_threads != 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    check(entered && !stat_succeeded && elapsed < std::chrono::seconds(1),
          "cancelled scan coordinator waited for the NAS syscall");
    check(still_blocked == 1 && g_scanner_detached_threads == 0,
          "cancelling a wait lost the blocked worker's slot");
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
    sql(cache.db, "CREATE TABLE cache(path TEXT PRIMARY KEY, w INT, h INT, preprocessed INT, bad INT); "
                  "INSERT INTO cache VALUES('known',1920,1080,0,0),('broken',1280,720,0,0); "
                  "CREATE TRIGGER reject_marker BEFORE UPDATE ON cache WHEN old.path = 'broken' "
                  "BEGIN SELECT RAISE(ABORT, 'injected batch failure'); END");
    const std::vector<std::string> rejected{"known", "broken"};
    check(!cache.upsert_batch({}, 1, rejected), "failing batch reported success");
    check(sqlite3_get_autocommit(cache.db), "failing batch left a transaction open");
    sqlite3_stmt* rolled_back = nullptr;
    check(sqlite3_prepare_v2(cache.db, "SELECT SUM(preprocessed) FROM cache", -1, &rolled_back, nullptr) == SQLITE_OK &&
          sqlite3_step(rolled_back) == SQLITE_ROW && sqlite3_column_int(rolled_back, 0) == 0,
          "failing batch partially changed completion markers");
    sqlite3_finalize(rolled_back);
    const std::vector<std::string> completed{"known"};
    check(cache.upsert_batch({}, 1, completed), "metadata completion marker failed");
    sqlite3_stmt* query = nullptr;
    check(sqlite3_prepare_v2(cache.db, "SELECT w,h,preprocessed FROM cache WHERE path='known'", -1, &query, nullptr) == SQLITE_OK, "marker query failed");
    check(sqlite3_step(query) == SQLITE_ROW && sqlite3_column_int(query, 0) == 1920 &&
          sqlite3_column_int(query, 1) == 1080 && sqlite3_column_int(query, 2) == 1,
          "failed metadata extraction erased known dimensions");
    sqlite3_finalize(query);
}

static void test_cache_migration() {
    namespace fs = std::filesystem;
    char path[] = "/tmp/pitrove-cache-XXXXXX";
    check(mkdtemp(path) != nullptr, "cache fixture directory creation failed");
    const fs::path root(path);
    struct Cleanup { fs::path root; ~Cleanup() { fs::remove_all(root); } } cleanup{root};
    auto database = (root / "cache.db").string();

    sqlite3* legacy = nullptr;
    check(sqlite3_open(database.c_str(), &legacy) == SQLITE_OK, "legacy database open failed");
    sql(legacy, "CREATE TABLE cache(path TEXT PRIMARY KEY, type TEXT, w INT, h INT, "
                "duration REAL, exif INT, last_shown INTEGER, timestamp INTEGER); "
                "INSERT INTO cache VALUES('clip','video',1280,720,5,1,200,100); BEGIN EXCLUSIVE");
    CacheManager cache;
    const bool opened_while_locked = cache.open(path);
    sql(legacy, "ROLLBACK");
    sqlite3_close(legacy);
    check(!opened_while_locked, "exclusive SQLite lock was not observed");
    check(cache.open(path), "legacy database did not migrate");

    MediaItem original;
    original.path = "clip";
    original.modified_time = 100;
    check(cache.load_cached(original) && original.width == 1280 && original.last_shown == 200,
          "legacy metadata or cooldown was lost");
    MediaItem replaced;
    replaced.path = original.path;
    replaced.modified_time = 300;
    check(!cache.load_cached(replaced) && replaced.width == 0 && replaced.last_shown == 200,
          "replacement media reused stale dimensions or lost cooldown");
    original.modified_time = 0;
    cache.upsert(original, 0, 1);
    MediaItem from_cache;
    from_cache.path = original.path;
    check(cache.load_cached(from_cache) && from_cache.modified_time == 100,
          "upsert with unknown timestamp erased cached modification time");

    cache.mark_shown(original.path);
    check(cache.load_cached(from_cache) && from_cache.last_shown > 200,
          "pending history was not visible to playlist lookup");
    const auto shown = from_cache.last_shown;
    const auto before = sqlite3_total_changes64(cache.db);
    cache.upsert(from_cache, 0, 1);
    check(sqlite3_total_changes64(cache.db) == before,
          "metadata update forced a write for a queued playback timestamp");
    cache.close();
    check(cache.open(path) && cache.load_cached(from_cache) && from_cache.last_shown == shown,
          "normal shutdown did not persist the buffered history");
}


static void test_mqtt_lifecycle() {
    const int probe = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(probe >= 0, "MQTT fixture socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    const bool bound = bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                       getsockname(probe, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    close(probe);
    check(bound, "MQTT fixture port");
    const int port = ntohs(address.sin_port);
    std::string port_text = std::to_string(port);
    char program[] = "mosquitto";
    char port_option[] = "-p";
    char* args[]{program, port_option, port_text.data(), nullptr};
    struct Broker {
        pid_t pid = 0;
        ~Broker() {
            stop_mqtt_client();
            if (pid > 0) {
                kill(pid, SIGTERM);
                waitpid(pid, nullptr, 0);
            }
        }
    } broker;
    check(posix_spawnp(&broker.pid, program, nullptr, nullptr, args, environ) == 0, "start fixture broker");
    const auto wait_for = [](auto condition) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < end) {
            if (condition()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        return false;
    };
    check(wait_for([&] {
        const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        const bool available = fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
        if (fd >= 0) close(fd);
        return available;
    }), "fixture broker unavailable");
    {
        std::lock_guard lock(g_config_mtx);
        g_cfg.mqtt_enabled = true;
        g_cfg.mqtt_broker = "127.0.0.1";
        g_cfg.mqtt_port = port;
        g_cfg.mqtt_tls_enabled = false;
        g_cfg.mqtt_topic_prefix = "fixture";
        g_cfg.mqtt_motionsensor_topic = "motion";
    }
    start_mqtt_client();
    struct Received { std::string state; std::string body; } received;
    const std::unique_ptr<mosquitto, decltype(&mosquitto_destroy)>
        observer(mosquitto_new(nullptr, true, &received), mosquitto_destroy);
    check(observer != nullptr, "fixture MQTT observer");
    mosquitto_message_callback_set(observer.get(), [](mosquitto*, void* context, const mosquitto_message* m) {
        auto& r = *static_cast<Received*>(context);
        const std::string_view topic(m->topic);
        if (topic == "fixture/status") r.state.assign(static_cast<const char*>(m->payload), m->payloadlen);
        else if (topic == "fixture/one") r.body.assign(static_cast<const char*>(m->payload), m->payloadlen);
    });
    check(mosquitto_connect(observer.get(), "127.0.0.1", port, 10) == MOSQ_ERR_SUCCESS, "observer connect");
    check(mosquitto_subscribe(observer.get(), nullptr, "fixture/#", 0) == MOSQ_ERR_SUCCESS, "observer subscribe");
    check(wait_for([&] {
        mosquitto_loop(observer.get(), 20, 1);
        return received.state == R"({"state":"online"})";
    }), "MQTT birth missing");
    const std::string body(100000, 'x');
    mqtt_publish("fixture/one", body, true);
    check(mosquitto_publish(observer.get(), nullptr, "fixture/command/next", 5, "PRESS", 0, false)
          == MOSQ_ERR_SUCCESS, "fixture command publish");
    check(wait_for([&] {
        mosquitto_loop(observer.get(), 20, 1);
        return received.body == body && g_remote_command.load() == 1;
    }), "MQTT publish or command failed");
    const auto started = std::chrono::steady_clock::now();
    stop_mqtt_client();
    check(std::chrono::steady_clock::now() - started < std::chrono::seconds(2),
          "MQTT delayed shutdown");
    check(wait_for([&] {
        mosquitto_loop(observer.get(), 20, 1);
        return received.state == R"({"state":"offline"})";
    }), "MQTT shutdown status missing");
    {
        std::lock_guard lock(g_config_mtx);
        g_cfg.mqtt_enabled = false;
    }
}


int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected font path");
        g_logger.level = LogLevel::ERROR;
        check(SDL_Init(0), SDL_GetError());
        test_fonts(argv[1]);
        test_fitted_panel_text(argv[1]);
        test_scanner();
        test_scan_result();
        test_scan_cancel();
        test_cache();
        test_cache_migration();
        test_preprocessed_marker();
        {
            // Upgraded cache.db with an older partial error_catalog must re-seed missing E527-E540 rows.
            char tmpl[] = "/tmp/pitrove-errcat-XXXXXX";
            char* dir = mkdtemp(tmpl);
            check(dir != nullptr, "errcat mkdtemp failed");
            const std::string db_path = std::string(dir) + "/cache.db";
            sqlite3* raw = nullptr;
            check(sqlite3_open(db_path.c_str(), &raw) == SQLITE_OK, "errcat raw open failed");
            check(sqlite3_exec(raw, "CREATE TABLE error_catalog(code TEXT PRIMARY KEY,title TEXT,description TEXT,recovery_action TEXT);"
                                    "INSERT INTO error_catalog VALUES('E100','Old','Old','Old');",
                               nullptr, nullptr, nullptr) == SQLITE_OK, "errcat seed setup failed");
            sqlite3_close(raw);
            CacheManager cm;
            check(cm.open(dir), "errcat reopen failed");
            std::string title, desc, rec;
            check(cm.get_error_details("E530", title, desc, rec) && !title.empty(), "E530 missing after catalog upgrade");
            check(cm.get_error_details("E540", title, desc, rec) && !title.empty(), "E540 missing after catalog upgrade");
            cm.close();
            std::filesystem::remove_all(dir);
        }
        test_mqtt_lifecycle();
        SDL_Quit();
        std::cout << "font eviction, scanner admission, and cache rollback regressions passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

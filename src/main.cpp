#include "util.h"
#include "config.h"
#include "cache.h"
#include "scanner.h"
#include "renderer.h"
#include "image_loader.h"
#include "font_render.h"
#include "transition.h"
#include "overlay.h"
#include "preload.h"
#include "mpv_player.h"

#include <SDL.h>
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <sys/file.h>
#include <unistd.h>
#include <csignal>
#include <algorithm>
#include <random>
#include <chrono>
#include <cmath>
#include <atomic>
#include <thread>
#include <mutex>
#include <future>
#include <functional>
#include <filesystem>

static TransitionEngine* g_transition = nullptr;
static OverlayManager* g_overlay = nullptr;

// Background Watchman & Dynamic Playlist State
static std::mutex g_playlist_mtx;
static std::vector<MediaItem> g_scanned_items;
static std::vector<MediaItem> g_eligible;
static int current_idx = 0;
static std::thread g_watchman_thread;
static std::atomic<bool> g_watchman_running{false};

static bool is_item_in_seasonal_window(const MediaItem& item, int window_days) {
    if (window_days <= 0) return true;

    // 1. Check parent folder name for month-spread filter
    std::filesystem::path p(item.path);
    std::string parent_name = p.parent_path().filename().string();
    
    int groups[2] = {0, 0};
    int gc = 0;
    size_t i = 0;
    while (i < parent_name.size() && gc < 2) {
        while (i < parent_name.size() && !isdigit(parent_name[i])) i++;
        if (i >= parent_name.size()) break;
        int val = 0;
        while (i < parent_name.size() && isdigit(parent_name[i])) {
            val = val * 10 + (parent_name[i] - '0');
            i++;
        }
        groups[gc++] = val;
        if (gc == 1 && i < parent_name.size() && (parent_name[i] == '-' || parent_name[i] == '_')) i++;
    }

    if (gc >= 2) {
        int folder_m = groups[1];
        if (folder_m >= 1 && folder_m <= 12) {
            time_t t = std::time(nullptr);
            struct tm tm_buf;
            struct tm* now = localtime_r(&t, &tm_buf);
            int curr_m = now->tm_mon + 1;
            
            int max_month_spread = std::ceil(window_days / 30.0);
            int diff = std::abs(curr_m - folder_m);
            if (diff > 6) diff = 12 - diff;
            
            if (diff > max_month_spread) {
                return false;
            }
        }
    }

    // 2. Check filename for seasonal window
    return is_in_seasonal_window(item.filename, window_days);
}

static std::vector<MediaItem> filter_playlist(const std::vector<MediaItem>& items, int cooldown_days, int window_days) {
    std::vector<MediaItem> filtered;
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    int64_t cutoff = now - (static_cast<int64_t>(cooldown_days) * 86400);
    
    for (const auto& item : items) {
        // 1. Cooldown filter
        if (item.last_shown >= cutoff) {
            continue;
        }
        // 2. Seasonal window filter
        if (!is_item_in_seasonal_window(item, window_days)) {
            continue;
        }
        filtered.push_back(item);
    }
    return filtered;
}

static void organize_playlist(std::vector<MediaItem>& eligible, int videos_per_photos, bool play_just_photos, bool play_just_videos, bool shuffle_enabled) {
    if (eligible.empty()) return;

    if (videos_per_photos <= 0 || play_just_photos) {
        std::vector<MediaItem> photos;
        for (auto& item : eligible) {
            if (item.type != "video") photos.push_back(std::move(item));
        }
        eligible = std::move(photos);
        if (shuffle_enabled) {
            unsigned long long seed = make_entropy_seed();
            std::mt19937_64 rng(seed);
            std::shuffle(eligible.begin(), eligible.end(), rng);
        }
    } else if (play_just_videos) {
        std::vector<MediaItem> videos;
        for (auto& item : eligible) {
            if (item.type == "video") videos.push_back(std::move(item));
        }
        eligible = std::move(videos);
        if (shuffle_enabled) {
            unsigned long long seed = make_entropy_seed();
            std::mt19937_64 rng(seed);
            std::shuffle(eligible.begin(), eligible.end(), rng);
        }
    } else {
        std::vector<MediaItem> photos, videos;
        for (auto& item : eligible) {
            if (item.type == "video") videos.push_back(std::move(item));
            else photos.push_back(std::move(item));
        }

        if (shuffle_enabled) {
            unsigned long long seed = make_entropy_seed();
            std::mt19937_64 rng_photo(seed);
            std::shuffle(photos.begin(), photos.end(), rng_photo);
            std::mt19937_64 rng_video(seed);
            std::shuffle(videos.begin(), videos.end(), rng_video);
        }

        int cycle_size = std::max(4, videos_per_photos);
        int photos_per_cycle = cycle_size - 3;
        
        eligible.clear();
        size_t p_idx = 0, v_idx = 0;
        while (p_idx < photos.size() || v_idx < videos.size()) {
            for (int i = 0; i < photos_per_cycle && p_idx < photos.size(); i++) {
                eligible.push_back(std::move(photos[p_idx++]));
            }
            for (int i = 0; i < 3 && v_idx < videos.size(); i++) {
                eligible.push_back(std::move(videos[v_idx++]));
            }
        }

        g_logger.info("Playlist organized: %zu photos + %zu videos = %zu total (ratio: 3 videos per %d cycle)",
            photos.size(), videos.size(), eligible.size(), cycle_size);
    }
}

static void watchman_loop() {
    g_logger.info("Watchman: Background watchman thread started.");
    
    // Get current day of year
    time_t last_check_time = std::time(nullptr);
    struct tm tm_buf;
    struct tm* last_check_tm = localtime_r(&last_check_time, &tm_buf);
    int last_yday = last_check_tm->tm_yday;
    
    while (g_watchman_running.load()) {
        // Sleep for 10 seconds between checks (quick responsive exit checks)
        for (int i = 0; i < 10 && g_watchman_running.load(); i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!g_watchman_running.load()) break;
        
        time_t now = std::time(nullptr);
        struct tm curr_tm;
        localtime_r(&now, &curr_tm);
        
        if (curr_tm.tm_yday != last_yday) {
            g_logger.info("Watchman: Midnight detected! Shifting temporal window. Old day=%d, New day=%d", last_yday, curr_tm.tm_yday);
            last_yday = curr_tm.tm_yday;
            
            // Re-filter playlist
            int cooldown_days = 0;
            int window_days = 0;
            bool shuffle_enabled = true;
            {
                std::lock_guard<std::mutex> lock(g_config_mtx);
                cooldown_days = g_cfg.cooldown_days;
                window_days = g_cfg.scan_window_days;
                shuffle_enabled = g_cfg.shuffle;
            }
            
            std::vector<MediaItem> new_eligible = filter_playlist(g_scanned_items, cooldown_days, window_days);
            g_logger.info("Watchman: New seasonal window calculation: %zu / %zu items eligible", new_eligible.size(), g_scanned_items.size());
            
            if (!new_eligible.empty()) {
                bool play_just_photos = false;
                bool play_just_videos = false;
                int videos_per_photos = 10;
                {
                    std::lock_guard<std::mutex> lock(g_config_mtx);
                    play_just_photos = g_cfg.play_just_photos;
                    play_just_videos = g_cfg.play_just_videos;
                    videos_per_photos = g_cfg.videos_per_photos;
                }
                organize_playlist(new_eligible, videos_per_photos, play_just_photos, play_just_videos, shuffle_enabled);
                
                // Swap seamlessly under mutex
                {
                    std::lock_guard<std::mutex> playlist_lock(g_playlist_mtx);
                    
                    // Try to preserve current playing item
                    std::string current_path = "";
                    if (current_idx >= 0 && current_idx < (int)g_eligible.size()) {
                        current_path = g_eligible[current_idx].path;
                    }
                    
                    g_eligible = std::move(new_eligible);
                    
                    // Find if current path is in new playlist
                    int new_idx = 0;
                    if (!current_path.empty()) {
                        for (int idx = 0; idx < (int)g_eligible.size(); idx++) {
                            if (g_eligible[idx].path == current_path) {
                                new_idx = idx;
                                break;
                            }
                        }
                    }
                    current_idx = new_idx;
                    g_logger.info("Watchman: Playlist swapped seamlessly. New size=%zu, current_idx=%d", g_eligible.size(), current_idx);
                }
            } else {
                g_logger.warn("Watchman: New playlist is empty. Keeping old playlist to prevent interruption.");
            }
        }
    }
    g_logger.info("Watchman: Background watchman thread exiting.");
}

static void draw_phase_splash(int phase, int progress, int total, int done, const char* label, int dot_counter, const char* filename = nullptr, bool animated = true) {
    g_renderer.render_splash(phase, progress, total, done, label, dot_counter, filename, animated);
}


int main(int argc, char** argv) {
    // --- Single-instance lock ---
    std::string lock_path = get_exe_dir() + "/piTrove.lock";
    int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0) { fprintf(stderr, "Failed to open lock file\n"); return 1; }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "Another instance is already running\n");
        close(lock_fd); return 1;
    }

    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGTERM, crash_handler);
    std::set_terminate(terminate_handler);

    g_logger.info("=== piTrove v%s started %s ===", VERSION, get_timestamp().c_str());

    // --- Config ---
    g_cfg.parse_args(argc, argv);
    std::string config_path;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) config_path = argv[++i];
    }
    if (config_path.empty()) {
        const char* candidates[] = {"config/config.toml", "src/config/config.toml",
            "/home/pi/piTrove/src/config/config.toml", "./src/config/config.toml"};
        for (const auto& c : candidates) {
            if (file_exists(c)) { config_path = c; break; }
        }
    }
    if (!config_path.empty()) {
        g_logger.info("Loading config: %s", config_path.c_str());
        if (!g_cfg.load(config_path)) g_logger.warn("Failed to load config from %s, using defaults", config_path.c_str());
    } else {
        g_logger.warn("No config file found, using defaults");
    }

    std::string media_dir = g_cfg.media_dir;
    std::string cache_dir = g_cfg.cache_dir.empty() ? (get_exe_dir() + "/cache") : g_cfg.cache_dir;
    std::string log_dir = g_cfg.log_dir.empty() ? (get_exe_dir() + "/logs") : g_cfg.log_dir;
    g_crash_cache_dir = cache_dir;

    g_logger.init(log_dir, LogLevel::DEBUG);
    g_logger.info("Media dir: %s, Cache dir: %s", media_dir.c_str(), cache_dir.c_str());

    // --- SDL2 Init ---
    g_logger.info("Initializing SDL2 (%dx%d)...", g_cfg.screen_w, g_cfg.screen_h);
    std::string splash_file = g_cfg.splash_file.empty() ? "src/splash.png" : g_cfg.splash_file;

    if (!g_renderer.init(g_cfg.screen_w, g_cfg.screen_h, g_cfg.fullscreen)) {
        g_logger.error("SDL2 initialization failed"); return 1;
    }
    g_logger.info("SDL2 context created: %dx%d", g_renderer.screen_w, g_renderer.screen_h);

    // Render 3 black frames to initialize page flipping under KMSDRM before any textures are created
    g_logger.info("Initializing EGL page flipping sweeps...");
    for (int i = 0; i < 3; i++) {
        g_renderer.clear(0, 0, 0, 255);
        g_renderer.present();
        SDL_Delay(16);
    }

    // --- Splash ---
    g_logger.info("Loading splash screen...");
    g_renderer.load_splash(splash_file);
    g_logger.info("Splash loaded");

    // Render initial splash immediately
    for (int i = 0; i < 3; i++) {
        draw_phase_splash(2, 0, 0, 0, "INIT", 0, nullptr, false);
        SDL_Delay(16);
    }

    // --- Fast-path: skip scan+cache if DB already exists ---
    std::string db_path = cache_dir + "/cache.db";
    struct stat db_stat{};
    bool db_exists = (stat(db_path.c_str(), &db_stat) == 0 && db_stat.st_size > 0);

    if (db_exists) {
        bool db_ok = verify_database(db_path);
        if (!db_ok) {
            g_logger.error("CORRUPT DB: cache.db is corrupted — removing and will rebuild");
            std::filesystem::remove(db_path);
            db_exists = false;
        }
    }
    if (db_exists) {
        CacheManager* fast_cache = new CacheManager();
        if (fast_cache->open(cache_dir)) {
            g_cache = fast_cache;
            sqlite3_stmt* stmt = nullptr;
            int load_rc = sqlite3_prepare_v2(fast_cache->db,
                "SELECT path, type, w, h, duration, exif, last_shown FROM cache WHERE bad = 0;",
                -1, &stmt, nullptr);
            if (load_rc == SQLITE_OK) {
                int row_count = 0;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    row_count++;
                    const unsigned char* raw_path = sqlite3_column_text(stmt, 0);
                    const unsigned char* raw_type = sqlite3_column_text(stmt, 1);
                    if (!raw_path || !raw_type) continue;
                    MediaItem mi;
                    mi.path = (const char*)raw_path;
                    mi.type = (const char*)raw_type;
                    mi.width = sqlite3_column_int(stmt, 2);
                    mi.height = sqlite3_column_int(stmt, 3);
                    mi.duration = sqlite3_column_double(stmt, 4);
                    mi.exif_rotation = sqlite3_column_int(stmt, 5);
                    mi.last_shown = sqlite3_column_int64(stmt, 6);
                    auto slash = mi.path.rfind('/');
                    mi.filename = (slash != std::string::npos) ? mi.path.substr(slash + 1) : mi.path;
                    auto dot = mi.filename.find_last_of('.');
                    mi.ext = (dot != std::string::npos && dot < mi.filename.size() - 1) ? mi.filename.substr(dot + 1) : "";
                    mi.cached = true;
                    g_scanned_items.push_back(mi);
                }
            }
            sqlite3_finalize(stmt);

            int photos = std::count_if(g_scanned_items.begin(), g_scanned_items.end(), [](const MediaItem& i){ return i.type == "image"; });
            int videos = std::count_if(g_scanned_items.begin(), g_scanned_items.end(), [](const MediaItem& i){ return i.type == "video"; });
            g_logger.info("Loaded %d items from cache DB (photos=%d videos=%d)", (int)g_scanned_items.size(), photos, videos);

            if (photos > 0 || videos > 0) {
                g_logger.info("Fast-path: skipping scan, going directly to slideshow");
                g_database_complete.store(true);
            } else {
                g_logger.warn("Cache DB loaded 0 valid items — will re-scan");
            }
        } else {
            g_logger.warn("Failed to open cache DB — will re-scan");
        }
        if (g_cache != fast_cache) {
            delete fast_cache;
        }
    }

    // --- PHASE 1: SCAN (simple filesystem walk, like legacy) ---
    // (skip if fast-path cache was valid)
    bool do_scan = g_scanned_items.empty();
    int dot_counter = 0;

    if (do_scan) {
        g_logger.info("Phase 1: Scanning media...");
        auto scan_start = std::chrono::steady_clock::now();
        draw_phase_splash(2, 0, 0, 0, "SCANNING", 0, nullptr, false);

        std::atomic<int64_t> scan_count{0};
        std::atomic<bool> scan_done{false};

        // Thread-safe progress callback updating only the atomic counter
        auto safe_progress_callback = [&](int count) {
            scan_count.store(count, std::memory_order_relaxed);
        };

        int depth = 10;
        { std::lock_guard<std::mutex> lk(g_config_mtx); depth = g_cfg.scan_depth; }

        std::thread scan_thread([&]() {
            scan_directory(media_dir, depth, g_scanned_items, scan_count, safe_progress_callback);
            scan_done.store(true);
        });

        // Main thread polls and renders splash safely on EGL context
        while (!scan_done.load()) {
            dot_counter++;
            draw_phase_splash(2, (int)scan_count.load(), 0, 0, "SCANNING", dot_counter, nullptr, false);
            SDL_Delay(33); // ~30 FPS throttling
        }

        if (scan_thread.joinable()) {
            scan_thread.join();
        }

        auto scan_end = std::chrono::steady_clock::now();
        auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(scan_end - scan_start).count();
        g_logger.info("Scan complete: %d items in %ld ms", (int)g_scanned_items.size(), (long)scan_ms);
    }

    // --- PHASE 2: CACHE (bulk transaction, render at 0.1s intervals) ---
    if (do_scan) {
        g_logger.info("Phase 2: Caching metadata...");

        if (!g_cache) {
            g_cache = new CacheManager();
            if (!g_cache->open(cache_dir)) {
                g_logger.error("Failed to open cache database");
                g_renderer.cleanup_splash(); g_renderer.cleanup();
                delete g_cache; return 1;
            }
        }

        auto get_display_path = [](const std::string& path) -> std::string {
            if (path.empty()) return "./";
            int slashes = 0;
            for (int i = (int)path.length() - 1; i >= 0; i--) {
                if (path[i] == '/') {
                    if (++slashes == 3) return "." + path.substr(i);
                }
            }
            return (path.front() != '/') ? ("./" + path) : ("." + path);
        };

        g_cache->begin_transaction();
        int cached = 0;
        auto last_render = std::chrono::steady_clock::now();

        int total_scanned = (int)g_scanned_items.size();
        draw_phase_splash(3, total_scanned, total_scanned, 0, "CACHING", ++dot_counter, nullptr, false);

        for (int i = 0; i < total_scanned; i++) {
            auto& mi = g_scanned_items[i];

            if (g_cache->load_cached(mi)) {
                mi.cached = true;
                cached++;
            } else {
                if (mi.type == "image") {
                    mi.exif_rotation = 1;
                    mi.width = 1920; mi.height = 1080;
                } else {
                    mi.width = g_cfg.screen_w;
                    mi.height = g_cfg.screen_h;
                    mi.duration = 0.0;
                }
                g_cache->upsert(mi, 0);
                cached++;
            }

            auto render_now = std::chrono::steady_clock::now();
            if (std::chrono::duration<float>(render_now - last_render).count() >= 0.1f) {
                dot_counter++;
                draw_phase_splash(3, total_scanned, total_scanned, cached, "CACHING",
                    dot_counter, get_display_path(mi.path).c_str(), false);
                last_render = render_now;
            }
        }
        g_cache->commit_transaction();
        g_logger.info("Cache complete: %d items", cached);
        g_database_complete.store(true);
    }

    // --- Dynamic Seasonal & Cooldown filter ---
    g_eligible = filter_playlist(g_scanned_items, g_cfg.cooldown_days, g_cfg.scan_window_days);
    g_logger.info("Initial Dynamic Playlist Setup: %d items eligible (cooldown=%d days, seasonal window=%d days)",
        (int)g_eligible.size(), g_cfg.cooldown_days, g_cfg.scan_window_days);

    if (g_eligible.empty()) {
        g_logger.warn("No eligible items after dynamic filters");
        draw_phase_splash(4, 0, 0, 0, "DONE", ++dot_counter, nullptr, false);
        SDL_Delay(3000);
        g_renderer.cleanup_splash(); g_renderer.cleanup();
        delete g_cache;
        flock(lock_fd, LOCK_UN); close(lock_fd);
        return 0;
    }

    // --- Organize playlist (shuffle and interleave photos & videos per configuration) ---
    {
        bool play_just_photos = false;
        bool play_just_videos = false;
        int videos_per_photos = 10;
        bool shuffle_enabled = true;
        {
            std::lock_guard<std::mutex> lock(g_config_mtx);
            play_just_photos = g_cfg.play_just_photos;
            play_just_videos = g_cfg.play_just_videos;
            videos_per_photos = g_cfg.videos_per_photos;
            shuffle_enabled = g_cfg.shuffle;
        }
        organize_playlist(g_eligible, videos_per_photos, play_just_photos, play_just_videos, shuffle_enabled);
    }

    g_logger.info("Starting slideshow with %zu items", g_eligible.size());

    g_transition = new TransitionEngine();
    g_transition->set_renderer(&g_renderer);
    g_overlay = new OverlayManager(&g_renderer);
    g_overlay->init();
    g_logger.info("Starting slideshow loop with %zu items", g_eligible.size());

    // --- Main slideshow loop setup ---
    current_idx = 0;
    double item_timer = 0.0;
    auto last_frame_time = std::chrono::steady_clock::now();

    std::shared_ptr<ImageData> current_data = nullptr;
    std::shared_ptr<ImageData> next_data = nullptr;
    SDL_Texture* current_tex = nullptr;
    SDL_Rect fit_rect{0, 0, 0, 0};

    // Find the first valid item to display
    int load_attempts = 0;
    while (load_attempts < (int)g_eligible.size() && load_attempts < 20) {
        if (g_eligible[current_idx].type == "video") {
            // First item is a video, let the main loop play it!
            g_logger.info("First item in playlist is a video: %s", g_eligible[current_idx].path.c_str());
            break;
        } else {
            // First item is an image, load it!
            current_data = ImageLoader::load(g_eligible[current_idx].path);
            if (current_data && current_data->valid) {
                ImageLoader::load_texture(current_data.get(), g_renderer.sdl_renderer);
                current_tex = current_data->texture;
                g_cache->mark_shown(g_eligible[current_idx].path);

                // Update in-memory item metadata with actual dimensions and EXIF rotation
                g_eligible[current_idx].width = current_data->width;
                g_eligible[current_idx].height = current_data->height;
                g_eligible[current_idx].exif_rotation = current_data->exif_rotation;
                for (auto& item : g_scanned_items) {
                    if (item.path == g_eligible[current_idx].path) {
                        item.width = current_data->width;
                        item.height = current_data->height;
                        item.exif_rotation = current_data->exif_rotation;
                        break;
                    }
                }
                // Persist the actual values to the cache database
                g_cache->upsert(g_eligible[current_idx], 0);

                g_renderer.calculate_fit_rect(g_eligible[current_idx].width, g_eligible[current_idx].height, fit_rect);
                g_logger.info("First item is an image, loaded successfully: %s (%dx%d)", g_eligible[current_idx].path.c_str(), g_eligible[current_idx].width, g_eligible[current_idx].height);
                break;
            } else {
                g_cache->mark_bad(g_eligible[current_idx].path);
                g_logger.warn("Bad first image, skipping: %s", g_eligible[current_idx].path.c_str());
                current_idx = (current_idx + 1) % (int)g_eligible.size();
                load_attempts++;
            }
        }
    }

    bool transitioning = false;
    double transition_timer = 0.0;
    std::string transition_effect = g_cfg.transition_effect;

    // --- Start Background Watchman Thread ---
    g_watchman_running.store(true);
    g_watchman_thread = std::thread(watchman_loop);
    g_logger.info("Watchman: Background watchman thread spawned successfully.");

    while (g_running.load()) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_frame_time).count();
        last_frame_time = now;

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT: g_running.store(false); break;
                case SDL_KEYDOWN:
                    switch (event.key.keysym.sym) {
                        case SDLK_ESCAPE: case SDLK_q: g_running.store(false); break;
                        case SDLK_RIGHT: case SDLK_SPACE: g_remote_command.store(1); break;
                        case SDLK_LEFT: g_remote_command.store(2); break;
                        case SDLK_p: g_remote_command.store(3); break;
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN: g_remote_command.store(1); break;
            }
        }

        int cmd = g_remote_command.exchange(0);

        // --- Mutex-Guarded playlist lookup & manipulation ---
        std::unique_lock<std::mutex> playlist_lock(g_playlist_mtx);

        if (g_eligible.empty()) {
            playlist_lock.unlock();
            SDL_Delay(100);
            continue;
        }

        // Apply Skip Arrow commands
        if (cmd == 1 || cmd == 2) {
            if (g_mpv_player.is_active()) {
                g_logger.info("Interrupted video playback via skip request: stopping mpv.");
                g_mpv_player.stop();
            }
            transitioning = true; transition_timer = 0.0;
            if (cmd == 1) {
                current_idx = (current_idx + 1) % (int)g_eligible.size();
            } else {
                current_idx = (current_idx - 1 + (int)g_eligible.size()) % (int)g_eligible.size();
            }
        }

        if (transitioning && !g_transition->is_active()) {
            TransitionEffect effect = TransitionEffect::Fade;
            if (transition_effect == "wipe") effect = TransitionEffect::WipeLeft;
            else if (transition_effect == "ken_burns") effect = TransitionEffect::KenBurns;
            else if (transition_effect == "pixelate") effect = TransitionEffect::Pixelate;
            g_transition->start(effect, g_cfg.transition_duration, 0, g_cfg.ken_burns_zoom);
        }

        // --- Video Player Handling ---
        if (g_eligible[current_idx].type == "video") {
            if (g_mpv_player.is_active()) {
                if (!g_mpv_player.check_status()) {
                    // Video EOF: stop transition and advance to next
                    g_logger.info("Video EOF detected: advancing playlist.");
                    transitioning = true; transition_timer = 0.0;
                    current_idx = (current_idx + 1) % (int)g_eligible.size();
                }
                playlist_lock.unlock(); // Unlock before delay sleep
                SDL_Delay(50); continue;
            }
            int volume;
            { std::lock_guard<std::mutex> lock(g_config_mtx); volume = g_cfg.video_volume; }
            
            std::string video_path = g_eligible[current_idx].path;
            g_logger.info("Playing video: %s", video_path.c_str());
            
            playlist_lock.unlock(); // Unlock while launching the mpv process
            if (!g_mpv_player.play(video_path, volume)) {
                g_logger.error("Failed to play video, skipping to next.");
                playlist_lock.lock(); // Re-lock
                transitioning = true; transition_timer = 0.0;
                current_idx = (current_idx + 1) % (int)g_eligible.size();
                playlist_lock.unlock();
                continue;
            } else {
                playlist_lock.lock(); // Re-lock to safely free image texture VRAM during playback
                if (current_data) {
                    current_data = nullptr;
                    current_tex = nullptr;
                }
                if (next_data) {
                    next_data = nullptr;
                }
                transitioning = false; // Bypass transitioning during video rendering
                playlist_lock.unlock();
            }
            SDL_Delay(50); continue;
        }

        // --- Image Rendering Handling ---
        item_timer += dt;

        if (transitioning) {
            transition_timer += dt;

            // Load next_data exactly once at the beginning of the transition
            if (!next_data) {
                int next_idx = current_idx % (int)g_eligible.size();
                std::string next_path = g_eligible[next_idx].path;
                
                playlist_lock.unlock(); // Unlock while performing blocking filesystem loading
                next_data = ImageLoader::load(next_path);
                playlist_lock.lock(); // Re-lock
                
                if (next_data && next_data->valid) {
                    ImageLoader::load_texture(next_data.get(), g_renderer.sdl_renderer);

                    // Update in-memory item metadata with actual loaded/rotated dimensions and EXIF
                    g_eligible[next_idx].width = next_data->width;
                    g_eligible[next_idx].height = next_data->height;
                    g_eligible[next_idx].exif_rotation = next_data->exif_rotation;
                    for (auto& item : g_scanned_items) {
                        if (item.path == next_path) {
                            item.width = next_data->width;
                            item.height = next_data->height;
                            item.exif_rotation = next_data->exif_rotation;
                            break;
                        }
                    }
                    // Persist the actual values to the cache database
                    g_cache->upsert(g_eligible[next_idx], 0);
                }
            }

            SDL_Texture* prev_tex = (current_data && current_data->texture) ? current_data->texture : nullptr;
            SDL_Texture* next_tex = (next_data && next_data->texture) ? next_data->texture : nullptr;

            if (prev_tex && next_tex) {
                g_renderer.clear(0, 0, 0, 255);
                g_transition->update(dt);
                g_transition->render(prev_tex, next_tex, g_renderer.screen_w, g_renderer.screen_h);
                g_renderer.present();

                if (g_transition->get_progress() >= 1.0f) {
                    transitioning = false; 
                    item_timer = 0.0;

                    // Clean RAII swap: assign next_data to current_data.
                    // The old current_data's destructor automatically releases its texture and surface.
                    current_data = next_data;
                    next_data = nullptr;
                    current_tex = current_data->texture;

                    g_cache->mark_shown(g_eligible[current_idx].path);
                    g_renderer.calculate_fit_rect(g_eligible[current_idx].width, g_eligible[current_idx].height, fit_rect);
                }
            } else {
                // If loading next image failed or prev_tex is null (swapping from video), abort transition and display immediately
                transitioning = false; 
                item_timer = 0.0;
                if (next_data) {
                    current_data = next_data;
                    next_data = nullptr;
                    current_tex = current_data->texture;
                    g_renderer.calculate_fit_rect(g_eligible[current_idx].width, g_eligible[current_idx].height, fit_rect);
                }
            }
        } else {
            if (current_tex) {
                g_renderer.clear(0, 0, 0, 255);
                if (g_cfg.bias_lighting && current_data) {
                    g_renderer.draw_bias_lighting(fit_rect,
                        current_data->avg_r, current_data->avg_g, current_data->avg_b,
                        g_cfg.bias_strength, (float)item_timer, g_cfg.bias_anim_speed, g_cfg.bias_anim_style, g_cfg.border_width);
                } else if (g_cfg.matting) {
                    g_renderer.draw_matte_borders(fit_rect);
                }
                SDL_Rect dst = {fit_rect.x, fit_rect.y, fit_rect.w, fit_rect.h};
                SDL_RenderCopy(g_renderer.sdl_renderer, current_tex, nullptr, &dst);

                if (g_overlay) {
                    g_overlay->draw_all(current_idx, (int)g_eligible.size(),
                        g_eligible[current_idx].filename, item_timer, false);
                }

                g_renderer.present();
            }

            if (item_timer >= g_cfg.transition_delay) {
                transitioning = true; transition_timer = 0.0;
                current_idx = (current_idx + 1) % (int)g_eligible.size();
            }
        }

        playlist_lock.unlock(); // Unlock before frame sleep throttling
        SDL_Delay(16);
    }

    // --- Cleanup ---
    g_logger.info("Shutting down...");
    
    // Stop background watchman thread safely
    g_watchman_running.store(false);
    if (g_watchman_thread.joinable()) {
        g_watchman_thread.join();
        g_logger.info("Watchman: Background watchman thread stopped successfully.");
    }

    if (g_mpv_player.is_active()) g_mpv_player.stop();
    if (g_transition) { g_transition->reset(); delete g_transition; }
    if (g_overlay) { g_overlay->cleanup(); delete g_overlay; }
    if (current_data) { current_data = nullptr; }
    if (g_cache) { g_cache->close(); delete g_cache; }
    g_renderer.cleanup();

    flock(lock_fd, LOCK_UN); close(lock_fd);
    g_logger.info("piTrove v%s shutdown complete", VERSION);
    return 0;
}

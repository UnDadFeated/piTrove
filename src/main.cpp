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

static PreloadQueue* g_preload = nullptr;
static TransitionEngine* g_transition = nullptr;
static OverlayManager* g_overlay = nullptr;

static void draw_phase_splash(int phase, int progress, int total, int done, const char* label, int dot_counter, const char* filename = nullptr, bool animated = true) {
    g_logger.info("[TRACE] draw_phase_splash phase=%d progress=%d total=%d done=%d label=%s", phase, progress, total, done, label);
    g_renderer.render_splash(phase, progress, total, done, label, dot_counter, filename, animated);
}

static std::vector<MediaItem> filter_by_cooldown(const std::vector<MediaItem>& items, int cooldown_days) {
    std::vector<MediaItem> filtered;
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    int64_t cutoff = now - (static_cast<int64_t>(cooldown_days) * 86400);
    for (const auto& item : items) {
        if (item.last_shown < cutoff) {
            filtered.push_back(item);
        }
    }
    return filtered;
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

    // --- Splash ---
    g_logger.info("Loading splash screen...");
    g_renderer.load_splash(splash_file);
    g_logger.info("Splash loaded");
    fprintf(stderr, "TRACE: after load_splash\n"); fflush(stderr);

    // --- Fast-path: skip scan+cache if DB already exists ---
    fprintf(stderr, "TRACE: before cache check\n"); fflush(stderr);
    std::vector<MediaItem> scanned_items;
    std::string db_path = cache_dir + "/cache.db";
    struct stat db_stat{};
    bool db_exists = (stat(db_path.c_str(), &db_stat) == 0 && db_stat.st_size > 0);
    fprintf(stderr, "TRACE: db_exists=%d\n", db_exists); fflush(stderr);

    if (db_exists) {
        fprintf(stderr, "TRACE: verify_database\n"); fflush(stderr);
        bool db_ok = verify_database(db_path);
        fprintf(stderr, "TRACE: db_ok=%d\n", db_ok); fflush(stderr);
        if (!db_ok) {
            g_logger.error("CORRUPT DB: cache.db is corrupted — removing and will rebuild");
            std::filesystem::remove(db_path);
            db_exists = false;
        }
    }
    if (db_exists) {
        fprintf(stderr, "TRACE: new CacheManager\n"); fflush(stderr);
        CacheManager* fast_cache = new CacheManager();
        fprintf(stderr, "TRACE: fast_cache->open\n"); fflush(stderr);
        if (fast_cache->open(cache_dir)) {
            fprintf(stderr, "TRACE: cache opened, loading items\n"); fflush(stderr);
            g_cache = fast_cache;
            sqlite3_stmt* stmt = nullptr;
            fprintf(stderr, "TRACE: prepare SELECT\n"); fflush(stderr);
            int load_rc = sqlite3_prepare_v2(fast_cache->db,
                "SELECT path, type, w, h, duration, exif, last_shown FROM cache WHERE bad = 0;",
                -1, &stmt, nullptr);
            fprintf(stderr, "TRACE: prepare rc=%d\n", load_rc); fflush(stderr);
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
                    scanned_items.push_back(mi);
                }
                 fprintf(stderr, "TRACE: loaded %d rows\n", row_count); fflush(stderr);
            }
            sqlite3_finalize(stmt);

            fprintf(stderr, "TRACE: count_if photos\n"); fflush(stderr);
            int photos = std::count_if(scanned_items.begin(), scanned_items.end(), [](const MediaItem& i){ return i.type == "image"; });
            fprintf(stderr, "TRACE: count_if videos\n"); fflush(stderr);
            int videos = std::count_if(scanned_items.begin(), scanned_items.end(), [](const MediaItem& i){ return i.type == "video"; });
            fprintf(stderr, "TRACE: photos=%d videos=%d\n", photos, videos); fflush(stderr);
            g_logger.info("Loaded %d items from cache DB (photos=%d videos=%d)", (int)scanned_items.size(), photos, videos);

            if (photos > 0 || videos > 0) {
                g_logger.info("Fast-path: skipping scan, going directly to slideshow");
                g_database_complete.store(true);
            } else {
                g_logger.warn("Cache DB loaded 0 valid items — will re-scan");
            }
        } else {
            g_logger.warn("Failed to open cache DB — will re-scan");
        }
        delete fast_cache;
        if (g_cache == fast_cache) g_cache = nullptr;
    }

    fprintf(stderr, "TRACE: after DB block, items=%d\n", (int)scanned_items.size()); fflush(stderr);

   // --- PHASE 1: SCAN (simple filesystem walk, like legacy) ---
    // (skip if fast-path cache was valid)
    bool do_scan = scanned_items.empty();
    int dot_counter = 0;

    if (do_scan) {
        g_logger.info("Phase 1: Scanning media...");
        auto scan_start = std::chrono::steady_clock::now();

        static const std::vector<std::string> image_exts = {".jpg",".jpeg",".png",".webp",".heic",".heif",".gif",".bmp",".tiff",".tif"};
        static const std::vector<std::string> video_exts = {".mp4",".mov",".mkv",".avi",".webm",".m4v"};

        auto add_file = [&](const std::string& path) {
            std::string ext;
            auto dot = path.find_last_of('.');
            if (dot != std::string::npos) ext = path.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            bool is_img = false, is_vid = false;
            for (auto& e : image_exts) if (ext == e) is_img = true;
            for (auto& e : video_exts) if (ext == e) is_vid = true;
            if (!is_img && !is_vid) return;

            std::string fname = path.substr(path.find_last_of('/') + 1);
            if (fname.empty() || fname[0] == '.') return;

            struct stat st;
            if (stat(path.c_str(), &st) != 0) return;

            MediaItem mi;
            mi.path = path;
            mi.filename = fname;
            mi.ext = ext.substr(1); // remove dot
            mi.file_size = st.st_size;
            mi.modified_time = st.st_mtime;
            mi.type = is_img ? "image" : "video";
            mi.cached = false;
            scanned_items.push_back(std::move(mi));
        };

        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(media_dir, ec)) {
            if (!entry.is_directory(ec)) continue;
            std::string dirname = entry.path().filename().string();
            if (dirname.empty() || dirname[0] == '.') continue;

            std::vector<std::string> ignore_f;
            { std::lock_guard<std::mutex> lk(g_config_mtx); ignore_f = g_cfg.ignore_folders; }
            bool ignored = false;
            for (const auto& ign : ignore_f) if (dirname == ign) { ignored = true; break; }
            if (ignored) continue;

            std::string dirpath = entry.path().string();
            for (const auto& sub : std::filesystem::directory_iterator(dirpath, ec)) {
                if (sub.is_directory(ec)) {
                    // Scan subdirectory
                    for (const auto& inner : std::filesystem::directory_iterator(sub.path(), ec)) {
                        if (inner.is_regular_file(ec)) add_file(inner.path().string());
                    }
                } else if (sub.is_regular_file(ec)) {
                    add_file(sub.path().string());
                }
            }
        }

        auto scan_end = std::chrono::steady_clock::now();
        auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(scan_end - scan_start).count();
        g_logger.info("Scan complete: %d items in %ld ms", (int)scanned_items.size(), (long)scan_ms);
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

        int total_scanned = (int)scanned_items.size();
        draw_phase_splash(3, total_scanned, total_scanned, 0, "CACHING", ++dot_counter, nullptr, false);

        for (int i = 0; i < total_scanned; i++) {
            auto& mi = scanned_items[i];

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

   // --- Cooldown filter ---
    fprintf(stderr, "TRACE: before cooldown filter\n"); fflush(stderr);
    std::vector<MediaItem> eligible = filter_by_cooldown(scanned_items, g_cfg.cooldown_days);
    fprintf(stderr, "TRACE: after cooldown, eligible=%d\n", (int)eligible.size()); fflush(stderr);
    g_logger.info("After cooldown (%d days): %d eligible", g_cfg.cooldown_days, (int)eligible.size());

    if (eligible.empty()) {
        g_logger.warn("No eligible items after cooldown filter");
        draw_phase_splash(4, 0, 0, 0, "DONE", ++dot_counter, nullptr, false);
        SDL_Delay(3000);
        g_renderer.cleanup_splash(); g_renderer.cleanup();
        delete g_cache;
        flock(lock_fd, LOCK_UN); close(lock_fd);
        return 0;
    }

    // --- Shuffle ---
    if (g_cfg.shuffle) {
        unsigned long long seed = make_entropy_seed();
        std::mt19937_64 rng(seed);
   std::shuffle(eligible.begin(), eligible.end(), rng);
        g_logger.info("Playlist shuffled with seed: 0x%llx", (unsigned long long)seed);
    }

    fprintf(stderr, "TRACE: before slideshow start\n"); fflush(stderr);
    g_logger.info("Starting slideshow with %d items", (int)eligible.size());
    // eligible already set from cooldown filter, don't overwrite

    fprintf(stderr, "TRACE: eligible[0].path size=%zu path=%s\n", eligible[0].path.size(), eligible[0].path.c_str()); fflush(stderr);

    fprintf(stderr, "TRACE: new TransitionEngine\n"); fflush(stderr);
    g_transition = new TransitionEngine();
    fprintf(stderr, "TRACE: new OverlayManager\n"); fflush(stderr);
    g_overlay = new OverlayManager(&g_renderer);
    fprintf(stderr, "TRACE: g_logger.info\n"); fflush(stderr);
    g_logger.info("Starting slideshow loop with %d items", (int)eligible.size());

    // --- Main slideshow loop ---
    int current_idx = 0;
    double item_timer = 0.0;
    auto last_frame_time = std::chrono::steady_clock::now();

    fprintf(stderr, "TRACE: make_shared ImageData\n"); fflush(stderr);
    auto current_data = std::make_shared<ImageData>();
    fprintf(stderr, "TRACE: start load loop\n"); fflush(stderr);
    int load_attempts = 0;
    while (load_attempts < 10) {
        fprintf(stderr, "TRACE: attempt %d load %s\n", load_attempts, eligible[current_idx].path.c_str()); fflush(stderr);
        current_data = ImageLoader::load(eligible[current_idx].path);
        fprintf(stderr, "TRACE: load returned valid=%d\n", current_data->valid); fflush(stderr);
        if (current_data && current_data->valid) break;
        g_cache->mark_bad(eligible[current_idx].path);
        g_logger.warn("Bad first image, skipping: %s", eligible[current_idx].path.c_str());
        current_idx = (current_idx + 1) % (int)eligible.size();
        load_attempts++;
    }

    if (current_data && current_data->valid) {
        ImageLoader::load_texture(current_data.get(), g_renderer.sdl_renderer);
        g_cache->mark_shown(eligible[current_idx].path);
    }

    bool transitioning = false;
    double transition_timer = 0.0;
    std::string transition_effect = g_cfg.transition_effect;

    SDL_Texture* current_tex = nullptr;
    if (current_data && current_data->texture) current_tex = current_data->texture;

    SDL_Rect fit_rect{0, 0, 0, 0};

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
        if (cmd == 1) {
            transitioning = true; transition_timer = 0.0;
            current_idx = (current_idx + 1) % (int)eligible.size();
        } else if (cmd == 2) {
            transitioning = true; transition_timer = 0.0;
            current_idx = (current_idx - 1 + (int)eligible.size()) % (int)eligible.size();
        }

        if (transitioning && !g_transition->is_active()) {
            TransitionEffect effect = TransitionEffect::Fade;
            if (transition_effect == "wipe") effect = TransitionEffect::WipeLeft;
            else if (transition_effect == "ken_burns") effect = TransitionEffect::KenBurns;
            else if (transition_effect == "pixelate") effect = TransitionEffect::Pixelate;
            g_transition->start(effect, g_cfg.transition_duration, 0, g_cfg.ken_burns_zoom);
        }

        // --- Video ---
        if (eligible[current_idx].type == "video") {
            if (g_mpv_player.is_active()) {
                if (!g_mpv_player.check_status()) {
                    transitioning = true; transition_timer = 0.0;
                    current_idx = (current_idx + 1) % (int)eligible.size();
                }
                SDL_Delay(50); continue;
            }
            int volume;
            { std::lock_guard<std::mutex> lock(g_config_mtx); volume = g_cfg.video_volume; }
            g_logger.info("Playing video: %s", eligible[current_idx].path.c_str());
            if (!g_mpv_player.play(eligible[current_idx].path, volume)) {
                g_logger.error("Failed to play video, skipping");
                transitioning = true; transition_timer = 0.0;
                current_idx = (current_idx + 1) % (int)eligible.size();
                continue;
            }
            SDL_Delay(50); continue;
        }

        // --- Image ---
        item_timer += dt;

        if (transitioning) {
            transition_timer += dt;
            double trans_duration = g_cfg.transition_duration;

            SDL_Texture* prev_tex = current_tex;
            SDL_Texture* next_tex = nullptr;
            auto next_data = std::make_shared<ImageData>();
            if (!next_tex) {
                int next_idx = current_idx % (int)eligible.size();
                next_data = ImageLoader::load(eligible[next_idx].path);
                if (next_data->valid) {
                    ImageLoader::load_texture(next_data.get(), g_renderer.sdl_renderer);
                    next_tex = next_data->texture;
                }
            }

            if (prev_tex && next_tex) {
                g_renderer.clear(0, 0, 0, 255);
                g_transition->update(dt);
                g_transition->render(prev_tex, next_tex, g_renderer.screen_w, g_renderer.screen_h);
                g_renderer.present();

                if (current_tex) {
                    auto old_data = std::make_shared<ImageData>();
                    old_data->texture = current_tex; old_data->valid = true;
                    ImageLoader::unload_texture(old_data.get());
                }
                current_tex = next_tex;

                if (g_transition->get_progress() >= 1.0f) {
                    transitioning = false; item_timer = 0.0;
                    g_cache->mark_shown(eligible[current_idx].path);
                    g_renderer.calculate_fit_rect(eligible[current_idx].width, eligible[current_idx].height, fit_rect);
                }
            } else {
                transitioning = false; item_timer = 0.0;
            }
        } else {
            if (current_tex) {
                g_renderer.clear(0, 0, 0, 255);
                if (g_cfg.matting) g_renderer.draw_matte_borders(fit_rect);
                SDL_Rect dst = {fit_rect.x, fit_rect.y, fit_rect.w, fit_rect.h};
                SDL_RenderCopy(g_renderer.sdl_renderer, current_tex, nullptr, &dst);
                g_renderer.present();
            }

            if (g_overlay) {
                g_overlay->draw_all(current_idx, (int)eligible.size(),
                    eligible[current_idx].filename, item_timer, false);
            }

            if (item_timer >= g_cfg.transition_delay) {
                transitioning = true; transition_timer = 0.0;
                current_idx = (current_idx + 1) % (int)eligible.size();
            }
        }

        SDL_Delay(16);
    }

    // --- Cleanup ---
    g_logger.info("Shutting down...");
    if (g_mpv_player.is_active()) g_mpv_player.stop();
    if (g_transition) { g_transition->reset(); delete g_transition; }
    if (g_overlay) { g_overlay->cleanup(); delete g_overlay; }
    if (current_data) ImageLoader::unload(current_data.get());
    if (g_cache) { g_cache->close(); delete g_cache; }
    g_renderer.cleanup();

    flock(lock_fd, LOCK_UN); close(lock_fd);
    g_logger.info("piTrove v%s shutdown complete", VERSION);
    return 0;
}

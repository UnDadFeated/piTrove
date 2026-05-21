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
#include <GLES3/gl3.h>
#include <sys/file.h>
#include <unistd.h>
#include <csignal>
#include <algorithm>
#include <random>
#include <chrono>
#include <cmath>

// ============================================================
// Globals
// ============================================================
static PreloadQueue* g_preload = nullptr;
static TransitionEngine* g_transition = nullptr;
static OverlayManager* g_overlay = nullptr;

// ============================================================
// Phase UI helpers (match renderer.cpp render_splash layout)
// ============================================================
static void draw_phase_splash(int phase, int progress, int total, int done, const char* label) {
    static int dot_counter = 0;
    dot_counter++;
    g_renderer.render_splash(phase, progress, total, done, label, dot_counter);
}

static std::vector<MediaItem> filter_by_cooldown(
    const std::vector<MediaItem>& items, int cooldown_days)
{
    std::vector<MediaItem> filtered;
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    int64_t cutoff = now - (static_cast<int64_t>(cooldown_days) * 86400);

    for (const auto& item : items) {
        if (item.last_shown < cutoff) {
            filtered.push_back(item);
        } else {
            g_logger.debug("Cooldown skip: %s (last shown: %lld, cutoff: %lld)",
                           item.path.c_str(),
                           static_cast<long long>(item.last_shown),
                           static_cast<long long>(cutoff));
        }
    }
    return filtered;
}

// ============================================================
// Main
// ============================================================
int main(int argc, char** argv)
{
    // --- Single-instance lock ---
    std::string lock_path = get_exe_dir() + "/piTrove.lock";
    int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0) {
        fprintf(stderr, "Failed to open lock file\n");
        return 1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "Another instance is already running\n");
        close(lock_fd);
        return 1;
    }

    // --- Signal handlers ---
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
        if (arg == "--config" && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    if (config_path.empty()) {
        // Try common locations
        const char* candidates[] = {
            "config/config.toml",
            "src/config/config.toml",
            "/home/pi/piTrove/src/config/config.toml",
            "./src/config/config.toml"
        };
        for (const auto& c : candidates) {
            if (file_exists(c)) {
                config_path = c;
                break;
            }
        }
    }

    if (!config_path.empty()) {
        g_logger.info("Loading config: %s", config_path.c_str());
        if (!g_cfg.load(config_path)) {
            g_logger.warn("Failed to load config from %s, using defaults", config_path.c_str());
        }
    } else {
        g_logger.warn("No config file found, using defaults");
    }

    // --- Directories ---
    std::string media_dir = g_cfg.media_dir;
    std::string cache_dir = g_cfg.cache_dir.empty() ? (get_exe_dir() + "/cache") : g_cfg.cache_dir;
    std::string log_dir = g_cfg.log_dir.empty() ? (get_exe_dir() + "/logs") : g_cfg.log_dir;

    // --- Logger ---
    g_logger.init(log_dir, g_cfg.verbose ? LogLevel::DEBUG : LogLevel::INFO);
    g_logger.info("Media dir: %s, Cache dir: %s", media_dir.c_str(), cache_dir.c_str());

    // --- SDL2 Init (kmsdrm) ---
    g_logger.info("Initializing SDL2 (%dx%d)...", g_cfg.screen_w, g_cfg.screen_h);

    std::string splash_file = g_cfg.splash_file;
    if (splash_file.empty()) splash_file = "src/splash.png";

    if (!g_renderer.init(g_cfg.screen_w, g_cfg.screen_h, g_cfg.fullscreen)) {
        g_logger.error("SDL2 initialization failed");
        return 1;
    }

    g_logger.info("SDL2 context created: %dx%d", g_renderer.screen_w, g_renderer.screen_h);

    // --- Splash ---
    g_logger.info("Loading splash screen...");
    g_renderer.load_splash(splash_file);

    // --- Cache ---
    g_cache = new CacheManager();
    if (!g_cache->open(cache_dir)) {
        g_logger.error("Failed to open cache database at %s", cache_dir.c_str());
        g_renderer.cleanup_splash();
        g_renderer.cleanup();
        delete g_cache;
        return 1;
    }
    g_logger.info("Cache opened: %s", cache_dir.c_str());

    // --- Scanner Phase ---
    g_logger.info("Starting directory scan...");

    std::vector<MediaItem> all_items;
    std::atomic<int64_t> scan_count{0};

    draw_phase_splash(2, 0, 0, 0, "SCANNING");

    scan_directory(media_dir, g_cfg.scan_depth, all_items, scan_count);

    int total_scanned = static_cast<int>(all_items.size());
    g_logger.info("Scan complete: %d items found", total_scanned);
    draw_phase_splash(2, total_scanned, total_scanned, 0, "SCANNED");

    // --- Cache Phase (upsert) ---
    g_cache->begin_transaction();
    int cached = 0;
    for (int i = 0; i < total_scanned; i++) {
        MediaItem& item = all_items[i];
        g_cache->upsert(item, 0);
        cached++;

        if (i % 100 == 0) {
            draw_phase_splash(3, total_scanned, total_scanned, cached, "CACHING");
        }
    }
    g_cache->commit_transaction();
    g_logger.info("Cache upsert complete: %d items", cached);

    // --- Cooldown filter ---
    std::vector<MediaItem> eligible = filter_by_cooldown(all_items, g_cfg.cooldown_days);
    g_logger.info("After cooldown (%d days): %d eligible items",
                  g_cfg.cooldown_days, static_cast<int>(eligible.size()));

    if (eligible.empty()) {
        g_logger.warn("No eligible items after cooldown filter");
        draw_phase_splash(4, 0, 0, 0, "DONE");
        SDL_Delay(3000);
        g_renderer.cleanup_splash();
        g_renderer.cleanup();
        delete g_cache;
        return 0;
    }

    // --- Shuffle (triple entropy) ---
    if (g_cfg.shuffle) {
        unsigned long long seed = make_entropy_seed();
        std::mt19937_64 rng(seed);
        std::shuffle(eligible.begin(), eligible.end(), rng);
        g_logger.info("Playlist shuffled with entropy seed: 0x%llx",
                       static_cast<unsigned long long>(seed));
    }

    // --- Transition engine ---
    g_transition = new TransitionEngine(g_renderer.sdl_renderer);
    g_transition->init();
    g_logger.info("Transition engine initialized");

    // --- Overlay manager ---
    g_overlay = new OverlayManager(g_renderer.sdl_renderer);
    g_overlay->init();
    g_logger.info("Overlay manager initialized");

    // --- Preload queue ---
    g_preload = new PreloadQueue(g_cfg.max_concurrent, g_cfg.max_concurrent, g_renderer.sdl_renderer);
    g_preload->start();
    g_logger.info("Preload queue started");

    // --- Phase 4: Ready ---
    g_database_complete.store(true);
    draw_phase_splash(4, 0, 0, 0, "READY");
    g_renderer.cleanup_splash();

    g_logger.info("Starting slideshow loop with %d items", static_cast<int>(eligible.size()));

    // --- Main loop ---
    int current_idx = 0;
    double item_timer = 0.0;
    auto last_frame_time = std::chrono::steady_clock::now();

    // Preload next item
    if (eligible.size() > 1) {
        int next_idx = (current_idx + 1) % eligible.size();
        g_preload->enqueue(eligible[next_idx].path);
    }

    // Load first item
    auto current_data = ImageLoader::load(eligible[current_idx].path);
    if (!current_data->valid) {
        g_logger.error("Failed to load first image: %s", eligible[current_idx].path.c_str());
    } else {
        ImageLoader::load_texture(current_data.get(), g_renderer.sdl_renderer);
    }

    // Mark as shown
    g_cache->mark_shown(eligible[current_idx].path);

    bool transitioning = false;
    double transition_timer = 0.0;
    std::string transition_effect = g_cfg.transition_effect;

    // Image display state
    SDL_Texture* current_tex = nullptr;
    if (current_data && current_data->texture) {
        current_tex = current_data->texture;
    }

    SDL_Rect fit_rect{0, 0, 0, 0};

    while (g_running.load()) {
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_frame_time).count();
        last_frame_time = now;

        // --- Event handling ---
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_QUIT:
                    g_running.store(false);
                    break;
                case SDL_KEYDOWN:
                    switch (event.key.keysym.sym) {
                        case SDLK_ESCAPE:
                        case SDLK_q:
                            g_running.store(false);
                            break;
                        case SDLK_RIGHT:
                        case SDLK_SPACE:
                            g_remote_command.store(1); // Next
                            break;
                        case SDLK_LEFT:
                            g_remote_command.store(2); // Prev
                            break;
                        case SDLK_p:
                            g_remote_command.store(3); // Pause toggle
                            break;
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                    g_remote_command.store(1); // Next on click
                    break;
            }
        }

        // Check remote commands
        int cmd = g_remote_command.exchange(0);
        if (cmd == 1) { // Next
            transitioning = true;
            transition_timer = 0.0;
            current_idx = (current_idx + 1) % static_cast<int>(eligible.size());
            g_logger.info("Next image: %d/%d", current_idx + 1, static_cast<int>(eligible.size()));
        } else if (cmd == 2) { // Prev
            transitioning = true;
            transition_timer = 0.0;
            current_idx = (current_idx - 1 + static_cast<int>(eligible.size())) % static_cast<int>(eligible.size());
            g_logger.info("Prev image: %d/%d", current_idx + 1, static_cast<int>(eligible.size()));
        } else if (cmd == 3) {
            // Pause toggle (simplified)
            g_logger.debug("Pause toggle");
        }

        // --- Video playback ---
        if (eligible[current_idx].type == "video") {
            // Check if mpv is still running
            if (g_mpv_player.is_active()) {
                if (!g_mpv_player.check_status()) {
                    // Video finished, advance
                    g_logger.info("Video playback finished");
                    transitioning = true;
                    transition_timer = 0.0;
                    current_idx = (current_idx + 1) % static_cast<int>(eligible.size());
                }
                SDL_Delay(50);
                continue;
            }

            // Start video
            int volume = 0;
            {
                std::lock_guard<std::mutex> lock(g_config_mtx);
                volume = g_cfg.video_volume;
            }

            g_logger.info("Playing video: %s", eligible[current_idx].path.c_str());
            if (!g_mpv_player.play(eligible[current_idx].path, volume)) {
                g_logger.error("Failed to play video, skipping");
                transitioning = true;
                transition_timer = 0.0;
                current_idx = (current_idx + 1) % static_cast<int>(eligible.size());
                continue;
            }

            // Preload next after video
            if (eligible.size() > 1) {
                int next_idx = (current_idx + 1) % static_cast<int>(eligible.size());
                g_preload->enqueue(eligible[next_idx].path);
            }

            SDL_Delay(50);
            continue;
        }

        // --- Image display ---
        item_timer += dt;

        // Handle transition
        if (transitioning) {
            transition_timer += dt;
            double trans_duration = g_cfg.transition_duration;

            float progress = static_cast<float>(std::min(transition_timer / trans_duration, 1.0));

            // Get current and next textures
            SDL_Texture* prev_tex = current_tex;
            SDL_Texture* next_tex = nullptr;

            // Dequeue preloaded image
            auto next_data = g_preload->try_dequeue();
            if (next_data && next_data->texture) {
                next_tex = next_data->texture;
            }

            // If no preloaded, load current
            if (!next_tex) {
                int next_idx = (current_idx) % static_cast<int>(eligible.size());
                next_data = ImageLoader::load(eligible[next_idx].path);
                if (next_data->valid) {
                    ImageLoader::load_texture(next_data.get(), g_renderer.sdl_renderer);
                    next_tex = next_data->texture;
                }
            }

            if (prev_tex && next_tex) {
                // Make current context active for raw GLES calls
                SDL_GL_MakeCurrent(g_renderer.window, g_renderer.gl_context);

                // Clear
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);

                // Apply transition shader
                if (transition_effect == "ken_burns") {
                    g_transition->render_ken_burns(next_tex, progress,
                        g_cfg.ken_burns_zoom, 1.0f + g_cfg.ken_burns_zoom, 0.05f, 0.05f,
                        g_renderer.screen_w, g_renderer.screen_h);
                } else if (transition_effect == "wipe") {
                    g_transition->render_wipe(prev_tex, next_tex, progress, 0);
                } else if (transition_effect == "pixelate") {
                    g_transition->render_pixelate(prev_tex, next_tex, progress, 50.0f,
                        g_renderer.screen_w, g_renderer.screen_h);
                } else { // crossfade
                    // Simple crossfade via post_process
                    g_transition->render_post_process(next_tex, progress, 0.0f, 0.0f, 0.0f, 0.0f,
                        g_renderer.screen_w, g_renderer.screen_h);
                }

                SDL_GL_SwapWindow(g_renderer.window);

                // Cleanup old texture
                if (current_tex) {
                    auto old_data = std::make_shared<ImageData>();
                    old_data->texture = current_tex;
                    old_data->valid = true;
                    ImageLoader::unload_texture(old_data.get());
                }

                // Update current texture
                current_tex = next_tex;

                if (progress >= 1.0f) {
                    transitioning = false;
                    item_timer = 0.0;

                    // Mark as shown
                    g_cache->mark_shown(eligible[current_idx].path);

                    // Preload next
                    if (eligible.size() > 1) {
                        int next_idx = (current_idx + 1) % static_cast<int>(eligible.size());
                        g_preload->enqueue(eligible[next_idx].path);
                    }

                    // Update fit rect
                    g_renderer.calculate_fit_rect(
                        eligible[current_idx].width,
                        eligible[current_idx].height,
                        fit_rect);
                }
            } else {
                transitioning = false;
                item_timer = 0.0;
            }
        } else {
            // --- Render image ---
            if (current_tex) {
                SDL_Renderer* sdl_r = g_renderer.sdl_renderer;

                // Clear
                SDL_SetRenderDrawColor(sdl_r, 0, 0, 0, 255);
                SDL_RenderClear(sdl_r);

                // Matte borders
                if (g_cfg.matting) {
                    g_renderer.draw_matte_borders(fit_rect);
                }

                // Draw image
                if (current_tex) {
                    SDL_RenderCopy(sdl_r, current_tex, nullptr, &fit_rect);
                }

                // Post-process via shader if enabled
                if (g_cfg.bias_lighting || g_cfg.vignette_enabled) {
                    SDL_GL_MakeCurrent(g_renderer.window, g_renderer.gl_context);

                    float bias_r = 0.0f, bias_g = 0.0f, bias_b = 0.0f;
                    float scanline = 0.0f;

                    // Get bias color (simplified)
                    SDL_GL_BindTexture(current_tex, nullptr, nullptr);
                    g_transition->render_post_process(current_tex,
                        item_timer * g_cfg.bias_anim_speed,
                        bias_r, bias_g, bias_b, scanline,
                        g_renderer.screen_w, g_renderer.screen_h);
                    SDL_GL_UnbindTexture(current_tex);

                    SDL_GL_SwapWindow(g_renderer.window);
                } else {
                    SDL_RenderPresent(sdl_r);
                }
            }

            // --- Overlay ---
            if (g_overlay) {
                g_overlay->draw_all(current_idx,
                                    static_cast<int>(eligible.size()),
                                    eligible[current_idx].filename,
                                    item_timer,
                                    false);
            }

            // Check timer
            double delay = g_cfg.transition_delay;
            if (item_timer >= delay) {
                transitioning = true;
                transition_timer = 0.0;
                current_idx = (current_idx + 1) % static_cast<int>(eligible.size());
            }
        }

        // Small sleep to prevent CPU spinning
        SDL_Delay(16); // ~60fps
    }

    // --- Cleanup ---
    g_logger.info("Shutting down...");
    g_running.store(false);

    if (g_mpv_player.is_active()) {
        g_mpv_player.stop();
    }

    if (g_preload) {
        g_preload->shutdown();
        delete g_preload;
    }

    if (g_transition) {
        g_transition->cleanup();
        delete g_transition;
    }

    if (g_overlay) {
        g_overlay->cleanup();
        delete g_overlay;
    }

    if (current_data) {
        ImageLoader::unload(current_data.get());
    }

    if (g_cache) {
        g_cache->close();
        delete g_cache;
    }

    g_renderer.cleanup();

    // Release lock
    flock(lock_fd, LOCK_UN);
    close(lock_fd);

    g_logger.info("piTrove v%s shutdown complete", VERSION);
    return 0;
}

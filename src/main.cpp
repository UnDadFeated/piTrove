#include "health.h"
#include "safe_mode.h"
#include "thermal.h"
#include "auth.h"
#include <sys/statvfs.h>
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
#include "video_decoder.h"
#include "tui.h"
#include "http_server.h"
#include "mqtt.h"
#include "google_photos.h"
#include "organizer.h"
#include "preprocess.h"
#include "news_ticker.h"
#include "calendar.h"
#include "stock_streamer.h"
#include "infopanel_layout.h"
#include "chores.h"
#include "chores_panel.h"


#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <sys/file.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#include <cstdlib>
#include <ctime>
#include <csignal>
#include <algorithm>
#include <random>
#include <chrono>
#include <cmath>
#include <atomic>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <future>

#include <filesystem>
#include <unordered_set>
#include <fstream>
#include <glob.h>
#include <numbers>
#include <utility>
#include <format>

static TransitionEngine* g_transition = nullptr;

// Probe the active connected DRM connector and card path
static std::string probe_connected_connector(std::string& out_card, int& out_card_index) {
    out_card = "";
    out_card_index = -1;
    std::string connected_connector = "";

    glob_t gres;
    // Scan card*-*/status
    if (glob("/sys/class/drm/card*-*/status", 0, nullptr, &gres) == 0) {
        for (size_t i = 0; i < gres.gl_pathc; i++) {
            std::string status_path = gres.gl_pathv[i];
            std::ifstream f(status_path);
            if (f.is_open()) {
                std::string status;
                std::getline(f, status);
                // trim status
                status.erase(status.find_last_not_of(" \t\r\n") + 1);
                if (status == "connected") {
                    // Extract connector and card from path.
                    // Example path: /sys/class/drm/card1-HDMI-A-1/status
                    size_t slash = status_path.rfind('/');
                    if (slash != std::string::npos) {
                        std::string dir_name = std::filesystem::path(status_path).parent_path().filename().string();
                        size_t dash = dir_name.find('-');
                        if (dash != std::string::npos) {
                            std::string card_part = dir_name.substr(0, dash); // "card1"
                            std::string conn_part = dir_name.substr(dash + 1); // "HDMI-A-1"
                            
                            if (card_part.rfind("card", 0) == 0) {
                                const int card_idx = safe_stoi(card_part.substr(4), -1);
                                if (card_idx < 0) continue;
                                out_card = "/dev/dri/" + card_part;
                                out_card_index = card_idx;
                                connected_connector = conn_part;
                                break;
                            }
                        }
                    }
                }
            }
        }
        globfree(&gres);
    }
    return connected_connector;
}

static OverlayManager* g_overlay = nullptr;
PreloadQueue* g_preload = nullptr;

// Background Watchman & Dynamic Playlist State
std::mutex g_playlist_mtx;
static uint64_t playlist_generation = 0; // Protected by g_playlist_mtx.
std::vector<MediaItem> g_scanned_items;
std::vector<MediaItem> g_eligible;
int current_idx = 0;
SDL_Texture* g_video_tex = nullptr;
int g_video_tex_w = 0, g_video_tex_h = 0;
SDL_PixelFormat g_video_tex_fmt = SDL_PIXELFORMAT_UNKNOWN;
static SDL_Colorspace g_video_colorspace = SDL_COLORSPACE_UNKNOWN;
static std::jthread g_watchman_thread;
// Held as a raw epoch count: the watchdog thread and the main loop both write it, and the
// 8-byte time_point could tear. A garbage value here makes `elapsed` huge, and a large
// elapsed triggers a forced _exit(99) container restart.
static std::atomic<int64_t> g_watchdog_last_time{0};
static inline int64_t steady_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static VideoDecoder g_video_decoder;

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
        int digit_count = 0;
        while (i < parent_name.size() && isdigit(parent_name[i])) {
            if (digit_count < 9) {
                val = val * 10 + (parent_name[i] - '0');
            }
            digit_count++;
            i++;
        }
        groups[gc++] = val;
        if (gc == 1 && i < parent_name.size() && (parent_name[i] == '-' || parent_name[i] == '_')) i++;
    }

    bool folder_has_prefix = false;
    if (gc >= 2) {
        int folder_m = groups[1];
        if (folder_m >= 1 && folder_m <= 12) {
            folder_has_prefix = true;
            time_t t = std::time(nullptr);
            struct tm tm_buf;
            struct tm* now = localtime_r(&t, &tm_buf);
            if (now) {
                int curr_m = now->tm_mon + 1;
                
                int max_month_spread = (int)std::ceil(window_days / 30.0);
                int diff = std::abs(curr_m - folder_m);
                if (diff > 6) diff = 12 - diff;
                
                if (diff > max_month_spread) {
                    return false;
                }
            }
        }
    }

    // 2. Check filename for seasonal window
    auto fn_date = parse_filename_date(item.filename);
    if (fn_date) {
        return is_in_seasonal_window(item.filename, window_days);
    }

    // If folder had prefix and matched, and filename has no prefix, it is in season!
    if (folder_has_prefix) {
        return true;
    }

    // 3. Fallback to file creation / modified attributes
    int64_t target_time = item.creation_time > 0 ? item.creation_time : item.modified_time;
    if (target_time <= 0) return true;

    time_t file_time = static_cast<time_t>(target_time);
    struct tm file_tm;
    if (!localtime_r(&file_time, &file_tm)) return true;
    int file_m = file_tm.tm_mon + 1;
    int file_d = file_tm.tm_mday;

    time_t t = std::time(nullptr);
    struct tm tm_buf;
    struct tm* now = localtime_r(&t, &tm_buf);
    if (!now) return true;
    int curr_m = now->tm_mon + 1;
    int curr_d = now->tm_mday;

    auto get_day_of_year = [](int month, int day) {
        static const int days_before_month[] = { 0, 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
        if (month < 1 || month > 12) return 0;
        return days_before_month[month] + day;
    };

    int file_doy = get_day_of_year(file_m, file_d);
    int curr_doy = get_day_of_year(curr_m, curr_d);

    int diff = std::abs(curr_doy - file_doy);
    if (diff > 365 / 2) diff = 365 - diff;

    return diff <= window_days;
}

static std::optional<int> find_twin_portrait(const std::vector<MediaItem>& items, int index, bool look_ahead) {
    const auto portrait = [](const MediaItem& item) {
        return item.type == "image" && item.height > item.width && item.height > 0;
    };
    if (!portrait(items[index])) return std::nullopt;
    const int end = look_ahead ? std::ssize(items) : std::min<int>(index + 2, std::ssize(items));
    for (int i = index + 1; i < end; ++i) {
        if (portrait(items[i])) return i;
    }
    return std::nullopt;
}

// NOTE: Matting/border offset logic duplicates Renderer::calculate_fit_rect (renderer.cpp:251)
// Keep in sync if adjusting matte or border inset calculations

static InfopanelLayout calculate_infopanel_layout(int screen_w, int screen_h) {
    InfopanelOptions options;
    {
        std::shared_lock lock(g_config_mtx);
        options = {g_cfg.infopanels_enabled, g_cfg.news_enabled, g_cfg.gcalendar_enabled,
                   g_cfg.stockstreamer_enabled, g_cfg.left_panel_enabled, g_cfg.matting,
                   g_cfg.matting_size, g_cfg.display_offset_x, g_cfg.display_offset_y};
    }
    return make_infopanel_layout(screen_w, screen_h, options);
}

static void calculate_fit_rect_in_area(int img_w, int img_h, int area_x, int area_y, int area_w, int area_h, SDL_Rect& out_rect) {
    if (img_w <= 0 || img_h <= 0) {
        out_rect.w = 0;
        out_rect.h = 0;
        out_rect.x = area_x + area_w / 2;
        out_rect.y = area_y + area_h / 2;
        return;
    }

    std::string border_mode = "off";
    int border_w = 0;
    {
        std::shared_lock lock(g_config_mtx);
        border_mode = g_cfg.border_mode;
        border_w = g_renderer.scale_px(g_cfg.border_width);
    }

    int left_margin = ((border_mode != "off") ? border_w : 0);
    int right_margin = ((border_mode != "off") ? border_w : 0);
    int top_margin = ((border_mode != "off") ? border_w : 0);
    int bottom_margin = ((border_mode != "off") ? ((border_mode == "polaroid") ? (border_w * 4) : border_w) : 0);

    // Padding for subtle drop-shadow / border clearance
    int pad = g_renderer.scale_px(28);
    left_margin += pad;
    right_margin += pad;
    top_margin += pad;
    bottom_margin += pad;

    int effective_x = area_x + left_margin;
    int effective_y = area_y + top_margin;
    int effective_w = area_w - (left_margin + right_margin);
    int effective_h = area_h - (top_margin + bottom_margin);

    if (effective_w < 1) effective_w = 1;
    if (effective_h < 1) effective_h = 1;

    float scale = std::min((float)effective_w / img_w, (float)effective_h / img_h);
    out_rect.w = (int)(img_w * scale);
    out_rect.h = (int)(img_h * scale);
    out_rect.x = effective_x + (effective_w - out_rect.w) / 2;
    out_rect.y = effective_y + (effective_h - out_rect.h) / 2;
}

static SDL_Texture* render_state_to_texture(
    SDL_Renderer* renderer,
    int sw, int sh,
    const std::shared_ptr<ImageData>& primary,
    const std::shared_ptr<ImageData>& twin,
    double item_timer
) {
    // Create a target texture
    SDL_Texture* target = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, sw, sh);
    if (!target) return nullptr;
    SDL_SetTextureBlendMode(target, SDL_BLENDMODE_BLEND);

    if (!SDL_SetRenderTarget(renderer, target)) {
        SDL_DestroyTexture(target);
        return nullptr;
    }

    bool snap_matte_color = false;
    {
        std::shared_lock lock(g_config_mtx);
        snap_matte_color = g_cfg.color_matched_matte;
    }
    if (snap_matte_color && primary && !twin) {
        SDL_SetRenderDrawColor(renderer, primary->matte_r, primary->matte_g, primary->matte_b, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    }
    SDL_RenderClear(renderer);

    InfopanelLayout layout = calculate_infopanel_layout(sw, sh);
    if (twin && twin->texture && primary && primary->texture) {
        // Draw twin portrait layout!
        SDL_Rect rect_l, rect_r;
        const auto& area = layout.slideshow_area;
        const int half_w = area.w / 2;
        const SDL_Rect left_area{area.x, area.y, half_w, area.h};
        const SDL_Rect right_area{area.x + half_w, area.y, area.w - half_w, area.h};
        calculate_fit_rect_in_area(primary->width, primary->height, left_area.x, left_area.y, left_area.w, left_area.h, rect_l);
        calculate_fit_rect_in_area(twin->width, twin->height, right_area.x, right_area.y, right_area.w, right_area.h, rect_r);

        bool has_bias = false;
        bool has_matting = false;
        bool has_border = false;
        std::string snap_border_mode = "off";
        int bias_strength = 110;
        float anim_speed = 0.5f;
        std::string style = "edge_glow";
        int border_w = 10;
        bool snap_blurred;
        int snap_glow_depth;
        float matte_op, vignette_str;
        {
            std::shared_lock lock(g_config_mtx);
            has_bias = g_cfg.bias_lighting;
            has_matting = g_cfg.matting;
            snap_border_mode = g_cfg.border_mode;
            has_border = (snap_border_mode != "off");
            bias_strength = g_cfg.bias_strength;
            anim_speed = g_cfg.bias_anim_speed;
            style = g_cfg.bias_anim_style;
            border_w = g_renderer.scale_px(g_cfg.border_width);
            snap_glow_depth = g_renderer.scale_px(g_cfg.glow_depth);
            snap_blurred = g_cfg.blurred_background;
            snap_matte_color = g_cfg.color_matched_matte;
            matte_op = g_cfg.matte_opacity;
            vignette_str = g_cfg.vignette_strength;
        }

        // 1. Draw background based on style
        std::string snap_bg_style;
        { std::shared_lock lk(g_config_mtx); snap_bg_style = g_cfg.bg_style; }
        if (snap_blurred || snap_bg_style != "photo") {
            g_renderer.draw_background(primary.get(), snap_bg_style, (Uint8)(255.0f * vignette_str));
        }

        // 2. Draw matte borders if enabled (solid black base layer) - ONLY if NOT color-matched or blurred!
        if (has_matting && !snap_matte_color && !snap_blurred) {
            g_renderer.draw_matte_borders(rect_l, left_area);
            g_renderer.draw_matte_borders(rect_r, right_area);
        }

        // 3. Color-matched matte for each portrait (opaque and edge-to-edge if enabled)
        if (snap_matte_color) {
            g_renderer.draw_color_matched_matte(rect_l,
                primary->matte_r, primary->matte_g, primary->matte_b, matte_op, left_area);
            g_renderer.draw_color_matched_matte(rect_r,
                twin->matte_r, twin->matte_g, twin->matte_b, matte_op, right_area);
        }

        // 4. Draw bias lighting if enabled
        if (has_bias) {
            int bw_l = has_border ? border_w : 0;
            g_renderer.draw_bias_lighting(rect_l, layout.slideshow_area, primary->avg_r, primary->avg_g, primary->avg_b,
                bias_strength, (float)item_timer, anim_speed, style, bw_l, snap_glow_depth);
            int bw_r = has_border ? border_w : 0;
            g_renderer.draw_bias_lighting(rect_r, layout.slideshow_area, twin->avg_r, twin->avg_g, twin->avg_b,
                bias_strength, (float)item_timer, anim_speed, style, bw_r, snap_glow_depth);
        }

        // 5. Draw borders if enabled
        if (has_border) {
            g_renderer.draw_border(rect_l, snap_border_mode, primary->avg_r, primary->avg_g, primary->avg_b, border_w, primary->filename);
            g_renderer.draw_border(rect_r, snap_border_mode, twin->avg_r, twin->avg_g, twin->avg_b, border_w, twin->filename);
        }

        // 6. Draw texture
        SDL_FRect dst_l = {(float)rect_l.x, (float)rect_l.y, (float)rect_l.w, (float)rect_l.h};
        SDL_RenderTexture(renderer, primary->texture, nullptr, &dst_l);

        SDL_FRect dst_r = {(float)rect_r.x, (float)rect_r.y, (float)rect_r.w, (float)rect_r.h};
        SDL_RenderTexture(renderer, twin->texture, nullptr, &dst_r);
    } else if (primary && primary->texture) {
        // Draw single image layout!
        SDL_Rect rect;
        calculate_fit_rect_in_area(primary->width, primary->height, layout.slideshow_area.x, layout.slideshow_area.y, layout.slideshow_area.w, layout.slideshow_area.h, rect);

        bool has_bias = false;
        bool has_matting = false;
        bool has_border = false;
        std::string snap_border_mode = "off";
        int bias_strength = 110;
        float anim_speed = 0.5f;
        std::string style = "edge_glow";
        int border_w = 10;
        bool snap_blurred;
        int snap_glow_depth;
        float matte_op, vignette_str;
        {
            std::shared_lock lock(g_config_mtx);
            has_bias = g_cfg.bias_lighting;
            has_matting = g_cfg.matting;
            snap_border_mode = g_cfg.border_mode;
            has_border = (snap_border_mode != "off");
            bias_strength = g_cfg.bias_strength;
            anim_speed = g_cfg.bias_anim_speed;
            style = g_cfg.bias_anim_style;
            border_w = g_renderer.scale_px(g_cfg.border_width);
            snap_glow_depth = g_renderer.scale_px(g_cfg.glow_depth);
            snap_blurred = g_cfg.blurred_background;
            snap_matte_color = g_cfg.color_matched_matte;
            matte_op = g_cfg.matte_opacity;
            vignette_str = g_cfg.vignette_strength;
        }

        // 1. Draw background based on style
        std::string snap_bg_style;
        { std::shared_lock lk(g_config_mtx); snap_bg_style = g_cfg.bg_style; }
        if (snap_blurred || snap_bg_style != "photo") {
            g_renderer.draw_background(primary.get(), snap_bg_style, (Uint8)(255.0f * vignette_str));
        }

        // 2. Draw matte borders if enabled (solid black base layer) - ONLY if NOT color-matched or blurred!
        if (has_matting && !snap_matte_color && !snap_blurred) {
            g_renderer.draw_matte_borders(rect);
        }

        // 3. Color-matched matte if enabled (opaque and edge-to-edge if enabled)
        if (snap_matte_color && primary) {
            g_renderer.draw_color_matched_matte(rect,
                primary->matte_r, primary->matte_g, primary->matte_b, matte_op);
        }

        // 4. Draw bias lighting if enabled
        if (has_bias) {
            int bw_param = has_border ? border_w : 0;
            g_renderer.draw_bias_lighting(rect, layout.slideshow_area, primary->avg_r, primary->avg_g, primary->avg_b,
                bias_strength, (float)item_timer, anim_speed, style, bw_param, snap_glow_depth);
        }

        // 5. Draw border if enabled
        if (has_border) {
            g_renderer.draw_border(rect, snap_border_mode, primary->avg_r, primary->avg_g, primary->avg_b, border_w, primary->filename);
        }

        // 6. Draw texture
        SDL_FRect dst = {(float)rect.x, (float)rect.y, (float)rect.w, (float)rect.h};
        SDL_RenderTexture(renderer, primary->texture, nullptr, &dst);
    }

    // Reset render target back to default (screen)
    SDL_SetRenderTarget(renderer, nullptr);
    return target;
}

struct PlaylistSettings {
    int cooldown_days, window_days, videos_per_photos;
    bool photos_only, videos_only, shuffle;
    bool on_this_day, people, animals, twin_portraits;

    bool operator==(const PlaylistSettings&) const = default;
};

static PlaylistSettings playlist_settings() {
    std::shared_lock lock(g_config_mtx);
    return {g_cfg.cooldown_days, g_cfg.scan_window_days, g_cfg.videos_per_photos,
            g_cfg.play_just_photos, g_cfg.play_just_videos, g_cfg.shuffle,
            g_cfg.on_this_day_enabled, g_cfg.show_people_faces, g_cfg.keep_animals,
            g_cfg.twin_portrait_enabled && g_cfg.collage_enabled};
}

static std::vector<MediaItem> filter_playlist(const std::vector<MediaItem>& items,
                                              const PlaylistSettings& settings) {
    const auto cooldown_days = settings.cooldown_days;
    const auto window_days = settings.window_days;
    if (settings.on_this_day) {
        std::time_t t = std::time(nullptr);
        struct tm tm_buf;
        std::tm* today = localtime_r(&t, &tm_buf);
        int today_m = today ? (today->tm_mon + 1) : 5;
        int today_d = today ? today->tm_mday : 22;

        std::vector<MediaItem> anniversary_items;
        for (const auto& item : items) {
            if (auto date = get_item_date(item)) {
                auto [y, m, d] = *date;
                if (m == today_m && d == today_d) {
                    anniversary_items.push_back(item);
                }
            }
        }

        if (!anniversary_items.empty()) {
            g_logger.info("ON_THIS_DAY: Found {} anniversary items for month={} day={}!", anniversary_items.size(), today_m, today_d);
            std::vector<MediaItem> anniversary_filtered;
            for (const auto& item : anniversary_items) {
                if (item.type != "video") {
                    bool has_people = false, has_animals = false, is_doc = false;
                    classify_media_item(item, has_people, has_animals, is_doc);
                    if (is_doc) continue;
                    
                    if ((settings.people || settings.animals) &&
                        !(settings.people && has_people) && !(settings.animals && has_animals)) continue;
                }
                anniversary_filtered.push_back(item);
            }
            if (!anniversary_filtered.empty()) {
                return anniversary_filtered;
            }
            g_logger.warn("ON_THIS_DAY: Anniversary items were all filtered out by category toggles. Falling back to normal playlist.");
        } else {
            g_logger.info("ON_THIS_DAY: No anniversary items matching today's month={} day={}. Falling back to normal playlist.", today_m, today_d);
        }
    }

    std::vector<MediaItem> filtered;
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    
    int seasonal_count = 0;
    for (const auto& item : items) {
        if (is_item_in_seasonal_window(item, window_days)) {
            seasonal_count++;
        }
    }
    
    // Reuse the seasonal and category filters on each cooldown-degradation pass.
    // drop_flags[i] != 0 => excluded regardless of the current cooldown.
    std::vector<uint8_t> drop_flags;
    drop_flags.reserve(items.size());
    for (const auto& item : items) {
        bool drop = false;
        if (!is_item_in_seasonal_window(item, window_days)) {
            drop = true; // seasonal window
        } else if (item.type != "video") { // only filter images, keep videos
            bool has_people = false;
            bool has_animals = false;
            bool is_doc = false;
            classify_media_item(item, has_people, has_animals, is_doc);
            if (is_doc) {
                drop = true;
            } else if (!settings.people && has_people) {
                drop = true;
            } else if (!settings.animals && has_animals) {
                drop = true;
            }
        }
        drop_flags.push_back(drop ? 1 : 0);
    }

    int target_min = std::min(15, seasonal_count);
    int current_cooldown = cooldown_days;

    while (true) {
        filtered.clear();
        int64_t cutoff = now - (static_cast<int64_t>(current_cooldown) * 86400);
        for (size_t i = 0; i < items.size(); ++i) {
            if (drop_flags[i]) continue;
            // Cooldown is the only filter that varies between passes.
            if (current_cooldown > 0 && items[i].last_shown >= cutoff) {
                continue;
            }
            filtered.push_back(items[i]);
        }
        
        if (std::ssize(filtered) >= target_min || current_cooldown <= 0) {
            break;
        }
        
        int next_cooldown = current_cooldown / 2;
        if (next_cooldown == current_cooldown || next_cooldown < 1) {
            current_cooldown = 0;
        } else {
            current_cooldown = next_cooldown;
        }
        g_logger.info("filter_playlist: Cooldown of {} days resulted in only {} items. Degrading to {} days to maintain variety.",
            cooldown_days, filtered.size(), current_cooldown);
    }
    
    return filtered;
}

static void organize_playlist(std::vector<MediaItem>& eligible, int videos_per_photos, bool play_just_photos, bool play_just_videos, bool shuffle_enabled) {
    if (eligible.empty()) return;

    if (play_just_photos || (videos_per_photos <= 0 && !play_just_videos)) {
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
        double photos_per_video = 10.0 / static_cast<double>(videos_per_photos);

        for (auto& item : eligible) {
            if (item.type == "video") videos.push_back(std::move(item));
            else photos.push_back(std::move(item));
        }

        if (shuffle_enabled) {
            std::mt19937_64 rng_photo(make_entropy_seed());
            std::shuffle(photos.begin(), photos.end(), rng_photo);
            std::mt19937_64 rng_video(make_entropy_seed());
            std::shuffle(videos.begin(), videos.end(), rng_video);
        }

        // Cap the number of videos based on videos_per_photos ratio to prevent clustering when pools are highly skewed
        if (!photos.empty() && !videos.empty()) {
            double target_video_ratio = static_cast<double>(videos_per_photos) / 10.0;
            size_t max_videos = static_cast<size_t>(std::ceil(static_cast<double>(photos.size()) * target_video_ratio));
            if (videos.size() > max_videos) {
                videos.resize(max_videos);
            }
        }

        size_t final_photos_size = photos.size();
        size_t final_videos_size = videos.size();

        eligible.clear();
        if (videos.empty()) {
            eligible = std::move(photos);
        } else if (photos.empty()) {
            eligible = std::move(videos);
        } else {
            // Interleave videos evenly between photos so no two videos are adjacent
            double spacing = (double)photos.size() / (double)videos.size();
            if (spacing < 1.0) spacing = 1.0;

            size_t p_idx = 0;
            size_t v_idx = 0;
            double rand_start = shuffle_enabled ? (static_cast<double>(make_entropy_seed() % 1000) / 1000.0) : 0.5;
            double next_video_target = rand_start * spacing;

            while (p_idx < photos.size()) {
                eligible.push_back(std::move(photos[p_idx]));
                p_idx++;

                if (v_idx < videos.size() && (double)p_idx >= next_video_target) {
                    eligible.push_back(std::move(videos[v_idx]));
                    v_idx++;
                    next_video_target += spacing;
                }
            }

            while (v_idx < videos.size()) {
                eligible.push_back(std::move(videos[v_idx++]));
            }
        }

        g_logger.info("Playlist organized: {} photos + {} videos = {} total (configured ratio: {} videos per 10 photos, spacing: {:.2f} photos per video)",
            final_photos_size, final_videos_size, eligible.size(), videos_per_photos, photos_per_video);
    }
}

static void update_image_metadata(MediaItem& item, const ImageData& data) {
    item.width = data.width;
    item.height = data.height;
    item.exif_rotation = data.exif_rotation;
    item.is_camera = data.is_camera ? 1 : 0;
    item.creation_time = data.creation_time;
    item.latitude = data.latitude;
    item.longitude = data.longitude;
    item.has_gps = data.has_gps;
}

static void mark_item_shown(const std::string& path) {
    const auto now = static_cast<int64_t>(std::time(nullptr));
    {
        std::lock_guard lock(g_playlist_mtx);
        for (auto* items : {&g_eligible, &g_scanned_items}) {
            for (auto& item : *items) {
                if (item.path == path) item.last_shown = now;
            }
        }
    }
    if (g_cache) g_cache->mark_shown(path);
}

static std::vector<MediaItem> build_playlist(const std::vector<MediaItem>& scanned,
                                             const PlaylistSettings& settings) {
    auto items = filter_playlist(scanned, settings);
    organize_playlist(items, settings.videos_per_photos, settings.photos_only,
                      settings.videos_only, settings.shuffle);
    return items;
}

static bool rebuild_playlist(const PlaylistSettings& settings) {
    std::vector<MediaItem> scanned;
    uint64_t generation;
    {
        std::lock_guard lock(g_playlist_mtx);
        scanned = g_scanned_items;
        generation = playlist_generation;
    }
    auto items = build_playlist(scanned, settings);
    std::lock_guard lock(g_playlist_mtx);
    if (generation != playlist_generation) {
        // A watchman scan published newer source data while we were filtering.
        // Apply this settings change again against that data on the next frame.
        g_config_changed.store(true);
        return false;
    }
    std::string path;
    if (!g_eligible.empty()) path = g_eligible[current_idx].path;
    const auto found = std::find_if(items.begin(), items.end(),
        [&](const MediaItem& item) { return item.path == path; });
    current_idx = found == items.end() ? 0 : static_cast<int>(found - items.begin());
    g_eligible = std::move(items);
    ++playlist_generation;
    return true;
}

struct PlaylistSelection {
    uint64_t generation;
    int index;
    int count;
    MediaItem item;
    std::optional<MediaItem> twin;

    // The playlist lock must be held while validating or applying the selection.
    bool matches() const {
        if (generation != playlist_generation || index != current_idx ||
            g_eligible.empty() || g_eligible[index].path != item.path) return false;
        return !twin || g_eligible[(index + 1) % count].path == twin->path;
    }
};

static PlaylistSelection select_media(bool twin_enabled, bool pair_portraits) {
    // Caller holds g_playlist_mtx. Keep portrait selection adjacent for the
    // existing slideshow and HTTP index semantics.
    if (g_eligible.empty()) return {playlist_generation, 0, 0, {}, {}};
    const int count = std::ssize(g_eligible);
    if (current_idx < 0 || current_idx >= count) current_idx = 0;
    std::optional<MediaItem> twin;
    if (twin_enabled) {
        if (const auto partner = find_twin_portrait(g_eligible, current_idx, pair_portraits)) {
            if (*partner != current_idx + 1) {
                std::swap(g_eligible[current_idx + 1], g_eligible[*partner]);
                ++playlist_generation;
            }
            twin = g_eligible[current_idx + 1];
        }
    }
    return {playlist_generation, current_idx, count, g_eligible[current_idx], std::move(twin)};
}

static void advance_playlist(int step, const PlaylistSelection& selection) {
    std::unique_lock lock(g_playlist_mtx);
    const auto still_selected = [&] {
        return !g_eligible.empty() && g_eligible[current_idx].path == selection.item.path;
    };
    const auto next_index = [&] {
        const int count = std::ssize(g_eligible);
        const bool still_a_pair = selection.twin && current_idx + 1 < count &&
            selection.twin->path == g_eligible[current_idx + 1].path;
        const int offset = (std::abs(step) == 2 && !still_a_pair) ? step / 2 : step;
        return current_idx + offset;
    };

    // A watchman refresh can move an active clip while its decoder stops.
    // Advance that clip at its new index if the refresh preserved it.
    if (!still_selected()) return;
    const int count = std::ssize(g_eligible);
    const int next = next_index();
    if (step < 0 || next < count) {
        current_idx = (next % count + count) % count;
        return;
    }

    auto scanned = g_scanned_items;
    const auto generation = playlist_generation;
    lock.unlock();
    auto items = build_playlist(scanned, playlist_settings());
    if (items.size() > 1 && items.front().path == selection.item.path) std::swap(items[0], items[1]);
    lock.lock();
    if (generation != playlist_generation) {
        if (still_selected()) current_idx = next_index() % std::ssize(g_eligible);
        return;
    }
    g_eligible = std::move(items);
    current_idx = 0;
    ++playlist_generation;
}

static ScanResult scan_gallery(const std::string& media_dir, int depth,
                               std::function<void(int)> progress, std::stop_token stop) {
    auto combined = scan_directory(media_dir, depth, std::move(progress), stop);
    std::string cloud_dir;
    {
        std::shared_lock lock(g_config_mtx);
        if (g_cfg.google_photos_enabled)
            cloud_dir = g_cfg.google_photos_cache_dir.empty() ? "/app/cache/google_photos" : g_cfg.google_photos_cache_dir;
    }
    std::error_code error;
    if (!cloud_dir.empty() && !stop.stop_requested() && std::filesystem::is_directory(cloud_dir, error) &&
        !std::filesystem::equivalent(media_dir, cloud_dir, error)) {
        auto selected = scan_directory(cloud_dir, 1, nullptr, stop);
        std::unordered_set<std::string_view> seen;
        for (const auto& item : combined.items) seen.insert(item.path);
        std::erase_if(selected.items, [&](const auto& item) { return seen.contains(item.path); });
        combined.items.insert(combined.items.end(), std::make_move_iterator(selected.items.begin()),
                              std::make_move_iterator(selected.items.end()));
        combined.complete = combined.complete && selected.complete;
    }
    return combined;
}

static void watchman_loop(std::stop_token stop) {
    g_logger.info("Watchman: Background watchman thread started.");
    
    // Get current day of year
    time_t last_check_time = std::time(nullptr);
    struct tm tm_buf;
    struct tm* last_check_tm = localtime_r(&last_check_time, &tm_buf);
    int last_yday = last_check_tm ? last_check_tm->tm_yday : 0;
    unsigned cloud_revision = g_google_photos.media_revision();
    
    std::mutex wait_mutex;
    std::condition_variable_any wake;
    std::unique_lock wait_lock(wait_mutex);
    auto retry_after = std::chrono::steady_clock::time_point::min();
    while (!stop.stop_requested()) {
        wake.wait_for(wait_lock, stop, std::chrono::seconds(10), [] { return false; });
        if (stop.stop_requested()) break;

        time_t now = std::time(nullptr);
        struct tm curr_tm;
        struct tm* tm_res = localtime_r(&now, &curr_tm);
        
        const unsigned photo_revision = g_google_photos.media_revision();
        bool needs_media;
        {
            std::lock_guard lock(g_playlist_mtx);
            needs_media = g_eligible.empty();
        }
        if (tm_res && (curr_tm.tm_yday != last_yday || photo_revision != cloud_revision || needs_media)) {
            const auto attempt = std::chrono::steady_clock::now();
            if (attempt < retry_after) continue;
            retry_after = attempt + std::chrono::minutes(1);
            if (g_offline_mode.load() && !needs_media) {
                g_logger.warn("Watchman: System is in Offline Recovery Mode. Skipping midnight temporal window shift.");
                continue;
            }
            g_logger.info("Watchman: Refreshing the playlist for a date change or new cloud media.");
            
            std::string media_dir;
            int depth, screen_w, screen_h;
            {
                std::shared_lock lock(g_config_mtx);
                media_dir = g_cfg.media_dir;
                depth = g_cfg.scan_depth;
                screen_w = g_cfg.screen_w;
                screen_h = g_cfg.screen_h;
            }

            ScanResult scan;
            try {
                scan = scan_gallery(media_dir, depth, nullptr, stop);
            } catch (const std::exception& e) {
                g_logger.error("Watchman: media scan failed ({}: {}); keeping the existing playlist.",
                               typeid(e).name(), e.what());
                continue;
            } catch (...) {
                g_logger.error("Watchman: media scan failed with an unknown exception; keeping the existing playlist.");
                continue;
            }
            if (stop.stop_requested()) break;
            if (!scan.complete) {
                g_logger.warn("Watchman: No complete media scan available. Keeping the playlist and cache.");
                continue;
            }
            auto& scanned = scan.items;
            g_logger.info("Watchman: Background scan complete. Scanned {} items. Caching metadata...", scanned.size());

            try {
                if (g_cache) {
                    std::vector<MediaItem> pending;
                    pending.reserve(64);
                    bool saved = true;
                    for (auto& mi : scanned) {
                        if (stop.stop_requested()) break;
                        if (g_cache->load_cached(mi)) {
                            mi.cached = true;
                        } else {
                            if (mi.type == "image") {
                                mi.exif_rotation = 1;
                                mi.width = 1920; mi.height = 1080;
                                mi.creation_time = 0;
                            } else {
                                mi.width = screen_w;
                                mi.height = screen_h;
                                mi.duration = 0.0;
                            }
                            // Metadata preprocessing handles image I/O off the watchman thread.
                            pending.push_back(mi);
                            if (pending.size() == 64) {
                                saved = g_cache->upsert_batch(pending, 0);
                                if (!saved) break;
                                pending.clear();
                            }
                        }
                    }
                    if (stop.stop_requested()) break;
                    if (!saved || !g_cache->upsert_batch(pending, 0)) continue;
                }

                const auto settings = playlist_settings();
                auto new_eligible = build_playlist(scanned, settings);
                if (stop.stop_requested()) break;
                {
                    std::lock_guard lock(g_playlist_mtx);
                    if (settings != playlist_settings()) {
                        retry_after = attempt;
                        continue;
                    }

                    std::string current_path;
                    if (!g_eligible.empty()) current_path = g_eligible[current_idx].path;
                    const auto found = std::find_if(new_eligible.begin(), new_eligible.end(),
                        [&](const MediaItem& item) { return item.path == current_path; });
                    current_idx = found == new_eligible.end() ? 0 : static_cast<int>(found - new_eligible.begin());
                    g_scanned_items = std::move(scanned);
                    g_eligible = std::move(new_eligible);
                    ++playlist_generation;
                }
                last_yday = curr_tm.tm_yday;
                cloud_revision = photo_revision;
            } catch (const std::exception& e) {
                g_logger.error("Watchman: post-scan processing failed ({}: {}); keeping the existing playlist.",
                               typeid(e).name(), e.what());
            } catch (...) {
                g_logger.error("Watchman: post-scan processing failed with an unknown exception; keeping the existing playlist.");
            }
        }
    }
    g_logger.info("Watchman: Background watchman thread exiting.");
}

static std::jthread g_watchdog_thread;
static std::atomic<bool> g_watchdog_running{false};

static FILE* g_event_log = nullptr;
static std::mutex g_event_log_mtx;
static size_t g_event_log_bytes = 0;
static constexpr size_t MAX_EVENT_LOG_BYTES = 64 * 1024;
static int64_t g_video_stall_ts = 0;

static void write_event_log(std::string_view line) {
    std::lock_guard lk(g_event_log_mtx);
    if (!g_event_log) return;
    if (g_event_log_bytes + line.size() > MAX_EVENT_LOG_BYTES) {
        if (ftruncate(fileno(g_event_log), 0) == 0) {
            rewind(g_event_log);
            g_event_log_bytes = 0;
        } else {
            return;
        }
    }
    const size_t written = fwrite(line.data(), 1, line.size(), g_event_log);
    g_event_log_bytes += written;
    fflush(g_event_log);
}
static void watchdog_loop() {
    g_logger.info("Watchdog: Software watchdog thread active.");
    g_watchdog_last_time.store(steady_now_ms());
    while (g_watchdog_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!g_watchdog_running.load()) break;

        // Only enforce watchdog if slideshow is active (not paused, screen not blanked, and playlist not empty)
        bool paused = g_slideshow_paused.load();
        bool blanked = g_screen_blanked.load();
        bool is_video = false;
        bool empty = false;
        {
            std::lock_guard<std::mutex> lk(g_playlist_mtx);
            empty = g_eligible.empty();
            if (!empty && current_idx >= 0 && current_idx < std::ssize(g_eligible)) {
                is_video = (g_eligible[current_idx].type == "video");
            }
        }

        // If playing video, SDL3 video decoder controls playback timing, so we skip heartbeat checks
        if (paused || blanked || empty || is_video) {
            // Keep resetting heartbeat while paused/blanked/playing video
            g_watchdog_last_time.store(steady_now_ms());
            continue;
        }

        double delay = 15.0;
        {
            std::shared_lock<std::shared_mutex> lk(g_config_mtx);
            delay = g_cfg.transition_delay;
        }

        // Allow up to 3x transition delay or a minimum of 45 seconds before forcing restart
        int64_t elapsed = (steady_now_ms() - g_watchdog_last_time.load()) / 1000;
        int64_t max_silent_time = std::max((int64_t)45, (int64_t)(delay * 3));
        if (elapsed > max_silent_time) {
            g_logger.error("[WATCHDOG] CRITICAL: Slideshow loop frozen! Last heartbeat was {} seconds ago. Forcing restart...", (int)elapsed);
            write_event_log(std::format("WATCHDOG_TRIP: elapsed={}s item={}\n", (long)elapsed, current_idx));
            trigger_error(809); // E809: WATCHDOG_FORCED_RESTART
            // Restore physical display power before exiting
            set_display_power(true);
            sync();
            pitrove::safe_mode::record_crash();
            _exit(99); // Force exit immediately to let Docker compose restart us
        }
    }
    g_logger.info("Watchdog: Software watchdog thread exiting.");
}

static std::jthread g_keepalive_thread;
static std::atomic<bool> g_keepalive_running{false};

static void keepalive_loop() {
    g_logger.info("Keepalive: Background connection monitoring thread active.");
    
    std::string gateway;
    std::string interface;
    int interval_secs = 120;
    
    {
        std::shared_lock<std::shared_mutex> lk(g_config_mtx);
        gateway = g_cfg.keepalive_gateway;
        interface = g_cfg.keepalive_interface;
        interval_secs = g_cfg.keepalive_interval;
    }
    
    if (gateway.empty() || interface.empty()) {
        g_logger.error("Keepalive: Gateway or interface is empty. Monitoring disabled.");
        return;
    }
    
    g_logger.info("Keepalive: Monitoring connection to {} on {} every {} seconds.", 
                  gateway.c_str(), interface.c_str(), interval_secs);
                  
    int64_t network_lost_time = 0;
                  
    while (g_keepalive_running.load() && g_running.load()) {
        // Poll faster (every 15 seconds) when network is down to catch recovery sooner
        int poll_secs = (network_lost_time == 0) ? interval_secs : 15;
        for (int i = 0; i < poll_secs && g_keepalive_running.load() && g_running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!g_keepalive_running.load() || !g_running.load()) break;
        
        int res = -1;
        if (!is_valid_host_or_ip(gateway)) {
            g_logger.warn("Keepalive: Ignoring invalid gateway address '{}'", gateway);
        } else {
            posix_spawn_file_actions_t actions;
            if (posix_spawn_file_actions_init(&actions) == 0) {
                posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
                posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
                posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
                char arg0[] = "ping";
                char arg1[] = "-c";
                char arg2[] = "2";
                char arg3[] = "-W";
                char arg4[] = "3";
                char arg5[] = "--";
                std::string gw_copy = gateway;
                char* const argv[] = {arg0, arg1, arg2, arg3, arg4, arg5, gw_copy.data(), nullptr};
                pid_t pid = -1;
                if (posix_spawn(&pid, "/bin/ping", &actions, nullptr, argv, environ) == 0 && pid > 0) {
                    int status = 0;
                    for (;;) {
                        const pid_t w = waitpid(pid, &status, WNOHANG);
                        if (w == pid) {
                            res = (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : 1;
                            break;
                        }
                        if (w < 0) {
                            if (errno == EINTR) continue;
                            break;
                        }
                        if (!g_keepalive_running.load() || !g_running.load()) {
                            kill(pid, SIGTERM);
                            waitpid(pid, &status, 0);
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                }
                posix_spawn_file_actions_destroy(&actions);
            }
        }
        
        // Also check if NAS mount is reachable — gateway may be up but NAS down
        bool nas_ok = is_nas_online();
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        
        if (res == 0 && nas_ok) {
            // Both gateway and NAS are reachable
            if (network_lost_time != 0) {
                WifiStats ws = read_wifi_stats(interface);
                if (ws.has_data) {
                    g_logger.info("Keepalive: Gateway {} is reachable and NAS mount is healthy. WiFi: quality={}/{}, signal={}dBm, noise={}dBm.",
                                  gateway.c_str(), ws.quality, 255, ws.signal_dbm, ws.noise_dbm);
                } else {
                    g_logger.info("Keepalive: Gateway {} is reachable and NAS mount is healthy. Connection restored.", gateway.c_str());
                }
                network_lost_time = 0;
                if (is_error_active(519)) clear_error(519);
            }
        } else {
            // Gateway unreachable OR NAS unreachable — track downtime
            if (network_lost_time == 0) {
                network_lost_time = now;
                WifiStats ws = read_wifi_stats(interface);
                
                std::string reason;
                if (res != 0) {
                    reason = "Gateway unreachable (ping failed)";
                } else {
                    reason = "NAS mount unreachable (gateway reachable)";
                }
                
                if (ws.has_data) {
                    g_logger.warn("Keepalive: {}. WiFi: quality={}/{}, signal={}dBm, noise={}dBm, missed_beacons={}u.",
                                  reason.c_str(), ws.quality, 255, ws.signal_dbm, ws.noise_dbm, ws.missed_beacons);
                } else {
                    g_logger.warn("Keepalive: {}. Start tracking downtime.", reason.c_str());
                }
                
            }

            if (now - network_lost_time >= 180 && !is_error_active(519)) {
                trigger_error(519);
                g_logger.warn("Keepalive: Network unavailable for 3 minutes. The host watchdog handles recovery.");
            }
        }
    }
    g_logger.info("Keepalive: Background connection monitoring thread exiting.");
}



int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    bool run_config = false;
    bool run_restart = false;
    bool run_organize = false;
    std::string undo_journal;
    std::string config_path;
    std::string organize_path;
    bool in_place = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--config-wizard") {
            run_config = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config_path = argv[i + 1];
                i++;
            }
        } else if (arg == "--config") {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config_path = argv[i + 1];
                i++;
            } else {
                run_config = true;
            }
        } else if (arg == "--restart") {
            run_restart = true;
        } else if (arg == "--organize") {
            run_organize = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                organize_path = argv[i + 1];
                i++;
            }
        } else if (arg == "--undo-organize" && i + 1 < argc) {
            undo_journal = argv[++i];
        } else if (arg == "--in-place") {
            in_place = true;
        } else if (arg == "--selftest-media") {
            if (i + 1 < argc) {
                const auto image = ImageLoader::decode_rgba(ImageLoader::read_file_to_buffer(argv[i+1]));
                if (!image.valid) {
                    fputs("Could not decode image\n", stderr);
                    return 1;
                }
                printf("%dx%d RGBA\n", image.width, image.height);
                return 0;
            }
        }
    }

    if (!undo_journal.empty()) {
        return undo_organize(undo_journal) ? 0 : 1;
    }
    if (run_organize) {
        if (organize_path.empty()) {
            fprintf(stderr, "Error: Missing target folder. Usage: piTrove --organize <folder_path> [--in-place]\n");
            return 1;
        }
        Config archive_config;
        if (!config_path.empty()) archive_config.load(config_path);
        bool success = organize_media_archive(organize_path, in_place, archive_config.ignore_folders);
        return success ? 0 : 1;
    }

    std::string exe_dir = get_exe_dir();
    std::string lock_path;
    const char* env_lock = std::getenv("PITROVE_LOCK_PATH");
    if (env_lock && env_lock[0] != '\0') {
        lock_path = env_lock;
    } else {
        lock_path = exe_dir + "/piTrove.lock";
    }

    if (run_restart) {
        FILE* lf = fopen(lock_path.c_str(), "r");
        if (!lf && lock_path != "/tmp/piTrove.lock") {
            lf = fopen("/tmp/piTrove.lock", "r");
        }
        if (lf) {
            pid_t old_pid = 0;
            if (fscanf(lf, "%d", &old_pid) == 1 && old_pid > 0 && old_pid != getpid()) {
                printf("\033[1;33m[INFO]\033[0m Gracefully terminating running instance (PID %d)...\n", old_pid);
                const int result = kill(old_pid, SIGTERM);
                fclose(lf);
                return result == 0 ? 0 : 1;
            }
            fclose(lf);
        }
        std::fputs("No running piTrove instance found.\n", stderr);
        return 1;
    }

    if (run_config) {
        if (config_path.empty()) {
            const char* candidates[] = {"/app/config/config.toml", "config/config.toml", "src/config/config.toml",
                "/home/pi/piTrove/src/config/config.toml", "./src/config/config.toml"};
            for (const auto& c : candidates) {
                if (file_exists(c)) { config_path = c; break; }
            }
            if (config_path.empty()) {
                config_path = "src/config/config.toml";
            }
        }
        
        g_cfg.load(config_path);
        config_wizard(config_path);
        
        if (g_config_changed.load()) {
            printf("\n  \033[1;32m[OK]\033[0m Configuration updated successfully.\n");
            printf("  \033[1;33m[NOTICE]\033[0m Use \033[1;36mpitrove restart\033[0m to apply your new settings.\n\n");
        }
        return 0;
    }

    // --- Single-instance lock ---
    int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        lock_path = "/tmp/piTrove.lock";
        lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
    }
    if (lock_fd < 0) { fprintf(stderr, "Failed to open lock file %s\n", lock_path.c_str()); return 1; }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "Another instance is already running\n");
        close(lock_fd); return 1;
    }
    
    // Write our PID into the lock file for future --restart usage
    [[maybe_unused]] auto trunc_rc = ftruncate(lock_fd, 0);
    [[maybe_unused]] auto pid_rc = dprintf(lock_fd, "%d\n", getpid());

    {
        int ev_fd = open("/app/cache/run/events.log", O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (ev_fd >= 0) {
            struct stat ev_st{};
            if (fstat(ev_fd, &ev_st) == 0 && S_ISREG(ev_st.st_mode)) {
                if (ev_st.st_size > 32 * 1024 && ftruncate(ev_fd, 0) == 0) {
                    lseek(ev_fd, 0, SEEK_SET);
                    g_event_log_bytes = 0;
                } else {
                    g_event_log_bytes = static_cast<size_t>(std::max<off_t>(0, ev_st.st_size));
                }
                g_event_log = fdopen(ev_fd, "ae");
                if (!g_event_log) close(ev_fd);
            } else {
                close(ev_fd);
            }
        }
        write_event_log("=== piTrove started ===\n");
    }
    pitrove::safe_mode::init();
    signal(SIGBUS, crash_handler);
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    // SIGTERM and SIGINT trigger graceful shutdown — let main loop exit and cleanup
    struct sigaction sa_term = {};
    sa_term.sa_handler = [](int sig) {
        const char* sig_name = (sig == SIGTERM) ? "SIGTERM" : (sig == SIGINT ? "SIGINT" : "UNKNOWN");
        constexpr char message[] = "SHUTDOWN_TRIGGER: Caught OS signal ";
        (void)write(STDERR_FILENO, message, sizeof(message) - 1);
        (void)write(STDERR_FILENO, sig_name, sig == SIGTERM ? 7 : 6);
        (void)write(STDERR_FILENO, "\n", 1);
        g_running.store(false);
    };
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGTERM, &sa_term, nullptr);
    sigaction(SIGINT, &sa_term, nullptr);
    std::set_terminate(terminate_handler);

    g_logger.info("=== piTrove v{} started {} ===", VERSION, get_timestamp().c_str());

    // --- Config ---
    g_cfg.parse_args(argc, argv);
    if (config_path.empty()) {
        const char* candidates[] = {"/app/config/config.toml", "config/config.toml", "src/config/config.toml",
            "/home/pi/piTrove/src/config/config.toml", "./src/config/config.toml"};
        for (const auto& c : candidates) {
            if (file_exists(c)) { config_path = c; break; }
        }
    }
    if (!config_path.empty()) {
        g_logger.info("Loading config: {}", config_path.c_str());
        if (!g_cfg.load(config_path)) g_logger.warn("Failed to load config from {}, using defaults", config_path.c_str());
    } else {
        g_logger.warn("No config file found, using defaults");
    }

    std::jthread thermal_thread;
    spawn_thread_safe(thermal_thread, "thermal", pitrove::thermal::monitor_thread);

    // Apply the configured timezone to libc. The container ships tzdata but sets no TZ
    // environment variable, so every localtime_r() call was resolving to UTC. That made
    // the watchman's day-rollover fire at 00:00 UTC (17:00 in America/Los_Angeles) and ran
    // the full daily rescan in the middle of the evening. Absent/empty timezone = unchanged.
    if (!g_cfg.timezone.empty()) {
        setenv("TZ", g_cfg.timezone.c_str(), 1);
        tzset();
        g_logger.info("Timezone set to '{}'", g_cfg.timezone.c_str());
    }

    
    // Initialize auth subsystem
    pitrove::auth::init();

    // Check for safe mode due to crash loops
    const bool safe_mode_active = pitrove::safe_mode::should_enter_safe_mode();
    if (safe_mode_active) {
        g_cfg.ken_burns = false;
        g_cfg.bias_lighting = false;
        g_cfg.blurred_background = false;
        g_cfg.diagnostics_hud_enabled = true;
        g_logger.warn("Entering safe mode due to repeated crashes");
    }

    // Verify media root is read-only if enforced
    if (g_cfg.enforce_read_only_media && !g_cfg.media_dir.empty()) {
        struct statvfs st {};
        if (statvfs(g_cfg.media_dir.c_str(), &st) == 0 && !(st.f_flag & ST_RDONLY)) {
            g_logger.error("Media root is not mounted read-only");
        }
    }

    if (g_cfg.rotation != 0) {
        g_logger.warn("Deprecated config key 'rotation' found - KMSDRM pipeline ignores this value");
    }

    // Validate that key directories exist
    {
        auto check_dir = [](const std::string& name, const std::string& dir) {
            if (dir.empty()) return;
            struct stat st;
            if (stat(dir.c_str(), &st) != 0) {
                g_logger.warn("Config directory '{}' ({}) does not exist - will create on first access", name.c_str(), dir.c_str());
            } else if (!S_ISDIR(st.st_mode)) {
                g_logger.warn("Config '{}' path ({}) is not a directory", name.c_str(), dir.c_str());
            }
        };
        check_dir("media_dir", g_cfg.media_dir);
        check_dir("cache_dir", g_cfg.cache_dir);
        check_dir("log_dir", g_cfg.log_dir);
    }



    std::string media_dir;
    std::string cache_dir;
    std::string log_dir;
    int keep_count = 3;
    {
        std::shared_lock<std::shared_mutex> lock(g_config_mtx);
        media_dir = g_cfg.media_dir;
        cache_dir = g_cfg.cache_dir.empty() ? (get_exe_dir() + "/cache") : g_cfg.cache_dir;
        log_dir = g_cfg.log_dir.empty() ? (get_exe_dir() + "/logs") : g_cfg.log_dir;
        keep_count = g_cfg.log_keep_count;
    }

    LogLevel effective_log_level = LogLevel::INFO;
    if (g_cfg.log_level == "debug" || g_cfg.verbose) {
        effective_log_level = LogLevel::DEBUG;
    } else if (g_cfg.log_level == "warn") {
        effective_log_level = LogLevel::WARN;
    } else if (g_cfg.log_level == "error") {
        effective_log_level = LogLevel::ERROR;
    }
    g_logger.init(log_dir, effective_log_level, keep_count);
    g_logger.info("Media dir: {}, Cache dir: {}", media_dir.c_str(), cache_dir.c_str());

    // Verify cache directory available space (E403) and writability (E405)
    {
        std::error_code space_ec;
        std::filesystem::create_directories(cache_dir, space_ec);
        auto space_info = std::filesystem::space(cache_dir, space_ec);
        if (!space_ec) {
            if (space_info.available < 50 * 1024 * 1024) {
                trigger_error(403); // E403: DISK_SPACE_CRITICAL
            } else {
                clear_error(403);
            }
        }

        std::string test_file = cache_dir + "/.write_test";
        FILE* tf = fopen(test_file.c_str(), "w");
        if (!tf) {
            trigger_error(405); // E405: SQLITE_READONLY_DATABASE
        } else {
            fclose(tf);
            unlink(test_file.c_str());
            clear_error(405);
        }
    }

    // Verify if media directory exists and is populated
    bool startup_media_empty = true;
    try {
        if (std::filesystem::exists(media_dir)) {
            for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(media_dir)) {
                startup_media_empty = false;
                break;
            }
        }
    } catch (...) {}
    if (startup_media_empty) {
        trigger_error(101); // E101: NAS_MOUNT_FAILED
    } else {
        clear_error(101);
    }

    // --- Dynamic DRM Probing and Environment Setup ---
    {
        std::lock_guard lock(g_config_mtx);
        std::string probed_card = "/dev/dri/card1";
        int probed_card_index = 1;
        std::string probed_connector = "HDMI-A-1";
        if (g_cfg.drm_card == "auto" || g_cfg.drm_connector == "auto") {
            std::string card;
            int card_idx = -1;
            std::string conn = probe_connected_connector(card, card_idx);
            if (!conn.empty() && !card.empty() && card_idx >= 0) {
                probed_card = card;
                probed_card_index = card_idx;
                probed_connector = conn;
                g_logger.info("DRM Probe: Found active connected connector {} on {} (index {})", conn.c_str(), card.c_str(), card_idx);
            } else {
                g_logger.warn("DRM Probe: No connected connector found. Using default fallbacks: {}, connector: {}", probed_card.c_str(), probed_connector.c_str());
            }
        }

        std::string final_card = (g_cfg.drm_card == "auto") ? probed_card : g_cfg.drm_card;
        std::string final_connector = (g_cfg.drm_connector == "auto") ? probed_connector : g_cfg.drm_connector;

        setenv("SDL_VIDEO_KMSDRM_DEVICE", final_card.c_str(), 1);

        std::string index_str;
        if (g_cfg.drm_card == "auto") {
            index_str = std::format("{}", probed_card_index);
        } else {
            size_t last_num = g_cfg.drm_card.find_last_of("0123456789");
            if (last_num != std::string::npos) {
                size_t first_num = last_num;
                while (first_num > 0 && std::isdigit(g_cfg.drm_card[first_num - 1])) {
                    first_num--;
                }
                index_str = g_cfg.drm_card.substr(first_num, last_num - first_num + 1);
            } else {
                index_str = "1";
            }
        }
        setenv("SDL_KMSDRM_DEVICE_INDEX", index_str.c_str(), 1);

        g_cfg.drm_card = final_card;
        g_cfg.drm_connector = final_connector;

        g_logger.info("KMSDRM Auto-Config: Using card {} (index {}), connector {}", final_card.c_str(), index_str.c_str(), final_connector.c_str());
    }

    // --- SDL3 Init ---
    g_logger.info("Initializing SDL3 ({}x{})...", g_cfg.screen_w, g_cfg.screen_h);
    std::string splash_file = g_cfg.splash_file.empty() ? "src/splash.png" : g_cfg.splash_file;

    if (!g_renderer.init(g_cfg.screen_w, g_cfg.screen_h, g_cfg.fullscreen)) {
        g_logger.error("SDL3 initialization failed"); return 1;
    }
    g_logger.info("SDL3 context created: {}x{}", g_renderer.screen_w, g_renderer.screen_h);

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
    g_renderer.render_splash(1, 0, 0, 0, "INIT", 0, nullptr, false);

    // --- Fast-path: skip scan+cache if DB already exists ---
    std::string db_path = cache_dir + "/cache.db";
    struct stat db_stat{};
    const bool db_exists = (stat(db_path.c_str(), &db_stat) == 0 && db_stat.st_size > 0);

    if (db_exists) {
        auto fast_cache = std::make_unique<CacheManager>();
        if (fast_cache->open(cache_dir)) {
            g_cache = fast_cache.release();
            if (g_cfg.reset_cooldown_on_restart) {
                g_cache->reset_all_cooldowns();
            }
            sqlite3_stmt* stmt = nullptr;
            int load_rc = sqlite3_prepare_v2(g_cache->db,
                "SELECT path, type, w, h, duration, framerate, exif, last_shown, is_camera, "
                "timestamp, creation_time, lat, lon, has_gps FROM cache WHERE bad = 0;",
                -1, &stmt, nullptr);
            if (load_rc == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const unsigned char* raw_path = sqlite3_column_text(stmt, 0);
                    const unsigned char* raw_type = sqlite3_column_text(stmt, 1);
                    if (!raw_path || !raw_type) continue;
                    MediaItem mi;
                    mi.path = (const char*)raw_path;
                    mi.type = (const char*)raw_type;
                    mi.width = sqlite3_column_int(stmt, 2);
                    mi.height = sqlite3_column_int(stmt, 3);
                    mi.duration = sqlite3_column_double(stmt, 4);
                    mi.framerate = sqlite3_column_double(stmt, 5);
                    mi.exif_rotation = sqlite3_column_int(stmt, 6);
                    mi.last_shown = sqlite3_column_int64(stmt, 7);
                    mi.is_camera = sqlite3_column_int(stmt, 8);
                    mi.modified_time = sqlite3_column_int64(stmt, 9);
                    mi.creation_time = sqlite3_column_int64(stmt, 10);
                    mi.latitude = sqlite3_column_double(stmt, 11);
                    mi.longitude = sqlite3_column_double(stmt, 12);
                    mi.has_gps = sqlite3_column_int(stmt, 13) != 0;
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
            g_logger.info("Loaded {} items from cache DB (photos={} videos={})", std::ssize(g_scanned_items), photos, videos);

            if (photos > 0 || videos > 0) {
                g_logger.info("Fast-path: skipping scan, going directly to slideshow");
                clear_error(401);
                clear_error(413);
            } else {
                g_logger.warn("Cache DB loaded 0 valid items. Scanning media.");
            }
        } else {
            g_logger.warn("Failed to open cache DB — will re-scan");
            trigger_error(413); // E413: SQLITE_OPEN_FAILED
        }
    }

    // --- Splash after fast-path check ---
    bool do_scan = g_scanned_items.empty();
    int dot_counter = 0;

    if (!do_scan) {
        // Cache loaded ? show item count briefly
        int cached_total = std::ssize(g_scanned_items);
        g_renderer.render_splash(2, cached_total, cached_total, cached_total, "CACHE", 0, nullptr, false);
        SDL_Delay(800);
    } else {
        // Fresh scan ? show INIT splash
        for (int i = 0; i < 3; i++) {
            g_renderer.render_splash(2, 0, 0, 0, "INIT", 0, nullptr, false);
            SDL_Delay(16);
        }
    }

    // --- PHASE 1: SCAN (simple filesystem walk, like legacy) ---
    // (skip if fast-path cache was valid)

    if (do_scan) {
        g_logger.info("Phase 1: Scanning media...");
        auto scan_start = std::chrono::steady_clock::now();
        g_renderer.render_splash(2, 0, 0, 0, "SCANNING", 0, nullptr, false);

        std::atomic<int64_t> scan_count{0};
        std::atomic<bool> scan_done{false};
        bool scan_complete = false;

        // Thread-safe progress callback updating only the atomic counter
        auto safe_progress_callback = [&](int count) {
            scan_count.store(count, std::memory_order_relaxed);
        };

        int depth = 10;
        { std::shared_lock lk(g_config_mtx); depth = g_cfg.scan_depth; }

        std::jthread scan_thread;
        if (!spawn_thread_safe(scan_thread, "scanner", [&](std::stop_token stop) {
            try {
                auto result = scan_gallery(media_dir, depth, safe_progress_callback, stop);
                scan_complete = result.complete;
                g_scanned_items = std::move(result.items);
            } catch (const std::exception& e) {
                g_logger.error("Startup scan failed: {}", e.what());
            } catch (...) {
                g_logger.error("Startup scan failed.");
            }
            scan_done.store(true);
        })) {
            // Spawn failed: proceed with an empty gallery rather than hang on the splash.
            scan_done.store(true);
        }

        // Main thread polls and renders splash safely on EGL context
        while (!scan_done.load()) {
            if (!g_running.load()) scan_thread.request_stop();
            dot_counter++;
            g_renderer.render_splash(2, (int)scan_count.load(), 0, 0, "SCANNING", dot_counter, nullptr, false);
            SDL_Delay(33); // ~30 FPS throttling
        }

        if (scan_thread.joinable()) {
            scan_thread.join();
        }
        if (!g_running.load()) {
            g_renderer.cleanup_splash();
            g_renderer.cleanup();
            delete g_cache;
            g_cache = nullptr;
            return 0;
        }

        auto scan_end = std::chrono::steady_clock::now();
        auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(scan_end - scan_start).count();
        g_logger.info("Scan complete: {} items in {}d ms", std::ssize(g_scanned_items), (long)scan_ms);

        // --- PHASE 2: CACHE (bulk transaction, render at 0.1s intervals) ---
        g_logger.info("Phase 2: Caching metadata...");

        if (!g_cache) {
            g_cache = new CacheManager();
            if (!g_cache->open(cache_dir)) {
                g_logger.error("Failed to open cache database");
                trigger_error(413); // E413: SQLITE_OPEN_FAILED
                g_renderer.cleanup_splash(); g_renderer.cleanup();
                delete g_cache; return 1;
            }
        }
        if (g_cfg.reset_cooldown_on_restart) {
            g_cache->reset_all_cooldowns();
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

        std::vector<MediaItem> pending;
        pending.reserve(64);
        int cached = 0;
        auto last_render = std::chrono::steady_clock::now() - std::chrono::seconds(1);

        int total_scanned = std::ssize(g_scanned_items);

        for (int i = 0; i < total_scanned; i++) {
            auto& mi = g_scanned_items[i];

            if (g_cache->load_cached(mi)) {
                mi.cached = true;
                cached++;
            } else {
                if (mi.type == "image") {
                    mi.exif_rotation = 1;
                    mi.width = 1920; mi.height = 1080;
                    mi.creation_time = 0;
                } else {
                    mi.width = g_cfg.screen_w;
                    mi.height = g_cfg.screen_h;
                    mi.duration = 0.0;
                }
                pending.push_back(mi);
                if (pending.size() == 64) {
                    if (g_cache->upsert_batch(pending, 0)) cached += static_cast<int>(pending.size());
                    pending.clear();
                }
            }

            auto render_now = std::chrono::steady_clock::now();
            if (std::chrono::duration<float>(render_now - last_render).count() >= 0.1f) {
                dot_counter++;
                g_renderer.render_splash(3, total_scanned, total_scanned, cached, "CACHING",
                    dot_counter, get_display_path(mi.path).c_str(), false);
                last_render = render_now;
            }
        }
        if (g_cache->upsert_batch(pending, 0)) cached += static_cast<int>(pending.size());
        if (scan_complete && g_cfg.scan_window_days == 0) g_cache->prune_missing(g_scanned_items);
        g_logger.info("Cache complete: {} items", cached);
        clear_error(401);
        clear_error(413);

        // Check for date prefixes in g_scanned_items
        bool has_prefix = false;
        for (const auto& item : g_scanned_items) {
            if (parse_filename_date(item.filename)) {
                has_prefix = true;
                break;
            }
            std::filesystem::path p(item.path);
            std::string parent_name = p.parent_path().filename().string();
            int groups[2] = {0, 0};
            int gc = 0;
            size_t idx = 0;
            while (idx < parent_name.size() && gc < 2) {
                while (idx < parent_name.size() && !isdigit(parent_name[idx])) idx++;
                if (idx >= parent_name.size()) break;
                int val = 0;
                int digit_count = 0;
                while (idx < parent_name.size() && isdigit(parent_name[idx])) {
                    if (digit_count < 9) {
                        val = val * 10 + (parent_name[idx] - '0');
                    }
                    digit_count++;
                    idx++;
                }
                groups[gc++] = val;
                if (gc == 1 && idx < parent_name.size() && (parent_name[idx] == '-' || parent_name[idx] == '_')) idx++;
            }
            if (gc >= 2 && groups[1] >= 1 && groups[1] <= 12) {
                has_prefix = true;
                break;
            }
        }

        if (!has_prefix) {
            trigger_error(808); // E808: SEASONAL_WINDOW_FALLBACK
            g_logger.warn("Seasonal Window: No YYYY-MM-DD_ folder or filename prefixes found. Falling back to file creation/modification dates.");
        } else {
            clear_error(808);
        }

        g_renderer.render_splash(3, total_scanned, total_scanned, cached, "CACHING", dot_counter, nullptr, false);
        SDL_Delay(500);
    }

    auto applied_playlist_settings = playlist_settings();
    g_eligible = build_playlist(g_scanned_items, applied_playlist_settings);
    g_logger.info("Initial playlist contains {} items.", g_eligible.size());

    // Household data lives beside config and survives media cache maintenance.
    {
        std::filesystem::path settings;
        { std::shared_lock lock(g_config_mtx); settings = g_cfg.loaded_path; }
        const auto directory = settings.has_parent_path() ? settings.parent_path() : std::filesystem::path(".");
        const auto result = chores::g_store.open((directory / "chores.db").string());
        if (!result.ok()) g_logger.error("CHORES: {}", result.error);
    }

    // Start HTTP Web Remote Dashboard if enabled
    {
        bool http_srv = false;
        int http_prt;
        {
            std::shared_lock lock(g_config_mtx);
            http_srv = g_cfg.http_enabled && g_cfg.web_dashboard_enabled;
            http_prt = g_cfg.http_port;
        }
        if (http_srv) {
            g_logger.info("HTTP: Starting background web remote server on port {}...", http_prt);
            start_http_server(http_prt);
        }
    }

    // Start MQTT subscriber client
    start_mqtt_client();
    // Start Infopanels background sync (News ticker & Google Calendar)
    {
        bool info_on = false, news_on = false, cal_on = false;
        {
            std::shared_lock lk(g_config_mtx);
            info_on = g_cfg.infopanels_enabled;
            news_on = g_cfg.news_enabled;
            cal_on = g_cfg.gcalendar_enabled;
        }
        if (info_on) {
            if (news_on) g_news_ticker.start();
            if (cal_on) g_calendar.start();
            bool st_on = false; { std::shared_lock lk(g_config_mtx); st_on = g_cfg.stockstreamer_enabled; } if (st_on) g_stock_streamer.start();
        }
    }


    // Start Google Photos background sync thread
    g_google_photos.start();

    // Start software watchdog thread
    g_watchdog_running.store(true);
    if (!spawn_thread_safe(g_watchdog_thread, "watchdog", watchdog_loop)) {
        g_watchdog_running.store(false);
    }

    // Start background Wi-Fi keep-alive thread if enabled
    bool keepalive_enabled = false;
    {
        std::shared_lock<std::shared_mutex> lk(g_config_mtx);
        keepalive_enabled = g_cfg.keepalive_enabled;
    }
    if (keepalive_enabled) {
        g_keepalive_running.store(true);
        if (!spawn_thread_safe(g_keepalive_thread, "keepalive", keepalive_loop)) {
            g_keepalive_running.store(false);
        }
    }

    g_logger.info("Starting slideshow with {} items", g_eligible.size());

    g_transition = new TransitionEngine();
    g_transition->set_renderer(&g_renderer);
    g_overlay = new OverlayManager(&g_renderer);
    g_overlay->init();
    g_renderer.set_caption_font(g_overlay->get_font_renderer(), g_overlay->get_font_path());

    // Check if touchscreen mode is enabled and verify physical device presence
    {
        bool touch_enabled = false;
        {
            std::shared_lock lk(g_config_mtx);
            touch_enabled = g_cfg.touch_enabled;
        }
        if (touch_enabled) {
            int touch_count = 0;
            SDL_TouchID* devices = SDL_GetTouchDevices(&touch_count);
            if (devices) {
                SDL_free(devices);
            }
            if (touch_count == 0) {
                g_logger.warn("TOUCH_INPUT: Enabled in config, but SDL_GetTouchDevices detected 0 touch devices.");
                trigger_error(619); // E619: TOUCHSCREEN_DEVICE_NOT_FOUND
            } else {
                g_logger.info("TOUCH_INPUT: Found {} active touch input device(s).", touch_count);
                clear_error(619);
            }
        }
    }

    // Initialize background preload queue
    g_preload = new PreloadQueue(g_cfg.preload_capacity, g_cfg.preload_workers, g_renderer.sdl_renderer);
    g_preload->start();

    {
        std::lock_guard lock(g_playlist_mtx);
        current_idx = g_eligible.empty() ? 0 : static_cast<int>(make_entropy_seed() % g_eligible.size());
    }

    double item_timer = 0.0;
    auto last_frame_time = std::chrono::steady_clock::now();
    std::shared_ptr<ImageData> current_data, current_twin_data, next_data, next_twin_data;
    std::string displayed_path, displayed_twin_path;
    std::optional<PlaylistSelection> transition_selection;
    bool next_is_twin = false;
    SDL_Texture* current_tex = nullptr;
    SDL_Texture* transition_prev_target = nullptr;
    SDL_Texture* transition_next_target = nullptr;
    SDL_Rect fit_rect{0, 0, 0, 0};
    bool transitioning = true;

    auto cancel_transition = [&] {
        if (g_transition) g_transition->reset();
        transitioning = false;
        next_data.reset();
        next_twin_data.reset();
        next_is_twin = false;
        transition_selection.reset();
        if (transition_prev_target) SDL_DestroyTexture(std::exchange(transition_prev_target, nullptr));
        if (transition_next_target) SDL_DestroyTexture(std::exchange(transition_next_target, nullptr));
    };

    struct Playback {
        std::string path;
        bool started = false;
        bool eof_reached = false;
        std::optional<double> displayed_pts;
        Uint64 last_frame_at = 0;
        Uint64 audio_drain_until = 0;
        std::optional<Uint64> paused_at;
        double fallback_due_at = 0.0;
        double last_frame_duration_ms = 0.0;
        bool drew_frame = false;
        SDL_FRect destination{};
        double rotation = 0.0;
    } video;

    auto stop_video = [&] {
        g_video_decoder.stop();
        video = {};
        g_video_stall_ts = 0;
        if (g_video_tex) SDL_DestroyTexture(std::exchange(g_video_tex, nullptr));
        g_video_tex_w = g_video_tex_h = 0;
        g_video_tex_fmt = SDL_PIXELFORMAT_UNKNOWN;
    };

    auto effective_transition = [&] {
        std::shared_lock lock(g_config_mtx);
        if (safe_mode_active && g_cfg.transition_effect == "ken_burns") return std::string("fade");
        return g_cfg.transition_effect;
    };
    std::string transition_effect = effective_transition();
    uint64_t observed_generation = 0;
    auto photo_retry_at = std::chrono::steady_clock::time_point{};
    bool showing_empty = false;

    // --- Start Background Watchman Thread ---
    if (spawn_thread_safe(g_watchman_thread, "watchman", watchman_loop)) {
        g_logger.info("Watchman: Background watchman thread spawned successfully.");
    }

    // --- Start Background Preprocess Thread ---
    start_preprocess_worker();

    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> photo_scene(nullptr, SDL_DestroyTexture);
    std::weak_ptr<ImageData> scene_image, scene_twin;
    std::chrono::steady_clock::time_point scene_due{};
    int scene_width = 0, scene_height = 0;
    double fps_timer = 0.0;
    int frame_counter = 0;
    int active_fps = 60;

    // Splash-to-first-photo fade state
    bool first_photo_fade = true;
    Uint64 fade_start_time = 0;

    while (g_running.load()) {
        if (g_config_changed.exchange(false)) {
            g_logger.info("MAIN_LOOP: Applying updated configuration in memory.");
            transition_effect = effective_transition();
            const auto settings = playlist_settings();
            if (settings != applied_playlist_settings && rebuild_playlist(settings))
                applied_playlist_settings = settings;
            if (g_preload) g_preload->cancel_all();
            if (transitioning) {
                cancel_transition();
                transitioning = true;
            }
            scene_due = {};
            scene_image.reset();
            scene_twin.reset();
        }
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_frame_time).count();
        last_frame_time = now;

        g_watchdog_last_time.store(steady_now_ms());
        // heartbeat_tick() rewrites the health file every call; the consumer only reads it
        // once per 30s and needs 15s granularity, so throttle the filesystem write.
        {
            static int64_t last_hb_write = 0;
            int64_t t = steady_now_ms();
            if (t - last_hb_write >= 5000) {
                last_hb_write = t;
                pitrove::health::heartbeat_tick();
                if (g_cache) g_cache->flush_shown();
            }
        }

        
        // Periodic HDMI sleep/wake schedule check (every 5 seconds)
        static double sleep_schedule_timer = 0.0;
        sleep_schedule_timer += dt;
        if (sleep_schedule_timer >= 5.0) {
            sleep_schedule_timer = 0.0;
            std::string s_time, w_time, tz_name;
            {
                std::shared_lock lk(g_config_mtx);
                s_time = g_cfg.sleep_time;
                w_time = g_cfg.wake_time;
                tz_name = g_cfg.timezone;
            }
            if (!s_time.empty() && !w_time.empty()) {
                int s_h = 0, s_m = 0, w_h = 0, w_m = 0;
                int now_h = 0, now_m = 0;
                if (sscanf(s_time.c_str(), "%d:%d", &s_h, &s_m) == 2 &&
                    sscanf(w_time.c_str(), "%d:%d", &w_h, &w_m) == 2 &&
                    display_local_hm(std::time(nullptr), tz_name, now_h, now_m)) {
                    int now_mins = now_h * 60 + now_m;
                    int sleep_mins = s_h * 60 + s_m;
                    int wake_mins = w_h * 60 + w_m;

                    bool inside_sleep_window = false;
                    if (sleep_mins > wake_mins) { // e.g. 23:00 to 07:00
                        inside_sleep_window = (now_mins >= sleep_mins || now_mins < wake_mins);
                    } else if (sleep_mins < wake_mins) { // e.g. 13:00 to 15:00
                        inside_sleep_window = (now_mins >= sleep_mins && now_mins < wake_mins);
                    }

                    static bool scheduled_asleep = false;
                    if (inside_sleep_window && !scheduled_asleep) {
                        scheduled_asleep = true;
                        g_logger.info("SCHEDULE_SLEEP: Sleep time {} reached. Turning off display power.", s_time.c_str());
                        set_display_power(false);
                    } else if (!inside_sleep_window && scheduled_asleep) {
                        scheduled_asleep = false;
                        g_logger.info("SCHEDULE_WAKE: Wake time {} reached. Turning on display power.", w_time.c_str());
                        set_display_power(true);
                    }
                }
            }
        }

        if (!g_screen_blanked.load() && display_hardware_blanked() && sleep_schedule_timer == 0.0) {
            set_display_power(true);
        }

        // Periodic touch screen check (every 5 seconds)
        static double touch_check_timer = 0.0;
        touch_check_timer += dt;
        if (touch_check_timer >= 5.0) {
            touch_check_timer = 0.0;
            bool touch_enabled = false;
            {
                std::shared_lock lk(g_config_mtx);
                touch_enabled = g_cfg.touch_enabled;
            }
            if (touch_enabled) {
                int touch_count = 0;
                SDL_TouchID* devices = SDL_GetTouchDevices(&touch_count);
                if (devices) {
                    SDL_free(devices);
                }
                if (touch_count == 0) {
                    if (g_active_error_code.load() != 619) {
                        g_logger.warn("TOUCH_INPUT: Detected 0 touch devices.");
                        trigger_error(619); // E619: TOUCHSCREEN_DEVICE_NOT_FOUND
                    }
                } else {
                    clear_error(619);
                }
            } else {
                clear_error(619);
            }
        }

        frame_counter++;
        fps_timer += dt;
        if (fps_timer >= 1.0) {
            active_fps = frame_counter;
            frame_counter = 0;
            fps_timer -= 1.0;
        }

        auto execute_menu_action = [&](int selection) {
            switch (selection) {
                case 0:
                    g_slideshow_paused = !g_slideshow_paused.load();
                    g_logger.info("SLIDESHOW: Play/pause toggled via pop-up menu. Paused = {}", g_slideshow_paused.load() ? "YES" : "NO");
                    break;
                case 1:
                    {
                        bool saved = false;
                        {
                            std::lock_guard lk(g_config_mtx);
                            Config prev = g_cfg;
                            g_cfg.shuffle = !g_cfg.shuffle;
                            const std::string target = !config_path.empty() ? config_path : (g_cfg.loaded_path.empty() ? "/app/config/config.toml" : g_cfg.loaded_path);
                            if (g_cfg.save(target)) {
                                saved = true;
                            } else {
                                g_cfg = std::move(prev);
                                g_logger.error("SLIDESHOW: Failed to save shuffle toggle to {}", target);
                            }
                        }
                        if (saved) {
                            g_config_changed.store(true);
                            g_logger.info("SLIDESHOW: Shuffle toggled via pop-up menu.");
                        }
                    }
                    break;
                case 2:
                    {
                        bool saved = false;
                        {
                            std::lock_guard lk(g_config_mtx);
                            Config prev = g_cfg;
                            double curr = g_cfg.transition_delay;
                            if (curr < 15.0) g_cfg.transition_delay = 30.0;
                            else if (curr < 45.0) g_cfg.transition_delay = 60.0;
                            else if (curr < 90.0) g_cfg.transition_delay = 120.0;
                            else if (curr < 210.0) g_cfg.transition_delay = 300.0;
                            else g_cfg.transition_delay = 10.0;
                            const std::string target = !config_path.empty() ? config_path : (g_cfg.loaded_path.empty() ? "/app/config/config.toml" : g_cfg.loaded_path);
                            if (g_cfg.save(target)) {
                                saved = true;
                            } else {
                                g_cfg = std::move(prev);
                                g_logger.error("SLIDESHOW: Failed to save interval change to {}", target);
                            }
                        }
                        if (saved) {
                            g_config_changed.store(true);
                            g_logger.info("SLIDESHOW: Interval changed via pop-up menu.");
                        }
                    }
                    break;
                case 3:
                    {
                        toggle_display_power();
                        std::string prefix;
                        { std::shared_lock lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; }
                        mqtt_publish(prefix + "/status/screen", g_screen_blanked.load() ? "OFF" : "ON", true);
                        g_logger.info("SLIDESHOW: Screen power toggled via pop-up menu.");
                    }
                    break;
                case 4:
                    if (g_overlay) {
                        g_overlay->menu_active = false;
                    }
                    break;
            }
        };

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
                case SDL_EVENT_QUIT: g_running.store(false); break;
                 case SDL_EVENT_KEY_DOWN:
                    if (set_display_power(true)) {
                        g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
                        std::string prefix;
                        { std::shared_lock lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; }
                        mqtt_publish(prefix + "/status/screen", "ON", true);
                    }
                    if (g_overlay && g_overlay->menu_active) {
                        switch (event.key.key) {
                            case SDLK_ESCAPE:
                            case SDLK_Q:
                                g_overlay->menu_active = false;
                                break;
                            case SDLK_UP:
                                g_overlay->menu_selected = (g_overlay->menu_selected - 1 + 5) % 5;
                                break;
                            case SDLK_DOWN:
                                g_overlay->menu_selected = (g_overlay->menu_selected + 1) % 5;
                                break;
                            case SDLK_RETURN:
                            case SDLK_SPACE:
                                execute_menu_action(g_overlay->menu_selected);
                                break;
                        }
                    } else {
                        switch (event.key.key) {
                            case SDLK_ESCAPE: case SDLK_Q: g_running.store(false); break;
                            case SDLK_RIGHT: case SDLK_SPACE: g_remote_command.store(1); break;
                            case SDLK_LEFT: g_remote_command.store(2); break;
                            case SDLK_P: g_remote_command.store(3); break;
                        }
                    }
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (event.button.which == SDL_TOUCH_MOUSEID) break;
                    if (set_display_power(true)) {
                        g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
                        std::string prefix;
                        { std::shared_lock lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; }
                        mqtt_publish(prefix + "/status/screen", "ON", true);
                    }
                    if (event.button.button == SDL_BUTTON_RIGHT) {
                        if (g_overlay) {
                            g_overlay->menu_active = !g_overlay->menu_active;
                        }
                    } else if (event.button.button == SDL_BUTTON_LEFT) {
                        if (g_overlay) {
                            float mx = event.button.x;
                            float my = event.button.y;
                            bool touch_mode = false;
                            {
                                std::shared_lock lk(g_config_mtx);
                                touch_mode = g_cfg.touch_enabled;
                            }
                            // Route clicks to active overlays (menu, keyboard, nav, PIN keypad)
                            if (g_overlay->menu_active || g_overlay->keyboard_active || g_overlay->nav_overlay_active || g_overlay->pin_active) {
                                g_overlay->handle_touch_click(mx, my);
                            } else {
                                // Check if PIN is configured and not yet unlocked
                                std::string stored_pin;
                                { std::shared_lock<std::shared_mutex> lk(g_config_mtx); stored_pin = g_cfg.pin_hash.empty() ? g_cfg.dashboard_pin : g_cfg.pin_hash; }
                                if (!stored_pin.empty() && !g_overlay->pin_unlocked) {
                                    g_overlay->pin_active = true;
                                    g_logger.info("TOUCH_INPUT: PIN required before showing navigation overlay.");
                                } else if (touch_mode) {
                                    if (g_video_decoder.is_running() || g_video_decoder.has_frames()) {
                                        g_logger.info("TOUCH_INPUT: Touch during video: opening navigation overlay on top of video.");
                                    }
                                    g_overlay->nav_overlay_active = true;
                                    g_overlay->nav_overlay_show_time = SDL_GetTicks();
                                    g_logger.info("TOUCH_INPUT: Activating navigation overlay via click.");
                                } else {
                                    // PIN unlocked or no PIN set, proceed with non-touch click
                                    g_remote_command.store(1);
                                }
                            }
                        }
                    }
                    break;
                case SDL_EVENT_FINGER_DOWN:
                    if (set_display_power(true)) {
                        g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
                        std::string prefix;
                        { std::shared_lock lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; }
                        mqtt_publish(prefix + "/status/screen", "ON", true);
                    }
                    if (g_overlay) {
                        float mx = event.tfinger.x * g_renderer.screen_w;
                        float my = event.tfinger.y * g_renderer.screen_h;
                        bool touch_mode = false;
                        {
                            std::shared_lock lk(g_config_mtx);
                            touch_mode = g_cfg.touch_enabled;
                        }
                        if (g_overlay->menu_active || g_overlay->keyboard_active || g_overlay->nav_overlay_active || g_overlay->pin_active) {
                            g_overlay->handle_touch_click(mx, my);
                        } else if (touch_mode) {
                            // Check if PIN is configured and not yet unlocked
                            std::string stored_pin;
                            { std::shared_lock<std::shared_mutex> lk(g_config_mtx); stored_pin = g_cfg.pin_hash.empty() ? g_cfg.dashboard_pin : g_cfg.pin_hash; }
                            if (!stored_pin.empty() && !g_overlay->pin_unlocked) {
                                g_overlay->pin_active = true;
                                g_logger.info("TOUCH_INPUT: PIN required before showing navigation overlay (finger).");
                            } else {
                                if (g_video_decoder.is_running() || g_video_decoder.has_frames()) {
                                    g_logger.info("TOUCH_INPUT: Finger touch during video: opening navigation overlay on top of video.");
                                }
                                g_overlay->nav_overlay_active = true;
                                g_overlay->nav_overlay_show_time = SDL_GetTicks();
                                g_logger.info("TOUCH_INPUT: Activating navigation overlay via finger touch.");
                            }
                        }
                    }
                    break;
            }
        }

        if (!g_running.load()) break;

        // Check motion sensor cooldown
        {
            bool mqtt_on = false;
            int cooldown = 0;
            { std::shared_lock lk(g_config_mtx); mqtt_on = g_cfg.mqtt_enabled; cooldown = g_cfg.mqtt_motionsensor_cooldown; }
            if (mqtt_on && cooldown > 0) {
                int64_t now_ts = static_cast<int64_t>(std::time(nullptr));
                int64_t last_motion = g_last_motion_time.load();
                if (last_motion > 0 && now_ts - last_motion >= cooldown &&
                    set_display_power(false)) {
                    g_logger.info("Motion sensor cooldown threshold reached ({} seconds). Blanking screen.", cooldown);
                    std::string prefix;
                    { std::shared_lock lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; }
                    mqtt_publish(prefix + "/status/screen", "OFF", true);
                }
            }
        }

        // Handle screen blanking state
        static bool black_frame_presented = false;
        if (g_screen_blanked.load()) {
            showing_empty = false;
            if (video.started || g_video_decoder.is_running() || g_video_decoder.has_frames()) stop_video();
            if (!black_frame_presented && !display_hardware_blanked()) {
                g_renderer.clear(0, 0, 0, 255);
                g_renderer.present();
                black_frame_presented = true;
            }
            SDL_Delay(100);
            continue;
        }

        black_frame_presented = false;
        int cmd = g_remote_command.exchange(0);

        if (cmd == 3) g_slideshow_paused = !g_slideshow_paused.load();
        const bool paused = g_slideshow_paused.load();
        PlaylistSelection selection;
        const bool twin_enabled = applied_playlist_settings.twin_portraits;
        bool playlist_replaced;
        {
            std::lock_guard lock(g_playlist_mtx);
            if (cmd == 5 && !g_eligible.empty()) {
                const int count = std::ssize(g_eligible);
                for (int i = 1; i < count; ++i) {
                    const int index = (current_idx + i) % count;
                    if (g_eligible[index].type == "video") {
                        std::swap(g_eligible[(current_idx + 1) % count], g_eligible[index]);
                        ++playlist_generation;
                        break;
                    }
                }
                cmd = 1;
            }
            playlist_replaced = playlist_generation != observed_generation;
            const bool pair_portraits = cmd != 1 && cmd != 2 &&
                ((transitioning && !transition_selection) || playlist_replaced);
            selection = select_media(twin_enabled, pair_portraits);
            if (!playlist_replaced && current_twin_data && displayed_path == selection.item.path &&
                selection.index + 1 < selection.count &&
                displayed_twin_path == g_eligible[selection.index + 1].path) {
                selection.twin = g_eligible[selection.index + 1];
            }
        }

        if (selection.generation != observed_generation) {
            observed_generation = selection.generation;
            const bool same_photo = displayed_path == selection.item.path &&
                displayed_twin_path == (selection.twin ? selection.twin->path : std::string{});
            if (!same_photo || transitioning) item_timer = 0.0;
            cancel_transition();
            transitioning = !same_photo;
            photo_retry_at = {};
            if (playlist_replaced && g_preload) g_preload->cancel_all();
        }

        if (!selection.count) {
            if (!showing_empty) {
                if (video.started || !g_video_decoder.get_current_path().empty()) stop_video();
                cancel_transition();
                current_data.reset();
                current_twin_data.reset();
                current_tex = nullptr;
                displayed_path.clear();
                displayed_twin_path.clear();
                showing_empty = true;
            }
            g_renderer.clear(0, 0, 0, 255);
            if (g_overlay) {
                auto* fonts = g_overlay->get_font_renderer();
                if (fonts) {
                    const auto font = fonts->load_font(g_overlay->get_font_path(), 24);
                    const auto area = calculate_infopanel_layout(g_renderer.screen_w, g_renderer.screen_h).slideshow_area;
                    fonts->draw_text_fitted(area, *font, "No media matches the slideshow settings", {220, 220, 220, 255});
                }
            }
            g_renderer.present();
            SDL_Delay(100);
            continue;
        }
        showing_empty = false;

        const auto advance = [&](int step) {
            advance_playlist(step, selection);
            transitioning = true;
        };
        const bool shown_twin = current_twin_data && displayed_path == selection.item.path &&
            selection.twin && displayed_twin_path == selection.twin->path;
        if (cmd == 1 || cmd == 2) {
            if (video.started) {
                g_renderer.clear(0, 0, 0, 255);
                g_renderer.present();
                stop_video();
            } else if (selection.item.type == "video" &&
                       g_video_decoder.get_current_path() == selection.item.path) {
                stop_video();
            }
            cancel_transition();
            photo_retry_at = {};
            item_timer = 0.0;
            advance((cmd == 1 ? 1 : -1) * (shown_twin ? 2 : 1));
            continue;
        }

        const bool is_video = selection.item.type == "video";
        if (video.started && (!is_video || video.path != selection.item.path)) stop_video();
        const auto draw_panels = [&](const InfopanelLayout& layout) {
            if (!g_overlay) return;
            const auto path = g_overlay->get_font_path();
            auto* font = g_overlay->get_font_renderer();
            if (layout.show_chores)
                render_chores_panel(g_renderer.sdl_renderer, font, path, layout.chores_area, g_renderer.screen_w, layout.content_top);
            if (layout.show_calendar)
                g_calendar.render(g_renderer.sdl_renderer, font, path, layout.calendar_area, g_renderer.screen_w);
            if (layout.show_stocks)
                g_stock_streamer.render(g_renderer.sdl_renderer, font, path, layout.stocks_area, g_renderer.screen_w);
            if (layout.show_news)
                g_news_ticker.render(g_renderer.sdl_renderer, font, path, layout.news_area, g_renderer.screen_w);
        };

        if (is_video) {
            try {
                if (transitioning) cancel_transition();
                if (!video.started) {
                    if (g_video_decoder.get_current_path() == selection.item.path &&
                        g_video_decoder.has_start_failed()) {
                        stop_video();
                        advance(1);
                        SDL_Delay(50);
                        continue;
                    }
                    int width, height;
                    {
                        std::shared_lock lock(g_config_mtx);
                        width = g_cfg.screen_w;
                        height = g_cfg.screen_h;
                        g_video_decoder.set_av_sync(g_cfg.av_sync);
                    }
                    g_logger.info("Playing video: {}", selection.item.path);
                    if (!g_video_decoder.start(selection.item.path, width, height)) {
                        stop_video();
                        advance(1);
                        SDL_Delay(50);
                        continue;
                    }
                    video.path = selection.item.path;
                    video.started = true;
                    current_data.reset();
                    current_twin_data.reset();
                    current_tex = nullptr;
                    displayed_path.clear();
                    displayed_twin_path.clear();
                }

                const auto ticks = SDL_GetTicks();
                const auto layout = calculate_infopanel_layout(g_renderer.screen_w, g_renderer.screen_h);
                g_video_decoder.set_paused(paused);
                const double frame_ms = 1000.0 * g_video_decoder.get_frame_duration();
                if (paused && !video.paused_at) video.paused_at = ticks;
                if (!paused && video.paused_at) {
                    const auto paused_ms = ticks - *video.paused_at;
                    if (video.last_frame_at) video.last_frame_at += paused_ms;
                    if (video.audio_drain_until) video.audio_drain_until += paused_ms;
                    if (video.fallback_due_at > 0.0) video.fallback_due_at += paused_ms;
                    video.paused_at.reset();
                    g_video_stall_ts = 0;
                }

                if (g_video_decoder.has_start_failed()) {
                    g_logger.error("VIDEO_DEC: Skipping video (open failed): {}", video.path);
                    stop_video();
                    advance(1);
                    SDL_Delay(50);
                    continue;
                }

                auto jump = [&](double pts, double previous, double duration_ms) {
                    return pts > previous + duration_ms / 1000.0 + 2.0;
                };
                auto due = [&](double pts) {
                    return g_video_decoder.get_anchor_wall_ms() +
                           (pts - g_video_decoder.get_anchor_pts()) * 1000.0;
                };

                const bool synced = g_video_decoder.av_sync_ready();
                const auto next_pts = g_video_decoder.next_frame_pts();
                bool frame_due = !video.displayed_pts || (!synced && ticks >= video.fallback_due_at);
                if (!paused && synced && next_pts && video.displayed_pts) {
                    if (jump(*next_pts, *video.displayed_pts, video.last_frame_duration_ms) &&
                        ticks >= video.last_frame_at + video.last_frame_duration_ms) {
                        g_video_decoder.rebase_presentation(*next_pts);
                        frame_due = true;
                    } else {
                        frame_due = ticks >= due(*next_pts);
                    }
                }

                VideoFrame frame;
                const bool buffering = !video.displayed_pts && !g_video_decoder.is_eof() &&
                    g_video_decoder.frame_queue_size() < std::min<size_t>(12, g_video_decoder.get_max_queued_frames());
                if (buffering && !g_video_stall_ts) g_video_stall_ts = ticks;
                const bool got_frame = !paused && frame_due &&
                    (!buffering || ticks - g_video_stall_ts >= 1000) && g_video_decoder.get_frame(frame);
                if (got_frame) {
                    if (!video.displayed_pts) {
                        auto& item = selection.item;
                        const double fps = g_video_decoder.get_fps();
                        const double duration = g_video_decoder.get_video_duration();
                        if (duration > 0.0 && (item.framerate != fps || item.duration != duration)) {
                            item.framerate = fps;
                            item.duration = duration;
                            bool publish;
                            {
                                std::lock_guard lock(g_playlist_mtx);
                                publish = selection.matches();
                                if (publish) g_eligible[selection.index] = item;
                            }
                            if (publish && g_cache) g_cache->upsert(item, 0);
                        }
                    }
                    g_video_decoder.note_presentation_start(frame.pts);
                    if (synced) {
                        while (ticks - due(frame.pts) > 40.0) {
                            const auto following = g_video_decoder.next_frame_pts();
                            const double duration_ms = frame.duration > 0.0 ? 1000.0 * frame.duration : frame_ms;
                            if (!following || jump(*following, frame.pts, duration_ms) || due(*following) > ticks ||
                                !g_video_decoder.get_frame(frame)) break;
                        }
                    }
                    video.displayed_pts = frame.pts;
                    video.last_frame_duration_ms = frame.duration > 0.0 ? 1000.0 * frame.duration : frame_ms;
                    g_video_decoder.set_displayed_pts(frame.pts);
                    g_video_stall_ts = 0;
                    SDL_PixelFormat target_fmt = frame.is_nv12 ? SDL_PIXELFORMAT_NV12 : SDL_PIXELFORMAT_RGBA32;
                    if (!g_video_tex || g_video_tex_w != frame.width || g_video_tex_h != frame.height ||
                        g_video_tex_fmt != target_fmt || g_video_colorspace != frame.colorspace) {
                        if (g_video_tex) SDL_DestroyTexture(g_video_tex);
                        const auto props = SDL_CreateProperties();
                        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, target_fmt);
                        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
                        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, frame.width);
                        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, frame.height);
                        SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER,
                                              frame.is_nv12 ? frame.colorspace : SDL_COLORSPACE_SRGB);
                        g_video_tex = SDL_CreateTextureWithProperties(g_renderer.sdl_renderer, props);
                        SDL_DestroyProperties(props);
                        if (!g_video_tex) g_video_tex = SDL_CreateTexture(g_renderer.sdl_renderer, target_fmt,
                            SDL_TEXTUREACCESS_STREAMING, frame.width, frame.height);
                        g_video_tex_w = frame.width;
                        g_video_tex_h = frame.height;
                        g_video_tex_fmt = target_fmt;
                        g_video_colorspace = frame.colorspace;
                    }
                    if (g_video_tex) {
                        if (frame.is_nv12) {
                            SDL_UpdateNVTexture(g_video_tex, nullptr,
                                frame.data, frame.linesize_y,
                                frame.data_uv, frame.linesize_uv);
                        } else {
                            SDL_UpdateTexture(g_video_tex, nullptr, frame.data, frame.width * 4);
                        }
                    }
                    const auto& area = layout.slideshow_area;
                    const double image_aspect = frame.aspect_ratio > 0 ? frame.aspect_ratio :
                                                static_cast<double>(frame.width) / frame.height;
                    const double radians = frame.rotation * std::numbers::pi / 180.0;
                    const double cosine = std::abs(std::cos(radians));
                    const double sine = std::abs(std::sin(radians));
                    const double h = std::min(area.w / (image_aspect * cosine + sine),
                                              area.h / (image_aspect * sine + cosine));
                    const float w = static_cast<float>(h * image_aspect);
                    const auto height = static_cast<float>(h);
                    const SDL_FRect destination{area.x + (area.w - w) / 2, area.y + (area.h - height) / 2, w, height};
                    video.destination = destination;
                    video.rotation = frame.rotation;
                    if (!video.fallback_due_at || ticks > video.fallback_due_at + 1000.0)
                        video.fallback_due_at = ticks;
                    video.fallback_due_at += frame_ms;
                }

                if (g_video_decoder.is_eof() && !g_video_decoder.has_frames()) {
                    if (!video.eof_reached) {
                        video.eof_reached = true;
                        video.audio_drain_until = ticks + 500;
                    }
                    const bool frame_held = !video.last_frame_at || ticks >= video.last_frame_at + video.last_frame_duration_ms;
                    if (!paused && !got_frame && frame_held &&
                        (ticks >= video.audio_drain_until || !g_video_decoder.has_queued_audio())) {
                        const auto path = video.path;
                        const bool played = video.displayed_pts.has_value();
                        stop_video();
                        if (played) mark_item_shown(path);
                        advance(1);
                        continue;
                    }
                } else if (!paused && !g_video_decoder.has_frames()) {
                    if (!g_video_stall_ts) g_video_stall_ts = ticks;
                    if (ticks - g_video_stall_ts > 30000) {
                        g_logger.error("[VIDEO_STALL] Decoder stuck 30s, forcing recovery");
                        trigger_error(529);
                        write_event_log(std::format("VIDEO_STALL: item={}\n", selection.index));
                        stop_video();
                        advance(1);
                        continue;
                    }
                }

                if (got_frame || paused || !video.drew_frame) {
                    g_renderer.clear(0, 0, 0, 255);
                    if (g_video_tex) {
                        SDL_RenderTextureRotated(g_renderer.sdl_renderer, g_video_tex, nullptr, &video.destination,
                                                 video.rotation, nullptr, SDL_FLIP_NONE);
                    }
                    draw_panels(layout);
                    if (g_overlay) {
                        const int seconds = static_cast<int>(std::ceil(g_video_decoder.get_video_remaining(selection.item.duration)));
                        const auto remaining = std::format("{}:{:02d}", seconds / 60, seconds % 60);
                        g_overlay->draw_all(selection.index, selection.count, &selection.item, nullptr, 0.0,
                            true, active_fps, nullptr, nullptr, remaining, &layout.slideshow_area);
                    }
                    g_renderer.present();
                    video.drew_frame = true;
                    if (got_frame) video.last_frame_at = SDL_GetTicks();
                }
                SDL_Delay(paused ? 16 : 1);
            } catch (const std::exception& error) {
                g_logger.error("VIDEO_DEC: Playback failed: {}", error.what());
                stop_video();
                advance(1);
            }
            continue;
        }

        // --- Image Rendering Handling ---
        double dt_scaled = paused ? 0.0 : dt;
        item_timer += dt_scaled;
        g_item_timer.store(static_cast<float>(item_timer));
        if (transitioning) {
            if (!transition_selection) {
                transition_selection = selection;
                next_is_twin = selection.twin.has_value();
                const auto load_photo = [&](const std::string& path) {
                    auto data = g_preload ? g_preload->try_dequeue(path) : nullptr;
                    if (!data || !data->valid) data = ImageLoader::load(path);
                    ImageLoader::load_texture(data.get(), g_renderer.sdl_renderer);
                    return data;
                };
                next_data = load_photo(selection.item.path);
                if (selection.twin) next_twin_data = load_photo(selection.twin->path);

                bool matches;
                {
                    std::lock_guard lock(g_playlist_mtx);
                    matches = transition_selection->matches();
                }
                if (!matches) {
                    cancel_transition();
                    continue;
                }

                if (next_data->texture && next_is_twin && !next_twin_data->texture) {
                    next_twin_data.reset();
                    next_is_twin = false;
                    transition_selection->twin.reset();
                }
                if (!next_data->texture) {
                    cancel_transition();
                    const auto failures = g_consecutive_failures.fetch_add(1) + 1;
                    if (failures >= 3 && !g_offline_mode.exchange(true)) {
                        g_logger.warn("SLIDESHOW: Entering Offline Recovery Mode after {} load failures.", failures);
                        for (const auto& fallback : {std::string("src/splash.png"), get_exe_dir() + "/src/splash.png",
                                                     get_exe_dir() + "/splash.png"}) {
                            if (!file_exists(fallback)) continue;
                            auto data = load_photo(fallback);
                            if (!data->texture) continue;
                            current_data = std::move(data);
                            current_twin_data.reset();
                            current_tex = current_data->texture;
                            displayed_path.clear();
                            displayed_twin_path.clear();
                            break;
                        }
                    }
                    photo_retry_at = std::chrono::steady_clock::now() +
                        (g_offline_mode.load() ? std::chrono::seconds(30) : std::chrono::seconds(2));
                    continue;
                }

                TransitionEffect effect = TransitionEffect::Fade;
                if (transition_effect == "wipe") effect = TransitionEffect::WipeLeft;
                else if (transition_effect == "ken_burns") effect = TransitionEffect::KenBurns;
                else if (transition_effect == "pixelate") effect = TransitionEffect::Pixelate;
                else if (transition_effect == "dissolve") effect = TransitionEffect::Dissolve;
                float duration, zoom;
                {
                    std::shared_lock lock(g_config_mtx);
                    duration = g_cfg.transition_duration;
                    zoom = g_cfg.ken_burns_zoom;
                }
                if (current_tex) {
                    transition_prev_target = render_state_to_texture(g_renderer.sdl_renderer,
                        g_renderer.screen_w, g_renderer.screen_h, current_data, current_twin_data, item_timer);
                    transition_next_target = render_state_to_texture(g_renderer.sdl_renderer,
                        g_renderer.screen_w, g_renderer.screen_h, next_data, next_twin_data, 0.0);
                }
                g_transition->start(effect, duration, 0, zoom);
                last_frame_time = std::chrono::steady_clock::now();
                dt_scaled = 0.0;
            }

            const bool animate = transition_prev_target && transition_next_target && g_transition->is_active();
            if (animate) g_transition->update(dt_scaled);
            if (!animate || g_transition->get_progress() >= 1.0f) {
                auto target = *transition_selection;
                bool publish;
                {
                    std::lock_guard lock(g_playlist_mtx);
                    publish = target.matches();
                    if (publish) {
                        auto apply = [&](MediaItem& item, const ImageData& data, int index) {
                            update_image_metadata(g_eligible[index], data);
                            item = g_eligible[index];
                            const auto scanned = std::find_if(g_scanned_items.begin(), g_scanned_items.end(),
                                [&](const MediaItem& entry) { return entry.path == item.path; });
                            if (scanned != g_scanned_items.end()) update_image_metadata(*scanned, data);
                        };
                        apply(target.item, *next_data, target.index);
                        if (target.twin) apply(*target.twin, *next_twin_data, (target.index + 1) % target.count);
                    }
                }
                if (publish) {
                    current_data = std::move(next_data);
                    current_twin_data = std::move(next_twin_data);
                    current_tex = current_data->texture;
                    displayed_path = target.item.path;
                    displayed_twin_path = target.twin ? target.twin->path : std::string{};
                    item_timer = 0.0;
                    photo_retry_at = {};
                    clear_error(201);
                    clear_error(101);
                    g_consecutive_failures.store(0);
                    if (g_offline_mode.exchange(false))
                        g_logger.info("SLIDESHOW: Exiting Offline Recovery Mode. Connection to NAS restored.");
                    if (g_cache) {
                        g_cache->upsert(target.item, 0, 1);
                        if (target.twin) g_cache->upsert(*target.twin, 0, 1);
                    }
                    mark_item_shown(target.item.path);
                    if (target.twin) mark_item_shown(target.twin->path);
                }
                cancel_transition();
            } else {
                g_renderer.clear(0, 0, 0, 255);
                g_transition->render(transition_prev_target, transition_next_target, g_renderer.screen_w, g_renderer.screen_h);
                const auto layout = calculate_infopanel_layout(g_renderer.screen_w, g_renderer.screen_h);
                draw_panels(layout);
                const auto& target = *transition_selection;
                if (g_overlay) {
                    g_overlay->draw_all(target.index, target.count, &target.item,
                        target.twin ? &*target.twin : nullptr, 0.0, false, active_fps,
                        next_data.get(), next_twin_data.get(), "", &layout.slideshow_area);
                }
                g_renderer.present();
            }
        } else {
            const auto layout = calculate_infopanel_layout(g_renderer.screen_w, g_renderer.screen_h);
            int scene_fps = 5;
            {
                std::shared_lock lock(g_config_mtx);
                if ((g_cfg.bias_lighting && g_cfg.bias_anim_speed > 0) || g_cfg.bg_style == "pattern")
                    scene_fps = std::clamp(g_cfg.pattern_fps, 5, 30);
            }
            const bool has_photo = current_data && (current_data->texture || current_tex);
            if (has_photo && (!photo_scene || scene_width != g_renderer.screen_w || scene_height != g_renderer.screen_h)) {
                scene_width = g_renderer.screen_w;
                scene_height = g_renderer.screen_h;
                photo_scene.reset(SDL_CreateTexture(g_renderer.sdl_renderer, SDL_PIXELFORMAT_RGBA32,
                    SDL_TEXTUREACCESS_TARGET, scene_width, scene_height));
                if (photo_scene) SDL_SetTextureBlendMode(photo_scene.get(), SDL_BLENDMODE_NONE);
                scene_due = {};
            }
            const bool reuse_scene = has_photo && photo_scene && now < scene_due &&
                scene_image.lock() == current_data && scene_twin.lock() == current_twin_data;
            const bool capture_scene = has_photo && photo_scene && !reuse_scene &&
                SDL_SetRenderTarget(g_renderer.sdl_renderer, photo_scene.get());
            bool rendered = reuse_scene;
            if (reuse_scene) {
                SDL_RenderTexture(g_renderer.sdl_renderer, photo_scene.get(), nullptr, nullptr);
            } else if (current_twin_data && current_twin_data->texture && current_data && current_data->texture) {
                bool has_bias = false;
                bool has_matting = false;
                bool has_border = false;
                std::string snap_border_mode = "off";
                int bias_strength = 110;
                float anim_speed = 0.5f;
                std::string style = "edge_glow";
                int border_w = 10;
                bool snap_blurred, snap_matte_color;
                int snap_glow_depth;
                float matte_op, vignette_str;
                {
                    std::shared_lock lock(g_config_mtx);
                    has_bias = g_cfg.bias_lighting;
                    has_matting = g_cfg.matting;
                    snap_border_mode = g_cfg.border_mode;
                    has_border = (snap_border_mode != "off");
                    bias_strength = g_cfg.bias_strength;
                    anim_speed = g_cfg.bias_anim_speed;
                    style = g_cfg.bias_anim_style;
                    border_w = g_renderer.scale_px(g_cfg.border_width);
                    snap_glow_depth = g_renderer.scale_px(g_cfg.glow_depth);
                    snap_blurred = g_cfg.blurred_background;
                    snap_matte_color = g_cfg.color_matched_matte;
                    matte_op = g_cfg.matte_opacity;
                    vignette_str = g_cfg.vignette_strength;
                }

                g_renderer.clear(0, 0, 0, 255);

                SDL_Rect rect_l, rect_r;
                const auto& area = layout.slideshow_area;
                const int half_w = area.w / 2;
                const SDL_Rect left_area{area.x, area.y, half_w, area.h};
                const SDL_Rect right_area{area.x + half_w, area.y, area.w - half_w, area.h};
                calculate_fit_rect_in_area(current_data->width, current_data->height, left_area.x, left_area.y, left_area.w, left_area.h, rect_l);
                calculate_fit_rect_in_area(current_twin_data->width, current_twin_data->height, right_area.x, right_area.y, right_area.w, right_area.h, rect_r);

                // 1. Draw background based on style
                std::string snap_bg_style;
                { std::shared_lock lk(g_config_mtx); snap_bg_style = g_cfg.bg_style; }
                if (snap_blurred || snap_bg_style != "photo") {
                    g_renderer.draw_background(current_data.get(), snap_bg_style, (Uint8)(255.0f * vignette_str));
                }

                // 2. Draw matte borders if enabled (solid black base layer) - ONLY if NOT color-matched or blurred!
                if (has_matting && !snap_matte_color && !snap_blurred) {
                    g_renderer.draw_matte_borders(rect_l, left_area);
                    g_renderer.draw_matte_borders(rect_r, right_area);
                }

                // 3. Color-matched matte for each portrait (opaque and edge-to-edge if enabled)
                if (snap_matte_color) {
                    g_renderer.draw_color_matched_matte(rect_l,
                        current_data->matte_r, current_data->matte_g, current_data->matte_b, matte_op, left_area);
                    g_renderer.draw_color_matched_matte(rect_r,
                        current_twin_data->matte_r, current_twin_data->matte_g, current_twin_data->matte_b, matte_op, right_area);
                }


                // 4. Draw bias lighting if enabled
                if (has_bias) {
                    int glow = snap_glow_depth;
                    int bw_l = has_border ? border_w : 0;
                    g_renderer.draw_bias_lighting(rect_l, layout.slideshow_area, current_data->avg_r, current_data->avg_g, current_data->avg_b,
                        bias_strength, (float)item_timer, anim_speed, style, bw_l, glow);
                    int bw_r = has_border ? border_w : 0;
                    g_renderer.draw_bias_lighting(rect_r, layout.slideshow_area, current_twin_data->avg_r, current_twin_data->avg_g, current_twin_data->avg_b,
                        bias_strength, (float)item_timer, anim_speed, style, bw_r, glow);
                }

                // 5. Draw borders if enabled
                if (has_border) {
                    g_renderer.draw_border(rect_l, snap_border_mode, current_data->avg_r, current_data->avg_g, current_data->avg_b, border_w, current_data->filename);
                    g_renderer.draw_border(rect_r, snap_border_mode, current_twin_data->avg_r, current_twin_data->avg_g, current_twin_data->avg_b, border_w, current_twin_data->filename);
                }

                // 6. Draw texture
                SDL_FRect dst_l = {(float)rect_l.x, (float)rect_l.y, (float)rect_l.w, (float)rect_l.h};
                SDL_RenderTexture(g_renderer.sdl_renderer, current_data->texture, nullptr, &dst_l);

                SDL_FRect dst_r = {(float)rect_r.x, (float)rect_r.y, (float)rect_r.w, (float)rect_r.h};
                SDL_RenderTexture(g_renderer.sdl_renderer, current_twin_data->texture, nullptr, &dst_r);
                rendered = true;
            } else if (current_tex) {
                // Snapshot config for render frame to avoid data race
                bool snap_bias, snap_matting, snap_border;
                std::string snap_border_mode = "off";
                int snap_bias_strength, snap_border_width, snap_glow_depth;
                float snap_anim_speed;
                std::string snap_anim_style;
                bool snap_blurred, snap_matte_color;
                float matte_op, vignette_str;
                {
                    std::shared_lock lk(g_config_mtx);
                    snap_bias = g_cfg.bias_lighting;
                    snap_matting = g_cfg.matting;
                    snap_border_mode = g_cfg.border_mode;
                    snap_border = (snap_border_mode != "off");
                    snap_bias_strength = g_cfg.bias_strength;
                    snap_border_width = g_renderer.scale_px(g_cfg.border_width);
                    snap_anim_speed = g_cfg.bias_anim_speed;
                    snap_anim_style = g_cfg.bias_anim_style;
                    snap_glow_depth = g_renderer.scale_px(g_cfg.glow_depth);
                    snap_blurred = g_cfg.blurred_background;
                    snap_matte_color = g_cfg.color_matched_matte;
                    matte_op = g_cfg.matte_opacity;
                    vignette_str = g_cfg.vignette_strength;
                }

                if (current_data) {
                    calculate_fit_rect_in_area(current_data->width, current_data->height, layout.slideshow_area.x, layout.slideshow_area.y, layout.slideshow_area.w, layout.slideshow_area.h, fit_rect);
                }

                if (snap_matte_color && current_data) {
                    g_renderer.clear(current_data->matte_r, current_data->matte_g, current_data->matte_b, 255);
                } else {
                    g_renderer.clear(0, 0, 0, 255);
                }

                // 1. Draw background based on style
                std::string snap_bg_style;
                { std::shared_lock lk(g_config_mtx); snap_bg_style = g_cfg.bg_style; }
                if (current_data && (snap_blurred || snap_bg_style != "photo")) {
                    g_renderer.draw_background(current_data.get(), snap_bg_style, (Uint8)(255.0f * vignette_str));
                }

                // 2. Draw matte borders if enabled (solid black base layer) - ONLY if NOT color-matched or blurred!
                if (snap_matting && !snap_matte_color && !snap_blurred) {
                    g_renderer.draw_matte_borders(fit_rect);
                }

                // 3. Color-matched matte if enabled (opaque and edge-to-edge if enabled)
                if (snap_matte_color && current_data) {
                    g_renderer.draw_color_matched_matte(fit_rect,
                        current_data->matte_r, current_data->matte_g, current_data->matte_b, matte_op);
                }

                // 4. Draw bias lighting if enabled
                if (snap_bias && current_data) {
                    int bw_param = snap_border ? snap_border_width : 0;
                    g_renderer.draw_bias_lighting(fit_rect, layout.slideshow_area,
                        current_data->avg_r, current_data->avg_g, current_data->avg_b,
                        snap_bias_strength, (float)item_timer, snap_anim_speed, snap_anim_style, bw_param, snap_glow_depth);
                }

                // 5. Draw border if enabled
                if (snap_border && current_data) {
                    g_renderer.draw_border(fit_rect, snap_border_mode,
                        current_data->avg_r, current_data->avg_g, current_data->avg_b, snap_border_width, current_data->filename);
                }

                // 6. Draw texture
                SDL_FRect dst = {(float)fit_rect.x, (float)fit_rect.y, (float)fit_rect.w, (float)fit_rect.h};
                SDL_RenderTexture(g_renderer.sdl_renderer, current_tex, nullptr, &dst);
                rendered = true;
            }

            const auto font_path = g_overlay ? g_overlay->get_font_path() : std::string{};
            auto* font = g_overlay ? g_overlay->get_font_renderer() : nullptr;
            if (rendered && !reuse_scene && font) {
                if (layout.show_chores)
                    render_chores_panel(g_renderer.sdl_renderer, font, font_path, layout.chores_area, g_renderer.screen_w, layout.content_top);
                if (layout.show_calendar)
                    g_calendar.render(g_renderer.sdl_renderer, font, font_path, layout.calendar_area, g_renderer.screen_w);
                if (layout.show_stocks)
                    g_stock_streamer.render(g_renderer.sdl_renderer, font, font_path, layout.stocks_area, g_renderer.screen_w);
            }
            if (capture_scene) {
                SDL_SetRenderTarget(g_renderer.sdl_renderer, nullptr);
                if (rendered) {
                    SDL_RenderTexture(g_renderer.sdl_renderer, photo_scene.get(), nullptr, nullptr);
                    scene_image = current_data;
                    scene_twin = current_twin_data;
                    scene_due = now + std::chrono::milliseconds(1000 / scene_fps);
                }
            }
            if (rendered) {
                if (g_overlay) {
                    if (layout.show_news)
                        g_news_ticker.render(g_renderer.sdl_renderer, font, font_path, layout.news_area, g_renderer.screen_w);
                    const auto* item = displayed_path == selection.item.path ? &selection.item : nullptr;
                    const auto* twin = item && shown_twin ? &*selection.twin : nullptr;
                    g_overlay->draw_all(selection.index, selection.count, item, twin, item_timer, false,
                        active_fps, current_data.get(), current_twin_data.get(), "", &layout.slideshow_area);
                }
                // Splash-to-first-photo fade-out
                if (first_photo_fade) {
                    if (fade_start_time == 0) fade_start_time = SDL_GetTicks();
                    Uint64 fade_elapsed = SDL_GetTicks() - fade_start_time;
                    float fade_alpha = 1.0f - std::min(1.0f, fade_elapsed / 500.0f);
                    if (fade_alpha > 0.0f) {
                        SDL_SetRenderDrawBlendMode(g_renderer.sdl_renderer, SDL_BLENDMODE_BLEND);
                        SDL_SetRenderDrawColor(g_renderer.sdl_renderer, 0, 0, 0, (Uint8)(fade_alpha * 255.0f));
                        SDL_FRect fade_rect = {0, 0, (float)g_renderer.screen_w, (float)g_renderer.screen_h};
                        SDL_RenderFillRect(g_renderer.sdl_renderer, &fade_rect);
                    }
                    if (fade_alpha <= 0.0f) {
                        first_photo_fade = false;
                        g_renderer.cleanup_splash();
                    }
                }
                g_renderer.present();
            }

            static auto next_lookahead = std::chrono::steady_clock::time_point{};
            if (g_preload && now >= next_lookahead) {
                next_lookahead = now + std::chrono::milliseconds(500);
                std::string photo, twin, warm_video;
                {
                    std::lock_guard lock(g_playlist_mtx);
                    if (selection.matches()) {
                        const int next = (selection.index + (shown_twin ? 2 : 1)) % selection.count;
                        if (g_eligible[next].type == "image") {
                            photo = g_eligible[next].path;
                            if (twin_enabled) {
                                if (const auto partner = find_twin_portrait(g_eligible, next, true))
                                    twin = g_eligible[*partner].path;
                            }
                        }
                        for (int i = 0; i < std::min(2, selection.count); ++i) {
                            const auto& item = g_eligible[(next + i) % selection.count];
                            if (item.type == "video") {
                                warm_video = item.path;
                                break;
                            }
                        }
                    }
                }
                g_preload->enqueue(photo);
                g_preload->enqueue(twin);
                if (!warm_video.empty()) {
                    int width, height;
                    {
                        std::shared_lock lock(g_config_mtx);
                        width = g_cfg.screen_w;
                        height = g_cfg.screen_h;
                        g_video_decoder.set_av_sync(g_cfg.av_sync);
                    }
                    g_video_decoder.prewarm(warm_video, width, height);
                }
            }

            double delay;
            {
                std::shared_lock lock(g_config_mtx);
                delay = g_cfg.transition_delay;
            }
            if (!paused && (photo_retry_at != std::chrono::steady_clock::time_point{} ?
                            now >= photo_retry_at : item_timer >= delay)) {
                photo_retry_at = {};
                advance(shown_twin ? 2 : 1);
            }
        }

        {
            int target_fps = 60;
            if (!transitioning && !first_photo_fade) {
                std::shared_lock lk(g_config_mtx);
                bool news_active = g_cfg.news_enabled && g_cfg.infopanels_enabled;
                target_fps = news_active ? 60 : std::clamp(g_cfg.pattern_fps, 1, 60);
            }
            switch (pitrove::thermal::get_quality()) {
                case pitrove::thermal::Quality::Minimal: target_fps = std::min(target_fps, 15); break;
                case pitrove::thermal::Quality::Reduced: target_fps = std::min(target_fps, 30); break;
                case pitrove::thermal::Quality::Normal: break;
            }

            static Uint64 next_target_ticks = 0;
            Uint64 target_frame_ms = (Uint64)(1000 / target_fps);
            Uint64 now_ticks = SDL_GetTicks();
            if (next_target_ticks == 0 || now_ticks > next_target_ticks + 500) {
                next_target_ticks = now_ticks + target_frame_ms;
            } else {
                next_target_ticks += target_frame_ms;
            }

            if (now_ticks < next_target_ticks) {
                SDL_Delay((Uint32)(next_target_ticks - now_ticks));
            } else {
                SDL_Delay(1);
            }
        }
    }

    // --- Cleanup ---
    g_logger.info("[TRACE] SHUTDOWN_TRIGGER: Main render loop exited (g_running=false). Initiating cleanup sequence...");
    g_logger.info("Shutting down...");
    
    // Fail-safe: Restore physical display power on exit
    set_display_power(true);
    
    // Signal thermal monitor early so it exits in parallel with other threads
    thermal_thread.request_stop();

    stop_video();

    // Stop background HTTP server
    g_watchman_thread.request_stop();
    stop_http_server();
    g_news_ticker.stop();
    g_calendar.stop();
    g_stock_streamer.stop();
    
    // Stop background MQTT client safely
    stop_mqtt_client();

    // Stop background Google Photos sync thread safely
    g_google_photos.stop();
    
    // Stop background preprocessing worker thread safely
    stop_preprocess_worker();

    // Stop software watchdog thread safely
    g_watchdog_running.store(false);
    if (g_watchdog_thread.joinable()) {
        g_watchdog_thread.join();
        g_logger.info("Watchdog: Software watchdog thread stopped successfully.");
    }
    
    // Stop keep-alive thread safely
    g_keepalive_running.store(false);
    if (g_keepalive_thread.joinable()) {
        g_keepalive_thread.join();
        g_logger.info("Keepalive: Background connection monitoring thread stopped successfully.");
    }
    
    // Its filesystem waits are cancellable, so the coordinator can always be
    // joined before cache and playlist teardown, even on an unresponsive NAS.
    if (g_watchman_thread.joinable()) g_watchman_thread.join();

    if (g_transition) { g_transition->reset(); delete g_transition; }
    g_renderer.set_caption_font(nullptr, {});
    if (g_overlay) { delete g_overlay; g_overlay = nullptr; }
    if (g_preload) {
        g_preload->shutdown();
        delete g_preload;
        g_preload = nullptr;
    }
    cleanup_nas_thread();
    if (current_data) { current_data = nullptr; }
    if (current_twin_data) { current_twin_data = nullptr; }
    if (next_data) { next_data = nullptr; }
    if (next_twin_data) { next_twin_data = nullptr; }
    if (transition_prev_target) { SDL_DestroyTexture(transition_prev_target); }
    if (transition_next_target) { SDL_DestroyTexture(transition_next_target); }
    if (g_cache) { delete g_cache; g_cache = nullptr; }
    photo_scene.reset();
    g_renderer.cleanup();

    flock(lock_fd, LOCK_UN); close(lock_fd);
    // Join thermal monitoring thread (already signaled at shutdown start)
    if (thermal_thread.joinable()) thermal_thread.join();

    write_event_log("=== clean exit ===\n");
    {
        std::lock_guard lk(g_event_log_mtx);
        if (g_event_log) {
            fclose(g_event_log);
            g_event_log = nullptr;
        }
    }
    // Clear crash state after stable runtime
    pitrove::safe_mode::clear();

    g_logger.info("piTrove v{} shutdown complete", VERSION);
    return 0;
}

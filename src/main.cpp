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
#include "tui.h"
#include "http_server.h"
#include "mqtt.h"
#include "google_photos.h"

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>
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
#include <shared_mutex>
#include <future>

#include <filesystem>
#include <fstream>
#include <glob.h>

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
                        std::string dir_name = status_path.substr(slash + 1);
                        size_t dash = dir_name.find('-');
                        if (dash != std::string::npos) {
                            std::string card_part = dir_name.substr(0, dash); // "card1"
                            std::string conn_part = dir_name.substr(dash + 1); // "HDMI-A-1"
                            
                            if (card_part.rfind("card", 0) == 0) {
                                int card_idx = 0;
                                try { card_idx = std::stoi(card_part.substr(4)); } catch(...) {}
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
std::vector<MediaItem> g_scanned_items;
std::vector<MediaItem> g_eligible;
int current_idx = 0;
static std::thread g_watchman_thread;
static std::atomic<bool> g_watchman_running{false};
static std::atomic<bool> g_watchman_finished{false};
static std::atomic<int64_t> g_last_heartbeat_time{0};

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

static bool should_be_twin_portrait(std::vector<MediaItem>& eligible, int idx) {
    bool twin_enabled = false;
    {
        std::lock_guard lock(g_config_mtx);
        twin_enabled = g_cfg.twin_portrait_enabled;
    }
    int size = (int)eligible.size();
    if (!twin_enabled || size < 2) return false;

    // Check if the current item is portrait
    if (idx >= size || idx < 0) return false;
    const auto& item1 = eligible[idx];
    if (item1.type == "video" || item1.height <= item1.width || item1.height <= 0) return false;

    // Check if the next item is portrait
    if (idx + 1 >= size) return false;
    const auto& item2 = eligible[idx + 1];
    bool item2_is_portrait = (item2.type == "image" && item2.height > item2.width && item2.height > 0);

    if (item2_is_portrait) {
        return true;
    }

    // If next item is not portrait, search forward in the playlist to find the nearest portrait image to pair
    for (int j = idx + 2; j < size; j++) {
        const auto& candidate = eligible[j];
        if (candidate.type == "image" && candidate.height > candidate.width && candidate.height > 0) {
            // Swap candidate into the adjacent slot (idx + 1)
            std::swap(eligible[idx + 1], eligible[j]);
            g_logger.info("Twin-Portrait: Dynamically paired portrait at index %d by bringing forward portrait from index %d", idx, j);
            return true;
        }
    }

    return false;
}

// NOTE: Matting/border offset logic duplicates Renderer::calculate_fit_rect (renderer.cpp:251)
// Keep in sync if adjusting matte or border inset calculations
static void calculate_fit_rect_in_area(int img_w, int img_h, int area_x, int area_y, int area_w, int area_h, SDL_Rect& out_rect) {
    if (img_w <= 0 || img_h <= 0) {
        out_rect.w = 0;
        out_rect.h = 0;
        out_rect.x = area_x + area_w / 2;
        out_rect.y = area_y + area_h / 2;
        return;
    }

    bool has_matting = false;
    bool has_border = false;
    int mat_size = 0;
    int border_w = 0;
    {
        std::lock_guard lock(g_config_mtx);
        has_matting = g_cfg.matting;
        has_border = g_cfg.border_enabled;
        mat_size = g_renderer.scale_px(g_cfg.matting_size);
        border_w = g_renderer.scale_px(g_cfg.border_width);
    }

    int effective_x = area_x;
    int effective_y = area_y;
    int effective_w = area_w;
    int effective_h = area_h;

    int mat = 0;
    if (has_matting) {
        mat += mat_size;
    }
    if (has_border) {
        mat += border_w;
    }

    if (mat > 0) {
        effective_x += mat;
        effective_y += mat;
        effective_w -= mat * 2;
        effective_h -= mat * 2;
        if (effective_w < 1) effective_w = 1;
        if (effective_h < 1) effective_h = 1;
    }
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

    // Set render target to our texture
    SDL_SetRenderTarget(renderer, target);

    bool snap_matte_color = false;
    {
        std::lock_guard lock(g_config_mtx);
        snap_matte_color = g_cfg.color_matched_matte;
    }
    if (snap_matte_color && primary) {
        SDL_SetRenderDrawColor(renderer, primary->matte_r, primary->matte_g, primary->matte_b, 255);
    } else {
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    }
    SDL_RenderClear(renderer);

    if (twin && twin->texture && primary && primary->texture) {
        // Draw twin portrait layout!
        SDL_Rect rect_l, rect_r;
        calculate_fit_rect_in_area(primary->width, primary->height, 0, 0, sw / 2, sh, rect_l);
        calculate_fit_rect_in_area(twin->width, twin->height, sw / 2, 0, sw - (sw / 2), sh, rect_r);

        bool has_bias = false;
        bool has_matting = false;
        bool has_border = false;
        int bias_strength = 110;
        float anim_speed = 0.5f;
        std::string style = "edge_glow";
        int border_w = 10;
        bool snap_blurred;
        int snap_glow_depth;
        float matte_op, vignette_str;
        {
            std::lock_guard lock(g_config_mtx);
            has_bias = g_cfg.bias_lighting;
            has_matting = g_cfg.matting;
            has_border = g_cfg.border_enabled;
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
        { std::lock_guard lk(g_config_mtx); snap_bg_style = g_cfg.bg_style; }
        if (snap_blurred || snap_bg_style != "photo") {
            g_renderer.draw_background(primary.get(), snap_bg_style, (Uint8)(255.0f * vignette_str));
        }

        // 2. Draw matte borders if enabled (solid black base layer) - ONLY if NOT color-matched or blurred!
        if (has_matting && !snap_matte_color && !snap_blurred) {
            g_renderer.draw_matte_borders(rect_l);
            g_renderer.draw_matte_borders(rect_r);
        }

        // 3. Color-matched matte for each portrait (opaque and edge-to-edge if enabled)
        if (snap_matte_color) {
            g_renderer.draw_color_matched_matte(rect_l,
                primary->matte_r, primary->matte_g, primary->matte_b, matte_op);
            g_renderer.draw_color_matched_matte(rect_r,
                twin->matte_r, twin->matte_g, twin->matte_b, matte_op);
        }

        // 4. Draw bias lighting if enabled
        if (has_bias) {
            int bw_l = has_border ? border_w : 0;
            g_renderer.draw_bias_lighting(rect_l, primary->avg_r, primary->avg_g, primary->avg_b,
                bias_strength, (float)item_timer, anim_speed, style, bw_l, snap_glow_depth);
            int bw_r = has_border ? border_w : 0;
            g_renderer.draw_bias_lighting(rect_r, twin->avg_r, twin->avg_g, twin->avg_b,
                bias_strength, (float)item_timer, anim_speed, style, bw_r, snap_glow_depth);
        }

        // 5. Draw 3D miter borders if enabled
        if (has_border) {
            g_renderer.draw_3d_border(rect_l, primary->avg_r, primary->avg_g, primary->avg_b, border_w);
            g_renderer.draw_3d_border(rect_r, twin->avg_r, twin->avg_g, twin->avg_b, border_w);
        }

        // 6. Draw texture
        SDL_FRect dst_l = {(float)rect_l.x, (float)rect_l.y, (float)rect_l.w, (float)rect_l.h};
        SDL_RenderTexture(renderer, primary->texture, nullptr, &dst_l);

        SDL_FRect dst_r = {(float)rect_r.x, (float)rect_r.y, (float)rect_r.w, (float)rect_r.h};
        SDL_RenderTexture(renderer, twin->texture, nullptr, &dst_r);
    } else if (primary && primary->texture) {
        // Draw single image layout!
        SDL_Rect rect;
        g_renderer.calculate_fit_rect(primary->width, primary->height, rect);

        bool has_bias = false;
        bool has_matting = false;
        bool has_border = false;
        int bias_strength = 110;
        float anim_speed = 0.5f;
        std::string style = "edge_glow";
        int border_w = 10;
        bool snap_blurred;
        int snap_glow_depth;
        float matte_op, vignette_str;
        {
            std::lock_guard lock(g_config_mtx);
            has_bias = g_cfg.bias_lighting;
            has_matting = g_cfg.matting;
            has_border = g_cfg.border_enabled;
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
        { std::lock_guard lk(g_config_mtx); snap_bg_style = g_cfg.bg_style; }
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
            g_renderer.draw_bias_lighting(rect, primary->avg_r, primary->avg_g, primary->avg_b,
                bias_strength, (float)item_timer, anim_speed, style, bw_param, snap_glow_depth);
        }

        // 5. Draw 3D miter border if enabled
        if (has_border) {
            g_renderer.draw_3d_border(rect, primary->avg_r, primary->avg_g, primary->avg_b, border_w);
        }

        // 6. Draw texture
        SDL_FRect dst = {(float)rect.x, (float)rect.y, (float)rect.w, (float)rect.h};
        SDL_RenderTexture(renderer, primary->texture, nullptr, &dst);
    }

    // Reset render target back to default (screen)
    SDL_SetRenderTarget(renderer, nullptr);
    return target;
}

static std::vector<MediaItem> filter_playlist(const std::vector<MediaItem>& items, int cooldown_days, int window_days) {
    bool on_this_day = false;
    {
        std::lock_guard lock(g_config_mtx);
        on_this_day = g_cfg.on_this_day_enabled;
    }

    if (on_this_day) {
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
            g_logger.info("ON_THIS_DAY: Found %zu anniversary items for month=%d day=%d!", anniversary_items.size(), today_m, today_d);
            std::vector<MediaItem> anniversary_filtered;
            for (const auto& item : anniversary_items) {
                if (item.type != "video") {
                    bool has_people = false, has_animals = false, is_doc = false;
                    classify_media_item(item, has_people, has_animals, is_doc);
                    if (is_doc) continue;
                    
                    bool filter_people, filter_animals;
                    {
                        std::lock_guard lk(g_config_mtx);
                        filter_people = g_cfg.show_people_faces;
                        filter_animals = g_cfg.keep_animals;
                    }
                    if (filter_people || filter_animals) {
                        bool keep = false;
                        if (filter_people && has_people) keep = true;
                        if (filter_animals && has_animals) keep = true;
                        if (!keep) continue;
                    }
                }
                anniversary_filtered.push_back(item);
            }
            if (!anniversary_filtered.empty()) {
                return anniversary_filtered;
            }
            g_logger.warn("ON_THIS_DAY: Anniversary items were all filtered out by category toggles. Falling back to normal playlist.");
        } else {
            g_logger.info("ON_THIS_DAY: No anniversary items matching today's month=%d day=%d. Falling back to normal playlist.", today_m, today_d);
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
    
    int target_min = std::min(15, seasonal_count);
    int current_cooldown = cooldown_days;
    
    while (true) {
        filtered.clear();
        int64_t cutoff = now - (static_cast<int64_t>(current_cooldown) * 86400);
        for (const auto& item : items) {
            // 1. Cooldown filter
            if (current_cooldown > 0 && item.last_shown >= cutoff) {
                continue;
            }
            // 2. Seasonal window filter
            if (!is_item_in_seasonal_window(item, window_days)) {
                continue;
            }
            
            // 3. People and Animal toggle filters
            if (item.type != "video") { // only filter images, keep videos
                bool has_people = false;
                bool has_animals = false;
                bool is_doc = false;
                classify_media_item(item, has_people, has_animals, is_doc);
                
                // Snapshot config values under lock to avoid data race
                bool snap_show_people, snap_keep_animals;
                {
                    std::lock_guard lk(g_config_mtx);
                    snap_show_people = g_cfg.show_people_faces;
                    snap_keep_animals = g_cfg.keep_animals;
                }

                if (is_doc) {
                    continue;
                } else {
                    bool filter_people = snap_show_people;
                    bool filter_animals = snap_keep_animals;
                    
                    if (filter_people || filter_animals) {
                        bool keep = false;
                        if (filter_people && has_people) keep = true;
                        if (filter_animals && has_animals) keep = true;
                        if (!keep) continue;
                    }
                }
            }

            filtered.push_back(item);
        }
        
        if ((int)filtered.size() >= target_min || current_cooldown <= 0) {
            break;
        }
        
        int next_cooldown = current_cooldown / 2;
        if (next_cooldown == current_cooldown || next_cooldown < 1) {
            current_cooldown = 0;
        } else {
            current_cooldown = next_cooldown;
        }
        g_logger.info("filter_playlist: Cooldown of %d days resulted in only %zu items. Degrading to %d days to maintain variety.",
            cooldown_days, filtered.size(), current_cooldown);
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
            // Distribute videos mathematically and evenly across photos based on the configured user ratio
            size_t p_idx = 0, v_idx = 0;
            double accumulator = 0.0;

            while (p_idx < photos.size() || v_idx < videos.size()) {
                if (v_idx >= videos.size()) {
                    // Out of videos: append remaining photos
                    eligible.push_back(std::move(photos[p_idx++]));
                } else if (p_idx >= photos.size()) {
                    // Out of photos: append remaining videos
                    eligible.push_back(std::move(videos[v_idx++]));
                } else {
                    if (accumulator >= photos_per_video) {
                        eligible.push_back(std::move(videos[v_idx++]));
                        accumulator -= photos_per_video;
                    } else {
                        eligible.push_back(std::move(photos[p_idx++]));
                        accumulator += 1.0;
                    }
                }
            }
        }

        g_logger.info("Playlist organized: %zu photos + %zu videos = %zu total (configured ratio: %d videos per 10 photos, spacing: %.2f photos per video)",
            final_photos_size, final_videos_size, eligible.size(), videos_per_photos, photos_per_video);
    }
}

static void mark_item_shown(const std::string& path, bool lock_playlist) {
    int64_t now = static_cast<int64_t>(std::time(nullptr));
    if (g_cache) g_cache->mark_shown(path);
    
    std::unique_lock<std::mutex> lock(g_playlist_mtx, std::defer_lock);
    if (lock_playlist) {
        lock.lock();
    }
    
    // Update last_shown in g_eligible
    for (auto& item : g_eligible) {
        if (item.path == path) {
            item.last_shown = now;
        }
    }
    // Update last_shown in g_scanned_items
    for (auto& item : g_scanned_items) {
        if (item.path == path) {
            item.last_shown = now;
        }
    }
}

static void advance_playlist(int step) {
    if (g_eligible.empty()) return;
    
    if (step > 0) {
        int next_idx = current_idx + step;
        if (next_idx >= (int)g_eligible.size()) {
            g_logger.info("Playlist reached end. Re-filtering and re-shuffling to prevent repetition...");
            
            int cooldown_days = 330;
            int window_days = 5;
            bool play_just_photos = false;
            bool play_just_videos = false;
            int videos_per_photos = 10;
            bool shuffle_enabled = true;
            {
                std::lock_guard lock(g_config_mtx);
                cooldown_days = g_cfg.cooldown_days;
                window_days = g_cfg.scan_window_days;
                play_just_photos = g_cfg.play_just_photos;
                play_just_videos = g_cfg.play_just_videos;
                videos_per_photos = g_cfg.videos_per_photos;
                shuffle_enabled = g_cfg.shuffle;
            }
            
            std::vector<MediaItem> new_eligible = filter_playlist(g_scanned_items, cooldown_days, window_days);
            if (!new_eligible.empty()) {
                organize_playlist(new_eligible, videos_per_photos, play_just_photos, play_just_videos, shuffle_enabled);
                
                std::string prev_path = g_eligible[current_idx].path;
                g_eligible = std::move(new_eligible);
                
                // Avoid immediate repetition if the new first item matches the previous item
                if (g_eligible.size() > 1 && g_eligible[0].path == prev_path) {
                    std::swap(g_eligible[0], g_eligible[1]);
                }
            }
            current_idx = 0;
        } else {
            current_idx = next_idx;
        }
    } else {
        int n = (int)g_eligible.size();
        current_idx = ((current_idx + step) % n + n) % n;
    }
}

static void watchman_loop() {
    g_logger.info("Watchman: Background watchman thread started.");
    g_watchman_finished.store(false);
    
    // Get current day of year
    time_t last_check_time = std::time(nullptr);
    struct tm tm_buf;
    struct tm* last_check_tm = localtime_r(&last_check_time, &tm_buf);
    int last_yday = last_check_tm ? last_check_tm->tm_yday : 0;
    
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
            if (g_offline_mode.load()) {
                g_logger.warn("Watchman: System is in Offline Recovery Mode. Skipping midnight temporal window shift.");
                continue;
            }
            g_logger.info("Watchman: Midnight detected! Shifting temporal window. Old day=%d, New day=%d", last_yday, curr_tm.tm_yday);
            
            // Validate media directory readability and accessibility to handle network drops gracefully
            std::string media_dir;
            {
                std::lock_guard lock(g_config_mtx);
                media_dir = g_cfg.media_dir;
            }
            if (!is_nas_online()) {
                g_logger.warn("Watchman: NAS/Media directory host is offline. Skipping shift.");
                continue;
            }
            struct stat st;
            if (stat(media_dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || access(media_dir.c_str(), R_OK) != 0) {
                g_logger.error("Watchman: Media directory '%s' is not readable/accessible (mount failure or permission error). Delaying seasonal window swap.", media_dir.c_str());
                continue;
            }

            last_yday = curr_tm.tm_yday;
            
            g_logger.info("Watchman: Midnight detected! Shifting temporal window. Old day=%d, New day=%d", last_yday, curr_tm.tm_yday);
            g_logger.info("Watchman: Starting background media scan for shifted seasonal window...");
            
            std::vector<MediaItem> scanned;
            int depth = 10;
            int screen_w = 1920, screen_h = 1080;
            {
                std::lock_guard lock(g_config_mtx);
                depth = g_cfg.scan_depth;
                screen_w = g_cfg.screen_w;
                screen_h = g_cfg.screen_h;
            }
            scan_directory(media_dir, depth, scanned, nullptr);
            g_logger.info("Watchman: Background scan complete. Scanned %zu items. Caching metadata...", scanned.size());

            if (g_cache) {
                g_cache->begin_transaction();
                for (auto& mi : scanned) {
                    if (g_cache->load_cached(mi)) {
                        mi.cached = true;
                    } else {
                        if (mi.type == "image") {
                            mi.exif_rotation = 1;
                            mi.width = 1920; mi.height = 1080;
                            mi.creation_time = ImageLoader::get_creation_time(mi.path);
                        } else {
                            mi.width = screen_w;
                            mi.height = screen_h;
                            mi.duration = 0.0;
                        }
                        g_cache->upsert(mi, 0);
                    }
                }
                g_cache->commit_transaction();
            }
            
            // Re-filter playlist under lock to prevent data race on g_scanned_items and g_eligible
            {
                std::scoped_lock lock(g_playlist_mtx, g_config_mtx);
                g_scanned_items = std::move(scanned);
                
                int cooldown_days = g_cfg.cooldown_days;
                int window_days = g_cfg.scan_window_days;
                bool shuffle_enabled = g_cfg.shuffle;
                
                std::vector<MediaItem> new_eligible = filter_playlist(g_scanned_items, cooldown_days, window_days);
                g_logger.info("Watchman: New seasonal window calculation: %zu / %zu items eligible", new_eligible.size(), g_scanned_items.size());
                
                if (!new_eligible.empty()) {
                    bool play_just_photos = g_cfg.play_just_photos;
                    bool play_just_videos = g_cfg.play_just_videos;
                    int videos_per_photos = g_cfg.videos_per_photos;
                    organize_playlist(new_eligible, videos_per_photos, play_just_photos, play_just_videos, shuffle_enabled);
                    
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
                } else {
                    g_logger.warn("Watchman: New playlist is empty. Keeping old playlist to prevent interruption.");
                }
            }
        }
    }
    g_watchman_finished.store(true);
    g_logger.info("Watchman: Background watchman thread exiting.");
}

static std::thread g_watchdog_thread;
static std::atomic<bool> g_watchdog_running{false};

static void watchdog_loop() {
    g_logger.info("Watchdog: Software watchdog thread active.");
    g_last_heartbeat_time.store(static_cast<int64_t>(std::time(nullptr)));
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
            if (!empty && current_idx >= 0 && current_idx < (int)g_eligible.size()) {
                is_video = (g_eligible[current_idx].type == "video");
            }
        }

        // If playing video, mpv player controls playback, so we skip checking heartbeat
        if (paused || blanked || empty || is_video) {
            // Keep resetting heartbeat while paused/blanked/playing video
            g_last_heartbeat_time.store(static_cast<int64_t>(std::time(nullptr)));
            continue;
        }

        int64_t now = static_cast<int64_t>(std::time(nullptr));
        int64_t last_heartbeat = g_last_heartbeat_time.load();
        double delay = 15.0;
        {
            std::lock_guard<std::shared_mutex> lk(g_config_mtx);
            delay = g_cfg.transition_delay;
        }

        // Allow up to 3x transition delay or a minimum of 45 seconds before forcing restart
        int64_t max_silent_time = std::max((int64_t)45, (int64_t)(delay * 3));
        if (now - last_heartbeat > max_silent_time) {
            g_logger.error("[WATCHDOG] CRITICAL: Slideshow loop frozen! Last heartbeat was %d seconds ago. Forcing restart...", (int)(now - last_heartbeat));
            // Restore physical display power before exiting
            int fd = open("/sys/class/graphics/fb0/blank", O_WRONLY);
            if (fd >= 0) {
                (void)write(fd, "0", 1);
                close(fd);
            }
            _exit(99); // Force exit immediately to let Docker compose restart us
        }
    }
    g_logger.info("Watchdog: Software watchdog thread exiting.");
}

int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    bool run_config = false;
    bool run_restart = false;
    std::string config_path;

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
        }
    }

    std::string exe_dir = get_exe_dir();
    std::string lock_path = exe_dir + "/piTrove.lock";

    if (run_restart) {
        FILE* lf = fopen(lock_path.c_str(), "r");
        if (lf) {
            pid_t old_pid = 0;
            if (fscanf(lf, "%d", &old_pid) == 1 && old_pid > 0 && old_pid != getpid()) {
                printf("\033[1;33m[INFO]\033[0m Gracefully terminating running instance (PID %d)...\n", old_pid);
                kill(old_pid, SIGTERM);
                std::this_thread::sleep_for(std::chrono::microseconds(800000)); // Give it a moment to release sockets/files/VRAM
            }
            fclose(lf);
        }
        printf("\033[1;32m[OK]\033[0m Restarting piTrove background service...\n");
        system("sudo systemctl restart piTrove.service 2>/dev/null || true");
        return 0;
    }

    if (run_config) {
        if (config_path.empty()) {
            const char* candidates[] = {"src/config/config.toml", "/home/pi/piTrove/src/config/config.toml",
                "config/config.toml", "./src/config/config.toml"};
            for (const auto& c : candidates) {
                if (file_exists(c)) { config_path = c; break; }
            }
            if (config_path.empty()) {
                config_path = "/home/pi/piTrove/src/config/config.toml";
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
    int lock_fd = open(lock_path.c_str(), O_CREAT | O_RDWR, 0644);
    if (lock_fd < 0) { fprintf(stderr, "Failed to open lock file\n"); return 1; }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "Another instance is already running\n");
        close(lock_fd); return 1;
    }
    
    // Write our PID into the lock file for future --restart usage
    [[maybe_unused]] auto trunc_rc = ftruncate(lock_fd, 0);
    [[maybe_unused]] auto pid_rc = dprintf(lock_fd, "%d\n", getpid());

    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    // SIGTERM and SIGINT trigger graceful shutdown — let main loop exit and cleanup
    struct sigaction sa_term = {};
    sa_term.sa_handler = [](int) { g_running.store(false); };
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    sigaction(SIGTERM, &sa_term, nullptr);
    sigaction(SIGINT, &sa_term, nullptr);
    std::set_terminate(terminate_handler);

    g_logger.info("=== piTrove v%s started %s ===", VERSION, get_timestamp().c_str());

    // --- Config ---
    g_cfg.parse_args(argc, argv);
    if (config_path.empty()) {
        const char* candidates[] = {"src/config/config.toml", "/home/pi/piTrove/src/config/config.toml",
            "config/config.toml", "./src/config/config.toml"};
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

    std::string media_dir;
    std::string cache_dir;
    std::string log_dir;
    int keep_count = 5;
    {
        std::shared_lock<std::shared_mutex> lock(g_config_mtx);
        media_dir = g_cfg.media_dir;
        cache_dir = g_cfg.cache_dir.empty() ? (get_exe_dir() + "/cache") : g_cfg.cache_dir;
        log_dir = g_cfg.log_dir.empty() ? (get_exe_dir() + "/logs") : g_cfg.log_dir;
        keep_count = g_cfg.log_keep_count;
    }
    g_crash_cache_dir = cache_dir;
    std::snprintf(g_crash_cache_dir_safe, sizeof(g_crash_cache_dir_safe), "%s", cache_dir.c_str());

    g_logger.init(log_dir, LogLevel::DEBUG, keep_count);
    g_logger.info("Media dir: %s, Cache dir: %s", media_dir.c_str(), cache_dir.c_str());

    // Verify cache directory available space (E403) and writability (E405)
    {
        std::error_code space_ec;
        std::filesystem::create_directories(cache_dir, space_ec);
        auto space_info = std::filesystem::space(cache_dir, space_ec);
        if (!space_ec) {
            if (space_info.available < 50 * 1024 * 1024) {
                trigger_error(403); // E403: DISK_SPACE_CRITICAL
            } else {
                if (g_active_error_code.load() == 403) {
                    trigger_error(0);
                }
            }
        }

        std::string test_file = cache_dir + "/.write_test";
        FILE* tf = fopen(test_file.c_str(), "w");
        if (!tf) {
            trigger_error(405); // E405: SQLITE_READONLY_DATABASE
        } else {
            fclose(tf);
            unlink(test_file.c_str());
            if (g_active_error_code.load() == 405) {
                trigger_error(0);
            }
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
        if (g_active_error_code.load() == 101) {
            trigger_error(0);
        }
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
                g_logger.info("DRM Probe: Found active connected connector %s on %s (index %d)", conn.c_str(), card.c_str(), card_idx);
            } else {
                g_logger.warn("DRM Probe: No connected connector found. Using default fallbacks: %s, connector: %s", probed_card.c_str(), probed_connector.c_str());
            }
        }

        std::string final_card = (g_cfg.drm_card == "auto") ? probed_card : g_cfg.drm_card;
        std::string final_connector = (g_cfg.drm_connector == "auto") ? probed_connector : g_cfg.drm_connector;

        setenv("SDL_VIDEO_KMSDRM_DEVICE", final_card.c_str(), 1);

        std::string index_str;
        if (g_cfg.drm_card == "auto") {
            index_str = std::to_string(probed_card_index);
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

        g_logger.info("KMSDRM Auto-Config: Using card %s (index %s), connector %s", final_card.c_str(), index_str.c_str(), final_connector.c_str());
    }

    // --- SDL3 Init ---
    g_logger.info("Initializing SDL3 (%dx%d)...", g_cfg.screen_w, g_cfg.screen_h);
    std::string splash_file = g_cfg.splash_file.empty() ? "src/splash.png" : g_cfg.splash_file;

    if (!g_renderer.init(g_cfg.screen_w, g_cfg.screen_h, g_cfg.fullscreen)) {
        g_logger.error("SDL3 initialization failed"); return 1;
    }
    g_logger.info("SDL3 context created: %dx%d", g_renderer.screen_w, g_renderer.screen_h);

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

    // --- Fast-path: skip scan+cache if DB already exists ---
    std::string db_path = cache_dir + "/cache.db";
    struct stat db_stat{};
    bool db_exists = (stat(db_path.c_str(), &db_stat) == 0 && db_stat.st_size > 0);

    if (db_exists) {
        bool db_ok = verify_database(db_path);
        if (!db_ok) {
            g_logger.error("CORRUPT DB: cache.db is corrupted — removing and will rebuild");
            trigger_error(401); // E401: SQLITE_DB_CORRUPTED
            std::filesystem::remove(db_path);
            std::filesystem::remove(db_path + "-wal");
            std::filesystem::remove(db_path + "-shm");
            db_exists = false;
        }
    }
    if (db_exists) {
        CacheManager* fast_cache = new CacheManager();
        if (fast_cache->open(cache_dir)) {
            g_cache = fast_cache;
            if (g_cfg.reset_cooldown_on_restart) {
                g_cache->reset_all_cooldowns();
            }
            sqlite3_stmt* stmt = nullptr;
            int load_rc = sqlite3_prepare_v2(fast_cache->db,
                "SELECT path, type, w, h, duration, exif, last_shown, is_camera FROM cache WHERE bad = 0;",
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
                    mi.exif_rotation = sqlite3_column_int(stmt, 5);
                    mi.last_shown = sqlite3_column_int64(stmt, 6);
                    mi.is_camera = sqlite3_column_int(stmt, 7);
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
                int current_err = g_active_error_code.load();
                if (current_err == 401 || current_err == 413) {
                    trigger_error(0);
                }
                g_database_complete.store(true);
            } else {
                g_logger.warn("Cache DB loaded 0 valid items — will re-scan and purge empty/corrupt DB files");
                g_cache = nullptr;
                fast_cache->close();
                delete fast_cache;
                std::filesystem::remove(db_path);
                std::filesystem::remove(db_path + "-wal");
                std::filesystem::remove(db_path + "-shm");
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
        int cached_total = (int)g_scanned_items.size();
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

        // Thread-safe progress callback updating only the atomic counter
        auto safe_progress_callback = [&](int count) {
            scan_count.store(count, std::memory_order_relaxed);
        };

        int depth = 10;
        { std::lock_guard lk(g_config_mtx); depth = g_cfg.scan_depth; }

        std::thread scan_thread([&]() {
            scan_directory(media_dir, depth, g_scanned_items, safe_progress_callback);
            scan_done.store(true);
        });

        // Main thread polls and renders splash safely on EGL context
        while (!scan_done.load()) {
            dot_counter++;
            g_renderer.render_splash(2, (int)scan_count.load(), 0, 0, "SCANNING", dot_counter, nullptr, false);
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

        g_cache->begin_transaction();
        int cached = 0;
        auto last_render = std::chrono::steady_clock::now() - std::chrono::seconds(1);

        int total_scanned = (int)g_scanned_items.size();

        for (int i = 0; i < total_scanned; i++) {
            auto& mi = g_scanned_items[i];

            if (g_cache->load_cached(mi)) {
                mi.cached = true;
                cached++;
            } else {
                if (mi.type == "image") {
                    mi.exif_rotation = 1;
                    mi.width = 1920; mi.height = 1080;
                    mi.creation_time = ImageLoader::get_creation_time(mi.path);
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
                g_renderer.render_splash(3, total_scanned, total_scanned, cached, "CACHING",
                    dot_counter, get_display_path(mi.path).c_str(), false);
                last_render = render_now;
            }
        }
        g_cache->commit_transaction();
        g_logger.info("Cache complete: %d items", cached);
        int current_err = g_active_error_code.load();
        if (current_err == 401 || current_err == 413) {
            trigger_error(0);
        }

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
            if (g_active_error_code.load() == 808) {
                trigger_error(0);
            }
        }

        g_renderer.render_splash(3, total_scanned, total_scanned, cached, "CACHING", dot_counter, nullptr, false);
        SDL_Delay(500);
        g_database_complete.store(true);
    }

    // --- Dynamic Seasonal & Cooldown filter ---
    int cooldown_days = 330;
    int window_days = 5;
    {
        std::lock_guard lock(g_config_mtx);
        cooldown_days = g_cfg.cooldown_days;
        window_days = g_cfg.scan_window_days;
    }
    g_eligible = filter_playlist(g_scanned_items, cooldown_days, window_days);
    g_logger.info("Initial Dynamic Playlist Setup: %d items eligible (cooldown=%d days, seasonal window=%d days)",
        (int)g_eligible.size(), cooldown_days, window_days);

    if (g_eligible.empty()) {
        g_logger.warn("No eligible items after dynamic filters");
        g_renderer.render_splash(4, 0, 0, 0, "DONE", ++dot_counter, nullptr, false);
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
            std::lock_guard lock(g_config_mtx);
            play_just_photos = g_cfg.play_just_photos;
            play_just_videos = g_cfg.play_just_videos;
            videos_per_photos = g_cfg.videos_per_photos;
            shuffle_enabled = g_cfg.shuffle;
        }
        organize_playlist(g_eligible, videos_per_photos, play_just_photos, play_just_videos, shuffle_enabled);
    }

    // Start HTTP Web Remote Dashboard if enabled
    {
        bool http_srv = false;
        int http_prt;
        {
            std::lock_guard lock(g_config_mtx);
            http_srv = g_cfg.http_enabled && g_cfg.web_dashboard_enabled;
            http_prt = g_cfg.http_port;
        }
        if (http_srv) {
            g_logger.info("HTTP: Starting background web remote server on port %d...", http_prt);
            start_http_server(http_prt);
        }
    }

    // Start MQTT subscriber client
    start_mqtt_client();

    // Start Google Photos background sync thread
    g_google_photos.start();

    // Start software watchdog thread
    g_watchdog_running.store(true);
    g_watchdog_thread = std::thread(watchdog_loop);

    g_logger.info("Starting slideshow with %zu items", g_eligible.size());

    g_transition = new TransitionEngine();
    g_transition->set_renderer(&g_renderer);
    g_overlay = new OverlayManager(&g_renderer);
    g_overlay->init();

    // Check if touchscreen mode is enabled and verify physical device presence
    {
        bool touch_enabled = false;
        {
            std::lock_guard lk(g_config_mtx);
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
                g_logger.info("TOUCH_INPUT: Found %d active touch input device(s).", touch_count);
                if (g_active_error_code.load() == 619) {
                    trigger_error(0);
                }
            }
        }
    }

    // Initialize background preload queue
    g_preload = new PreloadQueue(g_cfg.preload_capacity, g_cfg.preload_workers, g_renderer.sdl_renderer);
    g_preload->start();

    g_logger.info("Starting slideshow loop with %zu items", g_eligible.size());
    current_idx = 0;
    double item_timer = 0.0;
    auto last_frame_time = std::chrono::steady_clock::now();

    std::shared_ptr<ImageData> current_data = nullptr;
    std::shared_ptr<ImageData> current_twin_data = nullptr;
    std::shared_ptr<ImageData> next_data = nullptr;
    std::shared_ptr<ImageData> next_twin_data = nullptr;
    bool next_is_twin = false;
    SDL_Texture* current_tex = nullptr;
    SDL_Texture* transition_prev_target = nullptr;
    SDL_Texture* transition_next_target = nullptr;
    SDL_Rect fit_rect{0, 0, 0, 0};

    // Find the first valid item to display, skipping and removing bad/missing files
    int load_attempts = 0;
    while (load_attempts < (int)g_eligible.size() && load_attempts < 20) {
        std::string path = g_eligible[current_idx].path;
        if (!file_exists(path)) {
            g_logger.warn("MISSING_FILE: First media file is missing/deleted from disk: %s", path.c_str());
            if (is_media_dir_healthy(g_cfg.media_dir)) {
                if (g_cache) g_cache->mark_bad(path);

                // Erase from g_scanned_items and g_eligible
                auto it_scanned = std::remove_if(g_scanned_items.begin(), g_scanned_items.end(),
                    [&](const MediaItem& item) { return item.path == path; });
                if (it_scanned != g_scanned_items.end()) {
                    g_scanned_items.erase(it_scanned, g_scanned_items.end());
                }
                g_eligible.erase(g_eligible.begin() + current_idx);
            } else {
                g_logger.warn("Media directory is unhealthy/unmounted. Skipping removal of first item: %s", path.c_str());
            }

            if (g_eligible.empty()) break;
            if (current_idx >= (int)g_eligible.size()) current_idx = 0;
            load_attempts++;
            continue;
        }

        if (g_eligible[current_idx].type == "video") {
            // First item is a video, let the main loop play it!
            g_logger.info("First item in playlist is a video: %s", g_eligible[current_idx].path.c_str());
            break;
        } else {
            // First item is an image, check if should be twin portrait
            bool is_twin = should_be_twin_portrait(g_eligible, current_idx);
            if (is_twin) {
                int next_idx = (current_idx + 1) % (int)g_eligible.size();
                std::string path_l = g_eligible[current_idx].path;
                std::string path_r = g_eligible[next_idx].path;

                bool exists_l = file_exists(path_l);
                bool exists_r = file_exists(path_r);
                std::shared_ptr<ImageData> l_data = nullptr;
                std::shared_ptr<ImageData> r_data = nullptr;
                if (exists_l && exists_r) {
                    l_data = ImageLoader::load(path_l);
                    r_data = ImageLoader::load(path_r);
                }

                // Re-validate indices since we unlocked
                if (g_eligible.empty()) break;
                if (current_idx >= (int)g_eligible.size()) current_idx = 0;
                next_idx = (current_idx + 1) % (int)g_eligible.size();
                path_l = g_eligible[current_idx].path;
                path_r = g_eligible[next_idx].path;

                if (!exists_l) {
                    g_logger.warn("MISSING_FILE: Left twin file missing: %s", path_l.c_str());
                    if (is_media_dir_healthy(g_cfg.media_dir)) {
                        if (g_cache) g_cache->mark_bad(path_l);
                        g_eligible.erase(g_eligible.begin() + current_idx);
                    }
                    if (g_eligible.empty()) break;
                    if (current_idx >= (int)g_eligible.size()) current_idx = 0;
                    load_attempts++;
                    continue;
                }
                if (!exists_r) {
                    g_logger.warn("MISSING_FILE: Right twin file missing: %s", path_r.c_str());
                    if (is_media_dir_healthy(g_cfg.media_dir)) {
                        if (g_cache) g_cache->mark_bad(path_r);
                        g_eligible.erase(g_eligible.begin() + next_idx);
                    }
                    if (g_eligible.empty()) break;
                    if (current_idx >= (int)g_eligible.size()) current_idx = 0;
                    load_attempts++;
                    continue;
                }

                current_data = l_data;
                current_twin_data = r_data;

                if (current_data && current_data->valid && current_twin_data && current_twin_data->valid) {
                    ImageLoader::load_texture(current_data.get(), g_renderer.sdl_renderer);
                    ImageLoader::load_texture(current_twin_data.get(), g_renderer.sdl_renderer);
                    int curr_err = g_active_error_code.load();
                    if (curr_err == 201 || curr_err == 101) {
                        trigger_error(0);
                    }
                    current_tex = current_data->texture;
                    mark_item_shown(path_l, false);
                    mark_item_shown(path_r, false);

                    // Update metadata
                    g_eligible[current_idx].width = current_data->width;
                    g_eligible[current_idx].height = current_data->height;
                    g_eligible[current_idx].exif_rotation = current_data->exif_rotation;

                    g_eligible[next_idx].width = current_twin_data->width;
                    g_eligible[next_idx].height = current_twin_data->height;
                    g_eligible[next_idx].exif_rotation = current_twin_data->exif_rotation;

                    if (g_cache) {
                        g_cache->upsert(g_eligible[current_idx], 0);
                        g_cache->upsert(g_eligible[next_idx], 0);
                    }

                    g_logger.info("First item is twin-portrait, loaded successfully: %s and %s", path_l.c_str(), path_r.c_str());
                    break;
                } else {
                    if (!current_data || !current_data->valid) {
                        if (is_media_dir_healthy(g_cfg.media_dir) && (!current_data || !current_data->transient_error)) {
                            if (g_cache) g_cache->mark_bad(path_l);
                            g_eligible.erase(g_eligible.begin() + current_idx);
                        }
                    } else if (!current_twin_data || !current_twin_data->valid) {
                        if (is_media_dir_healthy(g_cfg.media_dir) && (!current_twin_data || !current_twin_data->transient_error)) {
                            if (g_cache) g_cache->mark_bad(path_r);
                            g_eligible.erase(g_eligible.begin() + next_idx);
                        }
                    }
                    current_data = nullptr;
                    current_twin_data = nullptr;
                    if (g_eligible.empty()) break;
                    if (current_idx >= (int)g_eligible.size()) current_idx = 0;
                    load_attempts++;
                }
            } else {
                std::shared_ptr<ImageData> single_data = ImageLoader::load(g_eligible[current_idx].path);

                if (g_eligible.empty()) break;
                if (current_idx >= (int)g_eligible.size()) current_idx = 0;

                current_data = single_data;
                if (current_data && current_data->valid) {
                    ImageLoader::load_texture(current_data.get(), g_renderer.sdl_renderer);
                    int curr_err = g_active_error_code.load();
                    if (curr_err == 201 || curr_err == 101) {
                        trigger_error(0);
                    }
                    current_tex = current_data->texture;
                    current_twin_data = nullptr;
                    mark_item_shown(g_eligible[current_idx].path, false);

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
                    if (g_cache) g_cache->upsert(g_eligible[current_idx], 0);

                    g_renderer.calculate_fit_rect(g_eligible[current_idx].width, g_eligible[current_idx].height, fit_rect);
                    g_logger.info("First item is an image, loaded successfully: %s (%dx%d)", g_eligible[current_idx].path.c_str(), g_eligible[current_idx].width, g_eligible[current_idx].height);
                    break;
                } else {
                    if (is_media_dir_healthy(g_cfg.media_dir) && (!single_data || !single_data->transient_error)) {
                        if (g_cache) g_cache->mark_bad(g_eligible[current_idx].path);
                        g_logger.warn("Bad first image, skipping: %s", g_eligible[current_idx].path.c_str());
                        g_eligible.erase(g_eligible.begin() + current_idx);
                    } else {
                        g_logger.warn("Failed to load first image due to network/unmounted media dir: %s", g_eligible[current_idx].path.c_str());
                    }
                    if (g_eligible.empty()) break;
                    if (current_idx >= (int)g_eligible.size()) current_idx = 0;
                    load_attempts++;
                }
            }
        }
    }

    bool transitioning = false;
    std::string transition_effect;
    { std::lock_guard lk(g_config_mtx); transition_effect = g_cfg.transition_effect; }

    // --- Start Background Watchman Thread ---
    g_watchman_running.store(true);
    g_watchman_thread = std::thread(watchman_loop);
    g_logger.info("Watchman: Background watchman thread spawned successfully.");

    double fps_timer = 0.0;
    int frame_counter = 0;
    int active_fps = 60;

    while (g_running.load()) {
        if (g_config_changed.load()) {
            g_logger.info("MAIN_LOOP: Dynamic configuration reload triggered!");
            {
                std::lock_guard lk(g_config_mtx);
                int active_port = g_cfg.http_port;
                g_cfg.load(config_path);
                g_cfg.http_port = active_port;
                transition_effect = g_cfg.transition_effect;
            }
            g_config_changed.store(false);
        }
        auto now = std::chrono::steady_clock::now();
        double dt = std::chrono::duration<double>(now - last_frame_time).count();
        last_frame_time = now;

        g_last_heartbeat_time.store(static_cast<int64_t>(std::time(nullptr)));

        // Periodic touch screen check (every 5 seconds)
        static double touch_check_timer = 0.0;
        touch_check_timer += dt;
        if (touch_check_timer >= 5.0) {
            touch_check_timer = 0.0;
            bool touch_enabled = false;
            {
                std::lock_guard lk(g_config_mtx);
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
                    if (g_active_error_code.load() == 619) {
                        g_logger.info("TOUCH_INPUT: Touch screen device reconnected.");
                        trigger_error(0);
                    }
                }
            } else {
                if (g_active_error_code.load() == 619) {
                    trigger_error(0);
                }
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
                    g_logger.info("SLIDESHOW: Play/pause toggled via pop-up menu. Paused = %s", g_slideshow_paused.load() ? "YES" : "NO");
                    break;
                case 1:
                    {
                        std::lock_guard lk(g_config_mtx);
                        g_cfg.shuffle = !g_cfg.shuffle;
                        g_cfg.save(config_path);
                    }
                    g_config_changed.store(true);
                    g_logger.info("SLIDESHOW: Shuffle toggled via pop-up menu.");
                    break;
                case 2:
                    {
                        std::lock_guard lk(g_config_mtx);
                        double curr = g_cfg.transition_delay;
                        if (curr < 15.0) g_cfg.transition_delay = 30.0;
                        else if (curr < 45.0) g_cfg.transition_delay = 60.0;
                        else if (curr < 90.0) g_cfg.transition_delay = 120.0;
                        else if (curr < 210.0) g_cfg.transition_delay = 300.0;
                        else g_cfg.transition_delay = 10.0;
                        g_cfg.save(config_path);
                    }
                    g_config_changed.store(true);
                    g_logger.info("SLIDESHOW: Interval changed via pop-up menu.");
                    break;
                case 3:
                    {
                        bool expected = g_screen_blanked.load();
                        bool desired = !expected;
                        while (!g_screen_blanked.compare_exchange_weak(expected, desired)) {
                            desired = !expected;
                        }
                        set_display_power(expected);
                        std::string prefix;
                        { std::lock_guard lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; }
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
                    if (g_screen_blanked.exchange(false)) {
                        set_display_power(true);
                        g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
                        std::string prefix;
                        { std::lock_guard lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; }
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
                    if (g_screen_blanked.exchange(false)) {
                        set_display_power(true);
                        g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
                        std::string prefix;
                        { std::lock_guard lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; }
                        mqtt_publish(prefix + "/status/screen", "ON", true);
                    }
                    if (event.button.button == SDL_BUTTON_RIGHT) {
                        if (g_overlay) {
                            if (g_mpv_player.is_active()) {
                                g_logger.info("Right-click during video: stopping mpv to open config menu.");
                                g_mpv_player.stop();
                            }
                            g_overlay->menu_active = !g_overlay->menu_active;
                        }
                    } else if (event.button.button == SDL_BUTTON_LEFT) {
                        if (g_overlay) {
                            bool touch_mode = false;
                            {
                                std::lock_guard lk(g_config_mtx);
                                touch_mode = g_cfg.touch_enabled;
                            }
                            if (touch_mode) {
                                float mx = event.button.x;
                                float my = event.button.y;
                                if (g_overlay->menu_active || g_overlay->keyboard_active || g_overlay->nav_overlay_active) {
                                    g_overlay->handle_touch_click(mx, my);
                                } else {
                                    if (g_mpv_player.is_active()) {
                                        g_logger.info("TOUCH_INPUT: Touch during video: stopping mpv to open touch navigation overlay.");
                                        g_mpv_player.stop();
                                    }
                                    g_overlay->nav_overlay_active = true;
                                    g_overlay->nav_overlay_show_time = SDL_GetTicks();
                                    g_logger.info("TOUCH_INPUT: Activating navigation overlay via click.");
                                }
                            } else {
                                g_remote_command.store(1);
                            }
                        }
                    }
                    break;
                case SDL_EVENT_FINGER_DOWN:
                    if (g_screen_blanked.exchange(false)) {
                        set_display_power(true);
                        g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
                        std::string prefix;
                        { std::lock_guard lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; }
                        mqtt_publish(prefix + "/status/screen", "ON", true);
                    }
                    if (g_overlay) {
                        bool touch_mode = false;
                        {
                            std::lock_guard lk(g_config_mtx);
                            touch_mode = g_cfg.touch_enabled;
                        }
                        if (touch_mode) {
                            float mx = event.tfinger.x * g_renderer.screen_w;
                            float my = event.tfinger.y * g_renderer.screen_h;
                            if (g_overlay->menu_active || g_overlay->keyboard_active || g_overlay->nav_overlay_active) {
                                g_overlay->handle_touch_click(mx, my);
                            } else {
                                if (g_mpv_player.is_active()) {
                                    g_logger.info("TOUCH_INPUT: Finger touch during video: stopping mpv to open touch navigation overlay.");
                                    g_mpv_player.stop();
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

        // Check motion sensor cooldown
        {
            bool mqtt_on = false;
            int cooldown = 0;
            { std::lock_guard lk(g_config_mtx); mqtt_on = g_cfg.mqtt_enabled; cooldown = g_cfg.mqtt_motionsensor_cooldown; }
            if (mqtt_on && cooldown > 0) {
                int64_t now_ts = static_cast<int64_t>(std::time(nullptr));
                int64_t last_motion = g_last_motion_time.load();
                if (last_motion > 0 && (now_ts - last_motion >= cooldown)) {
                    g_logger.info("Motion sensor cooldown threshold reached (%d seconds). Blanking screen.", cooldown);
                    if (!g_screen_blanked.load()) {
                        g_screen_blanked.store(true);
                        set_display_power(false);
                        std::string prefix;
                        { std::lock_guard lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; }
                        mqtt_publish(prefix + "/status/screen", "OFF", true);
                    }
                }
            }
        }

        // Handle screen blanking state
        if (g_screen_blanked.load()) {
            if (g_mpv_player.is_active()) {
                g_logger.info("Screen blanked: stopping active video playback.");
                g_mpv_player.stop();
            }
            g_renderer.clear(0, 0, 0, 255);
            g_renderer.present();
            SDL_Delay(100);
            continue;
        }

        int cmd = g_remote_command.exchange(0);

        // --- Mutex-Guarded playlist lookup & manipulation ---
        std::unique_lock<std::mutex> playlist_lock(g_playlist_mtx);

        // Apply Pause Toggle command
        if (cmd == 3) {
            g_slideshow_paused = !g_slideshow_paused.load();
            g_logger.info("SLIDESHOW: Pause toggled remotely. Current pause state = %s", g_slideshow_paused.load() ? "PAUSED" : "PLAYING");
        }

        // Validate current_idx (skip stat() to avoid CIFS hang)
        if (!g_eligible.empty()) {
            if (current_idx >= (int)g_eligible.size()) {
                current_idx = 0;
            }
        }

        if (g_eligible.empty()) {
            playlist_lock.unlock();
            SDL_Delay(100);
            continue;
        }

        // Apply Skip Arrow commands
        if (cmd == 1 || cmd == 2) {
            item_timer = 0.0;
            bool was_video = (g_eligible[current_idx].type == "video");
            if (g_mpv_player.is_active()) {
                g_logger.info("Interrupted video playback via skip request: stopping mpv.");
                g_mpv_player.stop();
            }
            if (was_video) {
                current_data = nullptr;
                current_twin_data = nullptr;
                current_tex = nullptr;
                g_renderer.clear(0, 0, 0, 255);
                g_renderer.present();
            }
            if (g_transition) {
                g_transition->reset();
            }
            if (transition_prev_target) { SDL_DestroyTexture(transition_prev_target); transition_prev_target = nullptr; }
            if (transition_next_target) { SDL_DestroyTexture(transition_next_target); transition_next_target = nullptr; }
            
            transitioning = true;
            if (cmd == 1) {
                advance_playlist(current_twin_data ? 2 : 1);
            } else {
                advance_playlist(current_twin_data ? -2 : -1);
            }
        }

        if (transitioning && !g_transition->is_active()) {
            TransitionEffect effect = TransitionEffect::Fade;
            if (transition_effect == "wipe") effect = TransitionEffect::WipeLeft;
            else if (transition_effect == "ken_burns") effect = TransitionEffect::KenBurns;
            else if (transition_effect == "pixelate") effect = TransitionEffect::Pixelate;
            else if (transition_effect == "dissolve") effect = TransitionEffect::Dissolve;
            else if (transition_effect == "crossfade") effect = TransitionEffect::Fade;

            float duration = 0.0f, kb_zoom = 0.1f;
            { std::lock_guard lk(g_config_mtx); duration = g_cfg.transition_duration; kb_zoom = g_cfg.ken_burns_zoom; }
            g_transition->start(effect, duration, 0, kb_zoom);
        }

        // --- Video Player Handling ---
        if (g_eligible[current_idx].type == "video") {
            if (g_mpv_player.is_active()) {
                // Peek if the next item is a video
                int next_idx = (current_idx + 1) % (int)g_eligible.size();
                bool next_is_video = (g_eligible[next_idx].type == "video");

                // If the next item is also a video, tell check_status not to reclaim DRM master context
                if (!g_mpv_player.check_status(!next_is_video)) {
                    g_logger.info("Video EOF detected: advancing playlist.");
                    current_data = nullptr;
                    current_twin_data = nullptr;
                    current_tex = nullptr;
                    g_renderer.clear(0, 0, 0, 255);
                    g_renderer.present();
                    transitioning = true;
                    advance_playlist(1);
                }
                playlist_lock.unlock(); // Unlock before delay sleep
                SDL_Delay(50); continue;
            }
            if (transitioning) {
                // Wait for the transition to finish rendering before launching mpv.
                // Proceed to the image rendering path which handles active transition.
            } else {
                int volume;
                { std::lock_guard lock(g_config_mtx); volume = g_cfg.video_volume; }
                
                std::string video_path = g_eligible[current_idx].path;
                g_logger.info("Playing video: %s", video_path.c_str());
                
                playlist_lock.unlock(); // Unlock while launching the mpv process
                if (!g_mpv_player.play(video_path, volume)) {
                    g_logger.error("Failed to play video, skipping to next.");
                    playlist_lock.lock(); // Re-lock
                    current_data = nullptr;
                    current_twin_data = nullptr;
                    current_tex = nullptr;
                    g_renderer.clear(0, 0, 0, 255);
                    g_renderer.present();
                    transitioning = true;
                    advance_playlist(1);
                    playlist_lock.unlock();
                    continue;
                } else {
                    playlist_lock.lock(); // Re-lock to safely free image texture VRAM during playback
                    mark_item_shown(video_path, false); // Mark the video as shown for cooldown
                    if (current_data) {
                        current_data = nullptr;
                        current_twin_data = nullptr;
                        current_tex = nullptr;
                    }
                    if (next_data) {
                        next_data = nullptr;
                        next_twin_data = nullptr;
                    }
                    transitioning = false; // Bypass transitioning during video rendering
                    if (g_transition) {
                        g_transition->reset();
                    }
                    playlist_lock.unlock();
                }
                SDL_Delay(50); continue;
            }
        }

        // --- Image Rendering Handling ---
        double dt_scaled = g_slideshow_paused.load() ? 0.0 : dt;
        item_timer += dt_scaled;
        g_item_timer.store((float)item_timer);
        if (transitioning) {
            // Load next_data exactly once at the beginning of the transition
            if (!next_data) {
                int next_idx = current_idx % (int)g_eligible.size();
                bool is_twin = should_be_twin_portrait(g_eligible, next_idx);
                next_is_twin = is_twin;

                int next_idx_twin = -1;
                std::string next_path, next_path_twin;
                if (is_twin) {
                    next_idx_twin = (next_idx + 1) % (int)g_eligible.size();
                    next_path = g_eligible[next_idx].path;
                    next_path_twin = g_eligible[next_idx_twin].path;
                } else {
                    next_path = g_eligible[next_idx].path;
                }

                // Capture paths and type under lock, then unlock for I/O to prevent race on g_eligible
                bool next_is_video = g_eligible[next_idx].type == "video";
                playlist_lock.unlock();
                if (g_preload && !next_is_video) {
                    next_data = g_preload->try_dequeue(next_path);
                    if (is_twin) {
                        next_twin_data = g_preload->try_dequeue(next_path_twin);
                    }
                }
                // Fallback to synchronous load if preloader bypassed or missed (skip for video items)
                if (!next_is_video) {
                    if (!next_data || !next_data->valid) {
                        next_data = ImageLoader::load(next_path);
                    }
                    if (is_twin && (!next_twin_data || !next_twin_data->valid)) {
                        next_twin_data = ImageLoader::load(next_path_twin);
                    }
                }
                playlist_lock.lock();

                // Update metadata using path-safe lookups (indices may have shifted)
                auto update_meta_safe = [&](const std::string& expected_path, const std::shared_ptr<ImageData>& data) {
                    int found = -1;
                    for (int i = 0; i < (int)g_eligible.size(); i++) {
                        if (g_eligible[i].path == expected_path) { found = i; break; }
                    }
                    if (found != -1) {
                        g_eligible[found].width = data->width;
                        g_eligible[found].height = data->height;
                        g_eligible[found].exif_rotation = data->exif_rotation;
                        if (g_cache) g_cache->upsert(g_eligible[found], 0);
                    }
                };
                if (g_eligible[next_idx].type != "video") {
                    update_meta_safe(next_path, next_data);
                    if (is_twin && next_twin_data) {
                        update_meta_safe(next_path_twin, next_twin_data);
                    }
                }

                if (next_data && next_data->valid && (!is_twin || (next_twin_data && next_twin_data->valid))) {
                    ImageLoader::load_texture(next_data.get(), g_renderer.sdl_renderer);
                    if (is_twin && next_twin_data) {
                        ImageLoader::load_texture(next_twin_data.get(), g_renderer.sdl_renderer);
                    }
                } else {
                    if (g_eligible[next_idx].type != "video") {
                        if (is_media_dir_healthy(g_cfg.media_dir)) {
                            if (g_cache && next_data && !next_data->valid && !next_data->transient_error) {
                                g_cache->mark_bad(next_path);
                            }
                            if (is_twin && g_cache && next_twin_data && !next_twin_data->valid && !next_twin_data->transient_error) {
                                g_cache->mark_bad(next_path_twin);
                            }
                        }
                    }
                    next_data = nullptr;
                    next_twin_data = nullptr;
                }
            }

            bool is_video_transition = (g_eligible[current_idx].type == "video");
            bool load_success = is_video_transition || (next_data && next_data->texture && (!next_is_twin || (next_twin_data && next_twin_data->texture)));

            if (load_success) {
                int curr_err = g_active_error_code.load();
                if (curr_err == 201 || curr_err == 101) {
                    trigger_error(0);
                }
                if (g_consecutive_failures.load() > 0) {
                    g_consecutive_failures.store(0);
                }
                if (g_offline_mode.exchange(false)) {
                    g_logger.info("SLIDESHOW: Exiting Offline Recovery Mode. Connection to NAS restored.");
                }
                // Generate intermediate flat textures representing full screen layout states
                bool just_started = false;
                if (!transition_prev_target) {
                    transition_prev_target = render_state_to_texture(g_renderer.sdl_renderer, g_renderer.screen_w, g_renderer.screen_h, current_data, current_twin_data, item_timer);
                    just_started = true;
                }
                if (!transition_next_target) {
                    transition_next_target = render_state_to_texture(g_renderer.sdl_renderer, g_renderer.screen_w, g_renderer.screen_h, next_data, next_twin_data, 0.0);
                    just_started = true;
                }

                if (transition_prev_target && transition_next_target) {
                    if (just_started) {
                        last_frame_time = std::chrono::steady_clock::now();
                        dt_scaled = 0.0;
                    }
                    g_renderer.clear(0, 0, 0, 255);
                    g_transition->update(dt_scaled);
                    g_transition->render(transition_prev_target, transition_next_target, g_renderer.screen_w, g_renderer.screen_h);
                    if (g_overlay) {
                        g_overlay->draw_all(current_idx, (int)g_eligible.size(),
                            &g_eligible[current_idx],
                            next_twin_data ? &g_eligible[(current_idx + 1) % (int)g_eligible.size()] : nullptr,
                            0.0, false, active_fps, next_data ? next_data.get() : nullptr, next_twin_data ? next_twin_data.get() : nullptr);
                    }
                    g_renderer.present();

                    if (g_transition->get_progress() >= 1.0f) {
                        transitioning = false;
                        item_timer = 0.0;

                        current_data = next_data;
                        current_twin_data = next_twin_data;
                        next_data = nullptr;
                        next_twin_data = nullptr;
                        next_is_twin = false;

                        if (transition_prev_target) { SDL_DestroyTexture(transition_prev_target); transition_prev_target = nullptr; }
                        if (transition_next_target) { SDL_DestroyTexture(transition_next_target); transition_next_target = nullptr; }

                        current_tex = current_data ? current_data->texture : nullptr;
                        if (!is_video_transition) {
                            mark_item_shown(g_eligible[current_idx].path, false);
                            if (current_twin_data) {
                                mark_item_shown(g_eligible[(current_idx + 1) % (int)g_eligible.size()].path, false);
                            }
                        }

                        // Update in-memory item metadata with actual dimensions and EXIF rotation
                        if (current_data) {
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
                            if (g_cache) g_cache->upsert(g_eligible[current_idx], 0);
                            g_renderer.calculate_fit_rect(current_data->width, current_data->height, fit_rect);
                        }
                    }
                } else {
                    if (transition_prev_target) { SDL_DestroyTexture(transition_prev_target); transition_prev_target = nullptr; }
                    if (transition_next_target) { SDL_DestroyTexture(transition_next_target); transition_next_target = nullptr; }

                    transitioning = false;
                    item_timer = 0.0;
                    if (g_transition) g_transition->reset();

                    if (next_data) {
                        current_data = next_data;
                        current_twin_data = next_twin_data;
                        next_data = nullptr;
                        next_twin_data = nullptr;
                        current_tex = current_data->texture;
                        mark_item_shown(g_eligible[current_idx].path, false);
                        if (current_twin_data) {
                            mark_item_shown(g_eligible[(current_idx + 1) % (int)g_eligible.size()].path, false);
                        }

                        // Update metadata
                        if (current_data) {
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
                            if (g_cache) g_cache->upsert(g_eligible[current_idx], 0);
                        }

                        g_renderer.calculate_fit_rect(current_data->width, current_data->height, fit_rect);
                    }
                }
            } else {
                transitioning = false;
                if (g_transition) g_transition->reset();
                if (transition_prev_target) { SDL_DestroyTexture(transition_prev_target); transition_prev_target = nullptr; }
                if (transition_next_target) { SDL_DestroyTexture(transition_next_target); transition_next_target = nullptr; }

                g_consecutive_failures.fetch_add(1);
                g_logger.warn("SLIDESHOW: Failed to load next media item. Consecutive failure count: %d", g_consecutive_failures.load());
                if (g_consecutive_failures.load() >= 3) {
                    if (!g_offline_mode.exchange(true)) {
                        g_logger.warn("SLIDESHOW: Entering Offline Recovery Mode due to multiple consecutive load failures.");
                        // Discard old textures so the fallback splash screen is forced to load
                        current_data = nullptr;
                        current_twin_data = nullptr;
                        current_tex = nullptr;
                    }
                }

                if (g_offline_mode.load()) {
                    item_timer = 0.0;
                    if (!current_tex) {
                        std::string fallback_path = "src/splash.png";
                        if (!file_exists(fallback_path)) {
                            fallback_path = get_exe_dir() + "/src/splash.png";
                            if (!file_exists(fallback_path)) {
                                fallback_path = get_exe_dir() + "/splash.png";
                            }
                        }
                        if (file_exists(fallback_path)) {
                            std::shared_ptr<ImageData> fallback_data = ImageLoader::load(fallback_path);
                            if (fallback_data && fallback_data->valid) {
                                ImageLoader::load_texture(fallback_data.get(), g_renderer.sdl_renderer);
                                current_data = fallback_data;
                                current_twin_data = nullptr;
                                current_tex = current_data->texture;
                                g_renderer.calculate_fit_rect(current_data->width, current_data->height, fit_rect);
                            }
                        }
                    }
                } else {
                    double delay;
                    {
                        std::lock_guard lk(g_config_mtx);
                        delay = g_cfg.transition_delay;
                    }
                    item_timer = std::max(0.0, delay - 2.0);
                    if (next_data) {
                        current_data = next_data;
                        current_twin_data = next_twin_data;
                        next_data = nullptr;
                        next_twin_data = nullptr;
                        current_tex = current_data->texture;
                        mark_item_shown(g_eligible[current_idx].path, false);
                        if (current_twin_data) {
                            mark_item_shown(g_eligible[(current_idx + 1) % (int)g_eligible.size()].path, false);
                        }

                        // Update metadata
                        if (current_data) {
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
                            if (g_cache) g_cache->upsert(g_eligible[current_idx], 0);
                        }

                        g_renderer.calculate_fit_rect(current_data->width, current_data->height, fit_rect);
                    }
                }
            }
        } else {
            bool rendered = false;
            if (current_twin_data && current_twin_data->texture && current_data && current_data->texture) {
                bool has_bias = false;
                bool has_matting = false;
                bool has_border = false;
                int bias_strength = 110;
                float anim_speed = 0.5f;
                std::string style = "edge_glow";
                int border_w = 10;
                bool snap_blurred, snap_matte_color;
                int snap_glow_depth;
                float matte_op, vignette_str;
                {
                    std::lock_guard lock(g_config_mtx);
                    has_bias = g_cfg.bias_lighting;
                    has_matting = g_cfg.matting;
                    has_border = g_cfg.border_enabled;
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

                if (snap_matte_color && current_data) {
                    g_renderer.clear(current_data->matte_r, current_data->matte_g, current_data->matte_b, 255);
                } else {
                    g_renderer.clear(0, 0, 0, 255);
                }

                SDL_Rect rect_l, rect_r;
                int sw = g_renderer.screen_w;
                int sh = g_renderer.screen_h;
                calculate_fit_rect_in_area(current_data->width, current_data->height, 0, 0, sw / 2, sh, rect_l);
                calculate_fit_rect_in_area(current_twin_data->width, current_twin_data->height, sw / 2, 0, sw - (sw / 2), sh, rect_r);

                // 1. Draw background based on style
                std::string snap_bg_style;
                { std::lock_guard lk(g_config_mtx); snap_bg_style = g_cfg.bg_style; }
                if (snap_blurred || snap_bg_style != "photo") {
                    g_renderer.draw_background(current_data.get(), snap_bg_style, (Uint8)(255.0f * vignette_str));
                }

                // 2. Draw matte borders if enabled (solid black base layer) - ONLY if NOT color-matched or blurred!
                if (has_matting && !snap_matte_color && !snap_blurred) {
                    g_renderer.draw_matte_borders(rect_l);
                    g_renderer.draw_matte_borders(rect_r);
                }

                // 3. Color-matched matte for each portrait (opaque and edge-to-edge if enabled)
                if (snap_matte_color) {
                    g_renderer.draw_color_matched_matte(rect_l,
                        current_data->matte_r, current_data->matte_g, current_data->matte_b, matte_op);
                    g_renderer.draw_color_matched_matte(rect_r,
                        current_twin_data->matte_r, current_twin_data->matte_g, current_twin_data->matte_b, matte_op);
                }


                // 4. Draw bias lighting if enabled
                if (has_bias) {
                    int glow = snap_glow_depth;
                    int bw_l = has_border ? border_w : 0;
                    g_renderer.draw_bias_lighting(rect_l, current_data->avg_r, current_data->avg_g, current_data->avg_b,
                        bias_strength, (float)item_timer, anim_speed, style, bw_l, glow);
                    int bw_r = has_border ? border_w : 0;
                    g_renderer.draw_bias_lighting(rect_r, current_twin_data->avg_r, current_twin_data->avg_g, current_twin_data->avg_b,
                        bias_strength, (float)item_timer, anim_speed, style, bw_r, glow);
                }

                // 5. Draw 3D miter borders if enabled
                if (has_border) {
                    g_renderer.draw_3d_border(rect_l, current_data->avg_r, current_data->avg_g, current_data->avg_b, border_w);
                    g_renderer.draw_3d_border(rect_r, current_twin_data->avg_r, current_twin_data->avg_g, current_twin_data->avg_b, border_w);
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
                int snap_bias_strength, snap_border_width, snap_glow_depth;
                float snap_anim_speed;
                std::string snap_anim_style;
                bool snap_blurred, snap_matte_color;
                float matte_op, vignette_str;
                {
                    std::lock_guard lk(g_config_mtx);
                    snap_bias = g_cfg.bias_lighting;
                    snap_matting = g_cfg.matting;
                    snap_border = g_cfg.border_enabled;
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

                if (snap_matte_color && current_data) {
                    g_renderer.clear(current_data->matte_r, current_data->matte_g, current_data->matte_b, 255);
                } else {
                    g_renderer.clear(0, 0, 0, 255);
                }

                // 1. Draw background based on style
                std::string snap_bg_style;
                { std::lock_guard lk(g_config_mtx); snap_bg_style = g_cfg.bg_style; }
                if (snap_blurred || snap_bg_style != "photo") {
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
                    g_renderer.draw_bias_lighting(fit_rect,
                        current_data->avg_r, current_data->avg_g, current_data->avg_b,
                        snap_bias_strength, (float)item_timer, snap_anim_speed, snap_anim_style, bw_param, snap_glow_depth);
                }

                // 5. Draw 3D miter border if enabled
                if (snap_border && current_data) {
                    g_renderer.draw_3d_border(fit_rect,
                        current_data->avg_r, current_data->avg_g, current_data->avg_b, snap_border_width);
                }

                // 6. Draw texture
                SDL_FRect dst = {(float)fit_rect.x, (float)fit_rect.y, (float)fit_rect.w, (float)fit_rect.h};
                SDL_RenderTexture(g_renderer.sdl_renderer, current_tex, nullptr, &dst);
                rendered = true;
            }

            if (rendered) {
                if (g_overlay) {
                    g_overlay->draw_all(current_idx, (int)g_eligible.size(),
                        &g_eligible[current_idx],
                        current_twin_data ? &g_eligible[(current_idx + 1) % (int)g_eligible.size()] : nullptr,
                        item_timer, false, active_fps, current_data.get(), current_twin_data.get());
                }
                g_renderer.present();
            }

            // Preload next items asynchronously while resting
            if (g_preload) {
                int lookahead_idx = g_eligible.empty() ? 0 : (current_idx + (current_twin_data ? 2 : 1)) % (int)g_eligible.size();
                bool next_is_twin = g_eligible.empty() ? false : should_be_twin_portrait(g_eligible, lookahead_idx);
                
                if (lookahead_idx >= 0 && lookahead_idx < (int)g_eligible.size()) {
                    if (g_eligible[lookahead_idx].type == "image") {
                        g_preload->enqueue(g_eligible[lookahead_idx].path);
                    }
                    if (next_is_twin) {
                        int lookahead_idx2 = (lookahead_idx + 1) % (int)g_eligible.size();
                        if (lookahead_idx2 >= 0 && lookahead_idx2 < (int)g_eligible.size()) {
                            if (g_eligible[lookahead_idx2].type == "image") {
                                g_preload->enqueue(g_eligible[lookahead_idx2].path);
                            }
                        }
                    }
                }
            }

            {
                double delay;
                if (g_offline_mode.load()) {
                    delay = 30.0; // Enforce a 30-second back-off delay during offline recovery
                } else {
                    std::lock_guard lk(g_config_mtx);
                    delay = g_cfg.transition_delay;
                }
                if (item_timer >= delay) {
                    transitioning = true;
                    advance_playlist(current_twin_data ? 2 : 1);
                }
            }
        }

        playlist_lock.unlock(); // Unlock before frame sleep throttling
        SDL_Delay(16);
    }

    // --- Cleanup ---
    g_logger.info("Shutting down...");
    
    // Fail-safe: Restore physical display power on exit
    set_display_power(true);
    
    // Stop background HTTP server
    stop_http_server();
    
    // Stop background MQTT client safely
    stop_mqtt_client();

    // Stop background Google Photos sync thread safely
    g_google_photos.stop();
    
    // Stop software watchdog thread safely
    g_watchdog_running.store(false);
    if (g_watchdog_thread.joinable()) {
        g_watchdog_thread.join();
        g_logger.info("Watchdog: Software watchdog thread stopped successfully.");
    }
    
    // Stop background watchman thread safely
    g_watchman_running.store(false);
    if (g_watchman_thread.joinable()) {
        int timeout_ms = 500;
        while (timeout_ms > 0 && !g_watchman_finished.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            timeout_ms -= 50;
        }
        if (g_watchman_finished.load()) {
            g_watchman_thread.join();
            g_logger.info("Watchman: Background watchman thread stopped successfully.");
        } else {
            g_watchman_thread.detach();
            g_logger.warn("Watchman: Watchman thread did not exit cleanly within 500ms. Detached.");
        }
    }

    if (g_mpv_player.is_active()) g_mpv_player.stop();
    if (g_transition) { g_transition->reset(); delete g_transition; }
    if (g_overlay) { g_overlay->cleanup(); delete g_overlay; }
    if (g_preload) {
        g_preload->shutdown();
        delete g_preload;
        g_preload = nullptr;
    }
    if (current_data) { current_data = nullptr; }
    if (current_twin_data) { current_twin_data = nullptr; }
    if (next_data) { next_data = nullptr; }
    if (next_twin_data) { next_twin_data = nullptr; }
    if (transition_prev_target) { SDL_DestroyTexture(transition_prev_target); }
    if (transition_next_target) { SDL_DestroyTexture(transition_next_target); }
    if (g_cache) { g_cache->close(); delete g_cache; }
    g_renderer.cleanup();

    flock(lock_fd, LOCK_UN); close(lock_fd);
    g_logger.info("piTrove v%s shutdown complete", VERSION);
    return 0;
}

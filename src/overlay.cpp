#include "overlay.h"
#include "util.h"
#include "config.h"
#include "media_item.h"
#include "image_loader.h"
#include "cache.h"
#include "mqtt.h"
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <format>


// Cached clock texture to avoid re-rendering every frame
static std::string g_last_clock_text;


OverlayManager::OverlayManager(Renderer* renderer)
    : renderer(renderer), font_renderer(nullptr), overlay_font(nullptr) {}

OverlayManager::~OverlayManager() {
    cleanup();
}

void OverlayManager::init() {
    g_logger.info("TRACE: OverlayManager::init start");
    cleanup();

    font_renderer = new FontRenderer(renderer);

    std::string exe_dir = get_exe_dir();
    std::string font_path = "";
    {
        std::shared_lock lock(g_config_mtx);
        if (g_cfg.font_path != "auto" && !g_cfg.font_path.empty()) {
            font_path = g_cfg.font_path;
        }
    }

    if (font_path.empty() || !file_exists(font_path)) {
        if (!font_path.empty()) {
            g_logger.warn("Configured font_path '{}' not found, falling back to defaults", font_path.c_str());
        }
        font_path = "/app/src/fonts/DejaVuSansMono-Bold.ttf";
        if (!file_exists(font_path)) {
            font_path = exe_dir + "/src/fonts/DejaVuSansMono-Bold.ttf";
            if (!file_exists(font_path)) {
                font_path = exe_dir + "/fonts/DejaVuSansMono-Bold.ttf";
                if (!file_exists(font_path)) {
                    font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf";
                    if (!file_exists(font_path)) {
                        font_path = "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf";
                    }
                }
            }
        }
    }

    if (file_exists(font_path)) {
        // Load default size 20, font_renderer load_font handles caching and resizing dynamically
        overlay_font = &font_renderer->load_font(font_path, 20);
        font_loaded = true;
        g_logger.info("Overlay manager font loaded: {}", font_path.c_str());
    } else {
        g_logger.error("No suitable monospace font found for OSD overlays.");
        font_loaded = false;
    }
}

void OverlayManager::cleanup() {
    g_logger.info("TRACE: OverlayManager::cleanup");
    if (font_renderer) {
        delete font_renderer;
        font_renderer = nullptr;
    }
    overlay_font = nullptr;
    font_loaded = false;
}

GpuColor OverlayManager::get_color_from_str(const std::string& name) {
    if (name == "red")    return {255, 0, 0, 255};
    if (name == "green")  return {0, 255, 0, 255};
    if (name == "blue")   return {0, 0, 255, 255};
    if (name == "yellow") return {255, 255, 0, 255};
    if (name == "cyan")   return {0, 255, 255, 255};
    if (name == "white")  return {255, 255, 255, 255};
    if (name == "gray")   return {130, 130, 130, 255};
    return {200, 200, 200, 255};
}

void OverlayManager::draw_text_with_shadow(int x, int y, FontHandle& font, const std::string& text, GpuColor color) {
    if (!font_renderer) return;
    // Draw shadow first
    font_renderer->draw_text(x + 2, y + 2, font, text, 0, 0, 0, 180);
    // Draw main text
    font_renderer->draw_text(x, y, font, text, color.r, color.g, color.b, color.a);
}

void OverlayManager::draw_text_with_outline(int x, int y, FontHandle& font, const std::string& text, GpuColor color, GpuColor outline_color) {
    if (!font_renderer) return;
    // Draw outline shifts: left, right, top, bottom
    font_renderer->draw_text(x - 1, y, font, text, outline_color.r, outline_color.g, outline_color.b, outline_color.a);
    font_renderer->draw_text(x + 1, y, font, text, outline_color.r, outline_color.g, outline_color.b, outline_color.a);
    font_renderer->draw_text(x, y - 1, font, text, outline_color.r, outline_color.g, outline_color.b, outline_color.a);
    font_renderer->draw_text(x, y + 1, font, text, outline_color.r, outline_color.g, outline_color.b, outline_color.a);
    // Draw main text
    font_renderer->draw_text(x, y, font, text, color.r, color.g, color.b, color.a);
}

void OverlayManager::get_adaptive_colors(const ImageData* img, int x, int y, GpuColor& text_col, GpuColor& shadow_col, bool adaptive) {
    if (!adaptive || !img || img->width <= 0 || img->height <= 0) {
        text_col = {255, 255, 255, 255};
        shadow_col = {0, 0, 0, 200};
        return;
    }

    int sw = g_renderer.screen_w;
    int sh = g_renderer.screen_h;
    if (sw <= 0 || sh <= 0) {
        text_col = {255, 255, 255, 255};
        shadow_col = {0, 0, 0, 200};
        return;
    }

    // Map screen x, y to image fit rectangle
    SDL_Rect fit_rect;
    float aspect_img = (float)img->width / img->height;
    float aspect_screen = (float)sw / sh;
    if (aspect_img > aspect_screen) {
        fit_rect.w = sw;
        fit_rect.h = (int)(sw / aspect_img);
        fit_rect.x = 0;
        fit_rect.y = (sh - fit_rect.h) / 2;
    } else {
        fit_rect.h = sh;
        fit_rect.w = (int)(sh * aspect_img);
        fit_rect.x = (sw - fit_rect.w) / 2;
        fit_rect.y = 0;
    }

    if (x < fit_rect.x || x >= fit_rect.x + fit_rect.w || y < fit_rect.y || y >= fit_rect.y + fit_rect.h) {
        // Outside image, drawn on black background, use default white text
        text_col = {255, 255, 255, 255};
        shadow_col = {0, 0, 0, 200};
        return;
    }

    int img_x = ((x - fit_rect.x) * img->width) / fit_rect.w;
    int img_y = ((y - fit_rect.y) * img->height) / fit_rect.h;
    img_x = std::clamp(img_x, 0, img->width - 1);
    img_y = std::clamp(img_y, 0, img->height - 1);

    int d_top = img_y;
    int d_bot = img->height - 1 - img_y;
    int d_lft = img_x;
    int d_rgt = img->width - 1 - img_x;
    int min_d = std::min({d_top, d_bot, d_lft, d_rgt});

    uint8_t r = img->avg_r, g = img->avg_g, b = img->avg_b;
    if (min_d == d_top && std::ssize(img->edge_top_rgb) >= img->width * 3) {
        r = img->edge_top_rgb[img_x * 3 + 0];
        g = img->edge_top_rgb[img_x * 3 + 1];
        b = img->edge_top_rgb[img_x * 3 + 2];
    } else if (min_d == d_bot && std::ssize(img->edge_bot_rgb) >= img->width * 3) {
        r = img->edge_bot_rgb[img_x * 3 + 0];
        g = img->edge_bot_rgb[img_x * 3 + 1];
        b = img->edge_bot_rgb[img_x * 3 + 2];
    } else if (min_d == d_lft && std::ssize(img->edge_lft_rgb) >= img->height * 3) {
        r = img->edge_lft_rgb[img_y * 3 + 0];
        g = img->edge_lft_rgb[img_y * 3 + 1];
        b = img->edge_lft_rgb[img_y * 3 + 2];
    } else if (min_d == d_rgt && std::ssize(img->edge_rgt_rgb) >= img->height * 3) {
        r = img->edge_rgt_rgb[img_y * 3 + 0];
        g = img->edge_rgt_rgb[img_y * 3 + 1];
        b = img->edge_rgt_rgb[img_y * 3 + 2];
    }

    double luma = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    if (luma > 140) { // bright local background
        text_col = {15, 15, 15, 255};
        shadow_col = {255, 255, 255, 200};
    } else { // dark local background
        text_col = {255, 255, 255, 255};
        shadow_col = {0, 0, 0, 200};
    }
}

void OverlayManager::draw_all(int current_idx, int total_items, const MediaItem* item, const MediaItem* twin_item, double item_timer, bool is_video, int active_fps, const ImageData* current_data, [[maybe_unused]] const ImageData* current_twin_data, const std::string& video_remaining, const SDL_Rect* viewport) {
    if (!font_loaded || !font_renderer || !overlay_font) return;

    int sw = g_renderer.screen_w;
    int sh = g_renderer.screen_h;
    int vx = 0, vy = 0, vw = sw, vh = sh;
    if (viewport && viewport->w > 0 && viewport->h > 0) {
        vx = viewport->x;
        vy = viewport->y;
        vw = viewport->w;
        vh = viewport->h;
    }
    int pad = (int)round(15.0 * vw / 1920.0);
    if (pad < 8) pad = 8;

    // Load configurations thread-safely
    bool date_enabled = false;
    std::string date_text = "%Y-%m-%d";
    float date_x = 0.1f, date_y = 0.08f;
    int date_size = 20;
    GpuColor date_col = {200, 200, 200, 255};

    bool geotag_enabled = false;
    float geotag_x = 0.5f, geotag_y = 0.966f;
    int geotag_offset_x = 0, geotag_offset_y = 0;
    int geotag_size = 12;
    GpuColor geotag_col = {255, 255, 255, 255};

    bool file_enabled = false;
    float file_x = 0.04f, file_y = 0.966f;
    int file_size = 12;

    bool count_enabled = false;
    float count_x = 0.5f, count_y = 0.02f;
    int count_size = 20;

    bool timer_enabled = false;
    float timer_x = 0.94f, timer_y = 0.03f;
    int timer_size = 12;
    GpuColor timer_col = {255, 255, 0, 255};
    double transition_delay = 120.0;

    bool clock_enabled = false;
    float clock_x = 0.5f, clock_y = 0.96f;
    int clock_size = 18;
    bool clock_24h = true;
    GpuColor clock_col = {255, 255, 255, 255};

    bool diagnostics_hud_enabled = false;
    bool on_this_day_enabled = false;
    int on_this_day_range = 0;
    bool adaptive_text_enabled = false;
    bool has_matting = false;

    {
        std::shared_lock lock(g_config_mtx);
        date_enabled = g_cfg.date_overlay_enabled;
        date_text = g_cfg.date_text;
        date_x = g_cfg.date_x;
        date_y = g_cfg.date_y;
        date_size = g_cfg.date_font_size;
        date_col = get_color_from_str(g_cfg.date_color);

        geotag_enabled = g_cfg.geotag_enabled;
        geotag_x = g_cfg.geotag_x;
        geotag_y = g_cfg.geotag_y;
        geotag_offset_x = g_cfg.geotag_offset_x;
        geotag_offset_y = g_cfg.geotag_offset_y;
        geotag_size = g_cfg.geotag_font_size;
        geotag_col = get_color_from_str(g_cfg.geotag_color);

        file_enabled = g_cfg.filename_enabled;
        file_x = g_cfg.filename_x;
        file_y = g_cfg.filename_y;
        file_size = g_cfg.filename_font_size;

        count_enabled = g_cfg.count_enabled;
        count_x = g_cfg.count_x;
        count_y = g_cfg.count_y;
        count_size = g_cfg.count_font_size;

        timer_enabled = g_cfg.timer_enabled;
        timer_x = g_cfg.timer_x;
        timer_y = g_cfg.timer_y;
        timer_size = g_cfg.timer_font_size;
        timer_col = get_color_from_str(g_cfg.timer_color);
        transition_delay = g_cfg.transition_delay;

        clock_enabled = g_cfg.clock_enabled;
        clock_x = g_cfg.clock_x;
        clock_y = g_cfg.clock_y;
        clock_size = g_cfg.clock_font_size;
        clock_24h = g_cfg.clock_24h;
        clock_col = get_color_from_str(g_cfg.clock_color);

        diagnostics_hud_enabled = g_cfg.diagnostics_hud_enabled;
        on_this_day_enabled = g_cfg.on_this_day_enabled;
        on_this_day_range = g_cfg.on_this_day_range;
        adaptive_text_enabled = g_cfg.adaptive_text_enabled;
        has_matting = g_cfg.matting;
    }

    auto draw_contrast_text = [&](int x, int y, FontHandle& font, const std::string& text, GpuColor def_col, const ImageData* img) {
        GpuColor txt_c = def_col;
        GpuColor shd_c = {0, 0, 0, 200};
        get_adaptive_colors(img, x, y, txt_c, shd_c, adaptive_text_enabled);
        
        if (adaptive_text_enabled) {
            draw_text_with_outline(x, y, font, text, txt_c, shd_c);
        } else {
            draw_text_with_shadow(x, y, font, text, def_col);
        }
    };

    // Overall image ref for global elements
    // 1. Date Overlay
    if (date_enabled) {
        char datebuf[64];
        time_t now = time(nullptr);
        struct tm tm_buf;
        struct tm* tm_info = localtime_r(&now, &tm_buf);
        if (tm_info && strftime(datebuf, sizeof(datebuf), date_text.c_str(), tm_info) != 0) {
            int dx = vx + pad + (int)((vw - pad * 2) * date_x);
            int dy = vy + pad + (int)((vh - pad * 2) * date_y);
            FontHandle& font = font_renderer->load_font(overlay_font->path, date_size);
            draw_contrast_text(dx, dy, font, datebuf, date_col, current_data);
        }
    }

    // 2. Filename Overlay (aligned neatly right above the bottom separator line)
    if (file_enabled && item) {
        FontHandle& font = font_renderer->load_font(overlay_font->path, file_size);
        int tw = 0, th = 0;
        font_renderer->measure(font, item->filename, tw, th);
        int fx = vx + (int)round(((double)(g_renderer.scale_px(g_cfg.matting_size)) * 0.5) + 10.0);
        int fy = vy + vh - th - (int)round(4.0 * sw / 1920.0);

        if (twin_item) {
            // Stack both filenames vertically in lower-left
            draw_contrast_text(fx, fy - th - 4, font, item->filename, {255, 255, 255, 255}, current_data);
            draw_contrast_text(fx, fy, font, twin_item->filename, {255, 255, 255, 255}, current_data);
        } else {
            draw_contrast_text(fx, fy, font, item->filename, {255, 255, 255, 255}, current_data);
        }
    }

    // 2b. Geotag (EXIF GPS Coordinates) Overlay - Bottom-Center Aligned
    if (geotag_enabled && item && item->has_gps && (item->latitude != 0.0 || item->longitude != 0.0)) {
        double lat = item->latitude;
        double lon = item->longitude;
        std::string geo_str = std::format("{:.4f}Â° {}, {:.4f}Â° {}",
                                          std::abs(lat), lat >= 0.0 ? "N" : "S",
                                          std::abs(lon), lon >= 0.0 ? "E" : "W");
        FontHandle& font = font_renderer->load_font(overlay_font->path, geotag_size);
        int tw = 0, th = 0;
        font_renderer->measure(font, geo_str, tw, th);
        int gx = vx + pad + (int)((vw - pad * 2) * geotag_x) - (tw / 2) + geotag_offset_x;
        int gy = vy + pad + (int)((vh - pad * 2) * geotag_y) + geotag_offset_y;
        draw_contrast_text(gx, gy, font, geo_str, geotag_col, current_data);
    }

    // 3. Count Overlay
    if (count_enabled && total_items > 0) {
        std::string cntbuf = std::format("{}/{}", current_idx + 1, total_items);
        int cx = vx + pad + (int)((vw - pad * 2) * count_x);
        int cy = vy + pad + (int)((vh - pad * 2) * count_y);
        FontHandle& font = font_renderer->load_font(overlay_font->path, count_size);
        int tw, th;
        font_renderer->measure(font, cntbuf, tw, th);
        cx -= tw / 2;
        draw_contrast_text(cx, cy, font, cntbuf, {200, 200, 200, 220}, current_data);
    }

    // 4. Timer Overlay (photo countdown or video remaining, right-aligned close to separator line)
    if (timer_enabled) {
        FontHandle& font = font_renderer->load_font(overlay_font->path, timer_size);
        std::string tbuf = (is_video && !video_remaining.empty()) ? video_remaining : std::format("{}s", std::max(0, (int)(transition_delay - item_timer)));
        int tw = 0, th = 0;
        font_renderer->measure(font, tbuf, tw, th);
        int tx = vx + vw - tw - (int)round(12.0 * sw / 1920.0);
        int ty = has_matting ? (int)round(((double)(g_renderer.scale_px(g_cfg.matting_size)) * 0.5) + 10.0) : (int)round(20.0 * sw / 1920.0);

        if (is_video && !video_remaining.empty()) {
            g_logger.debug("REM: remaining={}", video_remaining.c_str());
            font_renderer->draw_text_uncached(tx + 2, ty + 2, font, video_remaining, 0, 0, 0, 180);
            font_renderer->draw_text_uncached(tx, ty, font, video_remaining, timer_col.r, timer_col.g, timer_col.b, timer_col.a);
        } else if (!is_video) {
            draw_contrast_text(tx, ty, font, tbuf, timer_col, current_data);
        }
    }

    // 5. Clock Overlay
    if (clock_enabled) {
        char clkbuf[16];
        time_t now = time(nullptr);
        struct tm tm_buf_clk;
        struct tm* tmi = localtime_r(&now, &tm_buf_clk);
        if (tmi) {
            strftime(clkbuf, sizeof(clkbuf), clock_24h ? "%H:%M" : "%I:%M %p", tmi);
            FontHandle& font = font_renderer->load_font(overlay_font->path, clock_size);
            int clkw, clkh;
            font_renderer->measure(font, clkbuf, clkw, clkh);
            int clkx = vx + pad + (int)((vw - pad * 2) * clock_x) - clkw / 2;
            int clky = vy + pad + (int)((vh - pad * 2) * clock_y);
            draw_contrast_text(clkx, clky, font, clkbuf, clock_col, current_data);
        }
    }

    // 6. Diagnostics phosphor telemetry HUD
    if (diagnostics_hud_enabled) {
        float soc_temp = 0.0f;
        {
            std::ifstream temp_file("/sys/class/thermal/thermal_zone0/temp");
            if (temp_file.is_open()) {
                long long val;
                if (temp_file >> val) {
                    soc_temp = (float)val / 1000.0f;
                }
            }
        }

        if (soc_temp > 80.0f) {
            trigger_error(501); // E501: SYSTEM_OVERHEATING
        } else if (g_active_error_code.load() == 501) {
            trigger_error(0);
        }

        std::uintmax_t db_size_kb = 0;
        {
            std::string db_dir = "/app/cache";
            {
                std::shared_lock lock(g_config_mtx);
                db_dir = g_cfg.cache_dir;
            }
            std::string path = db_dir + "/cache.db";
            if (std::filesystem::exists(path)) {
                try {
                    db_size_kb = std::filesystem::file_size(path) / 1024;
                } catch(...) {}
            }
        }

        std::string res_str = "N/A";
        if (item) {
            res_str = std::format("{}x{}", item->width, item->height);
            if (twin_item) {
                res_str += " | " + std::format("{}x{}", twin_item->width, twin_item->height);
            }
        }

        bool has_p = false, has_a = false, is_d = false;
        if (item) {
            classify_media_item(*item, has_p, has_a, is_d);
        }
        std::string tags = (has_p ? "PEOPLE " : "") + std::string(has_a ? "ANIMAL " : "") + std::string(is_d ? "DOCUMENT" : "");
        if (tags.empty()) tags = "SCENERY";

        std::vector<std::string> lines;
        lines.push_back("--- TELEMETRY HUD ---");
        lines.push_back(std::format("FPS     : {}", active_fps));
        lines.push_back(std::format("SOC TEMP: {} C", (int)soc_temp));
        lines.push_back(std::format("DB SIZE : {} KB", db_size_kb));
        lines.push_back(std::format("ITEMS   : {} / {}", current_idx + 1, total_items));
        lines.push_back("RES     : " + res_str);
        lines.push_back("TAGS    : " + tags);
        if (item) {
            std::string path = item->path;
            if (path.length() > 30) {
                path = "..." + path.substr(path.length() - 27);
            }
            lines.push_back("PATH    : " + path);
        }

        FontHandle& hud_font = font_renderer->load_font(overlay_font->path, 11);
        int max_w = 0, total_h = 0;
        for (const auto& line : lines) {
            int lw, lh;
            font_renderer->measure(hud_font, line, lw, lh);
            max_w = std::max(max_w, lw);
            total_h += lh + 2;
        }

        int hx = vx + pad + 10;
        int hy = vy + pad + 10;

        SDL_Rect container = { hx - 8, hy - 8, max_w + 16, total_h + 16 };
        SDL_SetRenderDrawBlendMode(renderer->sdl_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 10, 10, 10, 200);
        SDL_FRect container_f = { (float)container.x, (float)container.y, (float)container.w, (float)container.h };
        SDL_RenderFillRect(renderer->sdl_renderer, &container_f);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 100, 100, 100, 255);
        SDL_FRect outline = { (float)container.x, (float)container.y, (float)container.w, (float)container.h };
        SDL_RenderRect(renderer->sdl_renderer, &outline);

        int curr_y = hy;
        for (const auto& line : lines) {
            font_renderer->draw_text(hx, curr_y, hud_font, line, 0, 230, 0, 240); // Phosphor green
            int lw, lh;
            font_renderer->measure(hud_font, line, lw, lh);
            curr_y += lh + 2;
        }
    }

    // 6b. Transition Progress Bar
    if (g_cfg.progress_bar_enabled && !is_video && transition_delay > 0.0) {
        int bar_h = 4;
        int bar_w = vw - pad * 2;
        int bar_x = vx + pad;
        int bar_y = vy + vh - bar_h - pad;
        double progress = std::min(1.0, item_timer / transition_delay);
        int fill_w = (int)(bar_w * progress);
        // Background bar
        SDL_SetRenderDrawBlendMode(g_renderer.sdl_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(g_renderer.sdl_renderer, 80, 80, 80, 120);
        SDL_FRect bg_rect = {(float)bar_x, (float)bar_y, (float)bar_w, (float)bar_h};
        SDL_RenderFillRect(g_renderer.sdl_renderer, &bg_rect);
        // Fill bar
        SDL_SetRenderDrawColor(g_renderer.sdl_renderer, 255, 255, 255, 180);
        SDL_FRect fill_rect = {(float)bar_x, (float)bar_y, (float)fill_w, (float)bar_h};
        SDL_RenderFillRect(g_renderer.sdl_renderer, &fill_rect);
    }

    // 7. Gold Ribbon Anniversary Banner (On This Day)
    bool show_ribbon = false;
    int anniversary_years = 0;
    if (on_this_day_enabled && item) {
        if (auto date = get_item_date(*item)) {
            auto [y, m, d] = *date;
            time_t now_t = time(nullptr);
            struct tm tm_now;
            struct tm* tmi = localtime_r(&now_t, &tm_now);
            if (tmi) {
                int today_y = tmi->tm_year + 1900;
                // Compute calendar-day difference for on-this-day range matching
                int diff_days = 0;
                struct tm photo_tm = {};
                photo_tm.tm_year = y;
                photo_tm.tm_mon = m - 1;
                photo_tm.tm_mday = d;
                photo_tm.tm_hour = 12;
                photo_tm.tm_isdst = -1;
                time_t photo_epoch = mktime(&photo_tm);
                if (photo_epoch != (time_t)-1) {
                    double secs = difftime(now_t, photo_epoch);
                    diff_days = abs((int)(secs / 86400.0)) % 365;
                }
                if (diff_days <= on_this_day_range) {
                    anniversary_years = today_y - y;
                    if (anniversary_years > 0) {
                        show_ribbon = true;
                    }
                }
            }
        }
    }

    if (show_ribbon) {
        std::string ribbon_text = std::format("★ {} {}", anniversary_years, anniversary_years == 1 ? " YEAR AGO TODAY" : " YEARS AGO TODAY");
        FontHandle& font = font_renderer->load_font(overlay_font->path, 22);
        int tw, th;
        font_renderer->measure(font, ribbon_text, tw, th);
        int rx = (sw - tw) / 2;
        int ry = pad + 15;

        SDL_Rect ribbon_bg = { rx - 20, ry - 6, tw + 40, th + 12 };
        SDL_SetRenderDrawBlendMode(renderer->sdl_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 218, 165, 32, 180); // Gold translucent
        SDL_FRect ribbon_bg_f = { (float)ribbon_bg.x, (float)ribbon_bg.y, (float)ribbon_bg.w, (float)ribbon_bg.h };
        SDL_RenderFillRect(renderer->sdl_renderer, &ribbon_bg_f);

        SDL_SetRenderDrawColor(renderer->sdl_renderer, 255, 223, 0, 255); // Solid gold outline
        SDL_FRect ribbon_outline = { (float)ribbon_bg.x, (float)ribbon_bg.y, (float)ribbon_bg.w, (float)ribbon_bg.h };
        SDL_RenderRect(renderer->sdl_renderer, &ribbon_outline);

        font_renderer->draw_text(rx, ry, font, ribbon_text, 255, 255, 255, 255);
    }

    // 8. Offline Mode / Error Overlay Console
    int code_num = g_active_error_code.load();
    bool is_media_error = (code_num == 101 || code_num == 201 || code_num == 202);
    bool show_error = g_offline_mode.load() || (code_num != 0 && !is_media_error);
    if (show_error) {
        if (code_num == 0) {
            // Default to E101 if offline mode is triggered but no explicit error code set
            code_num = 101;
        }

        std::string code_str = std::format("E{}", code_num);

        std::string title = "OFFLINE_MODE";
        std::string desc = "No network connection detected. Showing cached media library.";
        std::string rec = "Check your network connection or configure media paths in piTrove config.";

        if (g_cache) {
            g_cache->get_error_details(code_str, title, desc, rec);
        }

        FontHandle& header_font = font_renderer->load_font(overlay_font->path, 18);
        FontHandle& body_font = font_renderer->load_font(overlay_font->path, 12);

        std::string header_text = "★ ERROR " + code_str + ": " + title + " ★";
        std::string desc_text = "DESC: " + desc;
        std::string rec_text = "RECOVERY: " + rec;

        int hw = 0, hh = 0;
        int dw = 0, dh = 0;
        int rw = 0, rh = 0;
        font_renderer->measure(header_font, header_text, hw, hh);
        font_renderer->measure(body_font, desc_text, dw, dh);
        font_renderer->measure(body_font, rec_text, rw, rh);

        int max_w = std::max({hw, dw, rw});
        int total_h = hh + dh + rh + 16;

        int rx = (sw - max_w) / 2;
        int ry = sh - total_h - 60; // Placed neatly in the lower section

        SDL_Rect r_bg = { rx - 30, ry - 15, max_w + 60, total_h + 30 };
        SDL_SetRenderDrawBlendMode(renderer->sdl_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 30, 0, 0, 220); // Deep red-black translucent
        SDL_FRect r_bg_f = { (float)r_bg.x, (float)r_bg.y, (float)r_bg.w, (float)r_bg.h };
        SDL_RenderFillRect(renderer->sdl_renderer, &r_bg_f);

        SDL_SetRenderDrawColor(renderer->sdl_renderer, 255, 30, 30, 255); // Glowing hot red border
        SDL_FRect r_outline = { (float)r_bg.x, (float)r_bg.y, (float)r_bg.w, (float)r_bg.h };
        SDL_RenderRect(renderer->sdl_renderer, &r_outline);

        // Render glow shadow behind the text box
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 255, 0, 0, 80);
        for (int i = 1; i <= 3; i++) {
            SDL_FRect glow_r = { (float)(r_bg.x - i), (float)(r_bg.y - i), (float)(r_bg.w + i*2), (float)(r_bg.h + i*2) };
            SDL_RenderRect(renderer->sdl_renderer, &glow_r);
        }

        // Draw header centering
        int hx = rx + (max_w - hw) / 2;
        font_renderer->draw_text(hx, ry, header_font, header_text, 255, 255, 255, 255);

        // Draw description (light red-gray phosphor)
        font_renderer->draw_text(rx, ry + hh + 8, body_font, desc_text, 240, 180, 180, 255);

        // Draw recovery (bright yellow-green command style)
        font_renderer->draw_text(rx, ry + hh + dh + 16, body_font, rec_text, 255, 223, 0, 255);
    }

    if (menu_active) {
        draw_popup_menu();
    }
    if (keyboard_active) {
        draw_virtual_keyboard();
    }
    if (nav_overlay_active) {
        Uint64 now_ms = SDL_GetTicks();
        if (now_ms - nav_overlay_show_time > 3000) {
            nav_overlay_active = false;
            pin_unlocked = false;
        } else {
            draw_nav_overlay();
        }
    }
    if (pin_active) {
        draw_pin_keypad();
    }
}

void OverlayManager::draw_nav_overlay() {
    if (!font_loaded || !font_renderer || !overlay_font) return;

    int sw = g_renderer.screen_w;
    int sh = g_renderer.screen_h;

    // Define positions
    int btn_size = 70;
    int left_x = 40;
    int right_x = sw - 110;
    int center_x = (sw - btn_size) / 2;
    int btn_y = (sh - btn_size) / 2;

    FontHandle& font = font_renderer->load_font(overlay_font->path, 28);

    // Helper to draw a single navigation button
    auto draw_nav_btn = [&](int bx, int by, const std::string& symbol) {
        SDL_FRect rect = { (float)bx, (float)by, (float)btn_size, (float)btn_size };
        
        // Translucent dark background
        SDL_SetRenderDrawBlendMode(renderer->sdl_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 15, 15, 17, 200); // semi-translucent dark
        SDL_RenderFillRect(renderer->sdl_renderer, &rect);

        // Highlight/outline
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 150); // Zinc outline
        SDL_RenderRect(renderer->sdl_renderer, &rect);

        // Measure & draw symbol centered
        int sw_text = 0, sh_text = 0;
        font_renderer->measure(font, symbol, sw_text, sh_text);
        int sx = bx + (btn_size - sw_text) / 2;
        int sy = by + (btn_size - sh_text) / 2;
        font_renderer->draw_text(sx, sy, font, symbol, 244, 244, 245, 255);
    };

    draw_nav_btn(left_x, btn_y, "◀");
    draw_nav_btn(center_x, btn_y, "⚙");
    draw_nav_btn(right_x, btn_y, "▶");
}

void OverlayManager::draw_popup_menu() {
    if (!font_loaded || !font_renderer || !overlay_font) return;

    int sw = g_renderer.screen_w;
    int sh = g_renderer.screen_h;

    int menu_w = 520;
    int menu_h = 380;
    int menu_x = (sw - menu_w) / 2;
    int menu_y = (sh - menu_h) / 2;

    // Draw background card (translucent dark slate grey)
    SDL_Rect container = { menu_x, menu_y, menu_w, menu_h };
    SDL_SetRenderDrawBlendMode(renderer->sdl_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer->sdl_renderer, 15, 15, 17, 245); // Zinc dark background
    SDL_FRect container_f = { (float)container.x, (float)container.y, (float)container.w, (float)container.h };
    SDL_RenderFillRect(renderer->sdl_renderer, &container_f);

    // Draw card border (zinc gray/silver)
    SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 100); // Zinc border
    SDL_RenderRect(renderer->sdl_renderer, &container_f);

    FontHandle& title_font = font_renderer->load_font(overlay_font->path, 18);
    FontHandle& item_font = font_renderer->load_font(overlay_font->path, 13);
    FontHandle& btn_font = font_renderer->load_font(overlay_font->path, 12);
    FontHandle& footer_font = font_renderer->load_font(overlay_font->path, 11);

    // Draw Title centered
    std::string title_text = "piTrove Quick Configuration";
    int tw, th;
    font_renderer->measure(title_font, title_text, tw, th);
    font_renderer->draw_text(menu_x + (menu_w - tw) / 2, menu_y + 18, title_font, title_text, 244, 244, 245, 255);

    // Separator line
    SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 40);
    SDL_RenderLine(renderer->sdl_renderer, menu_x + 15, menu_y + 45, menu_x + menu_w - 15, menu_y + 45);

    // Get settings states
    bool paused = g_slideshow_paused.load();
    bool shuffle = false;
    double delay = 120.0;
    int volume = 0;
    {
        std::shared_lock lk(g_config_mtx);
        shuffle = g_cfg.shuffle;
        delay = g_cfg.transition_delay;
        volume = g_cfg.video_volume;
    }
    bool blanked = g_screen_blanked.load();

    // Now, render settings rows. Let's arrange them neatly in a layout:
    int start_y = menu_y + 55;
    int row_h = 32;

    // Row 0: Play / Pause
    {
        int iy = start_y + 0 * row_h;
        font_renderer->draw_text(menu_x + 24, iy + 4, item_font, "Slideshow Status:", 161, 161, 170, 255);
        std::string status_btn = paused ? "[ PAUSED - Play ]" : "[ PLAYING - Pause ]";
        SDL_FRect r_btn = { (float)(menu_x + 240), (float)iy, 240.0f, 26.0f };
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 30);
        SDL_RenderFillRect(renderer->sdl_renderer, &r_btn);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 80);
        SDL_RenderRect(renderer->sdl_renderer, &r_btn);
        int bw, bh;
        font_renderer->measure(btn_font, status_btn, bw, bh);
        font_renderer->draw_text(menu_x + 240 + (240 - bw)/2, iy + 4, btn_font, status_btn, 244, 244, 245, 255);
    }

    // Row 1: Shuffle
    {
        int iy = start_y + 1 * row_h;
        font_renderer->draw_text(menu_x + 24, iy + 4, item_font, "Playlist Shuffle:", 161, 161, 170, 255);
        std::string shuffle_btn = shuffle ? "SHUFFLE: ON" : "SHUFFLE: OFF";
        SDL_FRect r_btn = { (float)(menu_x + 240), (float)iy, 240.0f, 26.0f };
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 30);
        SDL_RenderFillRect(renderer->sdl_renderer, &r_btn);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 80);
        SDL_RenderRect(renderer->sdl_renderer, &r_btn);
        int bw, bh;
        font_renderer->measure(btn_font, shuffle_btn, bw, bh);
        font_renderer->draw_text(menu_x + 240 + (240 - bw)/2, iy + 4, btn_font, shuffle_btn, 244, 244, 245, 255);
    }

    // Row 2: Interval Delay with - / + buttons and click-to-input
    {
        int iy = start_y + 2 * row_h;
        font_renderer->draw_text(menu_x + 24, iy + 4, item_font, "Interval Delay (s):", 161, 161, 170, 255);
        
        // [-] button
        SDL_FRect r_minus = { (float)(menu_x + 240), (float)iy, 40.0f, 26.0f };
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 30);
        SDL_RenderFillRect(renderer->sdl_renderer, &r_minus);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 80);
        SDL_RenderRect(renderer->sdl_renderer, &r_minus);
        font_renderer->draw_text(menu_x + 256, iy + 4, btn_font, "-", 244, 244, 245, 255);

        // Value button (click to type)
        std::string val_str = std::format("{}s", (int)delay);
        SDL_FRect r_val = { (float)(menu_x + 290), (float)iy, 140.0f, 26.0f };
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 15);
        SDL_RenderFillRect(renderer->sdl_renderer, &r_val);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 60);
        SDL_RenderRect(renderer->sdl_renderer, &r_val);
        int bw, bh;
        font_renderer->measure(btn_font, val_str, bw, bh);
        font_renderer->draw_text(menu_x + 290 + (140 - bw)/2, iy + 4, btn_font, val_str, 255, 255, 255, 255);

        // [+] button
        SDL_FRect r_plus = { (float)(menu_x + 440), (float)iy, 40.0f, 26.0f };
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 30);
        SDL_RenderFillRect(renderer->sdl_renderer, &r_plus);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 80);
        SDL_RenderRect(renderer->sdl_renderer, &r_plus);
        font_renderer->draw_text(menu_x + 456, iy + 4, btn_font, "+", 244, 244, 245, 255);
    }

    // Row 3: Video Volume with - / + buttons and slider
    {
        int iy = start_y + 3 * row_h;
        font_renderer->draw_text(menu_x + 24, iy + 4, item_font, "Video Volume (%):", 161, 161, 170, 255);

        // [-] button
        SDL_FRect r_minus = { (float)(menu_x + 240), (float)iy, 40.0f, 26.0f };
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 30);
        SDL_RenderFillRect(renderer->sdl_renderer, &r_minus);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 80);
        SDL_RenderRect(renderer->sdl_renderer, &r_minus);
        font_renderer->draw_text(menu_x + 256, iy + 4, btn_font, "-", 244, 244, 245, 255);

        // Value button (click to type)
        std::string val_str = std::format("{}%", volume);
        SDL_FRect r_val = { (float)(menu_x + 290), (float)iy, 140.0f, 26.0f };
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 15);
        SDL_RenderFillRect(renderer->sdl_renderer, &r_val);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 60);
        SDL_RenderRect(renderer->sdl_renderer, &r_val);
        int bw, bh;
        font_renderer->measure(btn_font, val_str, bw, bh);
        font_renderer->draw_text(menu_x + 290 + (140 - bw)/2, iy + 4, btn_font, val_str, 255, 255, 255, 255);

        // [+] button
        SDL_FRect r_plus = { (float)(menu_x + 440), (float)iy, 40.0f, 26.0f };
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 30);
        SDL_RenderFillRect(renderer->sdl_renderer, &r_plus);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 80);
        SDL_RenderRect(renderer->sdl_renderer, &r_plus);
        font_renderer->draw_text(menu_x + 456, iy + 4, btn_font, "+", 244, 244, 245, 255);
    }

    // Row 4: Volume Slider underneath volume control
    {
        int iy = start_y + 4 * row_h - 2;
        int track_x = menu_x + 240;
        int track_w = 240;
        int track_y = iy + 6;
        int track_h = 4;

        SDL_FRect track_r = { (float)track_x, (float)track_y, (float)track_w, (float)track_h };
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 63, 63, 70, 255);
        SDL_RenderFillRect(renderer->sdl_renderer, &track_r);

        float pct = std::max(0.0f, std::min(1.0f, volume / 100.0f));
        int active_w = (int)(track_w * pct);
        if (active_w > 0) {
            SDL_FRect active_r = { (float)track_x, (float)track_y, (float)active_w, (float)track_h };
            SDL_SetRenderDrawColor(renderer->sdl_renderer, 244, 244, 245, 255);
            SDL_RenderFillRect(renderer->sdl_renderer, &active_r);
        }

        int knob_x = track_x + active_w;
        int knob_w = 12;
        int knob_h = 12;
        SDL_FRect knob_r = { (float)(knob_x - knob_w/2), (float)(track_y + track_h/2 - knob_h/2), (float)knob_w, (float)knob_h };
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 244, 244, 245, 255);
        SDL_RenderFillRect(renderer->sdl_renderer, &knob_r);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 255);
        SDL_RenderRect(renderer->sdl_renderer, &knob_r);
    }

    // Row 5: Screen Blanking
    {
        int iy = start_y + 5 * row_h;
        font_renderer->draw_text(menu_x + 24, iy + 4, item_font, "Physical Screen Power:", 161, 161, 170, 255);
        std::string screen_btn = blanked ? "SCREEN: OFF (Blanked)" : "SCREEN: ON (Active)";
        SDL_FRect r_btn = { (float)(menu_x + 240), (float)iy, 240.0f, 26.0f };
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 30);
        SDL_RenderFillRect(renderer->sdl_renderer, &r_btn);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 80);
        SDL_RenderRect(renderer->sdl_renderer, &r_btn);
        int bw, bh;
        font_renderer->measure(btn_font, screen_btn, bw, bh);
        font_renderer->draw_text(menu_x + 240 + (240 - bw)/2, iy + 4, btn_font, screen_btn, 244, 244, 245, 255);
    }

    // Row 6: Playlist Control Buttons
    {
        int iy = start_y + 6 * row_h;
        font_renderer->draw_text(menu_x + 24, iy + 4, item_font, "Playlist Control:", 161, 161, 170, 255);

        SDL_FRect r_prev = { (float)(menu_x + 240), (float)iy, 115.0f, 26.0f };
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 30);
        SDL_RenderFillRect(renderer->sdl_renderer, &r_prev);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 80);
        SDL_RenderRect(renderer->sdl_renderer, &r_prev);
        int bw, bh;
        font_renderer->measure(btn_font, "◀ Previous", bw, bh);
        font_renderer->draw_text(menu_x + 240 + (115 - bw)/2, iy + 4, btn_font, "◀ Previous", 244, 244, 245, 255);

        SDL_FRect r_next = { (float)(menu_x + 365), (float)iy, 115.0f, 26.0f };
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 30);
        SDL_RenderFillRect(renderer->sdl_renderer, &r_next);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 80);
        SDL_RenderRect(renderer->sdl_renderer, &r_next);
        font_renderer->measure(btn_font, "Next ▶", bw, bh);
        font_renderer->draw_text(menu_x + 365 + (115 - bw)/2, iy + 4, btn_font, "Next ▶", 244, 244, 245, 255);
    }

    // Row 7: Close Quick Menu Button
    {
        int iy = start_y + 7 * row_h + 4;
        SDL_FRect r_btn = { (float)(menu_x + 24), (float)iy, 472.0f, 30.0f };
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 228, 228, 231, 240);
        SDL_RenderFillRect(renderer->sdl_renderer, &r_btn);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 255, 255, 255, 255);
        SDL_RenderRect(renderer->sdl_renderer, &r_btn);
        int bw, bh;
        font_renderer->measure(item_font, "Close Settings Overlay Menu", bw, bh);
        font_renderer->draw_text(menu_x + 24 + (472 - bw)/2, iy + 6, item_font, "Close Settings Overlay Menu", 15, 15, 17, 255);
    }

    // Separator line before footer
    SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 40);
    SDL_RenderLine(renderer->sdl_renderer, menu_x + 15, menu_y + menu_h - 40, menu_x + menu_w - 15, menu_y + menu_h - 40);

    // Footer notice text
    std::string footer_text = "Run 'piTrove TUI' over SSH for advanced console settings.";
    int fw, fh;
    font_renderer->measure(footer_font, footer_text, fw, fh);
    font_renderer->draw_text(menu_x + (menu_w - fw) / 2, menu_y + menu_h - 28, footer_font, footer_text, 113, 113, 122, 255);
}

void OverlayManager::draw_virtual_keyboard() {
    if (!font_loaded || !font_renderer || !overlay_font) return;

    int sw = g_renderer.screen_w;
    int sh = g_renderer.screen_h;

    int kb_w = 340;
    int kb_h = 380;
    int kb_x = (sw - kb_w) / 2;
    int kb_y = (sh - kb_h) / 2;

    // Semi-translucent screen overlay to dim background
    SDL_FRect screen_overlay = { 0, 0, (float)sw, (float)sh };
    SDL_SetRenderDrawBlendMode(renderer->sdl_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer->sdl_renderer, 0, 0, 0, 130);
    SDL_RenderFillRect(renderer->sdl_renderer, &screen_overlay);

    // Draw keyboard card
    SDL_FRect kb_r = { (float)kb_x, (float)kb_y, (float)kb_w, (float)kb_h };
    SDL_SetRenderDrawColor(renderer->sdl_renderer, 20, 20, 23, 250);
    SDL_RenderFillRect(renderer->sdl_renderer, &kb_r);
    SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 255); // Zinc border
    SDL_RenderRect(renderer->sdl_renderer, &kb_r);

    FontHandle& title_font = font_renderer->load_font(overlay_font->path, 15);
    FontHandle& input_font = font_renderer->load_font(overlay_font->path, 22);
    FontHandle& key_font = font_renderer->load_font(overlay_font->path, 18);

    // Header title
    std::string title = (keyboard_target == 0) ? "Enter Interval Delay (sec)" : "Enter Video Volume (0-100%)";
    int tw, th;
    font_renderer->measure(title_font, title, tw, th);
    font_renderer->draw_text(kb_x + (kb_w - tw) / 2, kb_y + 15, title_font, title, 161, 161, 170, 255);

    // Input display box
    SDL_FRect display_r = { (float)(kb_x + 20), (float)(kb_y + 45), (float)(kb_w - 40), 45.0f };
    SDL_SetRenderDrawColor(renderer->sdl_renderer, 9, 9, 11, 255);
    SDL_RenderFillRect(renderer->sdl_renderer, &display_r);
    SDL_SetRenderDrawColor(renderer->sdl_renderer, 63, 63, 70, 255);
    SDL_RenderRect(renderer->sdl_renderer, &display_r);

    std::string display_val = keyboard_input.empty() ? "0" : keyboard_input;
    int dw, dh;
    font_renderer->measure(input_font, display_val, dw, dh);
    font_renderer->draw_text(kb_x + kb_w - 35 - dw, kb_y + 53, input_font, display_val, 255, 255, 255, 255);

    // Render Keys Grid (1-9, Backspace, 0, OK)
    int start_kx = kb_x + 20;
    int start_ky = kb_y + 110;
    int kw = 90;
    int kh = 50;
    int gap = 10;

    std::vector<std::string> keys = {
        "1", "2", "3",
        "4", "5", "6",
        "7", "8", "9",
        "⌫", "0", "OK"
    };

    for (int i = 0; i < 12; i++) {
        int col = i % 3;
        int row = i / 3;
        int kx = start_kx + col * (kw + gap);
        int ky = start_ky + row * (kh + gap);

        SDL_FRect key_r = { (float)kx, (float)ky, (float)kw, (float)kh };
        if (keys[i] == "OK") {
            SDL_SetRenderDrawColor(renderer->sdl_renderer, 244, 244, 245, 220); // OK is bright white
            SDL_RenderFillRect(renderer->sdl_renderer, &key_r);
            SDL_SetRenderDrawColor(renderer->sdl_renderer, 255, 255, 255, 255);
            SDL_RenderRect(renderer->sdl_renderer, &key_r);
            int kw_m, kh_m;
            font_renderer->measure(key_font, keys[i], kw_m, kh_m);
            font_renderer->draw_text(kx + (kw - kw_m)/2, ky + 12, key_font, keys[i], 15, 15, 17, 255);
        } else {
            SDL_SetRenderDrawColor(renderer->sdl_renderer, 39, 39, 42, 255);
            SDL_RenderFillRect(renderer->sdl_renderer, &key_r);
            SDL_SetRenderDrawColor(renderer->sdl_renderer, 82, 82, 91, 255);
            SDL_RenderRect(renderer->sdl_renderer, &key_r);
            int kw_m, kh_m;
            font_renderer->measure(key_font, keys[i], kw_m, kh_m);
            font_renderer->draw_text(kx + (kw - kw_m)/2, ky + 12, key_font, keys[i], 244, 244, 245, 255);
        }
    }

    // Cancel key button
    SDL_FRect cancel_r = { (float)(kb_x + 20), (float)(kb_y + 325), (float)(kb_w - 40), 36.0f };
    SDL_SetRenderDrawColor(renderer->sdl_renderer, 39, 39, 42, 100);
    SDL_RenderFillRect(renderer->sdl_renderer, &cancel_r);
    SDL_SetRenderDrawColor(renderer->sdl_renderer, 161, 161, 170, 100);
    SDL_RenderRect(renderer->sdl_renderer, &cancel_r);
    int cw, ch;
    font_renderer->measure(title_font, "Cancel / Close Keyboard", cw, ch);
    font_renderer->draw_text(kb_x + 20 + (kb_w - 40 - cw)/2, kb_y + 333, title_font, "Cancel / Close Keyboard", 161, 161, 170, 255);
}

void OverlayManager::draw_pin_keypad() {
    if (!font_loaded || !font_renderer || !overlay_font) return;

    int sw = g_renderer.screen_w;
    int sh = g_renderer.screen_h;

    // Semi-transparent dark overlay background
    SDL_SetRenderDrawBlendMode(renderer->sdl_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer->sdl_renderer, 0, 0, 0, 200);
    SDL_FRect bg = {0, 0, (float)sw, (float)sh};
    SDL_RenderFillRect(renderer->sdl_renderer, &bg);

    // Modal box dimensions
    int modal_w = 320;
    int modal_h = 420;
    int mx = (sw - modal_w) / 2;
    int my = (sh - modal_h) / 2;

    // Draw modal background
    SDL_SetRenderDrawColor(renderer->sdl_renderer, 20, 20, 25, 240);
    SDL_FRect mrect = {(float)mx, (float)my, (float)modal_w, (float)modal_h};
    SDL_RenderFillRect(renderer->sdl_renderer, &mrect);
    SDL_SetRenderDrawColor(renderer->sdl_renderer, 60, 60, 70, 200);
    SDL_RenderRect(renderer->sdl_renderer, &mrect);

    // Check lockout state
    Uint64 now = SDL_GetTicks();
    bool locked = (pin_locked_until > now);

    FontHandle& title_font = font_renderer->load_font(overlay_font->path, 24);
    FontHandle& body_font = font_renderer->load_font(overlay_font->path, 18);

    // Title
    std::string title = locked ? "Dashboard Locked" : "Enter PIN";
    font_renderer->draw_text(mx + (modal_w - (int)title.length() * 12) / 2, my + 20, title_font, title, 230, 230, 230, 255);

    if (locked) {
        int remaining = (int)((pin_locked_until - now) / 1000);
        std::string lock_msg = std::format("Locked for {}s", remaining);
        font_renderer->draw_text(mx + (modal_w - (int)lock_msg.length() * 10) / 2, my + 55, body_font, lock_msg, 255, 180, 80, 255);
        std::string hint = "Too many failed attempts";
        font_renderer->draw_text(mx + (modal_w - (int)hint.length() * 8) / 2, my + 80, body_font, hint, 180, 180, 180, 255);
        return;
    }

    // Draw 4 PIN dots (filled rectangles)
    int dot_size = 16;
    int dot_gap = 12;
    int dots_total_w = 4 * dot_size + 3 * dot_gap;
    int dot_start_x = mx + (modal_w - dots_total_w) / 2;
    int dot_y = my + 55;

    for (int i = 0; i < 4; i++) {
        SDL_FRect dot = {(float)(dot_start_x + i * (dot_size + dot_gap)), (float)dot_y, (float)dot_size, (float)dot_size};
        bool filled = (i < (int)pin_input.length());
        if (filled) {
            SDL_SetRenderDrawColor(renderer->sdl_renderer, 100, 180, 255, 255);
            SDL_RenderFillRect(renderer->sdl_renderer, &dot);
        } else {
            SDL_SetRenderDrawColor(renderer->sdl_renderer, 40, 40, 50, 255);
            SDL_RenderFillRect(renderer->sdl_renderer, &dot);
        }
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 80, 80, 100, 200);
        SDL_RenderRect(renderer->sdl_renderer, &dot);
    }

    // Draw numeric keypad
    int key_size = 64;
    int key_gap = 10;
    int keypad_w = 3 * key_size + 2 * key_gap;
    int kx = mx + (modal_w - keypad_w) / 2;
    int ky = my + 100;

    // Keys: 1-9, backspace, 0, OK  (row-major, 4 rows x 3 cols)
    static const char* keys[] = {"1","2","3","4","5","6","7","8","9","<","0","OK"};

    for (int i = 0; i < 12; i++) {
        int row = i / 3;
        int col = i % 3;
        int bx = kx + col * (key_size + key_gap);
        int by = ky + row * (key_size + key_gap);

        bool is_ok = (i == 11);
        bool is_del = (i == 9);
        if (is_ok) {
            SDL_SetRenderDrawColor(renderer->sdl_renderer, 30, 120, 50, 255);
        } else if (is_del) {
            SDL_SetRenderDrawColor(renderer->sdl_renderer, 120, 40, 40, 255);
        } else {
            SDL_SetRenderDrawColor(renderer->sdl_renderer, 40, 40, 50, 230);
        }

        SDL_FRect krect = {(float)bx, (float)by, (float)key_size, (float)key_size};
        SDL_RenderFillRect(renderer->sdl_renderer, &krect);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 60, 60, 80, 200);
        SDL_RenderRect(renderer->sdl_renderer, &krect);

        // Center the key label
        std::string label = keys[i];
        int tw = (int)label.length() * 14;
        font_renderer->draw_text(bx + (key_size - tw) / 2, by + 16, title_font, label, 220, 220, 230, 255);
    }

    // Error / attempt count message
    if (pin_attempts > 0) {
        int remaining = 3 - pin_attempts;
        std::string err = std::format("Wrong PIN ({} left)", remaining);
        font_renderer->draw_text(mx + (modal_w - (int)err.length() * 9) / 2, my + modal_h - 40, body_font, err, 255, 120, 120, 255);
    }
}

bool OverlayManager::check_pin(const std::string& input) {
    std::string stored;
    {
        std::shared_lock<std::shared_mutex> lk(g_config_mtx);
        stored = g_cfg.dashboard_pin;
    }
    if (stored.empty()) return true; // No PIN configured = always passes
    return input == stored;
}

bool OverlayManager::handle_touch_click(float x, float y) {
    int sw = g_renderer.screen_w;
    int sh = g_renderer.screen_h;

    // Check PIN keypad clicks first
    if (pin_active) {
        Uint64 now = SDL_GetTicks();
        if (pin_locked_until > now) return true; // Still locked

        int sw = g_renderer.screen_w;
        int sh = g_renderer.screen_h;
        int modal_w = 320;
        int modal_h = 420;
        int mx = (sw - modal_w) / 2;
        int my = (sh - modal_h) / 2;
        int key_size = 64;
        int key_gap = 10;
        int keypad_w = 3 * key_size + 2 * key_gap;
        int kx = mx + (modal_w - keypad_w) / 2;
        int ky = my + 100;

        for (int i = 0; i < 12; i++) {
            int row = i / 3;
            int col = i % 3;
            int bx = kx + col * (key_size + key_gap);
            int by = ky + row * (key_size + key_gap);

            if (x >= bx && x <= bx + key_size && y >= by && y <= by + key_size) {
                if (i == 9) {
                    // Backspace
                    if (!pin_input.empty()) pin_input.pop_back();
                    return true;
                } else if (i == 11) {
                    // OK - validate PIN
                    if ((int)pin_input.length() < 4) return true;
                    if (check_pin(pin_input)) {
                        pin_active = false;
                        pin_input.clear();
                        pin_attempts = 0;
                        pin_unlocked = true;
                        g_logger.info("TOUCH_INPUT: PIN correct, unlocking touch controls.");
                        nav_overlay_active = true;
                        nav_overlay_show_time = SDL_GetTicks();
                    } else {
                        pin_attempts++;
                        pin_input.clear();
                        g_logger.info("TOUCH_INPUT: Wrong PIN entered (attempt {}/3).", pin_attempts);
                        if (pin_attempts >= 3) {
                            pin_locked_until = SDL_GetTicks() + 60000;
                            pin_attempts = 0;
                            g_logger.info("TOUCH_INPUT: PIN lockout activated for 60 seconds.");
                        }
                    }
                    return true;
                } else {
                    // Digit 0-9
                    if ((int)pin_input.length() < 4) {
                        if (i == 10) pin_input += '0';
                        else pin_input += ('1' + i);
                    }
                    return true;
                }
            }
        }
        return true; // PIN click consumed (clicked outside buttons but on PIN overlay)
    }

    // Check Touch Navigation Overlay clicks first
    if (nav_overlay_active && !menu_active) {
        int btn_size = 70;
        int left_x = 40;
        int right_x = sw - 110;
        int center_x = (sw - btn_size) / 2;
        int btn_y = (sh - btn_size) / 2;

        // Reset/refresh timer on any screen interaction during overlay display
        nav_overlay_show_time = SDL_GetTicks();

        // Check Left button (Previous)
        if (x >= left_x && x <= left_x + btn_size && y >= btn_y && y <= btn_y + btn_size) {
            g_remote_command.store(2); // previous
            nav_overlay_active = false;
            pin_unlocked = false;
            g_logger.info("TOUCH_INPUT: Previous button clicked on navigation overlay.");
            return true;
        }
        // Check Right button (Next)
        if (x >= right_x && x <= right_x + btn_size && y >= btn_y && y <= btn_y + btn_size) {
            g_remote_command.store(1); // next
            nav_overlay_active = false;
            pin_unlocked = false;
            g_logger.info("TOUCH_INPUT: Next button clicked on navigation overlay.");
            return true;
        }
        // Check Center button (Config Settings)
        if (x >= center_x && x <= center_x + btn_size && y >= btn_y && y <= btn_y + btn_size) {
            menu_active = true;
            nav_overlay_active = false;
            pin_unlocked = false;
            g_logger.info("TOUCH_INPUT: Settings gear clicked on navigation overlay, opening menu.");
            return true;
        }

        // Clicked outside buttons: dismiss the navigation overlay
        nav_overlay_active = false;
        pin_unlocked = false;
        return true;
    }

    if (!menu_active) return false;

    // 1. Keyboard Clicks
    if (keyboard_active) {
        int kb_w = 340;
        int kb_h = 380;
        int kb_x = (sw - kb_w) / 2;
        int kb_y = (sh - kb_h) / 2;

        if (x >= kb_x + 20 && x <= kb_x + kb_w - 20 && y >= kb_y + 325 && y <= kb_y + 361) {
            keyboard_active = false;
            keyboard_input = "";
            return true;
        }

        int start_kx = kb_x + 20;
        int start_ky = kb_y + 110;
        int kw = 90;
        int kh = 50;
        int gap = 10;

        std::vector<std::string> keys = {
            "1", "2", "3",
            "4", "5", "6",
            "7", "8", "9",
            "⌫", "0", "OK"
        };

        for (int i = 0; i < 12; i++) {
            int col = i % 3;
            int row = i / 3;
            int kx = start_kx + col * (kw + gap);
            int ky = start_ky + row * (kh + gap);

            if (x >= kx && x <= kx + kw && y >= ky && y <= ky + kh) {
                std::string key = keys[i];
                if (key == "OK") {
                    int val = safe_stoi(keyboard_input, 0);
                    if (keyboard_target == 0) {
                        if (val < 1) val = 1;
                        {
                            std::lock_guard lk(g_config_mtx);
                            g_cfg.transition_delay = (double)val;
                            g_cfg.save(g_cfg.loaded_path.c_str());
                        }
                        g_config_changed.store(true);
                        g_logger.info("TOUCH_INPUT: Set transition delay to {} seconds.", val);
                    } else if (keyboard_target == 1) {
                        if (val < 0) val = 0;
                        if (val > 100) val = 100;
                        {
                            std::lock_guard lk(g_config_mtx);
                            g_cfg.video_volume = val;
                            g_cfg.save(g_cfg.loaded_path.c_str());
                        }
                        g_config_changed.store(true);
                        g_logger.info("TOUCH_INPUT: Set video volume to {}%%.", val);
                    }
                    keyboard_active = false;
                    keyboard_input = "";
                } else if (key == "⌫") {
                    if (!keyboard_input.empty()) {
                        keyboard_input.pop_back();
                    }
                } else {
                    if (keyboard_input.length() < 4) {
                        keyboard_input += key;
                    }
                }
                return true;
            }
        }

        if (x < kb_x || x > kb_x + kb_w || y < kb_y || y > kb_y + kb_h) {
            keyboard_active = false;
            keyboard_input = "";
            return true;
        }
        return true;
    }

    // 2. Menu Clicks
    int menu_w = 520;
    int menu_h = 380;
    int menu_x = (sw - menu_w) / 2;
    int menu_y = (sh - menu_h) / 2;

    int start_y = menu_y + 55;
    int row_h = 32;

    // Row 0: Play/Pause Toggle
    if (x >= menu_x + 240 && x <= menu_x + 480 && y >= start_y && y <= start_y + 26) {
        g_slideshow_paused = !g_slideshow_paused.load();
        g_logger.info("TOUCH_INPUT: Slideshow state (Play/Pause) toggled.");
        return true;
    }

    // Row 1: Shuffle Toggle
    if (x >= menu_x + 240 && x <= menu_x + 480 && y >= start_y + 1 * row_h && y <= start_y + 1 * row_h + 26) {
        bool curr;
        {
            std::lock_guard lk(g_config_mtx);
            g_cfg.shuffle = !g_cfg.shuffle;
            g_cfg.save(g_cfg.loaded_path.c_str());
            curr = g_cfg.shuffle;
        }
        g_config_changed.store(true);
        g_logger.info("TOUCH_INPUT: Shuffle state set to {}.", curr ? "ON" : "OFF");
        return true;
    }

    // Row 2: Interval Delay
    int r2_y = start_y + 2 * row_h;
    if (y >= r2_y && y <= r2_y + 26) {
        double current_val;
        { std::lock_guard lk(g_config_mtx); current_val = g_cfg.transition_delay; }
        
        if (x >= menu_x + 240 && x <= menu_x + 280) {
            current_val = std::max(5.0, current_val - 5.0);
            {
                std::lock_guard lk(g_config_mtx);
                g_cfg.transition_delay = current_val;
                g_cfg.save(g_cfg.loaded_path.c_str());
            }
            g_config_changed.store(true);
            return true;
        }
        if (x >= menu_x + 440 && x <= menu_x + 480) {
            current_val = std::min(3600.0, current_val + 5.0);
            {
                std::lock_guard lk(g_config_mtx);
                g_cfg.transition_delay = current_val;
                g_cfg.save(g_cfg.loaded_path.c_str());
            }
            g_config_changed.store(true);
            return true;
        }
        if (x >= menu_x + 290 && x <= menu_x + 430) {
            keyboard_active = true;
            keyboard_target = 0;
            keyboard_input = "";
            return true;
        }
    }

    // Row 3: Video Volume
    int r3_y = start_y + 3 * row_h;
    if (y >= r3_y && y <= r3_y + 26) {
        int current_val;
        { std::lock_guard lk(g_config_mtx); current_val = g_cfg.video_volume; }

        if (x >= menu_x + 240 && x <= menu_x + 280) {
            current_val = std::max(0, current_val - 5);
            {
                std::lock_guard lk(g_config_mtx);
                g_cfg.video_volume = current_val;
                g_cfg.save(g_cfg.loaded_path.c_str());
            }
            g_config_changed.store(true);
            return true;
        }
        if (x >= menu_x + 440 && x <= menu_x + 480) {
            current_val = std::min(100, current_val + 5);
            {
                std::lock_guard lk(g_config_mtx);
                g_cfg.video_volume = current_val;
                g_cfg.save(g_cfg.loaded_path.c_str());
            }
            g_config_changed.store(true);
            return true;
        }
        if (x >= menu_x + 290 && x <= menu_x + 430) {
            keyboard_active = true;
            keyboard_target = 1;
            keyboard_input = "";
            return true;
        }
    }

    // Volume Slider Touch Track (Row 4)
    int r4_y = start_y + 4 * row_h - 2;
    if (x >= menu_x + 230 && x <= menu_x + 490 && y >= r4_y && y <= r4_y + 16) {
        float pct = (x - (menu_x + 240)) / 240.0f;
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 1.0f) pct = 1.0f;
        int val = (int)(pct * 100);
        {
            std::lock_guard lk(g_config_mtx);
            g_cfg.video_volume = val;
            g_cfg.save(g_cfg.loaded_path.c_str());
        }
        g_config_changed.store(true);
        return true;
    }

    // Row 5: Screen Blanking Toggles
    if (x >= menu_x + 240 && x <= menu_x + 480 && y >= start_y + 5 * row_h && y <= start_y + 5 * row_h + 26) {
        bool expected = g_screen_blanked.load();
        bool desired = !expected;
        while (!g_screen_blanked.compare_exchange_weak(expected, desired)) {
            desired = !expected;
        }
        set_display_power(expected);
        std::string prefix;
        { std::lock_guard lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; }
        mqtt_publish(prefix + "/status/screen", g_screen_blanked.load() ? "OFF" : "ON", true);
        g_logger.info("TOUCH_INPUT: Physical screen power toggled to {} via menu.", desired ? "OFF" : "ON");
        return true;
    }

    // Row 6: Playlist Control Buttons
    int r6_y = start_y + 6 * row_h;
    if (y >= r6_y && y <= r6_y + 26) {
        if (x >= menu_x + 240 && x <= menu_x + 355) {
            g_remote_command.store(2);
            g_logger.info("TOUCH_INPUT: Previous button clicked in settings popup menu.");
            return true;
        }
        if (x >= menu_x + 365 && x <= menu_x + 480) {
            g_remote_command.store(1);
            g_logger.info("TOUCH_INPUT: Next button clicked in settings popup menu.");
            return true;
        }
    }

    // Row 7: Close Quick Menu
    int r7_y = start_y + 7 * row_h + 4;
    if (x >= menu_x + 24 && x <= menu_x + 496 && y >= r7_y && y <= r7_y + 30) {
        menu_active = false;
        return true;
    }

    if (x < menu_x || x > menu_x + menu_w || y < menu_y || y > menu_y + menu_h) {
        menu_active = false;
        return true;
    }

    return true;
}

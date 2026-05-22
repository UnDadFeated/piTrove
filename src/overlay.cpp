#include "overlay.h"
#include "util.h"
#include "config.h"
#include "media_item.h"
#include "image_loader.h"
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <filesystem>
#include <fstream>

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
        std::lock_guard<std::mutex> lock(g_config_mtx);
        if (g_cfg.font_path != "auto" && !g_cfg.font_path.empty()) {
            font_path = g_cfg.font_path;
        }
    }

    if (font_path.empty() || !file_exists(font_path)) {
        if (!font_path.empty()) {
            g_logger.warn("Configured font_path '%s' not found, falling back to defaults", font_path.c_str());
        }
        font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf";
        if (!file_exists(font_path)) {
            font_path = exe_dir + "/src/fonts/DejaVuSansMono-Bold.ttf";
            if (!file_exists(font_path)) {
                font_path = exe_dir + "/fonts/DejaVuSansMono-Bold.ttf";
                if (!file_exists(font_path)) {
                    font_path = "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf";
                }
            }
        }
    }

    if (file_exists(font_path)) {
        // Load default size 20, font_renderer load_font handles caching and resizing dynamically
        overlay_font = &font_renderer->load_font(font_path, 20);
        font_loaded = true;
        g_logger.info("Overlay manager font loaded: %s", font_path.c_str());
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

void OverlayManager::get_adaptive_colors(const ImageData* img, GpuColor& text_col, GpuColor& shadow_col) {
    bool adaptive = false;
    {
        std::lock_guard<std::mutex> lock(g_config_mtx);
        adaptive = g_cfg.adaptive_text_enabled;
    }
    if (!adaptive || !img) {
        text_col = {255, 255, 255, 255};
        shadow_col = {0, 0, 0, 200};
        return;
    }
    double luma = 0.2126 * img->avg_r + 0.7152 * img->avg_g + 0.0722 * img->avg_b;
    if (luma > 140) { // bright background
        text_col = {15, 15, 15, 255};
        shadow_col = {255, 255, 255, 200};
    } else { // dark background
        text_col = {255, 255, 255, 255};
        shadow_col = {0, 0, 0, 200};
    }
}

void OverlayManager::draw_all(int current_idx, int total_items, const MediaItem* item, const MediaItem* twin_item, double item_timer, bool is_video, int active_fps, const ImageData* current_data, const ImageData* current_twin_data) {
    if (!font_loaded || !font_renderer || !overlay_font) return;
    (void)current_twin_data; // used for twin texture rendering elsewhere

    int pad = 15;
    int sw = g_renderer.screen_w;
    int sh = g_renderer.screen_h;

    // Load configurations thread-safely
    bool date_enabled = false;
    std::string date_text = "%Y-%m-%d";
    float date_x = 0.1f, date_y = 0.08f;
    int date_size = 20;
    GpuColor date_col = {200, 200, 200, 255};

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

    {
        std::lock_guard<std::mutex> lock(g_config_mtx);
        date_enabled = g_cfg.date_overlay_enabled;
        date_text = g_cfg.date_text;
        date_x = g_cfg.date_x;
        date_y = g_cfg.date_y;
        date_size = g_cfg.date_font_size;
        date_col = get_color_from_str(g_cfg.date_color);

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
    }

    // Helper to render contrast-aware text with outline or shadow
    auto draw_contrast_text = [&](int x, int y, FontHandle& font, const std::string& text, GpuColor def_col, const ImageData* img) {
        GpuColor txt_c = def_col;
        GpuColor shd_c = {0, 0, 0, 200};
        get_adaptive_colors(img, txt_c, shd_c);
        
        bool adaptive = false;
        {
            std::lock_guard<std::mutex> lock(g_config_mtx);
            adaptive = g_cfg.adaptive_text_enabled;
        }
        if (adaptive) {
            draw_text_with_outline(x, y, font, text, txt_c, shd_c);
        } else {
            draw_text_with_shadow(x, y, font, text, def_col);
        }
    };

    // Overall image ref for global elements
    const ImageData* global_img = current_data;

    // 1. Date Overlay
    if (date_enabled) {
        char datebuf[64];
        time_t now = time(nullptr);
        struct tm tm_buf;
        struct tm* tm_info = localtime_r(&now, &tm_buf);
        if (tm_info && strftime(datebuf, sizeof(datebuf), date_text.c_str(), tm_info) != 0) {
            int dx = pad + (int)((sw - pad * 2) * date_x);
            int dy = pad + (int)((sh - pad * 2) * date_y);
            FontHandle& font = font_renderer->load_font(overlay_font->path, date_size);
            draw_contrast_text(dx, dy, font, datebuf, date_col, global_img);
        }
    }

    // 2. Filename Overlay
    if (file_enabled && item) {
        int fx = pad + (int)((sw - pad * 2) * file_x) - 10;
        int fy = pad + (int)((sh - pad * 2) * file_y);
        FontHandle& font = font_renderer->load_font(overlay_font->path, file_size);

        if (twin_item) {
            // Stack both filenames vertically in lower-left
            int th = 0, tw = 0;
            font_renderer->measure(font, item->filename, tw, th);
            draw_contrast_text(fx, fy - th - 4, font, item->filename, {255, 255, 255, 255}, global_img);
            draw_contrast_text(fx, fy, font, twin_item->filename, {255, 255, 255, 255}, global_img);
        } else {
            draw_contrast_text(fx, fy, font, item->filename, {255, 255, 255, 255}, current_data);
        }
    }

    // 3. Count Overlay
    if (count_enabled && total_items > 0) {
        char cntbuf[128];
        std::snprintf(cntbuf, sizeof(cntbuf), "%d / %d", current_idx + 1, total_items);
        int cx = pad + (int)((sw - pad * 2) * count_x);
        int cy = pad + (int)((sh - pad * 2) * count_y);
        FontHandle& font = font_renderer->load_font(overlay_font->path, count_size);
        int tw, th;
        font_renderer->measure(font, cntbuf, tw, th);
        cx -= tw / 2;
        draw_contrast_text(cx, cy, font, cntbuf, {200, 200, 200, 220}, global_img);
    }

    // 4. Timer Overlay
    if (timer_enabled && !is_video) {
        char tbuf[32];
        int rem = std::max(0, (int)(transition_delay - item_timer));
        std::snprintf(tbuf, sizeof(tbuf), "%ds", rem);
        int tx = pad + (int)((sw - pad * 2) * timer_x) + 25;
        int ty = pad + (int)((sh - pad * 2) * timer_y) - 10;
        FontHandle& font = font_renderer->load_font(overlay_font->path, timer_size);
        draw_contrast_text(tx, ty, font, tbuf, timer_col, global_img);
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
            int clkx = pad + (int)((sw - pad * 2) * clock_x) - clkw / 2;
            int clky = pad + (int)((sh - pad * 2) * clock_y);
            draw_contrast_text(clkx, clky, font, clkbuf, clock_col, global_img);
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

        std::uintmax_t db_size_kb = 0;
        {
            std::string db_dir = "/home/pi/piTrove/cache";
            {
                std::lock_guard<std::mutex> lock(g_config_mtx);
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
            res_str = std::to_string(item->width) + "x" + std::to_string(item->height);
            if (twin_item) {
                res_str += " | " + std::to_string(twin_item->width) + "x" + std::to_string(twin_item->height);
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
        lines.push_back("FPS     : " + std::to_string(active_fps));
        lines.push_back("SOC TEMP: " + std::to_string((int)soc_temp) + " C");
        lines.push_back("DB SIZE : " + std::to_string(db_size_kb) + " KB");
        lines.push_back("ITEMS   : " + std::to_string(current_idx + 1) + " / " + std::to_string(total_items));
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

        int hx = pad + 10;
        int hy = pad + 10;

        SDL_Rect container = { hx - 8, hy - 8, max_w + 16, total_h + 16 };
        SDL_SetRenderDrawBlendMode(renderer->sdl_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 10, 10, 10, 200);
        SDL_RenderFillRect(renderer->sdl_renderer, (SDL_FRect*)&container);
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

    // 7. Gold Ribbon Anniversary Banner (On This Day)
    bool show_ribbon = false;
    int anniversary_years = 0;
    if (on_this_day_enabled && item) {
        int y = 0, m = 0, d = 0;
        if (get_item_date(*item, y, m, d)) {
            time_t now_t = time(nullptr);
            struct tm tm_now;
            struct tm* tmi = localtime_r(&now_t, &tm_now);
            if (tmi) {
                int today_y = tmi->tm_year + 1900;
                anniversary_years = today_y - y;
                if (anniversary_years > 0) {
                    show_ribbon = true;
                }
            }
        }
    }

    if (show_ribbon) {
        std::string ribbon_text = "★ " + std::to_string(anniversary_years) + (anniversary_years == 1 ? " YEAR AGO TODAY ★" : " YEARS AGO TODAY ★");
        FontHandle& font = font_renderer->load_font(overlay_font->path, 22);
        int tw, th;
        font_renderer->measure(font, ribbon_text, tw, th);
        int rx = (sw - tw) / 2;
        int ry = pad + 15;

        SDL_Rect ribbon_bg = { rx - 20, ry - 6, tw + 40, th + 12 };
        SDL_SetRenderDrawBlendMode(renderer->sdl_renderer, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer->sdl_renderer, 218, 165, 32, 180); // Gold translucent
        SDL_RenderFillRect(renderer->sdl_renderer, (SDL_FRect*)&ribbon_bg);

        SDL_SetRenderDrawColor(renderer->sdl_renderer, 255, 223, 0, 255); // Solid gold outline
        SDL_FRect ribbon_outline = { (float)ribbon_bg.x, (float)ribbon_bg.y, (float)ribbon_bg.w, (float)ribbon_bg.h };
        SDL_RenderRect(renderer->sdl_renderer, &ribbon_outline);

        font_renderer->draw_text(rx, ry, font, ribbon_text, 255, 255, 255, 255);
    }
}

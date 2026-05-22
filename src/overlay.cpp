#include "overlay.h"
#include "util.h"
#include "config.h"
#include <ctime>
#include <iomanip>
#include <sstream>
#include <algorithm>

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
    std::string font_path = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf";
    if (!file_exists(font_path)) {
        font_path = exe_dir + "/src/fonts/DejaVuSansMono-Bold.ttf";
        if (!file_exists(font_path)) {
            font_path = exe_dir + "/fonts/DejaVuSansMono-Bold.ttf";
            if (!file_exists(font_path)) {
                font_path = "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf";
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

void OverlayManager::draw_all(int current_idx, int total_items, const std::string& filename, double item_timer, bool is_video) {
    if (!font_loaded || !font_renderer || !overlay_font) return;

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
    }

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
            draw_text_with_shadow(dx, dy, font, datebuf, date_col);
        }
    }

    // 2. Filename Overlay
    if (file_enabled && current_idx >= 0 && current_idx < total_items) {
        int fx = pad + (int)((sw - pad * 2) * file_x);
        int fy = pad + (int)((sh - pad * 2) * file_y);
        FontHandle& font = font_renderer->load_font(overlay_font->path, file_size);
        draw_text_with_shadow(fx, fy, font, filename, {255, 255, 255, 255});
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
        draw_text_with_shadow(cx, cy, font, cntbuf, {200, 200, 200, 220});
    }

    // 4. Timer Overlay
    if (timer_enabled && !is_video) {
        char tbuf[32];
        int rem = std::max(0, (int)(transition_delay - item_timer));
        std::snprintf(tbuf, sizeof(tbuf), "%ds", rem);
        int tx = pad + (int)((sw - pad * 2) * timer_x);
        int ty = pad + (int)((sh - pad * 2) * timer_y);
        FontHandle& font = font_renderer->load_font(overlay_font->path, timer_size);
        draw_text_with_shadow(tx, ty, font, tbuf, timer_col);
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
            draw_text_with_shadow(clkx, clky, font, clkbuf, clock_col);
        }
    }
}

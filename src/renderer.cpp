#include "renderer.h"
#include "util.h"
#include "config.h"
#include <SDL3_image/SDL_image.h>
#include <stb_image.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <unistd.h>
#include <cstring>
#include <ctime>

static float read_ram_usage() {
    static float cached_val = 0.0f;
    static time_t cached_ts = 0;
    time_t now = time(nullptr);
    if (now == cached_ts) return cached_val;

    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) return 0.0f;

    long total = 0, available = 0;
    std::string line;
    while (std::getline(meminfo, line)) {
        if (line.find("MemTotal:") == 0) {
            std::sscanf(line.c_str(), "MemTotal: %ld kB", &total);
        } else if (line.find("MemAvailable:") == 0) {
            std::sscanf(line.c_str(), "MemAvailable: %ld kB", &available);
        }
    }
    meminfo.close();

    if (total <= 0) { cached_val = 0.0f; cached_ts = now; return 0.0f; }
    cached_val = ((total - available) / (float)total) * 100.0f;
    cached_ts = now;
    return cached_val;
}

static float read_cpu_usage() {
    // Instantaneous CPU usage via two-sample delta on /proc/stat
    struct Sample { long long total; long long idle; };

    auto read_sample = []() -> Sample {
        std::ifstream stat("/proc/stat");
        std::string line;
        std::getline(stat, line);
        std::istringstream iss(line);
        std::string label;
        long long user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0;
        iss >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq;
        long long total = user + nice + system + idle + iowait + irq + softirq;
        return {total, idle};
    };

    static Sample prev = {};
    if (prev.total == 0) {
        prev = read_sample();
        return 0.0f;
    }

    Sample cur = read_sample();
    long long d_total = cur.total - prev.total;
    long long d_busy = d_total - (cur.idle - prev.idle);
    prev = cur;

    if (d_total == 0) return 0.0f;
    return ((float)d_busy / (float)d_total) * 100.0f;
}

static float read_cpu_freq() {
    long long freq = 0;
    std::ifstream freqfile("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
    if (freqfile.is_open()) {
        freqfile >> freq;
        freqfile.close();
    }
    if (freq <= 0) {
        std::ifstream freqfile2("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq");
        if (freqfile2.is_open()) {
            freqfile2 >> freq;
            freqfile2.close();
        }
    }
    return (freq > 0) ? (freq / 1000.0f) : 0.0f;
}

static double get_uptime() {
    std::ifstream uptime("/proc/uptime");
    if (!uptime.is_open()) return 0.0;
    double up = 0;
    uptime >> up;
    uptime.close();
    return up;
}

Renderer g_renderer;

static GpuColor get_pixel_color(SDL_Surface* surface, int x, int y) {
    if (!surface || x < 0 || x >= surface->w || y < 0 || y >= surface->h) {
        return {0, 0, 0, 255};
    }

    int bpp = SDL_BYTESPERPIXEL(surface->format);
    uint8_t* p = (uint8_t*)surface->pixels + y * surface->pitch + x * bpp;

    uint32_t pixel = 0;
    switch (bpp) {
        case 1: pixel = *p; break;
        case 2: pixel = *(uint16_t*)p; break;
        case 3:
            if (SDL_BYTEORDER == SDL_BIG_ENDIAN) {
                pixel = p[0] << 16 | p[1] << 8 | p[2];
            } else {
                pixel = p[0] | p[1] << 8 | p[2] << 16;
            }
            break;
        case 4: pixel = *(uint32_t*)p; break;
        default: return {0, 0, 0, 255};
    }

    uint8_t r = 0, g = 0, b = 0, a = 255;
    const SDL_PixelFormatDetails* details = SDL_GetPixelFormatDetails(surface->format);
    if (details) {
        SDL_GetRGBA(pixel, details, nullptr, &r, &g, &b, &a);
    }
    return {r, g, b, a};
}

Renderer::Renderer() {}

Renderer::~Renderer() {
    cleanup();
}

bool Renderer::init(int w, int h, bool fullscreen) {
    g_logger.info("[TRACE] Renderer::init w=%d h=%d fullscreen=%d", w, h, fullscreen);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        g_logger.error("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    g_logger.info("[TRACE] SDL_Init OK");

    Uint32 flags = SDL_WINDOW_OPENGL;
    if (fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }
    const char* driver = SDL_GetCurrentVideoDriver();
    g_logger.info("SDL Selected Video Driver: %s", driver ? driver : "UNKNOWN");

    window = SDL_CreateWindow(
        APP_NAME " v" VERSION,
        w,
        h,
        flags
    );

    if (!window) {
        g_logger.error("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return false;
    }
    g_logger.info("[TRACE] SDL_CreateWindow OK window=%p", (void*)window);

    SDL_GetWindowSize(window, &screen_w, &screen_h);
    g_logger.info("SDL Context & Window created successfully (%dx%d)", screen_w, screen_h);

    sdl_renderer = SDL_CreateRenderer(window, nullptr);
    if (!sdl_renderer) {
        g_logger.error("SDL_CreateRenderer failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return false;
    }
    g_logger.info("[TRACE] SDL_CreateRenderer OK renderer=%p", (void*)sdl_renderer);

    SDL_SetRenderDrawBlendMode(sdl_renderer, SDL_BLENDMODE_BLEND);
    g_logger.info("[TRACE] Renderer::init complete");
    return true;
}

void Renderer::cleanup() {
    g_logger.info("[TRACE] Renderer::cleanup");
    static bool already_cleaned = false;
    if (already_cleaned) return;
    already_cleaned = true;
    cleanup_splash();

    if (sdl_renderer) {
        SDL_DestroyRenderer(sdl_renderer);
        sdl_renderer = nullptr;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    SDL_Quit();
}

GpuColor Renderer::get_average_color(SDL_Surface* surface) {
    if (!surface || surface->w <= 0 || surface->h <= 0) {
        return {220, 210, 195, 255};
    }
    const int MAX_STEPS = 64;
    int step_x = std::max(1, surface->w / MAX_STEPS);
    int step_y = std::max(1, surface->h / MAX_STEPS);
    long r = 0, g = 0, b = 0, samples = 0;
    for (int y = 0; y < surface->h; y += step_y) {
        for (int x = 0; x < surface->w; x += step_x) {
            GpuColor c = get_pixel_color(surface, x, y);
            r += c.r; g += c.g; b += c.b;
            ++samples;
        }
    }
    if (samples == 0) return {220, 210, 195, 255};
    return {(uint8_t)(r / samples), (uint8_t)(g / samples), (uint8_t)(b / samples), 255};
}

GpuColor Renderer::get_edge_average_color(SDL_Surface* surface, int depth, int which) {
    if (!surface || surface->w <= 0 || surface->h <= 0) {
        return {0, 0, 0, 255};
    }
    int x0 = 0, y0 = 0, x1 = surface->w, y1 = surface->h;
    depth = std::max(1, depth);
    if (which == 0) { // top
        x0 = 0; y0 = 0; x1 = surface->w; y1 = std::min(depth, surface->h);
    } else if (which == 1) { // bottom
        x0 = 0; y1 = surface->h; x1 = surface->w; y0 = std::max(0, surface->h - depth);
    } else if (which == 2) { // left
        y0 = 0; x0 = 0; y1 = surface->h; x1 = std::min(depth, surface->w);
    } else { // right
        y0 = 0; x1 = surface->w; y1 = surface->h; x0 = std::max(0, surface->w - depth);
    }
    const int STEPS = 32;
    int sx = std::max(1, (x1 - x0) / STEPS);
    int sy = std::max(1, (y1 - y0) / STEPS);
    long r = 0, g = 0, b = 0, n = 0;
    for (int y = y0; y < y1; y += sy) {
        for (int x = x0; x < x1; x += sx) {
            GpuColor c = get_pixel_color(surface, x, y);
            r += c.r; g += c.g; b += c.b;
            n++;
        }
    }
    if (n == 0) return {0, 0, 0, 255};
    return {(uint8_t)(r / n), (uint8_t)(g / n), (uint8_t)(b / n), 255};
}

void Renderer::calculate_fit_rect(int img_w, int img_h, SDL_Rect& out_rect) {
    if (img_w <= 0 || img_h <= 0) {
        out_rect.w = 0;
        out_rect.h = 0;
        out_rect.x = screen_w / 2;
        out_rect.y = screen_h / 2;
        return;
    }

    int area_w = screen_w, area_h = screen_h;
    
    bool has_matting = false;
    bool has_border = false;
    int mat_size = 0;
    int border_w = 0;
    {
        std::lock_guard<std::mutex> lock(g_config_mtx);
        has_matting = g_cfg.matting;
        has_border = g_cfg.border_enabled;
        mat_size = g_renderer.scale_px(g_cfg.matting_size);
        border_w = g_renderer.scale_px(g_cfg.border_width);
    }

    int mat = 0;
    if (has_matting) {
        mat += mat_size;
    }
    if (has_border) {
        mat += border_w;
    }

    if (mat > 0) {
        area_w = screen_w - mat * 2;
        area_h = screen_h - mat * 2;
        if (area_w < 1) area_w = 1;
        if (area_h < 1) area_h = 1;
    }
    float scale = std::min((float)area_w / img_w, (float)area_h / img_h);
    out_rect.w = (int)(img_w * scale);
    out_rect.h = (int)(img_h * scale);
    out_rect.x = (screen_w - out_rect.w) / 2;
    out_rect.y = (screen_h - out_rect.h) / 2;
}

void Renderer::clear(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    SDL_SetRenderDrawColor(sdl_renderer, r, g, b, a);
    SDL_RenderClear(sdl_renderer);
}

void Renderer::present() {
    SDL_RenderPresent(sdl_renderer);
}

void Renderer::draw_blurred_background(SDL_Texture* blur_texture, Uint8 vignette_alpha) {
    if (!blur_texture || !sdl_renderer) return;

    SDL_SetRenderTarget(sdl_renderer, nullptr);
    SDL_SetRenderDrawBlendMode(sdl_renderer, SDL_BLENDMODE_BLEND);

    // Render the blur texture fullscreen with alpha modulation for vignette darkening
    SDL_SetTextureAlphaMod(blur_texture, vignette_alpha);
    SDL_FRect dst = {0.0f, 0.0f, (float)screen_w, (float)screen_h};
    SDL_RenderTexture(sdl_renderer, blur_texture, nullptr, &dst);
    SDL_SetTextureAlphaMod(blur_texture, 255);
}

void Renderer::draw_blurred_from_raw(const RawImage& blur_raw, Uint8 vignette_alpha) {
    if (!blur_raw.valid || !blur_raw.pixels || !sdl_renderer) return;

    SDL_ClearError();
    // Create surface from raw pixels
    SDL_Surface* blur_surf = SDL_CreateSurface(blur_raw.width, blur_raw.height, SDL_PIXELFORMAT_RGBA32);
    if (!blur_surf) {
        g_logger.error("SDL_CreateSurface failed in draw_blurred_from_raw: %s", SDL_GetError());
        return;
    }

    uint8_t* dst_pixels = (uint8_t*)blur_surf->pixels;
    const uint8_t* src = blur_raw.pixels;
    for (int y = 0; y < blur_raw.height; y++) {
        memcpy(dst_pixels + y * blur_surf->pitch, src + y * blur_raw.width * 4, blur_raw.width * 4);
    }

    // Create texture from surface
    SDL_Texture* blur_tex = SDL_CreateTextureFromSurface(sdl_renderer, blur_surf);
    SDL_DestroySurface(blur_surf);
    if (!blur_tex) {
        g_logger.error("SDL_CreateTextureFromSurface failed in draw_blurred_from_raw: %s", SDL_GetError());
        return;
    }

    // Render with alpha modulation
    SDL_SetRenderTarget(sdl_renderer, nullptr);
    SDL_SetRenderDrawBlendMode(sdl_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetTextureAlphaMod(blur_tex, vignette_alpha);
    SDL_FRect dst = {0.0f, 0.0f, (float)screen_w, (float)screen_h};
    SDL_RenderTexture(sdl_renderer, blur_tex, nullptr, &dst);
    SDL_SetTextureAlphaMod(blur_tex, 255);

    // Clean up
    SDL_DestroyTexture(blur_tex);
}

void Renderer::draw_color_matched_matte(const SDL_Rect& fit_rect,
    Uint8 matte_r, Uint8 matte_g, Uint8 matte_b, float matte_opacity) {

    if (matte_opacity <= 0.0f) return;

    Uint8 aa = (Uint8)(matte_opacity * 255.0f);
    SDL_SetRenderDrawBlendMode(sdl_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(sdl_renderer, matte_r, matte_g, matte_b, aa);

    // Top strip (full width, above photo)
    if (fit_rect.y > 0) {
        SDL_FRect top = {0.0f, 0.0f, (float)screen_w, (float)fit_rect.y};
        SDL_RenderFillRect(sdl_renderer, &top);
    }
    // Bottom strip (full width, below photo)
    if (fit_rect.y + fit_rect.h < screen_h) {
        int bottom_h = screen_h - (fit_rect.y + fit_rect.h);
        SDL_FRect bottom = {0.0f, (float)(fit_rect.y + fit_rect.h), (float)screen_w, (float)bottom_h};
        SDL_RenderFillRect(sdl_renderer, &bottom);
    }
    // Left strip (between top/bottom edges only — non-overlapping)
    if (fit_rect.x > 0 && fit_rect.h > 0) {
        SDL_FRect left = {0.0f, (float)fit_rect.y, (float)fit_rect.x, (float)fit_rect.h};
        SDL_RenderFillRect(sdl_renderer, &left);
    }
    // Right strip (between top/bottom edges only — non-overlapping)
    if (fit_rect.x + fit_rect.w < screen_w && fit_rect.h > 0) {
        int right_w = screen_w - (fit_rect.x + fit_rect.w);
        SDL_FRect right = {(float)(fit_rect.x + fit_rect.w), (float)fit_rect.y, (float)right_w, (float)fit_rect.h};
        SDL_RenderFillRect(sdl_renderer, &right);
    }
}

void Renderer::draw_matte_borders(const SDL_Rect& fit_rect) {
    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
    
    // Left border
    if (fit_rect.x > 0) {
        SDL_FRect left = {0.0f, 0.0f, (float)fit_rect.x, (float)screen_h};
        SDL_RenderFillRect(sdl_renderer, &left);
    }
    // Right border
    if (fit_rect.x + fit_rect.w < screen_w) {
        int right_w = screen_w - (fit_rect.x + fit_rect.w);
        SDL_FRect right = {(float)(fit_rect.x + fit_rect.w), 0.0f, (float)right_w, (float)screen_h};
        SDL_RenderFillRect(sdl_renderer, &right);
    }
    // Top border
    if (fit_rect.y > 0) {
        SDL_FRect top = {0.0f, 0.0f, (float)screen_w, (float)fit_rect.y};
        SDL_RenderFillRect(sdl_renderer, &top);
    }
    // Bottom border
    if (fit_rect.y + fit_rect.h < screen_h) {
        int bottom_h = screen_h - (fit_rect.y + fit_rect.h);
        SDL_FRect bottom = {0.0f, (float)(fit_rect.y + fit_rect.h), (float)screen_w, (float)bottom_h};
        SDL_RenderFillRect(sdl_renderer, &bottom);
    }
}

void Renderer::draw_bias_lighting(const SDL_Rect& fit_rect, Uint8 avg_r, Uint8 avg_g, Uint8 avg_b,
        int bias_strength, float item_timer, float anim_speed, const std::string& style, int border_width, int glow_depth) {
    if (!sdl_renderer) return;
    int sw = screen_w, sh = screen_h;
    int bw = border_width; // use config border_width (default 10)
    int glow_steps = glow_depth > 0 ? glow_depth : 16; // configurable glow fade depth
    float sf = (float)std::max(0, std::min(bias_strength, 200)) / 150.0f;
    float pulse = 1.0f;
    if (style == "pulse" || style == "edge_glow" || style == "breathe") {
        float t = item_timer * anim_speed;
        if (style == "breathe") {
            pulse = 0.7f + 0.3f * (0.5f + 0.5f * sinf(t * 3.14159f));
        } else {
            pulse = 0.85f + 0.15f * (0.5f + 0.5f * sinf(t * 3.14159f * 2));
        }
    }

    // Glow: gradient strips outward from border, brightest at border edge, fading into matte
    auto glow_up = [&](int gx, int gy, int gw, int max_depth) {
        for (int s = 0; s <= max_depth; s++) {
            float t2 = (float)s / glow_steps;
            float alpha_f = std::expf(-2.5f * t2 * t2) * sf * pulse;
            if (alpha_f < 0.01f) break;
            Uint8 aa = (Uint8)(alpha_f * 255.0f);
            SDL_SetRenderDrawColor(sdl_renderer, avg_r, avg_g, avg_b, aa);
            SDL_FRect r = {(float)gx, (float)(gy - s), (float)gw, 1.0f};
            SDL_RenderFillRect(sdl_renderer, &r);
        }
    };
    auto glow_down = [&](int gx, int gy, int gw, int max_depth) {
        for (int s = 0; s < max_depth; s++) {
            float t2 = (float)s / glow_steps;
            float alpha_f = std::expf(-2.5f * t2 * t2) * sf * pulse;
            if (alpha_f < 0.01f) break;
            Uint8 aa = (Uint8)(alpha_f * 255.0f);
            SDL_SetRenderDrawColor(sdl_renderer, avg_r, avg_g, avg_b, aa);
            SDL_FRect r = {(float)gx, (float)(gy + s), (float)gw, 1.0f};
            SDL_RenderFillRect(sdl_renderer, &r);
        }
    };
    auto glow_left = [&](int gx, int gy, int max_depth, int gh) {
        for (int s = 0; s <= max_depth; s++) {
            float t2 = (float)s / glow_steps;
            float alpha_f = std::expf(-2.5f * t2 * t2) * sf * pulse;
            if (alpha_f < 0.01f) break;
            Uint8 aa = (Uint8)(alpha_f * 255.0f);
            SDL_SetRenderDrawColor(sdl_renderer, avg_r, avg_g, avg_b, aa);
            SDL_FRect r = {(float)(gx - s), (float)gy, 1.0f, (float)gh};
            SDL_RenderFillRect(sdl_renderer, &r);
        }
    };
    auto glow_right = [&](int gx, int gy, int max_depth, int gh) {
        for (int s = 0; s < max_depth; s++) {
            float t2 = (float)s / glow_steps;
            float alpha_f = std::expf(-2.5f * t2 * t2) * sf * pulse;
            if (alpha_f < 0.01f) break;
            Uint8 aa = (Uint8)(alpha_f * 255.0f);
            SDL_SetRenderDrawColor(sdl_renderer, avg_r, avg_g, avg_b, aa);
            SDL_FRect r = {(float)(gx + s), (float)gy, 1.0f, (float)gh};
            SDL_RenderFillRect(sdl_renderer, &r);
        }
    };

    if (g_cfg.edge_glow_shadow) {
        // 3D Shadow Look: only right and bottom glows, and bottom-right corner glow
        { int depth = sh - (fit_rect.y + fit_rect.h + bw); if (depth > 0) glow_down(fit_rect.x - bw, fit_rect.y + fit_rect.h + bw, fit_rect.w + 2*bw + 1, depth); }
        { int depth = sw - (fit_rect.x + fit_rect.w + bw); if (depth > 0) glow_right(fit_rect.x + fit_rect.w + bw, fit_rect.y - bw, depth, fit_rect.h + 2*bw + 1); }

        auto glow_corner = [&](int px, int py, int dx, int dy, int region_w, int region_h) {
            for (int i = 1; i <= region_w; i++) {
                for (int j = 1; j <= region_h; j++) {
                    float dist = std::sqrtf((float)(i * i + j * j));
                    float t2 = dist / glow_steps;
                    float alpha_f = std::expf(-2.5f * t2 * t2) * sf * pulse;
                    if (alpha_f < 0.01f) continue;
                    Uint8 aa = (Uint8)(alpha_f * 255.0f);
                    SDL_SetRenderDrawColor(sdl_renderer, avg_r, avg_g, avg_b, aa);
                    SDL_FRect r = {(float)(px + i * dx), (float)(py + j * dy), 1.0f, 1.0f};
                    SDL_RenderFillRect(sdl_renderer, &r);
                }
            }
        };

        { int rw = sw - (fit_rect.x + fit_rect.w + bw), rh = sh - (fit_rect.y + fit_rect.h + bw); if (rw > 0 && rh > 0) glow_corner(fit_rect.x + fit_rect.w + bw, fit_rect.y + fit_rect.h + bw, 1, 1, rw + 1, rh + 1); }
    } else {
        // Normal 4-sided edge glow and all corners
        { int depth = fit_rect.y - bw; if (depth > 0) glow_up(fit_rect.x - bw, fit_rect.y - bw, fit_rect.w + 2*bw + 1, depth); }
        { int depth = sh - (fit_rect.y + fit_rect.h + bw); if (depth > 0) glow_down(fit_rect.x - bw, fit_rect.y + fit_rect.h + bw, fit_rect.w + 2*bw + 1, depth); }
        { int depth = fit_rect.x - bw; if (depth > 0) glow_left(fit_rect.x - bw, fit_rect.y - bw, depth, fit_rect.h + 2*bw + 1); }
        { int depth = sw - (fit_rect.x + fit_rect.w + bw); if (depth > 0) glow_right(fit_rect.x + fit_rect.w + bw, fit_rect.y - bw, depth, fit_rect.h + 2*bw + 1); }

        auto glow_corner = [&](int px, int py, int dx, int dy, int region_w, int region_h) {
            for (int i = 1; i <= region_w; i++) {
                for (int j = 1; j <= region_h; j++) {
                    float dist = std::sqrtf((float)(i * i + j * j));
                    float t2 = dist / glow_steps;
                    float alpha_f = std::expf(-2.5f * t2 * t2) * sf * pulse;
                    if (alpha_f < 0.01f) continue;
                    Uint8 aa = (Uint8)(alpha_f * 255.0f);
                    SDL_SetRenderDrawColor(sdl_renderer, avg_r, avg_g, avg_b, aa);
                    SDL_FRect r = {(float)(px + i * dx), (float)(py + j * dy), 1.0f, 1.0f};
                    SDL_RenderFillRect(sdl_renderer, &r);
                }
            }
        };

        { int rw = fit_rect.x - bw, rh = fit_rect.y - bw; if (rw > 0 && rh > 0) glow_corner(fit_rect.x - bw, fit_rect.y - bw, -1, -1, rw, rh); }
        { int rw = sw - (fit_rect.x + fit_rect.w + bw), rh = fit_rect.y - bw; if (rw > 0 && rh > 0) glow_corner(fit_rect.x + fit_rect.w + bw, fit_rect.y - bw, 1, -1, rw + 1, rh); }
        { int rw = fit_rect.x - bw, rh = sh - (fit_rect.y + fit_rect.h + bw); if (rw > 0 && rh > 0) glow_corner(fit_rect.x - bw, fit_rect.y + fit_rect.h + bw, -1, 1, rw, rh + 1); }
        { int rw = sw - (fit_rect.x + fit_rect.w + bw), rh = sh - (fit_rect.y + fit_rect.h + bw); if (rw > 0 && rh > 0) glow_corner(fit_rect.x + fit_rect.w + bw, fit_rect.y + fit_rect.h + bw, 1, 1, rw + 1, rh + 1); }
    }
}

void Renderer::draw_3d_border(const SDL_Rect& fit_rect, Uint8 avg_r, Uint8 avg_g, Uint8 avg_b, int border_width) {
    if (!sdl_renderer) return;
    int sw = screen_w, sh = screen_h;
    int bw = border_width;

    Uint8 hi_r = (Uint8)std::min(255, (int)avg_r + 65);
    Uint8 hi_g = (Uint8)std::min(255, (int)avg_g + 65);
    Uint8 hi_b = (Uint8)std::min(255, (int)avg_b + 65);
    Uint8 lo_r = (Uint8)(avg_r * 0.25f);
    Uint8 lo_g = (Uint8)(avg_g * 0.25f);
    Uint8 lo_b = (Uint8)(avg_b * 0.25f);

    // Seam colors
    Uint8 tl_seam_r = (Uint8)std::max(0, (int)avg_r - 35);
    Uint8 tl_seam_g = (Uint8)std::max(0, (int)avg_g - 35);
    Uint8 tl_seam_b = (Uint8)std::max(0, (int)avg_b - 35);
    Uint8 br_seam_r = (Uint8)(avg_r * 0.18f);
    Uint8 br_seam_g = (Uint8)(avg_g * 0.18f);
    Uint8 br_seam_b = (Uint8)(avg_b * 0.18f);
    Uint8 glint_r = (Uint8)std::min(255, (int)avg_r + 85);
    Uint8 glint_g = (Uint8)std::min(255, (int)avg_g + 85);
    Uint8 glint_b = (Uint8)std::min(255, (int)avg_b + 85);

    int ix1 = fit_rect.x, iy1 = fit_rect.y;
    int ix2 = fit_rect.x + fit_rect.w, iy2 = fit_rect.y + fit_rect.h;

    // Fill lo triangle scanline: vertices (cx+bw,cy), (cx+bw,cy+bw), (cx,cy+bw)
    auto fill_tri_br = [&](int cx, int cy, Uint8 r, Uint8 g, Uint8 b) {
        SDL_SetRenderDrawColor(sdl_renderer, r, g, b, 255);
        for (int row = 0; row < bw; row++) {
            int fill_w = row + 1;
            SDL_FRect tr = {(float)(cx + bw - fill_w), (float)(cy + row), (float)fill_w, 1.0f};
            SDL_RenderFillRect(sdl_renderer, &tr);
        }
    };

    // ── Side faces (full-width, full-height — overlaps corners) ──
    if (iy1 > 0) {
        SDL_SetRenderDrawColor(sdl_renderer, hi_r, hi_g, hi_b, 255);
        SDL_FRect r = {(float)ix1, (float)(iy1 - bw), (float)(ix2 - ix1), (float)bw};
        SDL_RenderFillRect(sdl_renderer, &r);
    }
    if (iy2 + bw <= sh) {
        SDL_SetRenderDrawColor(sdl_renderer, lo_r, lo_g, lo_b, 255);
        SDL_FRect r = {(float)ix1, (float)iy2, (float)(ix2 - ix1), (float)bw};
        SDL_RenderFillRect(sdl_renderer, &r);
    }
    if (ix1 > 0) {
        SDL_SetRenderDrawColor(sdl_renderer, hi_r, hi_g, hi_b, 255);
        SDL_FRect r = {(float)(ix1 - bw), (float)iy1, (float)bw, (float)(iy2 - iy1)};
        SDL_RenderFillRect(sdl_renderer, &r);
    }
    if (ix2 + bw <= sw) {
        SDL_SetRenderDrawColor(sdl_renderer, lo_r, lo_g, lo_b, 255);
        SDL_FRect r = {(float)ix2, (float)iy1, (float)bw, (float)(iy2 - iy1)};
        SDL_RenderFillRect(sdl_renderer, &r);
    }

    // ── TL corner (miter) — solid hi + dark crease ──
    {
        SDL_SetRenderDrawColor(sdl_renderer, hi_r, hi_g, hi_b, 255);
        SDL_FRect r = {(float)(ix1 - bw), (float)(iy1 - bw), (float)bw, (float)bw};
        SDL_RenderFillRect(sdl_renderer, &r);
        SDL_SetRenderDrawColor(sdl_renderer, tl_seam_r, tl_seam_g, tl_seam_b, 215);
        SDL_RenderLine(sdl_renderer, (float)(ix1 - 1), (float)(iy1 - 1), (float)(ix1 - bw + 1), (float)(iy1 - bw + 1));
    }

    // ── TR corner — solid hi base + lo triangle overlay + bright glint ──
    {
        SDL_SetRenderDrawColor(sdl_renderer, hi_r, hi_g, hi_b, 255);
        SDL_FRect r = {(float)ix2, (float)(iy1 - bw), (float)bw, (float)bw};
        SDL_RenderFillRect(sdl_renderer, &r);
        fill_tri_br(ix2, iy1 - bw, lo_r, lo_g, lo_b);
        SDL_SetRenderDrawColor(sdl_renderer, glint_r, glint_g, glint_b, 255);
        SDL_RenderLine(sdl_renderer, (float)(ix2 + 1), (float)(iy1 - 1), (float)(ix2 + bw - 1), (float)(iy1 - bw + 1));
    }

    // ── BL corner — solid hi base + lo triangle overlay + bright glint ──
    {
        SDL_SetRenderDrawColor(sdl_renderer, hi_r, hi_g, hi_b, 255);
        SDL_FRect r = {(float)(ix1 - bw), (float)iy2, (float)bw, (float)bw};
        SDL_RenderFillRect(sdl_renderer, &r);
        SDL_SetRenderDrawColor(sdl_renderer, lo_r, lo_g, lo_b, 255);
        for (int row = 0; row < bw; row++) {
            int fill_w = row + 1;
            SDL_FRect tr = {(float)(ix1 - fill_w), (float)(iy2 + row), (float)fill_w, 1.0f};
            SDL_RenderFillRect(sdl_renderer, &tr);
        }
        SDL_SetRenderDrawColor(sdl_renderer, glint_r, glint_g, glint_b, 255);
        SDL_RenderLine(sdl_renderer, (float)(ix1 - 1), (float)(iy2 + 1), (float)(ix1 - bw + 1), (float)(iy2 + bw - 1));
    }

    // ── BR corner (miter) — solid lo + near-black crease ──
    {
        SDL_SetRenderDrawColor(sdl_renderer, lo_r, lo_g, lo_b, 255);
        SDL_FRect r = {(float)ix2, (float)iy2, (float)bw, (float)bw};
        SDL_RenderFillRect(sdl_renderer, &r);
        SDL_SetRenderDrawColor(sdl_renderer, br_seam_r, br_seam_g, br_seam_b, 215);
        SDL_RenderLine(sdl_renderer, (float)(ix2 + 1), (float)(iy2 + 1), (float)(ix2 + bw - 1), (float)(iy2 + bw - 1));
    }

    // 1px outline at exact photo boundary
    {
        SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 180);
        SDL_FRect r = {(float)(ix1 - 1), (float)(iy1 - 1), (float)(ix2 - ix1 + 2), (float)(iy2 - iy1 + 2)};
        SDL_RenderRect(sdl_renderer, &r);
    }
}

void Renderer::draw_solid_border(int width, uint8_t r, uint8_t g, uint8_t b) {
    if (width <= 0) return;
    
    SDL_SetRenderDrawColor(sdl_renderer, r, g, b, 255);
    
    // Top
    SDL_FRect top = {0.0f, 0.0f, (float)screen_w, (float)width};
    SDL_RenderFillRect(sdl_renderer, &top);
    
    // Bottom
    SDL_FRect bottom = {0.0f, (float)(screen_h - width), (float)screen_w, (float)width};
    SDL_RenderFillRect(sdl_renderer, &bottom);
    
    // Left
    SDL_FRect left = {0.0f, 0.0f, (float)width, (float)screen_h};
    SDL_RenderFillRect(sdl_renderer, &left);
    
    // Right
    SDL_FRect right = {(float)(screen_w - width), 0.0f, (float)width, (float)screen_h};
    SDL_RenderFillRect(sdl_renderer, &right);
}

float Renderer::read_sys_f(const char* path, float divisor) {
    std::ifstream file(path);
    if (!file.is_open()) return 0.0f;
    std::string line;
    float result = 0.0f;
    if (std::getline(file, line)) {
        if (!line.empty()) {
            try {
                size_t pos;
                long long v = std::stoll(line, &pos);
                if (pos > 0) {
                    result = (float)v / divisor;
                }
            } catch (...) {}
        }
    }
    file.close();
    return result;
}

std::string Renderer::folder_and_file(const std::string& path) {
    auto slash2 = path.find_last_of('/');
    if (slash2 == std::string::npos) return path;
    auto slash1 = path.find_last_of('/', slash2 - 1);
    if (slash1 == std::string::npos) return path.substr(slash2 + 1);
    return path.substr(slash1 + 1);
}

void Renderer::load_splash(const std::string& path) {
    g_logger.info("[TRACE] Renderer::load_splash path=%s", path.c_str());
    cleanup_splash();

    std::string exe_dir = get_exe_dir();
    std::string splash_path = path;
    if (!file_exists(splash_path)) {
        splash_path = exe_dir + "/src/" + path;
        if (!file_exists(splash_path)) {
            splash_path = exe_dir + "/" + path;
            if (!file_exists(splash_path)) {
                // proc self exe fallback
                char exe_buf[4096];
                ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
                if (len > 0) {
                    exe_buf[len] = '\0';
                    std::string real_dir = std::filesystem::path(exe_buf).parent_path().string();
                    std::string real_src = real_dir + "/src/" + path;
                    if (file_exists(real_src)) {
                        splash_path = real_src;
                    }
                }
            }
        }
    }

    if (file_exists(splash_path)) {
        int w = 0, h = 0, ch = 0;
        uint8_t* pixels = stbi_load(splash_path.c_str(), &w, &h, &ch, 4); // Force RGBA32
        if (pixels) {
            SDL_ClearError();
            SDL_Surface* surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
            if (surface) {
                uint8_t* dst = (uint8_t*)surface->pixels;
                const uint8_t* src = pixels;
                for (int y = 0; y < h; y++) {
                    memcpy(dst + y * surface->pitch, src + y * w * 4, w * 4);
                }
                splash_logo = SDL_CreateTextureFromSurface(sdl_renderer, surface);
                if (!splash_logo) {
                    g_logger.error("SDL_CreateTextureFromSurface failed for splash: %s", SDL_GetError());
                } else {
                    splash_logo_w = w;
                    splash_logo_h = h;
                    splash_logo_loaded = true;
                    g_logger.info("Splash image loaded: %s (%dx%d)", splash_path.c_str(), splash_logo_w, splash_logo_h);
                }
                SDL_DestroySurface(surface);
            } else {
                g_logger.error("SDL_CreateSurface failed for splash: %s", SDL_GetError());
            }
            stbi_image_free(pixels);
        } else {
            g_logger.error("stbi_load failed for splash logo: %s %s", splash_path.c_str(), stbi_failure_reason());
        }
    }

    if (!splash_logo_loaded) {
        g_logger.warn("Splash image not found/loaded: %s, using fallback green terminal screen", splash_path.c_str());
    }

    // FontRenderer now uses SDL_Renderer
    font_renderer = new FontRenderer(this);
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
        crt_font = &font_renderer->load_font(font_path, 15);
        font_loaded = true;
        g_logger.info("Loaded splash terminal font: %s", font_path.c_str());
    } else {
        g_logger.error("No suitable monospace font found for splash terminal.");
        font_loaded = false;
    }
}

void Renderer::cleanup_splash() {
    if (splash_logo) {
        SDL_DestroyTexture(splash_logo);
        splash_logo = nullptr;
    }

    if (font_renderer) {
        delete font_renderer;
        font_renderer = nullptr;
    }
    crt_font = nullptr;
    font_loaded = false;
}

void Renderer::add_splash_log(const std::string& line) {
    std::lock_guard<std::mutex> lock(log_mutex);
    log_buffer.push_back(line);
    if ((int)log_buffer.size() > 50) {
        log_buffer.erase(log_buffer.begin());
    }
}

void Renderer::draw_splash_box(int x, int y, int w, int h) {
    // Outer border
    SDL_SetRenderDrawColor(sdl_renderer, 0, 200, 0, 240);
    
    SDL_FRect rect;
    // Top
    rect = {(float)x, (float)y, (float)w, 2.0f};
    SDL_RenderFillRect(sdl_renderer, &rect);
    // Bottom
    rect = {(float)x, (float)(y + h - 2), (float)w, 2.0f};
    SDL_RenderFillRect(sdl_renderer, &rect);
    // Left
    rect = {(float)x, (float)y, 2.0f, (float)h};
    SDL_RenderFillRect(sdl_renderer, &rect);
    // Right
    rect = {(float)(x + w - 2), (float)y, 2.0f, (float)h};
    SDL_RenderFillRect(sdl_renderer, &rect);

    // Inner shadow border
    SDL_SetRenderDrawColor(sdl_renderer, 0, 130, 0, 220);
    float ix = (float)x + 4.0f, iy = (float)y + 4.0f, iw = (float)w - 8.0f, ih = (float)h - 8.0f;
    // Top
    rect = {ix, iy, iw, 1.0f};
    SDL_RenderFillRect(sdl_renderer, &rect);
    // Bottom
    rect = {ix, iy + ih - 1.0f, iw, 1.0f};
    SDL_RenderFillRect(sdl_renderer, &rect);
    // Left
    rect = {ix, iy, 1.0f, ih};
    SDL_RenderFillRect(sdl_renderer, &rect);
    // Right
    rect = {ix + iw - 1.0f, iy, 1.0f, ih};
    SDL_RenderFillRect(sdl_renderer, &rect);

    // Title box
    int title_size = 20;
    int title_w = 100; // estimated fallback w
    if (font_loaded && crt_font) {
        int title_h;
        font_renderer->measure(font_renderer->load_font(crt_font->path, title_size), APP_NAME, title_w, title_h);
    }
    
    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
    rect = {(float)(x + 15), (float)(y - 2), (float)(title_w + 10), 6.0f};
    SDL_RenderFillRect(sdl_renderer, &rect);

    draw_splash_text(APP_NAME, x + 20, y - 10, title_size, {0, 200, 0, 240});

    // Version label inside box
    int ver_size = 14;
    std::string ver_str = "v" VERSION;
    int ver_w = 40;
    if (font_loaded && crt_font) {
        int ver_h;
        font_renderer->measure(font_renderer->load_font(crt_font->path, ver_size), ver_str, ver_w, ver_h);
    }
    draw_splash_text(ver_str, x + w - ver_w - 28, y + 10, ver_size, {0, 130, 0, 220});
}

void Renderer::draw_splash_text(const std::string& text, int x, int y, int size, GpuColor color) {
    if (!font_renderer || !crt_font || !font_loaded) return;
    FontHandle& font = font_renderer->load_font(crt_font->path, size);
    font_renderer->draw_text_glow(
        x, y, font, text,
        color.r, color.g, color.b, color.a,
        color.r, color.g, color.b, (uint8_t)(color.a * 0.4f)
    );
}

void Renderer::draw_splash_progress_bar(int x, int y, int w, int h, float pct) {
    pct = std::max(0.0f, std::min(1.0f, pct));
    float bar_w = w * 0.9f;
    float filled = bar_w * pct;
    if (filled > bar_w) filled = bar_w;

    SDL_SetRenderDrawColor(sdl_renderer, 0, 40, 0, 200);
    SDL_FRect rect = {(float)x, (float)y, (float)w, (float)h};
    SDL_RenderFillRect(sdl_renderer, &rect);

    SDL_SetRenderDrawColor(sdl_renderer, 0, 200, 0, 240);
    rect = {(float)x, (float)y, filled, (float)h};
    SDL_RenderFillRect(sdl_renderer, &rect);
}

void Renderer::render_splash(int phase, int progress, int total, int done, const char* label, int dot_counter, const char* filename, bool animated) {
    (void)label;
    if (filename) { current_cache_file = filename; }
    int sw = screen_w;
    int sh = screen_h;

    // Clear Screen
    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
    SDL_RenderClear(sdl_renderer);

    // 1. Draw Splash cleanly with Aspect Ratio Scaling
    if (splash_logo_loaded && splash_logo) {
        float scale = std::min((float)sw / (float)splash_logo_w, (float)sh / (float)splash_logo_h);
        int dest_w = (int)(splash_logo_w * scale);
        int dest_h = (int)(splash_logo_h * scale);
        int dest_x = (sw - dest_w) / 2;
        int dest_y = (sh - dest_h) / 2;
        
        SDL_FRect dst = {(float)dest_x, (float)dest_y, (float)dest_w, (float)dest_h};
        SDL_RenderTexture(sdl_renderer, splash_logo, nullptr, &dst);
    }

    // Live Uptime tracking
    static double start_ticks = (double)SDL_GetTicks();
    double uptime = ((double)SDL_GetTicks() - start_ticks) / 1000.0;

    int terminal_start_y = sh * 0.56f;
    int bottom_matte_padding = sh * 0.06f;
    int black_area_end = sh - bottom_matte_padding;

    // 2. Animated scanlines for top white/graphics area (removed for performance)

    // 3. UI Box dimensions
    int box_w = sw * 0.90f;
    int box_x = (sw - box_w) / 2;
    int box_y = terminal_start_y + (sh * 0.015f);
    int box_h = black_area_end - box_y - (sh * 0.015f);

    draw_splash_box(box_x, box_y, box_w, box_h);

   // 4. Dense Phosphorous Green Scanlines over bottom area (CRT effect)
    int scanline_start = terminal_start_y - (sh * 0.02f);
    int scanline_end = black_area_end + (sh * 0.03f);

    for (int sy = scanline_start; sy < scanline_end; sy += 3) {
        // Core phosphor bleed
        SDL_SetRenderDrawColor(sdl_renderer, 0, 255, 0, 10);
        SDL_FRect rect = {0.0f, (float)(sy - 1), (float)sw, 2.0f};
        SDL_RenderFillRect(sdl_renderer, &rect);
        
        // Sharp scanline
        SDL_SetRenderDrawColor(sdl_renderer, 0, 255, 0, 38);
        rect = {0.0f, (float)sy, (float)sw, 1.0f};
        SDL_RenderFillRect(sdl_renderer, &rect);
    }

    // Main Horizontal Center Box Divider
    SDL_SetRenderDrawColor(sdl_renderer, 0, 200, 0, 240);
    int mid_y = box_y + (box_h / 2);
    SDL_FRect rect = {(float)box_x, (float)(mid_y - 1), (float)box_w, 2.0f};
    SDL_RenderFillRect(sdl_renderer, &rect);

    SDL_SetRenderDrawColor(sdl_renderer, 0, 130, 0, 220);
    rect = {(float)(box_x + 4), (float)(mid_y - 5), (float)(box_w - 8), 1.0f};
    SDL_RenderFillRect(sdl_renderer, &rect);
    rect = {(float)(box_x + 4), (float)(mid_y + 4), (float)(box_w - 8), 1.0f};
    SDL_RenderFillRect(sdl_renderer, &rect);

    // Grid Math for Columns
    int col_w = box_w / 4;
    int text_x = box_x + 20;
    int col2_x = box_x + col_w + 15;
    int col3_x = box_x + col_w * 2 + 15;
    int col4_x = box_x + col_w * 3 + 15;

    // Inner Grid Lines (Top Section)
    int inner_y_offset = box_y + 38;
    int top_inner_h = (box_h / 2) - 41;
    
    SDL_SetRenderDrawColor(sdl_renderer, 0, 255, 0, 76);
    rect = {(float)(box_x + 10), (float)inner_y_offset, (float)(box_w - 20), 1.0f};
    SDL_RenderFillRect(sdl_renderer, &rect);

    rect = {(float)(box_x + col_w), (float)inner_y_offset, 1.0f, (float)top_inner_h};
    SDL_RenderFillRect(sdl_renderer, &rect);
    rect = {(float)(box_x + col_w * 2), (float)inner_y_offset, 1.0f, (float)top_inner_h};
    SDL_RenderFillRect(sdl_renderer, &rect);
    rect = {(float)(box_x + col_w * 3), (float)inner_y_offset, 1.0f, (float)top_inner_h};
    SDL_RenderFillRect(sdl_renderer, &rect);

    int dots = (dot_counter / 15) % 4;
    std::string dot_str(dots, '.');
    std::string cursor = "\u2588";

    // Real Hardware Telemetry
    float ram_usage = read_ram_usage();
    float cpu_usage = read_cpu_usage();
    float cpu_freq = read_cpu_freq();
    double sys_uptime = get_uptime();
    static int cached_mem_mb = 0;
    static time_t mem_ts = 0;
    time_t cur_time = time(nullptr);
    if (cur_time != mem_ts) {
        {
            std::ifstream meminfo("/proc/meminfo");
            if (meminfo.is_open()) {
                char label[32];
                long val;
                while (meminfo >> label >> val) {
                    if (std::strcmp(label, "MemAvailable:") == 0) {
                        cached_mem_mb = val / 1024;
                        break;
                    }
                }
            }
        }
        mem_ts = cur_time;
    }
    int mem_mb = cached_mem_mb;
    
    int speed = uptime > 0.001 ? (int)(progress / uptime) : 0;
    int latency = (phase <= 2) ? (rand() % 13 + 2) : 0;

    // TOP HALF: SCANNER
    draw_splash_text("PHASE 2: DIRECTORY SCANNER", text_x, box_y + 12, 20, {0, 200, 0, 240});

    int row_start_y = inner_y_offset + 10;
    float row_space = (float)(box_h * 0.08f);

    if (phase <= 2) {
        draw_splash_text("SYS_STAT : SCAN_ACTIVE" + dot_str, text_x, row_start_y, 16, {0, 200, 0, 240});
        
        char fnd_buf[64];
        std::snprintf(fnd_buf, sizeof(fnd_buf), "FILES FND: %d", progress);
        draw_splash_text(fnd_buf, text_x, (int)(row_start_y + row_space * 1.2f), 18, {0, 200, 0, 240});

        std::snprintf(fnd_buf, sizeof(fnd_buf), "I/O SPEED: %d nodes/s", speed);
        draw_splash_text(fnd_buf, text_x, (int)(row_start_y + row_space * 2.5f), 14, {0, 130, 0, 220});

        std::snprintf(fnd_buf, sizeof(fnd_buf), "LATENCY  : %d ms", latency);
        draw_splash_text(fnd_buf, text_x, (int)(row_start_y + row_space * 3.5f), 14, {0, 130, 0, 220});
    } else {
        draw_splash_text("SYS_STAT : SCAN_COMPLETE", text_x, row_start_y, 16, {0, 130, 0, 220});
        
        char fnd_buf[64];
        std::snprintf(fnd_buf, sizeof(fnd_buf), "FILES FND: %d", progress);
        draw_splash_text(fnd_buf, text_x, (int)(row_start_y + row_space * 1.2f), 18, {0, 130, 0, 220});
        draw_splash_text("I/O SPEED: 0 nodes/s", text_x, (int)(row_start_y + row_space * 2.5f), 14, {0, 130, 0, 220});
        draw_splash_text("LATENCY  : 0 ms", text_x, (int)(row_start_y + row_space * 3.5f), 14, {0, 130, 0, 220});
    }

    // Column 2: System Memory
    time_t now = time(nullptr);
    if (now != telemetry_ts) {
        telemetry_temp = read_sys_f("/sys/class/thermal/thermal_zone0/temp", 1000.0f);
        telemetry_freq = read_sys_f("/sys/class/cpufreq/policy0/scaling_cur_freq", 1000.0f);
        if (telemetry_freq == 0.0f) {
            telemetry_freq = read_sys_f("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", 1000.0f);
        }
        telemetry_ts = now;
    }

    char sys_buf[128];
    std::snprintf(sys_buf, sizeof(sys_buf), "PID      : %d", getpid());
    draw_splash_text(sys_buf, col2_x, row_start_y, 14, {0, 130, 0, 220});

    std::snprintf(sys_buf, sizeof(sys_buf), "RAM AVAIL: %d MB", mem_mb);
    draw_splash_text(sys_buf, col2_x, (int)(row_start_y + row_space), 14, {0, 130, 0, 220});

    std::snprintf(sys_buf, sizeof(sys_buf), "RAM USAGE: %.1f%%", ram_usage);
    draw_splash_text(sys_buf, col2_x, (int)(row_start_y + row_space * 2.2f), 14, {0, 130, 0, 220});

    std::snprintf(sys_buf, sizeof(sys_buf), "UPTIME   : %.0f s", sys_uptime);
    draw_splash_text(sys_buf, col2_x, (int)(row_start_y + row_space * 3.5f), 14, {0, 130, 0, 220});

    // Column 3: CPU & Hardware Info
    std::snprintf(sys_buf, sizeof(sys_buf), "SOC TEMP : %.1f C", telemetry_temp);
    draw_splash_text(sys_buf, col3_x, row_start_y, 14, {0, 130, 0, 220});

    std::snprintf(sys_buf, sizeof(sys_buf), "CPU USAGE: %.1f%%", cpu_usage);
    draw_splash_text(sys_buf, col3_x, (int)(row_start_y + row_space), 14, {0, 130, 0, 220});

    std::snprintf(sys_buf, sizeof(sys_buf), "CPU FREQ : %.1f MHz", cpu_freq);
    draw_splash_text(sys_buf, col3_x, (int)(row_start_y + row_space * 2.2f), 14, {0, 130, 0, 220});

    std::snprintf(sys_buf, sizeof(sys_buf), "CORES    : %d", std::thread::hardware_concurrency());
    draw_splash_text(sys_buf, col3_x, (int)(row_start_y + row_space * 3.5f), 14, {0, 130, 0, 220});

    // Column 4: Storage & Renderer info
    draw_splash_text("HW_DECODE: READY", col4_x, row_start_y, 14, {0, 130, 0, 220});
    draw_splash_text("RENDERER : SDL3", col4_x, (int)(row_start_y + row_space), 14, {0, 130, 0, 220});

    std::snprintf(sys_buf, sizeof(sys_buf), "I/O SPEED: %d ops/s", speed);
    draw_splash_text(sys_buf, col4_x, (int)(row_start_y + row_space * 2.2f), 14, {0, 130, 0, 220});

    std::snprintf(sys_buf, sizeof(sys_buf), "LATENCY  : %d ms", latency);
    draw_splash_text(sys_buf, col4_x, (int)(row_start_y + row_space * 3.5f), 14, {0, 130, 0, 220});

    // BOTTOM HALF: CACHER
    GpuColor theme_color = {0, 200, 0, 240};
    GpuColor theme_dim = {0, 130, 0, 220};
    draw_splash_text("PHASE 3: SQLITE CACHE DB", text_x, mid_y + 12, 20, theme_color);

     // Divider under bottom title
    int bot_inner_y_offset = mid_y + 38;
    SDL_SetRenderDrawColor(sdl_renderer, 0, 255, 0, 76);
    rect = {(float)(box_x + 10), (float)bot_inner_y_offset, (float)(box_w - 20), 1.0f};
    SDL_RenderFillRect(sdl_renderer, &rect);

    int bot_inner_h = 75;
    rect = {(float)(box_x + col_w), (float)bot_inner_y_offset, 1.0f, (float)bot_inner_h};
    SDL_RenderFillRect(sdl_renderer, &rect);
    rect = {(float)(box_x + col_w * 2), (float)bot_inner_y_offset, 1.0f, (float)bot_inner_h};
    SDL_RenderFillRect(sdl_renderer, &rect);
    rect = {(float)(box_x + col_w * 3), (float)bot_inner_y_offset, 1.0f, (float)bot_inner_h};
    SDL_RenderFillRect(sdl_renderer, &rect);

    int bot_row_start_y = bot_inner_y_offset + 10;
    int bot_row_space = 22;

    if (phase > 2 && phase < 4) { // Active caching
        // Col 1: DB Status
        draw_splash_text("DB_STAT  : BULK_INSERT" + dot_str, text_x, bot_row_start_y, 16, theme_color);
        
        char cache_buf[64];
        std::snprintf(cache_buf, sizeof(cache_buf), "CACHED   : %d", done);
        draw_splash_text(cache_buf, text_x, bot_row_start_y + (int)(bot_row_space * 1.2f), 18, theme_color);

        std::snprintf(cache_buf, sizeof(cache_buf), "I/O SPEED: %d ops/s", speed + (rand() % 35 + 10));
        draw_splash_text(cache_buf, text_x, bot_row_start_y + (int)(bot_row_space * 2.5f), 14, theme_dim);

        // Col 2: SQLite details
        draw_splash_text("VFS_MODE : WAL | NORMAL", col2_x, bot_row_start_y, 14, theme_dim);

        std::snprintf(cache_buf, sizeof(cache_buf), "PG_CACHE : %d KB", 4096 + (done % 1024));
        draw_splash_text(cache_buf, col2_x, bot_row_start_y + bot_row_space, 14, theme_dim);

        float mmap_mb = 0.0f;
        {
            std::lock_guard<std::mutex> lock(g_config_mtx);
            mmap_mb = (float)g_cfg.cache_mmap_size / (1024.0f * 1024.0f);
        }
        std::snprintf(cache_buf, sizeof(cache_buf), "MMAP_SIZE: %.0f MB", mmap_mb);
        draw_splash_text(cache_buf, col2_x, bot_row_start_y + bot_row_space * 2, 14, theme_dim);

        // Col 3: Metadata extractor details
        const char* ext_op = (done % 3 == 0) ? "ffprobe -v quiet" : "libexif_rotate";
        const char* parse_op = (done % 2 == 0) ? "JPEG_MARKER" : "MP4_MOOV_ATOM";
        draw_splash_text("EXTRACTOR: ACTIVE", col3_x, bot_row_start_y, 14, theme_dim);
        
        std::snprintf(cache_buf, sizeof(cache_buf), "EXEC     : %s", ext_op);
        draw_splash_text(cache_buf, col3_x, bot_row_start_y + bot_row_space, 14, theme_dim);

        std::snprintf(cache_buf, sizeof(cache_buf), "META_TAG : %s", parse_op);
        draw_splash_text(cache_buf, col3_x, bot_row_start_y + bot_row_space * 2, 14, theme_dim);

        // Col 4: Commit Q
        draw_splash_text("PIPE_STAT: BUFFER_FILL", col4_x, bot_row_start_y, 14, theme_dim);
        
        std::snprintf(cache_buf, sizeof(cache_buf), "TRANSACT : PENDING Q=%d", rand() % 15 + 1);
        draw_splash_text(cache_buf, col4_x, bot_row_start_y + bot_row_space, 14, theme_dim);

        std::snprintf(cache_buf, sizeof(cache_buf), "COMMIT_ID: %s", VERSION);
        draw_splash_text(cache_buf, col4_x, bot_row_start_y + bot_row_space * 2, 14, theme_dim);

        // Live Log Streams
        int log_y = bot_inner_y_offset + bot_inner_h + 8;
        std::vector<std::string> local_logs;
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            int start_log = log_buffer.size() > 2 ? log_buffer.size() - 2 : 0;
            for (size_t i = start_log; i < log_buffer.size(); i++) {
                local_logs.push_back(log_buffer[i]);
            }
        }
        for (const auto& line : local_logs) {
            draw_splash_text("> [DATA_STREAM] " + line, text_x, log_y, 14, theme_dim);
            log_y += 18;
        }

        // Segmented Progress Bar
        if (total > 0) {
            float pct = (float)done / (float)total;
            int bar_y = mid_y + 130;
            int bar_w = box_w - 120;

            SDL_SetRenderDrawColor(sdl_renderer, 0, 130, 0, 100);
            SDL_FRect bar_rect = {(float)text_x, (float)bar_y, (float)bar_w, 18.0f};
            SDL_RenderFillRect(sdl_renderer, &bar_rect);

            int fill_w = (int)((bar_w - 4) * pct);
            for (int bx = 2; bx < fill_w; bx += 10) {
                int w = std::min(8, fill_w - bx);
                SDL_SetRenderDrawColor(sdl_renderer, 0, 200, 0, 240);
                SDL_FRect chunk_rect = {(float)(text_x + bx), (float)(bar_y + 2), (float)w, 14.0f};
                SDL_RenderFillRect(sdl_renderer, &chunk_rect);
            }

            char pct_buf[32];
            std::snprintf(pct_buf, sizeof(pct_buf), "[%3d%%]", (int)(pct * 100));
            draw_splash_text(pct_buf, text_x + bar_w + 15, bar_y, 16, theme_color);

            if (!current_cache_file.empty()) {
                draw_splash_text(current_cache_file, text_x, bar_y + 28, 14, theme_dim);
            }
        }
    } else if (phase >= 4) { // Success
        draw_splash_text("DB_STAT  : COMMIT_SUCCESS " + cursor, text_x, bot_row_start_y, 16, theme_dim);

        int bar_y = mid_y + 130;
        int bar_w = box_w - 120;
        
        SDL_SetRenderDrawColor(sdl_renderer, 0, 130, 0, 220);
        SDL_FRect bar_rect = {(float)text_x, (float)bar_y, (float)bar_w, 18.0f};
        SDL_RenderFillRect(sdl_renderer, &bar_rect);

        SDL_SetRenderDrawColor(sdl_renderer, 0, 200, 0, 240);
        bar_rect = {(float)(text_x + 2), (float)(bar_y + 2), (float)(bar_w - 4), 14.0f};
        SDL_RenderFillRect(sdl_renderer, &bar_rect);

        draw_splash_text("[100%]", text_x + bar_w + 15, bar_y, 16, theme_dim);
    } else {
        draw_splash_text("DB_STAT  : AWAITING_I/O_PIPELINE... " + cursor, text_x, bot_row_start_y, 16, theme_dim);
        draw_splash_text("QUEUE    : BLOCKED", text_x, bot_row_start_y + 22, 14, theme_dim);

        int bar_y = mid_y + 130;
        int bar_w = box_w - 120;
        
        SDL_SetRenderDrawColor(sdl_renderer, 0, 130, 0, 76);
        SDL_FRect bar_rect = {(float)text_x, (float)bar_y, (float)bar_w, 18.0f};
        SDL_RenderFillRect(sdl_renderer, &bar_rect);
        draw_splash_text("[000%]", text_x + bar_w + 15, bar_y, 16, theme_dim);
    }

    // CRT screen curvature vignette at 4px steps (skip during scanning for performance)
    if (animated && phase > 2) {
        float screen_h_f = (float)sh;
        for (int y = 0; y < sh; y += 4) {
            float v = 2.0f * (float)y / screen_h_f - 1.0f;
            float edge = 1.0f - 0.3f * (v * v);
            if (edge < 1.0f) {
                uint8_t alpha = (uint8_t)(255.0f * (1.0f - edge));
                SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, alpha);
                SDL_FRect vignette_rect = {0.0f, (float)y, (float)sw, 4.0f};
                SDL_RenderFillRect(sdl_renderer, &vignette_rect);
            }
        }
    }

    SDL_RenderPresent(sdl_renderer);
    SDL_PumpEvents();
}

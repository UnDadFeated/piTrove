#include "renderer.h"
#include "util.h"
#include "config.h"
#include <SDL_image.h>
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
    // Cached value — /proc/stat delta requires 1s sleep, too slow for per-frame use
    static float cached_val = 0.0f;
    static time_t cached_ts = 0;
    time_t now = time(nullptr);
    if (now == cached_ts) return cached_val;

    std::ifstream stat1("/proc/stat");
    if (!stat1.is_open()) return 0.0f;

    char label[32];
    long long user1, nice1, system1, idle1, iowait1, irq1, softirq1;
    stat1 >> label >> user1 >> nice1 >> system1 >> idle1 >> iowait1 >> irq1 >> softirq1;
    stat1.close();

    usleep(500000); // 500ms instead of 1s

    std::ifstream stat2("/proc/stat");
    if (!stat2.is_open()) return 0.0f;

    long long user2, nice2, system2, idle2, iowait2, irq2, softirq2;
    stat2 >> label >> user2 >> nice2 >> system2 >> idle2 >> iowait2 >> irq2 >> softirq2;
    stat2.close();
    
    long long total1 = user1 + nice1 + system1 + idle1 + iowait1 + irq1 + softirq1;
    long long total2 = user2 + nice2 + system2 + idle2 + iowait2 + irq2 + softirq2;
    long long total_diff = total2 - total1;
    long long idle_diff = idle2 - idle1;
    
    if (total_diff == 0) { cached_val = 0.0f; cached_ts = now; return 0.0f; }
    cached_val = ((float)(total_diff - idle_diff) / (float)total_diff) * 100.0f;
    cached_ts = now;
    return cached_val;
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

    int bpp = surface->format->BytesPerPixel;
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
    SDL_GetRGBA(pixel, surface->format, &r, &g, &b, &a);
    return {r, g, b, a};
}

Renderer::Renderer() {}

Renderer::~Renderer() {
    cleanup();
}

bool Renderer::init(int w, int h, bool fullscreen) {
    g_logger.info("[TRACE] Renderer::init w=%d h=%d fullscreen=%d", w, h, fullscreen);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        g_logger.error("SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    g_logger.info("[TRACE] SDL_Init OK");

    if (IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG) == 0) {
        g_logger.error("IMG_Init failed: %s", IMG_GetError());
        SDL_Quit();
        return false;
    }
    g_logger.info("[TRACE] IMG_Init OK");

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
    if (fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver && std::strcmp(driver, "kmsdrm") == 0) {
        flags |= SDL_WINDOW_FULLSCREEN;
    }

    window = SDL_CreateWindow(
        APP_NAME " v" VERSION,
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
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

    SDL_GL_SetSwapInterval(1);
    g_logger.info("[TRACE] SDL_GL context created (implicit from CreateWindow)");

    SDL_GetWindowSize(window, &screen_w, &screen_h);
    g_logger.info("SDL Context & Window created successfully (%dx%d)", screen_w, screen_h);

    sdl_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
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
    cleanup_splash();

    if (sdl_renderer) {
        SDL_DestroyRenderer(sdl_renderer);
        sdl_renderer = nullptr;
    }
    if (gl_context) {
        SDL_GL_DeleteContext(gl_context);
        gl_context = nullptr;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    IMG_Quit();
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
    float scale = std::min((float)screen_w / img_w, (float)screen_h / img_h);
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

void Renderer::draw_matte_borders(const SDL_Rect& fit_rect) {
    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
    
    // Left border
    if (fit_rect.x > 0) {
        SDL_Rect left = {0, 0, fit_rect.x, screen_h};
        SDL_RenderFillRect(sdl_renderer, &left);
    }
    // Right border
    if (fit_rect.x + fit_rect.w < screen_w) {
        int right_w = screen_w - (fit_rect.x + fit_rect.w);
        SDL_Rect right = {fit_rect.x + fit_rect.w, 0, right_w, screen_h};
        SDL_RenderFillRect(sdl_renderer, &right);
    }
    // Top border
    if (fit_rect.y > 0) {
        SDL_Rect top = {0, 0, screen_w, fit_rect.y};
        SDL_RenderFillRect(sdl_renderer, &top);
    }
    // Bottom border
    if (fit_rect.y + fit_rect.h < screen_h) {
        int bottom_h = screen_h - (fit_rect.y + fit_rect.h);
        SDL_Rect bottom = {0, fit_rect.y + fit_rect.h, screen_w, bottom_h};
        SDL_RenderFillRect(sdl_renderer, &bottom);
    }
}

void Renderer::draw_solid_border(int width, uint8_t r, uint8_t g, uint8_t b) {
    if (width <= 0) return;
    
    SDL_SetRenderDrawColor(sdl_renderer, r, g, b, 255);
    
    // Top
    SDL_Rect top = {0, 0, screen_w, width};
    SDL_RenderFillRect(sdl_renderer, &top);
    
    // Bottom
    SDL_Rect bottom = {0, screen_h - width, screen_w, width};
    SDL_RenderFillRect(sdl_renderer, &bottom);
    
    // Left
    SDL_Rect left = {0, 0, width, screen_h};
    SDL_RenderFillRect(sdl_renderer, &left);
    
    // Right
    SDL_Rect right = {screen_w - width, 0, width, screen_h};
    SDL_RenderFillRect(sdl_renderer, &right);
}

float Renderer::read_sys_f(const char* path, float divisor) {
    std::ifstream file(path);
    if (!file.is_open()) return 0.0f;
    std::string line;
    if (std::getline(file, line)) {
        if (line.empty()) return 0.0f;
        try {
            size_t pos;
            long long v = std::stoll(line, &pos);
            if (pos == 0) return 0.0f;
            return (float)v / divisor;
        } catch (...) {
            return 0.0f;
        }
    }
    return 0.0f;
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
            SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
            if (surface) {
                memcpy(surface->pixels, pixels, (size_t)w * h * 4);
                splash_logo = SDL_CreateTextureFromSurface(sdl_renderer, surface);
                if (splash_logo) {
                    splash_logo_w = w;
                    splash_logo_h = h;
                    splash_logo_loaded = true;
                    g_logger.info("Splash image loaded: %s (%dx%d)", splash_path.c_str(), splash_logo_w, splash_logo_h);
                }
                SDL_FreeSurface(surface);
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
    // Find DejaVu font
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
    
    SDL_Rect rect;
    // Top
    rect = {x, y, w, 2};
    SDL_RenderFillRect(sdl_renderer, &rect);
    // Bottom
    rect = {x, y + h - 2, w, 2};
    SDL_RenderFillRect(sdl_renderer, &rect);
    // Left
    rect = {x, y, 2, h};
    SDL_RenderFillRect(sdl_renderer, &rect);
    // Right
    rect = {x + w - 2, y, 2, h};
    SDL_RenderFillRect(sdl_renderer, &rect);

    // Inner shadow border
    SDL_SetRenderDrawColor(sdl_renderer, 0, 130, 0, 220);
    int ix = x + 4, iy = y + 4, iw = w - 8, ih = h - 8;
    // Top
    rect = {ix, iy, iw, 1};
    SDL_RenderFillRect(sdl_renderer, &rect);
    // Bottom
    rect = {ix, iy + ih - 1, iw, 1};
    SDL_RenderFillRect(sdl_renderer, &rect);
    // Left
    rect = {ix, iy, 1, ih};
    SDL_RenderFillRect(sdl_renderer, &rect);
    // Right
    rect = {ix + iw - 1, iy, 1, ih};
    SDL_RenderFillRect(sdl_renderer, &rect);

    // Title box
    int title_size = 20;
    int title_w = 100; // estimated fallback w
    if (font_loaded && crt_font) {
        int title_h;
        font_renderer->measure(font_renderer->load_font(crt_font->path, title_size), APP_NAME, title_w, title_h);
    }
    
    SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, 255);
    rect = {x + 15, y - 2, title_w + 10, 6};
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
    int bar_w = (int)(w * 0.9f);
    int filled = (int)(bar_w * pct);
    if (filled > bar_w) filled = bar_w;

    SDL_SetRenderDrawColor(sdl_renderer, 0, 40, 0, 200);
    SDL_Rect rect = {x, y, w, h};
    SDL_RenderFillRect(sdl_renderer, &rect);

    SDL_SetRenderDrawColor(sdl_renderer, 0, 200, 0, 240);
    rect = {x, y, filled, h};
    SDL_RenderFillRect(sdl_renderer, &rect);
}

void Renderer::render_splash(int phase, int progress, int total, int done, const char* label, int dot_counter, const char* filename, bool animated) {
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
        
        SDL_Rect dst = {dest_x, dest_y, dest_w, dest_h};
        SDL_RenderCopy(sdl_renderer, splash_logo, nullptr, &dst);
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
        SDL_Rect rect = {0, sy - 1, sw, 2};
        SDL_RenderFillRect(sdl_renderer, &rect);
        
        // Sharp scanline
        SDL_SetRenderDrawColor(sdl_renderer, 0, 255, 0, 38);
        rect = {0, sy, sw, 1};
        SDL_RenderFillRect(sdl_renderer, &rect);
    }

    // Main Horizontal Center Box Divider
    SDL_SetRenderDrawColor(sdl_renderer, 0, 200, 0, 240);
    int mid_y = box_y + (box_h / 2);
    SDL_Rect rect = {box_x, mid_y - 1, box_w, 2};
    SDL_RenderFillRect(sdl_renderer, &rect);

    SDL_SetRenderDrawColor(sdl_renderer, 0, 130, 0, 220);
    rect = {box_x + 4, mid_y - 5, box_w - 8, 1};
    SDL_RenderFillRect(sdl_renderer, &rect);
    rect = {box_x + 4, mid_y + 4, box_w - 8, 1};
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
    rect = {box_x + 10, inner_y_offset, box_w - 20, 1};
    SDL_RenderFillRect(sdl_renderer, &rect);

    rect = {box_x + col_w, inner_y_offset, 1, top_inner_h};
    SDL_RenderFillRect(sdl_renderer, &rect);
    rect = {box_x + col_w * 2, inner_y_offset, 1, top_inner_h};
    SDL_RenderFillRect(sdl_renderer, &rect);
    rect = {box_x + col_w * 3, inner_y_offset, 1, top_inner_h};
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
        std::snprintf(fnd_buf, sizeof(fnd_buf), "FILES FND: %06d", progress);
        draw_splash_text(fnd_buf, text_x, (int)(row_start_y + row_space * 1.2f), 18, {0, 200, 0, 240});

        std::snprintf(fnd_buf, sizeof(fnd_buf), "I/O SPEED: %d nodes/s", speed);
        draw_splash_text(fnd_buf, text_x, (int)(row_start_y + row_space * 2.5f), 14, {0, 130, 0, 220});

        std::snprintf(fnd_buf, sizeof(fnd_buf), "LATENCY  : %d ms", latency);
        draw_splash_text(fnd_buf, text_x, (int)(row_start_y + row_space * 3.5f), 14, {0, 130, 0, 220});
    } else {
        draw_splash_text("SYS_STAT : SCAN_COMPLETE", text_x, row_start_y, 16, {0, 130, 0, 220});
        
        char fnd_buf[64];
        std::snprintf(fnd_buf, sizeof(fnd_buf), "FILES FND: %06d", progress);
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
    draw_splash_text("RENDERER : SDL2", col4_x, (int)(row_start_y + row_space), 14, {0, 130, 0, 220});

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
    rect = {box_x + 10, bot_inner_y_offset, box_w - 20, 1};
    SDL_RenderFillRect(sdl_renderer, &rect);

    int bot_inner_h = 75;
    rect = {box_x + col_w, bot_inner_y_offset, 1, bot_inner_h};
    SDL_RenderFillRect(sdl_renderer, &rect);
    rect = {box_x + col_w * 2, bot_inner_y_offset, 1, bot_inner_h};
    SDL_RenderFillRect(sdl_renderer, &rect);
    rect = {box_x + col_w * 3, bot_inner_y_offset, 1, bot_inner_h};
    SDL_RenderFillRect(sdl_renderer, &rect);

    int bot_row_start_y = bot_inner_y_offset + 10;
    int bot_row_space = 22;

    if (phase > 2 && phase < 4) { // Active caching
        // Col 1: DB Status
        draw_splash_text("DB_STAT  : BULK_INSERT" + dot_str, text_x, bot_row_start_y, 16, theme_color);
        
        char cache_buf[64];
        std::snprintf(cache_buf, sizeof(cache_buf), "CACHED   : %06d", done);
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
            SDL_Rect rect = {text_x, bar_y, bar_w, 18};
            SDL_RenderFillRect(sdl_renderer, &rect);

            int fill_w = (int)((bar_w - 4) * pct);
            for (int bx = 2; bx < fill_w; bx += 10) {
                int w = std::min(8, fill_w - bx);
                SDL_SetRenderDrawColor(sdl_renderer, 0, 200, 0, 240);
                rect = {text_x + bx, bar_y + 2, w, 14};
                SDL_RenderFillRect(sdl_renderer, &rect);
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
        SDL_Rect rect = {text_x, bar_y, bar_w, 18};
        SDL_RenderFillRect(sdl_renderer, &rect);

        SDL_SetRenderDrawColor(sdl_renderer, 0, 200, 0, 240);
        rect = {text_x + 2, bar_y + 2, bar_w - 4, 14};
        SDL_RenderFillRect(sdl_renderer, &rect);

        draw_splash_text("[100%]", text_x + bar_w + 15, bar_y, 16, theme_dim);
    } else {
        draw_splash_text("DB_STAT  : AWAITING_I/O_PIPELINE... " + cursor, text_x, bot_row_start_y, 16, theme_dim);
        draw_splash_text("QUEUE    : BLOCKED", text_x, bot_row_start_y + 22, 14, theme_dim);

        int bar_y = mid_y + 130;
        int bar_w = box_w - 120;
        
        SDL_SetRenderDrawColor(sdl_renderer, 0, 130, 0, 76);
        SDL_Rect rect = {text_x, bar_y, bar_w, 18};
        SDL_RenderFillRect(sdl_renderer, &rect);
        draw_splash_text("[000%]", text_x + bar_w + 15, bar_y, 16, theme_dim);
    }

    // CRT screen curvature vignette at 4px steps (skip during scanning for performance)
    if (animated && phase > 2) {
        float screen_h_f = (float)sh;
        for (int y = 0; y < sh; y += 4) {
            float edge = 1.0f - 0.3f * (1.0f - (2.0f * (float)y / screen_h_f - 1.0f) * (2.0f * (float)y / screen_h_f - 1.0f));
            if (edge < 0.7f) {
                uint8_t alpha = (uint8_t)(255.0f * (1.0f - edge));
                SDL_SetRenderDrawColor(sdl_renderer, 0, 0, 0, alpha);
                SDL_Rect rect = {0, y, sw, 4};
                SDL_RenderFillRect(sdl_renderer, &rect);
            }
        }
    }

    SDL_RenderPresent(sdl_renderer);
    SDL_PumpEvents();
}

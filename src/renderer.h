#ifndef PITROVE_RENDERER_H
#define PITROVE_RENDERER_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>
#include <vector>
#include <mutex>
#include "font_render.h"

// Color structure matching custom rendering needs
struct GpuColor {
    uint8_t r, g, b, a;
};

class Renderer {
public:
    SDL_Renderer* sdl_renderer{nullptr};
    SDL_Window* window{nullptr};
    int screen_w{1920};
    int screen_h{1080};

    // Splash Logo
    SDL_Texture* splash_logo{nullptr};
    int splash_logo_w{0};
    int splash_logo_h{0};
    bool splash_logo_loaded{false};

    // Splash font rendering
    FontRenderer* font_renderer{nullptr};
    FontHandle* crt_font{nullptr};
    bool font_loaded{false};

    // Telemetry and logs for retro terminal overlay
    float telemetry_temp{0.0f};
    float telemetry_freq{0.0f};
    time_t telemetry_ts{0};
    std::string current_cache_file;
    std::vector<std::string> log_buffer;
    std::mutex log_mutex;

public:
    Renderer();
    ~Renderer();

    bool init(int w, int h, bool fullscreen);
    void cleanup();

    // Screen info
    int get_width() const { return screen_w; }
    int get_height() const { return screen_h; }

    // Average color calculators (CPU-side on SDL_Surface)
    static GpuColor get_average_color(SDL_Surface* surface);
    static GpuColor get_edge_average_color(SDL_Surface* surface, int depth, int which);

    // Scaling helpers
    void calculate_fit_rect(int img_w, int img_h, SDL_Rect& out_rect);

    // Screen cleaning
    void clear(uint8_t r = 0, uint8_t g = 0, uint8_t b = 0, uint8_t a = 255);
    void present();
    void draw_matte_borders(const SDL_Rect& fit_rect);
    void draw_bias_lighting(const SDL_Rect& fit_rect, Uint8 avg_r, Uint8 avg_g, Uint8 avg_b,
        int bias_strength, float item_timer, float anim_speed, const std::string& style, int border_width);
    void draw_3d_border(const SDL_Rect& fit_rect, Uint8 avg_r, Uint8 avg_g, Uint8 avg_b, int border_width);
    void draw_solid_border(int width, uint8_t r = 0, uint8_t g = 0, uint8_t b = 0);

    // Splash Screen methods
    void load_splash(const std::string& path);
    void cleanup_splash();
    void render_splash(int phase, int progress, int total, int done, const char* label, int dot_counter, const char* filename = nullptr, bool animated = true);
    void add_splash_log(const std::string& line);
    void draw_splash_box(int x, int y, int w, int h);
    void draw_splash_text(const std::string& text, int x, int y, int size, GpuColor color);
    void draw_splash_progress_bar(int x, int y, int w, int h, float pct);

private:
    float read_sys_f(const char* path, float divisor);
    std::string folder_and_file(const std::string& path);
};

extern Renderer g_renderer;

#endif // PITROVE_RENDERER_H

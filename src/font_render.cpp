#include "font_render.h"
#include "renderer.h"
#include "util.h"
#include <stdexcept>

static std::atomic<int> g_font_renderer_instances{0};
static std::mutex g_font_init_mutex;

FontRenderer::FontRenderer(Renderer* renderer) : renderer(renderer) {
    g_logger.info("TRACE: FontRenderer::ctor TTF_WasInit=%d", TTF_WasInit());
    std::lock_guard<std::mutex> lk(g_font_init_mutex);
    if (g_font_renderer_instances.fetch_add(1) == 0) {
        if (TTF_WasInit() == 0) {
            if (!TTF_Init()) {
                g_logger.error("TTF_Init failed: %s", SDL_GetError());
                g_font_renderer_instances.fetch_sub(1);
                throw std::runtime_error("TTF Init failed");
            }
        }
    }
}

FontRenderer::~FontRenderer() {
    g_logger.info("TRACE: FontRenderer::dtor fonts=%d", (int)fonts.size());
    for (auto& [key, cached_text] : text_cache) {
        if (cached_text.texture) {
            SDL_DestroyTexture(cached_text.texture);
        }
    }
    text_cache.clear();

    for (auto& [key, handle] : fonts) {
        if (handle && handle->font) {
            TTF_CloseFont(handle->font);
        }
    }
    fonts.clear();

    std::lock_guard<std::mutex> lk(g_font_init_mutex);
    if (g_font_renderer_instances.fetch_sub(1) == 1) {
        if (TTF_WasInit() != 0) {
            TTF_Quit();
        }
    }
}

FontHandle& FontRenderer::load_font(const std::string& path, int size) {
    std::string key = path + ":" + std::to_string(size);
    auto it = fonts.find(key);
    if (it != fonts.end()) return *it->second;

    TTF_Font* font = TTF_OpenFont(path.c_str(), size);
    if (!font) {
        std::string exe_dir = get_exe_dir();
        std::vector<std::string> fallbacks = {
            exe_dir + "/src/fonts/DejaVuSansMono-Bold.ttf",
            exe_dir + "/fonts/DejaVuSansMono-Bold.ttf",
            "/app/src/fonts/DejaVuSansMono-Bold.ttf",
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf"
        };
        for (const auto& f : fallbacks) {
            if (f != path) {
                font = TTF_OpenFont(f.c_str(), size);
                if (font) {
                    g_logger.warn("Failed to load font '%s', fell back to '%s'", path.c_str(), f.c_str());
                    break;
                }
            }
        }
    }
    if (!font) {
        trigger_error(204); // E204: MISSING_FONT_FILES
        throw std::runtime_error("Font load failed");
    }

    auto handle = std::make_shared<FontHandle>();
    handle->font = font;
    handle->path = path;
    handle->size = size;
    fonts[key] = handle;

    if (is_error_active(204)) {
        clear_error(204);
    }

    g_logger.debug("Successfully loaded font: %s", key.c_str());
    return *handle;
}



void FontRenderer::draw_text(int x, int y, const FontHandle& font, const std::string& text,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!renderer || !renderer->sdl_renderer || text.empty() || !font.font) return;

    std::string key = text + "|" + font.path + "|" + std::to_string(font.size) + "|" + std::to_string((r << 24) | (g << 16) | (b << 8) | a);
    auto it = text_cache.find(key);
    if (it != text_cache.end()) {
        SDL_FRect dst = {(float)x, (float)y, (float)it->second.w, (float)it->second.h};
        SDL_RenderTexture(renderer->sdl_renderer, it->second.texture, nullptr, &dst);
        return;
    }

    if (text_cache.size() >= 64) {
        auto it = text_cache.begin();
        if (it->second.texture) SDL_DestroyTexture(it->second.texture);
        text_cache.erase(it);
    }

    SDL_Surface* main_surf = TTF_RenderText_Blended(font.font, text.c_str(), 0, {r, g, b, a});
    if (main_surf) {
        SDL_Texture* main_tex = SDL_CreateTextureFromSurface(renderer->sdl_renderer, main_surf);
        int tw = main_surf->w;
        int th = main_surf->h;
        SDL_DestroySurface(main_surf);
        if (main_tex) {
            text_cache[key] = {main_tex, tw, th};
            SDL_FRect dst = {(float)x, (float)y, (float)tw, (float)th};
            SDL_RenderTexture(renderer->sdl_renderer, main_tex, nullptr, &dst);
        }
    }
}

void FontRenderer::draw_text_glow(int x, int y, const FontHandle& font, const std::string& text,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                  uint8_t gr, uint8_t gg, uint8_t gb, uint8_t ga) {
    if (!renderer || !renderer->sdl_renderer || text.empty() || !font.font) return;

    // Draw outline by drawing it in 4 directions using our cached draw_text
    draw_text(x - 1, y, font, text, gr, gg, gb, ga);
    draw_text(x + 1, y, font, text, gr, gg, gb, ga);
    draw_text(x, y - 1, font, text, gr, gg, gb, ga);
    draw_text(x, y + 1, font, text, gr, gg, gb, ga);

    // Draw main text on top
    draw_text(x, y, font, text, r, g, b, a);
}



void FontRenderer::measure(const FontHandle& font, const std::string& text, int& w, int& h) {
    w = 0; h = 0;
    if (text.empty() || !font.font) return;
    TTF_GetStringSize(font.font, text.c_str(), 0, &w, &h);
}

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
    for (auto& pair : text_cache) {
        if (pair.second.texture) {
            SDL_DestroyTexture(pair.second.texture);
        }
    }
    text_cache.clear();

    for (auto& pair : fonts) {
        if (pair.second && pair.second->font) {
            TTF_CloseFont(pair.second->font);
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
        trigger_error(204); // E204: MISSING_FONT_FILES
        throw std::runtime_error("Font load failed");
    }

    auto handle = std::make_shared<FontHandle>();
    handle->font = font;
    handle->path = path;
    handle->size = size;
    fonts[key] = handle;

    if (g_active_error_code.load() == 204) {
        trigger_error(0);
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

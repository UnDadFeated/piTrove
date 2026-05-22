#include "font_render.h"
#include "renderer.h"
#include "util.h"
#include <stdexcept>

FontRenderer::FontRenderer(Renderer* renderer) : renderer(renderer) {
    g_logger.info("TRACE: FontRenderer::ctor TTF_WasInit=%d", TTF_WasInit());
    if (TTF_WasInit() == 0) {
        if (TTF_Init() != 0) {
            g_logger.error("TTF_Init failed: %s", TTF_GetError());
            throw std::runtime_error("TTF Init failed");
        }
    }
}

FontRenderer::~FontRenderer() {
    g_logger.info("TRACE: FontRenderer::dtor fonts=%d", (int)fonts.size());
    for (auto& pair : fonts) {
        if (pair.second && pair.second->font) {
            TTF_CloseFont(pair.second->font);
        }
    }
    fonts.clear();
    if (TTF_WasInit() != 0) {
        TTF_Quit();
    }
}

FontHandle& FontRenderer::load_font(const std::string& path, int size) {
    std::string key = path + ":" + std::to_string(size);
    auto it = fonts.find(key);
    if (it != fonts.end()) return *it->second;

    TTF_Font* font = TTF_OpenFont(path.c_str(), size);
    if (!font) {
        g_logger.error("TTF_OpenFont failed for '%s' (size=%d): %s", path.c_str(), size, TTF_GetError());
        throw std::runtime_error("Font load failed");
    }

    auto handle = std::make_shared<FontHandle>();
    handle->font = font;
    handle->path = path;
    handle->size = size;
    fonts[key] = handle;

    g_logger.debug("Successfully loaded font: %s", key.c_str());
    return *handle;
}

void FontRenderer::unload_font(const std::string& key) {
    auto it = fonts.find(key);
    if (it != fonts.end()) {
        if (it->second->font) {
            TTF_CloseFont(it->second->font);
        }
        fonts.erase(it);
        g_logger.debug("Unloaded font key: %s", key.c_str());
    }
}

void FontRenderer::draw_text(int x, int y, const FontHandle& font, const std::string& text,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!renderer || !renderer->sdl_renderer || text.empty() || !font.font) return;

    SDL_Surface* main_surf = TTF_RenderUTF8_Blended(font.font, text.c_str(), {r, g, b, a});
    if (main_surf) {
        SDL_Texture* main_tex = SDL_CreateTextureFromSurface(renderer->sdl_renderer, main_surf);
        int tw = main_surf->w;
        int th = main_surf->h;
        SDL_FreeSurface(main_surf);
        if (main_tex) {
            SDL_Rect dst = {x, y, tw, th};
            SDL_RenderCopy(renderer->sdl_renderer, main_tex, nullptr, &dst);
            SDL_DestroyTexture(main_tex);
        }
    }
}

void FontRenderer::draw_text_glow(int x, int y, const FontHandle& font, const std::string& text,
                                  uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                                  uint8_t gr, uint8_t gg, uint8_t gb, uint8_t ga) {
    if (!renderer || !renderer->sdl_renderer || text.empty() || !font.font) return;

    // Draw glow (shadow/outline) by drawing it offset in 4 directions
    SDL_Surface* glow_surf = TTF_RenderUTF8_Blended(font.font, text.c_str(), {gr, gg, gb, ga});
    if (glow_surf) {
        SDL_Texture* glow_tex = SDL_CreateTextureFromSurface(renderer->sdl_renderer, glow_surf);
        int tw = glow_surf->w;
        int th = glow_surf->h;
        SDL_FreeSurface(glow_surf);
        if (glow_tex) {
            SDL_Rect dst = {x - 1, y, tw, th};
            SDL_RenderCopy(renderer->sdl_renderer, glow_tex, nullptr, &dst);
            dst.x = x + 1;
            SDL_RenderCopy(renderer->sdl_renderer, glow_tex, nullptr, &dst);
            dst.x = x;
            dst.y = y - 1;
            SDL_RenderCopy(renderer->sdl_renderer, glow_tex, nullptr, &dst);
            dst.y = y + 1;
            SDL_RenderCopy(renderer->sdl_renderer, glow_tex, nullptr, &dst);
            SDL_DestroyTexture(glow_tex);
        }
    }

    // Draw main text on top
    draw_text(x, y, font, text, r, g, b, a);
}

void FontRenderer::draw_text_shaded(int x, int y, const FontHandle& font, const std::string& text,
                                    uint8_t text_r, uint8_t text_g, uint8_t text_b, uint8_t text_a,
                                    uint8_t shade_r, uint8_t shade_g, uint8_t shade_b, uint8_t shade_a) {
    if (!renderer || !renderer->sdl_renderer || text.empty() || !font.font) return;

    // Drop shadow: render 1 offset copy in shadow color
    SDL_Surface* shadow_surf = TTF_RenderUTF8_Blended(font.font, text.c_str(), {shade_r, shade_g, shade_b, shade_a});
    if (shadow_surf) {
        SDL_Texture* shadow_tex = SDL_CreateTextureFromSurface(renderer->sdl_renderer, shadow_surf);
        int tw = shadow_surf->w;
        int th = shadow_surf->h;
        SDL_FreeSurface(shadow_surf);
        if (shadow_tex) {
            SDL_Rect dst = {x + 2, y + 2, tw, th};
            SDL_RenderCopy(renderer->sdl_renderer, shadow_tex, nullptr, &dst);
            SDL_DestroyTexture(shadow_tex);
        }
    }

    // Draw main text
    draw_text(x, y, font, text, text_r, text_g, text_b, text_a);
}

void FontRenderer::measure(const FontHandle& font, const std::string& text, int& w, int& h) {
    w = 0; h = 0;
    if (text.empty() || !font.font) return;
    TTF_SizeUTF8(font.font, text.c_str(), &w, &h);
}

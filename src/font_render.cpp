#include "font_render.h"
#include "renderer.h"
#include "util.h"
#include <cmath>
#include <stdexcept>

#include <unordered_map>
#include <format>

FontHandle::~FontHandle() {
    if (font) TTF_CloseFont(font);
}

static std::atomic<int> g_font_renderer_instances{0};
static std::mutex g_font_init_mutex;

FontRenderer::FontRenderer(Renderer* renderer) : renderer(renderer) {
    g_logger.info("TRACE: FontRenderer::ctor TTF_WasInit={}", TTF_WasInit());
    std::lock_guard<std::mutex> lk(g_font_init_mutex);
    if (g_font_renderer_instances.fetch_add(1) == 0) {
        if (TTF_WasInit() == 0) {
            if (!TTF_Init()) {
                g_logger.error("TTF_Init failed: {}", SDL_GetError());
                g_font_renderer_instances.fetch_sub(1);
                throw std::runtime_error("TTF Init failed");
            }
        }
    }
}

FontRenderer::~FontRenderer() {
    g_logger.info("TRACE: FontRenderer::dtor fonts={}", std::ssize(fonts));
    for (auto& [key, cached_text] : text_cache) {
        if (cached_text.texture) {
            SDL_DestroyTexture(cached_text.texture);
        }
    }
    text_cache.clear();

    fonts.clear();

    std::lock_guard<std::mutex> lk(g_font_init_mutex);
    if (g_font_renderer_instances.fetch_sub(1) == 1) {
        if (TTF_WasInit() != 0) {
            TTF_Quit();
        }
    }
}

std::shared_ptr<FontHandle> FontRenderer::load_font(const std::string& path, int size) {
    std::string key = std::format("{}:{}", path, size);
    auto it = fonts.find(key);
    if (it != fonts.end()) {
        it->second->last_used = SDL_GetTicks();
        return it->second;
    }

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
                    g_logger.warn("Failed to load font '{}', fell back to '{}'", path.c_str(), f.c_str());
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
    handle->last_used = SDL_GetTicks();
    fonts[key] = handle;

    // Eviction drops only the cache's ownership. Active overlays and local drawing
    // code retain their own owners, so neither the handle nor TTF face can dangle.
    while (fonts.size() > 32) {
        auto oldest = fonts.end();
        for (auto it = fonts.begin(); it != fonts.end(); ++it) {
            if (it->first != key &&
                (oldest == fonts.end() || it->second->last_used < oldest->second->last_used)) {
                oldest = it;
            }
        }
        if (oldest == fonts.end()) break;
        fonts.erase(oldest);
    }

    if (is_error_active(204)) {
        clear_error(204);
    }

    g_logger.debug("Successfully loaded font: {}", key.c_str());
    return handle;
}



const FontRenderer::TextCacheEntry* FontRenderer::get_text_texture(
    const FontHandle& font, const std::string& text, SDL_Color color) {
    if (!renderer || !renderer->sdl_renderer || text.empty() || !font.font) return nullptr;

    const uint32_t rgba = (uint32_t(color.r) << 24) | (uint32_t(color.g) << 16) |
                          (uint32_t(color.b) << 8) | color.a;
    const std::string key = std::format("{}|{}|{}|{}", text, font.path, font.size, rgba);
    const auto cached = text_cache.find(key);
    if (cached != text_cache.end()) return &cached->second;

    const size_t max_text_cache = std::clamp(
        static_cast<size_t>(get_available_ram_bytes() / (32 * 1024 * 1024)), size_t(64), size_t(256));
    while (text_cache.size() >= max_text_cache) {
        auto entry = text_cache.begin();
        SDL_DestroyTexture(entry->second.texture);
        text_cache.erase(entry);
    }

    SDL_Surface* surface = TTF_RenderText_Blended(font.font, text.c_str(), 0, color);
    if (!surface) return nullptr;
    TextCacheEntry entry{SDL_CreateTextureFromSurface(renderer->sdl_renderer, surface),
                         surface->w, surface->h};
    SDL_DestroySurface(surface);
    if (!entry.texture) return nullptr;

    // Own the texture until it has been successfully inserted in the cache.
    std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture(entry.texture, SDL_DestroyTexture);
    const auto [inserted, success] = text_cache.emplace(key, entry);
    if (success) texture.release();
    return &inserted->second;
}

void FontRenderer::draw_text(int x, int y, const FontHandle& font, const std::string& text,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const auto* entry = get_text_texture(font, text, {r, g, b, a});
    if (!entry) return;
    const SDL_FRect dst{float(x), float(y), float(entry->w), float(entry->h)};
    SDL_RenderTexture(renderer->sdl_renderer, entry->texture, nullptr, &dst);
}

int FontRenderer::draw_text_fitted(const SDL_Rect& area, const FontHandle& font,
                                  const std::string& text, SDL_Color color) {
    if (area.w <= 0 || area.h <= 0) return 0;
    const auto* entry = get_text_texture(font, text, color);
    if (!entry || entry->w <= 0 || entry->h <= 0) return 0;

    const float scale = std::min(float(area.w) / entry->w, float(area.h) / entry->h);
    const float width = std::min(float(area.w), entry->w * scale);
    const float height = std::min(float(area.h), entry->h * scale);
    const SDL_FRect dst{area.x + (area.w - width) / 2.0f, float(area.y), width, height};
    if (!SDL_RenderTexture(renderer->sdl_renderer, entry->texture, nullptr, &dst)) return 0;
    return static_cast<int>(std::ceil(height));
}

std::string FontRenderer::ellipsize(const FontHandle& font, std::string_view text, int width) {
    if (width <= 0 || text.empty() || !font.font) return {};
    size_t length = 0;
    if (!TTF_MeasureString(font.font, text.data(), text.size(), width, nullptr, &length)) return {};
    if (length == text.size()) return std::string(text);

    constexpr std::string_view ellipsis = "…";
    int ellipsis_width = 0;
    if (!TTF_GetStringSize(font.font, ellipsis.data(), ellipsis.size(), &ellipsis_width, nullptr) ||
        ellipsis_width > width) return {};
    if (ellipsis_width == width) return std::string(ellipsis);
    if (!TTF_MeasureString(font.font, text.data(), text.size(), width - ellipsis_width,
                           nullptr, &length)) return {};

    // Check the complete string in case glyph spacing changes at the ellipsis.
    while (true) {
        std::string result(text.substr(0, length));
        result += ellipsis;
        int measured_width = 0;
        if (TTF_GetStringSize(font.font, result.c_str(), result.size(), &measured_width, nullptr) &&
            measured_width <= width) return result;
        if (length == 0) return {};
        do { --length; }
        while (length > 0 && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80);
    }
}

void FontRenderer::draw_text_uncached(int x, int y, const FontHandle& font, const std::string& text,
                             uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (!renderer || !renderer->sdl_renderer || text.empty() || !font.font) return;
    SDL_Surface* main_surf = TTF_RenderText_Blended(font.font, text.c_str(), 0, {r, g, b, a});
    if (main_surf) {
        SDL_Texture* main_tex = SDL_CreateTextureFromSurface(renderer->sdl_renderer, main_surf);
        int tw = main_surf->w;
        int th = main_surf->h;
        SDL_DestroySurface(main_surf);
        if (main_tex) {
            SDL_FRect dst = {(float)x, (float)y, (float)tw, (float)th};
            SDL_RenderTexture(renderer->sdl_renderer, main_tex, nullptr, &dst);
            SDL_DestroyTexture(main_tex);
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

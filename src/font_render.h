#ifndef PITROVE_FONT_RENDER_H
#define PITROVE_FONT_RENDER_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>
#include <string_view>
#include <map>
#include <unordered_map>
#include <memory>
#include <vector>

struct FontHandle {
    TTF_Font* font{nullptr};
    ~FontHandle();
    FontHandle() = default;
    FontHandle(const FontHandle&) = delete;
    FontHandle& operator=(const FontHandle&) = delete;
    std::string path;
    int size{0};
    uint64_t last_used{0};
};

class Renderer;

class FontRenderer {
private:
    Renderer* renderer;
    std::map<std::string, std::shared_ptr<FontHandle>> fonts;

    struct TextCacheEntry {
        SDL_Texture* texture = nullptr;
        int w = 0;
        int h = 0;
        uint64_t last_used = 0;

        size_t pixel_bytes() const { return static_cast<size_t>(w) * h * 4; }
    };
    std::unordered_map<std::string, TextCacheEntry> text_cache;
    size_t text_cache_bytes = 0;
    uint64_t text_sequence = 0;
    const TextCacheEntry* get_text_texture(const FontHandle& font, const std::string& text,
                                           SDL_Color color);

public:
    FontRenderer(Renderer* renderer);
    ~FontRenderer();



    // Keep the returned owner alive while using the font. Release it before this renderer.
    std::shared_ptr<FontHandle> load_font(const std::string& path, int size);

    // Text rendering
    void draw_text(int x, int y, const FontHandle& font, const std::string& text,
                   uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    // Fit to both dimensions, center horizontally, and return the height occupied.
    int draw_text_fitted(const SDL_Rect& area, const FontHandle& font,
                         const std::string& text, SDL_Color color);
    void draw_text_glow(int x, int y, const FontHandle& font, const std::string& text,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                        uint8_t gr, uint8_t gg, uint8_t gb, uint8_t ga);


    // Measure and shorten text for narrow columns without splitting UTF-8.
    std::string ellipsize(const FontHandle& font, std::string_view text, int width);
    void measure(const FontHandle& font, const std::string& text, int& w, int& h);
};

#endif // PITROVE_FONT_RENDER_H

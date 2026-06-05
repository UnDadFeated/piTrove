#ifndef PITROVE_FONT_RENDER_H
#define PITROVE_FONT_RENDER_H

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <string>
#include <map>
#include <unordered_map>
#include <memory>

struct FontHandle {
    TTF_Font* font{nullptr};
    std::string path;
    int size{0};
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
    };
    std::unordered_map<std::string, TextCacheEntry> text_cache;

public:
    FontRenderer(Renderer* renderer);
    ~FontRenderer();



    FontHandle& load_font(const std::string& path, int size);

    // Text rendering
    void draw_text(int x, int y, const FontHandle& font, const std::string& text,
                   uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void draw_text_glow(int x, int y, const FontHandle& font, const std::string& text,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                        uint8_t gr, uint8_t gg, uint8_t gb, uint8_t ga);


    // Measure
    void measure(const FontHandle& font, const std::string& text, int& w, int& h);
};

#endif // PITROVE_FONT_RENDER_H

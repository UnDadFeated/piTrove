#ifndef PITROVE_FONT_RENDER_H
#define PITROVE_FONT_RENDER_H

#include <SDL.h>
#include <SDL_ttf.h>
#include <string>
#include <map>
#include <memory>

struct FontHandle {
    TTF_Font* font{nullptr};
    std::string path;
    int size{0};
};

class FontRenderer {
private:
    SDL_Renderer* renderer;
    std::map<std::string, std::shared_ptr<FontHandle>> fonts;

public:
    FontRenderer(SDL_Renderer* renderer);
    ~FontRenderer();

    FontHandle& load_font(const std::string& path, int size);
    void unload_font(const std::string& key);

    // Text rendering
    void draw_text(int x, int y, const FontHandle& font, const std::string& text,
                   uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void draw_text_glow(int x, int y, const FontHandle& font, const std::string& text,
                        uint8_t r, uint8_t g, uint8_t b, uint8_t a,
                        uint8_t gr, uint8_t gg, uint8_t gb, uint8_t ga);
    void draw_text_shaded(int x, int y, const FontHandle& font, const std::string& text,
                          uint8_t text_r, uint8_t text_g, uint8_t text_b, uint8_t text_a,
                          uint8_t shade_r, uint8_t shade_g, uint8_t shade_b, uint8_t shade_a);

    // Measure
    void measure(const FontHandle& font, const std::string& text, int& w, int& h);
};

#endif // PITROVE_FONT_RENDER_H

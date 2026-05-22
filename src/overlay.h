#ifndef PITROVE_OVERLAY_H
#define PITROVE_OVERLAY_H

#include <SDL3/SDL.h>
#include <string>
#include "font_render.h"
#include "renderer.h"

class OverlayManager {
private:
    Renderer* renderer;
    FontRenderer* font_renderer;
    FontHandle* overlay_font;
    bool font_loaded{false};

public:
    OverlayManager(Renderer* renderer);
    ~OverlayManager();

    void init();
    void cleanup();

    // Render all configured overlays
    void draw_all(int current_idx, int total_items, const std::string& filename, double item_timer, bool is_video);

private:
    GpuColor get_color_from_str(const std::string& name);
    void draw_text_with_shadow(int x, int y, FontHandle& font, const std::string& text, GpuColor color);
};

#endif // PITROVE_OVERLAY_H

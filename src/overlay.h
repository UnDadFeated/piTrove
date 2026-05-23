#ifndef PITROVE_OVERLAY_H
#define PITROVE_OVERLAY_H

#include <SDL3/SDL.h>
#include <string>
#include "font_render.h"
#include "renderer.h"

struct MediaItem;
struct ImageData;

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
    void draw_all(int current_idx, int total_items, const MediaItem* item, const MediaItem* twin_item, double item_timer, bool is_video, int active_fps, const ImageData* current_data, const ImageData* current_twin_data);

private:
    GpuColor get_color_from_str(const std::string& name);
    void draw_text_with_shadow(int x, int y, FontHandle& font, const std::string& text, GpuColor color);
    void draw_text_with_outline(int x, int y, FontHandle& font, const std::string& text, GpuColor color, GpuColor outline_color);
    void get_adaptive_colors(const ImageData* img, int x, int y, GpuColor& text_color, GpuColor& shadow_color);
};

#endif // PITROVE_OVERLAY_H

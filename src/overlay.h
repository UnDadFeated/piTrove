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
    bool menu_active{false};
    int menu_selected{0};

    // Touchscreen / virtual keyboard state
    bool keyboard_active{false};
    std::string keyboard_input{""};
    int keyboard_target{0}; // 0 = delay, 1 = volume

    // Touch navigation overlay
    bool nav_overlay_active{false};
    Uint64 nav_overlay_show_time{0};

    // PIN keypad state
    bool pin_active{false};
    std::string pin_input{""};
    int pin_attempts{0};
    Uint64 pin_locked_until{0};
    bool pin_unlocked{false};

    OverlayManager(Renderer* renderer);
    ~OverlayManager();

    void init();
    void cleanup();
    FontRenderer* get_font_renderer() const { return font_renderer; }
    std::string get_font_path() const { return overlay_font ? overlay_font->path : "/app/src/fonts/DejaVuSansMono-Bold.ttf"; }

    // Render all configured overlays
    void draw_all(int current_idx, int total_items, const MediaItem* item, const MediaItem* twin_item, double item_timer, bool is_video, int active_fps, const ImageData* current_data, const ImageData* current_twin_data, const std::string& video_remaining = "");
    void draw_popup_menu();
    void draw_virtual_keyboard();
    void draw_nav_overlay();
    void draw_pin_keypad();
    bool check_pin(const std::string& input);

    // Handle touch/click events on menu or keyboard. Returns true if handled.
    bool handle_touch_click(float x, float y);

private:
    GpuColor get_color_from_str(const std::string& name);
    void draw_text_with_shadow(int x, int y, FontHandle& font, const std::string& text, GpuColor color);
    void draw_text_with_outline(int x, int y, FontHandle& font, const std::string& text, GpuColor color, GpuColor outline_color);
    void get_adaptive_colors(const ImageData* img, int x, int y, GpuColor& text_color, GpuColor& shadow_color, bool adaptive);
};

#endif // PITROVE_OVERLAY_H

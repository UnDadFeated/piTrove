#pragma once

#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>

inline constexpr int panel_heading_size = 22;
inline constexpr int panel_body_size = 14;
inline constexpr int panel_detail_size = 12;

inline int panel_px(int pixels, int screen_width) {
    return std::max(1, static_cast<int>(std::lround(pixels * screen_width / 1920.0)));
}

// Preserve any clip set by the caller. A disjoint clip draws nothing.
class InfopanelClip {
public:
    InfopanelClip(SDL_Renderer* renderer, const SDL_Rect& bounds)
        : renderer_(renderer), had_clip_(SDL_RenderClipEnabled(renderer)) {
        SDL_Rect clip = bounds;
        if (had_clip_) {
            if (!SDL_GetRenderClipRect(renderer_, &previous_) ||
                !SDL_GetRectIntersection(&bounds, &previous_, &clip)) {
                return;
            }
        }
        active_ = SDL_SetRenderClipRect(renderer_, &clip);
    }

    ~InfopanelClip() {
        if (active_) SDL_SetRenderClipRect(renderer_, had_clip_ ? &previous_ : nullptr);
    }

    InfopanelClip(const InfopanelClip&) = delete;
    InfopanelClip& operator=(const InfopanelClip&) = delete;
    explicit operator bool() const { return active_; }

private:
    SDL_Renderer* renderer_;
    SDL_Rect previous_{};
    bool had_clip_;
    bool active_ = false;
};

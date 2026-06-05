#include "transition.h"
#include "renderer.h"
#include "util.h"
#include "config.h"
#include <cmath>
#include <cstring>

TransitionEngine::TransitionEngine() {}

TransitionEngine::~TransitionEngine() {
    reset();
}

void TransitionEngine::start(TransitionEffect effect, float duration, int direction, float ken_burns_zoom) {
    g_logger.debug("TRACE: TransitionEngine::start effect=%d duration=%f", (int)effect, duration);
    if (duration <= 0.0f) {
        duration = 0.001f;
    }
    reset();
    config.effect = effect;
    config.duration = duration;
    config.progress = 0.0f;
    config.direction = direction;
    config.ken_burns_zoom = ken_burns_zoom;
    active = true;
    elapsed = 0.0f;
}

void TransitionEngine::update(float delta_time) {
    if (!active) return;
    
    elapsed += delta_time;
    config.progress = std::min(1.0f, elapsed / config.duration);
    
    if (config.progress >= 1.0f) {
        active = false;
    }
}

void TransitionEngine::render(SDL_Texture* prev_tex, SDL_Texture* next_tex, int screen_w, int screen_h) {
    if (!renderer) return;
    if (!active && config.progress < 1.0f) return;
    if (!prev_tex || !next_tex) return;
    
    switch (config.effect) {
        case TransitionEffect::Fade:
            render_fade(prev_tex, next_tex, screen_w, screen_h);
            break;
        case TransitionEffect::WipeLeft:
        case TransitionEffect::WipeRight:
        case TransitionEffect::WipeUp:
        case TransitionEffect::WipeDown:
            render_wipe(prev_tex, next_tex, config.direction, screen_w, screen_h);
            break;
        case TransitionEffect::KenBurns:
            render_ken_burns(prev_tex, next_tex, screen_w, screen_h, config.ken_burns_zoom);
            break;
        case TransitionEffect::Pixelate:
            render_pixelate(prev_tex, next_tex, screen_w, screen_h);
            break;
        case TransitionEffect::Dissolve:
            render_dissolve(prev_tex, next_tex, screen_w, screen_h);
            break;
        default:
            break;
    }
}

void TransitionEngine::reset() {
    active = false;
    config.progress = 0.0f;
    elapsed = 0.0f;
}



void TransitionEngine::render_fade(SDL_Texture* prev_tex, SDL_Texture* next_tex, int screen_w, int screen_h) {
    if (!renderer) return;
    
    SDL_Renderer* sdl = renderer->sdl_renderer;
    float p = config.progress;
    
    SDL_Rect prev_dst = {0, 0, screen_w, screen_h};
    SDL_Rect next_dst = {0, 0, screen_w, screen_h};
    
    // Draw previous texture fading out
    SDL_SetTextureAlphaMod(prev_tex, (Uint8)(255.0f * (1.0f - p)));
    SDL_FRect prev_dst_f = {(float)prev_dst.x, (float)prev_dst.y, (float)prev_dst.w, (float)prev_dst.h};
    SDL_RenderTexture(sdl, prev_tex, nullptr, &prev_dst_f);
    SDL_SetTextureAlphaMod(prev_tex, 255);
    
    // Draw next texture fading in
    SDL_SetTextureAlphaMod(next_tex, (Uint8)(255.0f * p));
    SDL_FRect next_dst_f = {(float)next_dst.x, (float)next_dst.y, (float)next_dst.w, (float)next_dst.h};
    SDL_RenderTexture(sdl, next_tex, nullptr, &next_dst_f);
    SDL_SetTextureAlphaMod(next_tex, 255);
}

void TransitionEngine::render_wipe(SDL_Texture* prev_tex, SDL_Texture* next_tex, int direction, int screen_w, int screen_h) {
    if (!renderer) return;
    
    SDL_Renderer* sdl = renderer->sdl_renderer;
    float p = config.progress;
    
    SDL_Rect prev_dst = {0, 0, screen_w, screen_h};
    SDL_Rect next_dst = {0, 0, screen_w, screen_h};
    
    // Draw previous texture fully
    SDL_FRect prev_dst_f = {(float)prev_dst.x, (float)prev_dst.y, (float)prev_dst.w, (float)prev_dst.h};
    SDL_RenderTexture(sdl, prev_tex, nullptr, &prev_dst_f);
    
    // Calculate wipe clip rect in screen coordinates
    SDL_Rect clip_rect = {0, 0, screen_w, screen_h};
    switch (direction) {
        case 0: // Left to Right
            clip_rect.w = (int)(screen_w * p);
            break;
        case 1: // Right to Left
            clip_rect.x = screen_w - (int)(screen_w * p);
            clip_rect.w = (int)(screen_w * p);
            break;
        case 2: // Top to Bottom
            clip_rect.h = (int)(screen_h * p);
            break;
        case 3: // Bottom to Top
            clip_rect.y = screen_h - (int)(screen_h * p);
            clip_rect.h = (int)(screen_h * p);
            break;
    }
    
    SDL_SetRenderClipRect(sdl, &clip_rect);
    SDL_FRect next_dst_f = {(float)next_dst.x, (float)next_dst.y, (float)next_dst.w, (float)next_dst.h};
    SDL_RenderTexture(sdl, next_tex, nullptr, &next_dst_f);
    SDL_SetRenderClipRect(sdl, nullptr);
}

void TransitionEngine::render_ken_burns(SDL_Texture* prev_tex, SDL_Texture* next_tex, int screen_w, int screen_h, float zoom) {
    if (!renderer || !next_tex) return;
    
    SDL_Renderer* sdl = renderer->sdl_renderer;
    float p = config.progress;
    
    // 1. Render previous texture fading out
    if (prev_tex) {
        SDL_SetTextureAlphaMod(prev_tex, (Uint8)(255.0f * (1.0f - p)));
        SDL_FRect prev_dst_f = {0.0f, 0.0f, (float)screen_w, (float)screen_h};
        SDL_RenderTexture(sdl, prev_tex, nullptr, &prev_dst_f);
        SDL_SetTextureAlphaMod(prev_tex, 255);
    }
    
    // 2. Render next texture fading in, smoothly zooming out and centering
    float scale = 1.0f + zoom * (1.0f - p);
    
    int dst_w = (int)(screen_w * scale);
    int dst_h = (int)(screen_h * scale);
    
    float pan_x = (1.0f - p) * screen_w * 0.03f;
    float pan_y = (1.0f - p) * screen_h * 0.02f;
    
    int dst_x = (screen_w - dst_w) / 2 + (int)pan_x;
    int dst_y = (screen_h - dst_h) / 2 + (int)pan_y;
    
    SDL_FRect dst_f = {(float)dst_x, (float)dst_y, (float)dst_w, (float)dst_h};
    
    SDL_SetTextureAlphaMod(next_tex, (Uint8)(255.0f * p));
    SDL_RenderTexture(sdl, next_tex, nullptr, &dst_f);
    SDL_SetTextureAlphaMod(next_tex, 255);
}

void TransitionEngine::render_pixelate(SDL_Texture* prev_tex, SDL_Texture* next_tex, int screen_w, int screen_h) {
    if (!renderer) return;

    SDL_Renderer* sdl = renderer->sdl_renderer;
    float p = config.progress;

    SDL_Rect prev_dst = {0, 0, screen_w, screen_h};
    SDL_Rect next_dst = {0, 0, screen_w, screen_h};

    // Render prev normally
    SDL_FRect prev_dst_f = {(float)prev_dst.x, (float)prev_dst.y, (float)prev_dst.w, (float)prev_dst.h};
    SDL_RenderTexture(sdl, prev_tex, nullptr, &prev_dst_f);

    // Pixelate overlay: draw prev texture again in large blocks for blocky effect
    int block = std::max(4, (int)(64.0f * p));
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    float block_alpha = 0.6f * p;
    SDL_SetRenderDrawColor(sdl, 0, 0, 0, (Uint8)(block_alpha * 255.0f));
    std::vector<SDL_FRect> rects;
    rects.reserve((size_t)(screen_w / block + 1) * (screen_h / block + 1));
    for (int by = 0; by < screen_h; by += block) {
        for (int bx = 0; bx < screen_w; bx += block) {
            rects.push_back({(float)bx, (float)by, (float)block - 1.0f, (float)block - 1.0f});
        }
    }
    if (!rects.empty()) {
        SDL_RenderFillRects(sdl, rects.data(), (int)rects.size());
    }

    // Crossfade next
    SDL_SetTextureAlphaMod(next_tex, (Uint8)(255.0f * p));
    SDL_FRect next_dst_f = {(float)next_dst.x, (float)next_dst.y, (float)next_dst.w, (float)next_dst.h};
    SDL_RenderTexture(sdl, next_tex, nullptr, &next_dst_f);
    SDL_SetTextureAlphaMod(next_tex, 255);
    SDL_SetRenderDrawColor(sdl, 255, 255, 255, 255);
}

void TransitionEngine::render_dissolve(SDL_Texture* prev_tex, SDL_Texture* next_tex, int screen_w, int screen_h) {
    if (!renderer) return;

    SDL_Renderer* sdl = renderer->sdl_renderer;
    float p = config.progress;

    SDL_Rect prev_dst = {0, 0, screen_w, screen_h};
    SDL_Rect next_dst = {0, 0, screen_w, screen_h};

    // Render prev
    SDL_FRect prev_dst_f = {(float)prev_dst.x, (float)prev_dst.y, (float)prev_dst.w, (float)prev_dst.h};
    SDL_RenderTexture(sdl, prev_tex, nullptr, &prev_dst_f);

    // Dissolve: draw random white rectangles for scatter effect
    unsigned int seed = (unsigned int)(p * 60000.0f) ^ 0x5DEECE66u;
    int count = (int)(1200.0f * p * p);
    SDL_SetRenderDrawBlendMode(sdl, SDL_BLENDMODE_BLEND);
    float alpha = 40.0f + 60.0f * p;
    SDL_SetRenderDrawColor(sdl, 255, 255, 255, (Uint8)alpha);

    // Pre-allocated buffer to avoid per-frame heap churn
    static thread_local std::vector<SDL_FRect> rects;
    rects.clear();
    rects.reserve(count);
    for (int i = 0; i < count; i++) {
        seed = seed * 1103515245u + 12345u;
        int px = (seed >> 16) % screen_w;
        seed = seed * 1103515245u + 12345u;
        int py = (seed >> 16) % screen_h;
        int sz = 2 + ((seed >> 8) % 8);
        rects.push_back({(float)px, (float)py, (float)sz, (float)sz});
    }
    if (!rects.empty()) {
        SDL_RenderFillRects(sdl, rects.data(), (int)rects.size());
    }

    // Crossfade next
    SDL_SetTextureAlphaMod(next_tex, (Uint8)(255.0f * p));
    SDL_FRect next_dst_f = {(float)next_dst.x, (float)next_dst.y, (float)next_dst.w, (float)next_dst.h};
    SDL_RenderTexture(sdl, next_tex, nullptr, &next_dst_f);
    SDL_SetTextureAlphaMod(next_tex, 255);
    SDL_SetRenderDrawColor(sdl, 255, 255, 255, 255);
}

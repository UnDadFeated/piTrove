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
    g_logger.info("TRACE: TransitionEngine::start effect=%d duration=%f", (int)effect, duration);
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
    g_logger.info("TRACE: TransitionEngine::update active=%d delta=%f elapsed=%f progress=%f", active, delta_time, elapsed, config.progress);
    if (!active) return;
    
    elapsed += delta_time;
    config.progress = std::min(1.0f, elapsed / config.duration);
    
    if (config.progress >= 1.0f) {
        active = false;
    }
}

void TransitionEngine::render(SDL_Texture* prev_tex, SDL_Texture* next_tex, int screen_w, int screen_h) {
    g_logger.info("TRACE: TransitionEngine::render active=%d effect=%d", active, (int)config.effect);
    if (!renderer || !active) return;
    
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
            render_ken_burns(next_tex, screen_w, screen_h, config.ken_burns_zoom);
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
    g_logger.info("TRACE: TransitionEngine::reset");
    active = false;
    config.progress = 0.0f;
    elapsed = 0.0f;
}

void TransitionEngine::render_fade(SDL_Texture* prev_tex, SDL_Texture* next_tex, int screen_w, int screen_h) {
    if (!renderer) return;
    
    SDL_Renderer* sdl = renderer->sdl_renderer;
    float p = config.progress;
    
    // Draw previous texture fading out
    SDL_SetTextureAlphaMod(prev_tex, (Uint8)(255.0f * (1.0f - p)));
    SDL_Rect dst = {0, 0, screen_w, screen_h};
    SDL_RenderCopy(sdl, prev_tex, nullptr, &dst);
    SDL_SetTextureAlphaMod(prev_tex, 255);
    
    // Draw next texture fading in
    SDL_SetTextureAlphaMod(next_tex, (Uint8)(255.0f * p));
    SDL_RenderCopy(sdl, next_tex, nullptr, &dst);
    SDL_SetTextureAlphaMod(next_tex, 255);
}

void TransitionEngine::render_wipe(SDL_Texture* prev_tex, SDL_Texture* next_tex, int direction, int screen_w, int screen_h) {
    if (!renderer) return;
    
    SDL_Renderer* sdl = renderer->sdl_renderer;
    float p = config.progress;
    
    SDL_Rect dst_full = {0, 0, screen_w, screen_h};
    
    // Draw previous texture fully
    SDL_RenderCopy(sdl, prev_tex, nullptr, &dst_full);
    
    // Calculate wipe rect based on direction
    SDL_Rect src_rect = {0, 0, screen_w, screen_h};
    SDL_Rect dst_rect = {0, 0, screen_w, screen_h};
    
    switch (direction) {
        case 0: // Left to Right
            src_rect.w = (int)(screen_w * p);
            dst_rect.w = (int)(screen_w * p);
            break;
        case 1: // Right to Left
            src_rect.x = screen_w - (int)(screen_w * p);
            src_rect.w = (int)(screen_w * p);
            dst_rect.x = screen_w - (int)(screen_w * p);
            dst_rect.w = (int)(screen_w * p);
            break;
        case 2: // Top to Bottom
            src_rect.h = (int)(screen_h * p);
            dst_rect.h = (int)(screen_h * p);
            break;
        case 3: // Bottom to Top
            src_rect.y = screen_h - (int)(screen_h * p);
            src_rect.h = (int)(screen_h * p);
            dst_rect.y = screen_h - (int)(screen_h * p);
            dst_rect.h = (int)(screen_h * p);
            break;
    }
    
    SDL_RenderCopy(sdl, next_tex, &src_rect, &dst_rect);
}

void TransitionEngine::render_ken_burns(SDL_Texture* tex, int screen_w, int screen_h, float zoom) {
    if (!renderer || !tex) return;
    
    SDL_Renderer* sdl = renderer->sdl_renderer;
    float p = config.progress;
    
    // Simple Ken Burns: scale from 1.0 to 1.0+zoom over the transition
    float scale = 1.0f + zoom * p;
    
    // Get texture dimensions
    int tex_w = 0, tex_h = 0;
    SDL_QueryTexture(tex, nullptr, nullptr, &tex_w, &tex_h);
    
    // Calculate fit rect with scale
    float fit_w = screen_w / scale;
    float fit_h = screen_h / scale;
    
    // Animate position slightly (pan)
    float pan_x = sinf(p * 3.14159f) * screen_w * 0.05f;
    float pan_y = cosf(p * 3.14159f) * screen_h * 0.03f;
    
    int dst_w = (int)(fit_w);
    int dst_h = (int)(fit_h);
    int dst_x = (screen_w - dst_w) / 2 + (int)pan_x;
    int dst_y = (screen_h - dst_h) / 2 + (int)pan_y;
    
    SDL_Rect dst = {dst_x, dst_y, dst_w, dst_h};
    
    // Draw scaled texture centered
    SDL_RenderCopy(sdl, tex, nullptr, &dst);
}

void TransitionEngine::render_pixelate(SDL_Texture* prev_tex, SDL_Texture* next_tex, int screen_w, int screen_h) {
    if (!renderer) return;
    
    SDL_Renderer* sdl = renderer->sdl_renderer;
    float p = config.progress;
    
    SDL_Rect dst_full = {0, 0, screen_w, screen_h};
    
    // Draw previous texture fully as background
    SDL_RenderCopy(sdl, prev_tex, nullptr, &dst_full);
    
    // For pixelate effect, draw next texture with a reveal mask
    // Since SDL_Renderer doesn't support shaders, we skip the actual pixelation
    // and use a simple crossfade as fallback
    SDL_SetTextureAlphaMod(next_tex, (Uint8)(255.0f * p));
    SDL_RenderCopy(sdl, next_tex, nullptr, &dst_full);
    SDL_SetTextureAlphaMod(next_tex, 255);
}

void TransitionEngine::render_dissolve(SDL_Texture* prev_tex, SDL_Texture* next_tex, int screen_w, int screen_h) {
    if (!renderer) return;
    
    SDL_Renderer* sdl = renderer->sdl_renderer;
    float p = config.progress;
    
    SDL_Rect dst_full = {0, 0, screen_w, screen_h};
    
    // Draw previous texture fully
    SDL_RenderCopy(sdl, prev_tex, nullptr, &dst_full);
    
    // Draw next texture with alpha based on progress
    SDL_SetTextureAlphaMod(next_tex, (Uint8)(255.0f * p));
    SDL_RenderCopy(sdl, next_tex, nullptr, &dst_full);
    SDL_SetTextureAlphaMod(next_tex, 255);
}

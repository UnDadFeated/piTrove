#ifndef PITROVE_TRANSITION_H
#define PITROVE_TRANSITION_H

#include <SDL3/SDL.h>
#include <string>
#include <functional>

enum class TransitionEffect {
    None,
    Fade,
    WipeLeft,
    WipeRight,
    WipeUp,
    WipeDown,
    KenBurns,
    Pixelate,
    Dissolve
};

struct TransitionConfig {
    TransitionEffect effect{TransitionEffect::None};
    float duration{1.0f};
    float progress{0.0f};
    int direction{0};  // 0=L->R, 1=R->L, 2=T->B, 3=B->T
    float ken_burns_zoom{0.1f};
    float ken_burns_pan{0.0f};
};

class Renderer;

class TransitionEngine {
private:
    TransitionConfig config;
    bool active{false};
    float elapsed{0.0f};
    Renderer* renderer{nullptr};

public:
    TransitionEngine();
    ~TransitionEngine();

    void set_renderer(Renderer* r) { renderer = r; }
    
    // Start a transition
    void start(TransitionEffect effect, float duration = 1.0f, int direction = 0, float ken_burns_zoom = 0.1f);
    
    // Update transition progress
    void update(float delta_time);
    
    // Check if transition is active
    bool is_active() const { return active; }
    
    // Get current progress (0.0 to 1.0)
    float get_progress() const { return config.progress; }
    
    // Get current effect
    TransitionEffect get_effect() const { return config.effect; }
    
    // Render transition frame
    void render(SDL_Texture* prev_tex, SDL_Texture* next_tex, int screen_w, int screen_h);
    
    // Reset
    void reset();

private:
    void render_fade(SDL_Texture* prev_tex, SDL_Texture* next_tex, int screen_w, int screen_h);
    void render_wipe(SDL_Texture* prev_tex, SDL_Texture* next_tex, int direction, int screen_w, int screen_h);
    void render_ken_burns(SDL_Texture* prev_tex, SDL_Texture* next_tex, int screen_w, int screen_h, float zoom);
    void render_pixelate(SDL_Texture* prev_tex, SDL_Texture* next_tex, int screen_w, int screen_h);
    void render_dissolve(SDL_Texture* prev_tex, SDL_Texture* next_tex, int screen_w, int screen_h);
};

#endif // PITROVE_TRANSITION_H

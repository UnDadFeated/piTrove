#pragma once
#include <SDL3/SDL.h>
#include <string>
class FontRenderer;
void render_chores_panel(SDL_Renderer*, FontRenderer*, const std::string& font_path, const SDL_Rect& bounds, int screen_w);

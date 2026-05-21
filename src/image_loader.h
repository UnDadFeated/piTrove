#ifndef PITROVE_IMAGE_LOADER_H
#define PITROVE_IMAGE_LOADER_H

#include <SDL.h>
#include <SDL_image.h>
#include <memory>
#include <string>

struct ImageData {
    SDL_Surface* surface{nullptr};   // CPU memory (for processing/preloading)
    SDL_Texture* texture{nullptr};   // GPU VRAM (SDL_Texture handle)
    int width{0};
    int height{0};
    bool valid{false};
    int exif_rotation{1};
    uint8_t avg_r{0};
    uint8_t avg_g{0};
    uint8_t avg_b{0};
};

class Renderer;

class ImageLoader {
public:
    static std::shared_ptr<ImageData> load(const std::string& path);
    static void load_texture(ImageData* data, SDL_Renderer* renderer);
    static void unload_texture(ImageData* data);
    static void unload(ImageData* data);

    // EXIF orientation helpers (CPU-side)
    static int read_exif_rotation(const char* path);
    static SDL_Surface* apply_exif_rotation(SDL_Surface* surface, int exif);

    // Format-specific loaders (resilient fallbacks)
    static SDL_Surface* load_heic_surface(const char* path);
    static SDL_Surface* load_webp_surface(const char* path);
    static SDL_Surface* load_tiff_surface(const char* path);
    static SDL_Surface* load_jpeg_surface(const char* path);
    static SDL_Surface* load_png_surface(const char* path);

    // CPU-side image manipulation
    static SDL_Surface* flip_horizontal(SDL_Surface* src);
    static SDL_Surface* flip_vertical(SDL_Surface* src);
    static SDL_Surface* rotate_90_cw(SDL_Surface* src);
    static SDL_Surface* rotate_90_ccw(SDL_Surface* src);
    static SDL_Surface* rotate_180(SDL_Surface* src);
};

#endif // PITROVE_IMAGE_LOADER_H

#ifndef PITROVE_IMAGE_LOADER_H
#define PITROVE_IMAGE_LOADER_H

#include <memory>
#include <string>
#include <cstdint>
#include <SDL.h>

enum class ImageFormat { Unknown, RGBA32, RGB24, BGRA32, BGR24 };

// Raw decoded image data (no SDL dependency) — safe to use in worker threads
struct RawImage {
    uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int channels = 4;  // 3 = RGB, 4 = RGBA
    ImageFormat format = ImageFormat::Unknown;
    bool valid = false;

    ~RawImage() {
        free(pixels);
        pixels = nullptr;
    }
};

struct ImageData {
    int width = 0;
    int height = 0;
    int exif_rotation = 1;
    bool valid = false;
    SDL_Surface* surface = nullptr;
    SDL_Texture* texture = nullptr;
    // Average color for bias lighting
    uint8_t avg_r = 0, avg_g = 0, avg_b = 0;
};

class ImageLoader {
public:
    // Decode image to raw buffer (no SDL calls) — safe for worker threads
    static RawImage load_raw(const std::string& path);

    // Load image and create SDL_Surface + texture (must be called on main thread)
    static std::shared_ptr<ImageData> load(const std::string& path);

    // Upload surface to VRAM texture (must be called on main thread)
    static void load_texture(ImageData* data, SDL_Renderer* renderer);

    // Unload VRAM texture
    static void unload_texture(ImageData* data);

    // Unload everything
    static void unload(ImageData* data);

    // Apply EXIF rotation to an SDL surface (must be called on main thread)
    static SDL_Surface* apply_exif_rotation(SDL_Surface* surface, int exif);

    // Read EXIF orientation from file
    static int read_exif_rotation(const char* path);
};

#endif // PITROVE_IMAGE_LOADER_H

#ifndef PITROVE_IMAGE_LOADER_H
#define PITROVE_IMAGE_LOADER_H

#include <memory>
#include <string>
#include <cstdint>
#include <cstring>
#include <vector>
#include <SDL3/SDL.h>

enum class ImageFormat { Unknown, RGBA32, RGB24, BGRA32, BGR24 };

// Raw decoded image data (no SDL dependency) — safe to use in worker threads
struct RawImage {
    uint8_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    int channels = 4;  // 3 = RGB, 4 = RGBA
    ImageFormat format = ImageFormat::Unknown;
    bool valid = false;

    RawImage() = default;

    // Move constructor
    RawImage(RawImage&& other) noexcept
        : pixels(other.pixels),
          width(other.width),
          height(other.height),
          channels(other.channels),
          format(other.format),
          valid(other.valid) {
        other.pixels = nullptr;
        other.valid = false;
    }

    // Move assignment
    RawImage& operator=(RawImage&& other) noexcept {
        if (this != &other) {
            free(pixels);
            pixels = other.pixels;
            width = other.width;
            height = other.height;
            channels = other.channels;
            format = other.format;
            valid = other.valid;
            other.pixels = nullptr;
            other.valid = false;
        }
        return *this;
    }

    // Delete copy operations to prevent shallow copy crashes
    RawImage(const RawImage&) = delete;
    RawImage& operator=(const RawImage&) = delete;

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
    // Per-edge colors (averaged, fallback)
    uint8_t edge_r[4] = {0, 0, 0, 0};
    uint8_t edge_g[4] = {0, 0, 0, 0};
    uint8_t edge_b[4] = {0, 0, 0, 0};
    // Per-pixel edge strips for bias glow: packed RGB, one entry per screen pixel along the edge
    std::vector<uint8_t> edge_top_rgb, edge_bot_rgb;  // top/bottom: width pixels
    std::vector<uint8_t> edge_lft_rgb, edge_rgt_rgb;  // left/right: height pixels

    ImageData() = default;

    // Destructor to safely release SDL surface and texture
    ~ImageData() {
        if (texture) {
            SDL_DestroyTexture(texture);
            texture = nullptr;
        }
        if (surface) {
            SDL_DestroySurface(surface);
            surface = nullptr;
        }
    }

    // Delete copy operations to prevent shallow copy double-free crashes
    ImageData(const ImageData&) = delete;
    ImageData& operator=(const ImageData&) = delete;

    // Support move operations
    ImageData(ImageData&& other) noexcept
        : width(other.width),
          height(other.height),
          exif_rotation(other.exif_rotation),
          valid(other.valid),
          surface(other.surface),
          texture(other.texture),
          avg_r(other.avg_r),
          avg_g(other.avg_g),
          avg_b(other.avg_b) {
        memcpy(edge_r, other.edge_r, sizeof(edge_r));
        memcpy(edge_g, other.edge_g, sizeof(edge_g));
        memcpy(edge_b, other.edge_b, sizeof(edge_b));
        edge_top_rgb = std::move(other.edge_top_rgb);
        edge_bot_rgb = std::move(other.edge_bot_rgb);
        edge_lft_rgb = std::move(other.edge_lft_rgb);
        edge_rgt_rgb = std::move(other.edge_rgt_rgb);
        other.surface = nullptr;
        other.texture = nullptr;
        other.valid = false;
    }

    ImageData& operator=(ImageData&& other) noexcept {
        if (this != &other) {
            if (texture) SDL_DestroyTexture(texture);
            if (surface) SDL_DestroySurface(surface);
            width = other.width;
            height = other.height;
            exif_rotation = other.exif_rotation;
            valid = other.valid;
            surface = other.surface;
            texture = other.texture;
            avg_r = other.avg_r;
            avg_g = other.avg_g;
            avg_b = other.avg_b;
            memcpy(edge_r, other.edge_r, sizeof(edge_r));
            memcpy(edge_g, other.edge_g, sizeof(edge_g));
            memcpy(edge_b, other.edge_b, sizeof(edge_b));
            edge_top_rgb = std::move(other.edge_top_rgb);
            edge_bot_rgb = std::move(other.edge_bot_rgb);
            edge_lft_rgb = std::move(other.edge_lft_rgb);
            edge_rgt_rgb = std::move(other.edge_rgt_rgb);
            other.surface = nullptr;
            other.texture = nullptr;
            other.valid = false;
        }
        return *this;
    }
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

    // Check if file has camera EXIF tags (not screenshot/screen grab)
    static bool has_camera_exif(const char* path);
};

#endif // PITROVE_IMAGE_LOADER_H

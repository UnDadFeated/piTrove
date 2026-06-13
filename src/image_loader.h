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
    bool transient_error = false;
    SDL_Surface* surface = nullptr;
    SDL_Texture* texture = nullptr;
    SDL_Texture* blur_texture = nullptr;
    // Average color for bias lighting
    uint8_t avg_r = 0, avg_g = 0, avg_b = 0;
    // Per-edge colors (averaged, fallback)
    uint8_t edge_r[4] = {0, 0, 0, 0};
    uint8_t edge_g[4] = {0, 0, 0, 0};
    uint8_t edge_b[4] = {0, 0, 0, 0};
    // Per-pixel edge strips for bias glow: packed RGB, one entry per screen pixel along the edge
    std::vector<uint8_t> edge_top_rgb, edge_bot_rgb;  // top/bottom: width pixels
    std::vector<uint8_t> edge_lft_rgb, edge_rgt_rgb;  // left/right: height pixels
    // Blurred background for fullscreen backdrop (computed in worker thread)
    RawImage blur_raw;
    // Color-matched matte color (center-average, computed in worker thread)
    uint8_t matte_r = 0, matte_g = 0, matte_b = 0;
 
    ImageData() = default;
 
    // Destructor to safely release SDL surface and textures
    ~ImageData() {
        if (texture) {
            SDL_DestroyTexture(texture);
            texture = nullptr;
        }
        if (blur_texture) {
            SDL_DestroyTexture(blur_texture);
            blur_texture = nullptr;
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
          transient_error(other.transient_error),
          surface(other.surface),
          texture(other.texture),
          blur_texture(other.blur_texture),
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
        blur_raw = std::move(other.blur_raw);
        matte_r = other.matte_r;
        matte_g = other.matte_g;
        matte_b = other.matte_b;
        other.surface = nullptr;
        other.texture = nullptr;
        other.blur_texture = nullptr;
        other.valid = false;
    }
 
    ImageData& operator=(ImageData&& other) noexcept {
        if (this != &other) {
            if (texture) SDL_DestroyTexture(texture);
            if (blur_texture) SDL_DestroyTexture(blur_texture);
            if (surface) SDL_DestroySurface(surface);
            width = other.width;
            height = other.height;
            exif_rotation = other.exif_rotation;
            valid = other.valid;
            transient_error = other.transient_error;
            surface = other.surface;
            texture = other.texture;
            blur_texture = other.blur_texture;
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
            blur_raw = std::move(other.blur_raw);
            matte_r = other.matte_r;
            matte_g = other.matte_g;
            matte_b = other.matte_b;
            other.surface = nullptr;
            other.texture = nullptr;
            other.blur_texture = nullptr;
            other.valid = false;
        }
        return *this;
    }
};

class ImageLoader {
public:


    // Load image and create SDL_Surface + texture (must be called on main thread)
    static std::shared_ptr<ImageData> load(const std::string& path);

    // Upload surface to VRAM texture (must be called on main thread)
    static void load_texture(ImageData* data, SDL_Renderer* renderer);





    // Apply EXIF rotation to an SDL surface (must be called on main thread)
    static SDL_Surface* apply_exif_rotation(SDL_Surface* surface, int exif);



    // Read EXIF orientation from a memory buffer
    static int read_exif_rotation_from_memory(const uint8_t* buffer, unsigned int size);

    // Helper to read a file fully into a memory buffer with checks
    static std::vector<uint8_t> read_file_to_buffer(const std::string& path);

    // Check if file has camera EXIF tags (not screenshot/screen grab)
    static bool has_camera_exif(const char* path);

    // Extract capture / creation date from EXIF tags
    static int64_t get_creation_time(std::string_view path);
};

#endif // PITROVE_IMAGE_LOADER_H

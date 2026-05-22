#define _GNU_SOURCE
#include "image_loader.h"
#include "renderer.h"
#include "util.h"
#include <SDL_image.h>
#include <stb_image.h>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <memory>

// EXIF tags and library
#include <libexif/exif-data.h>
#include <libexif/exif-entry.h>
#include <libexif/exif-content.h>
#include <libexif/exif-tag.h>
#include <libexif/exif-utils.h>

// ============================================================
// Public API
// ============================================================

RawImage ImageLoader::load_raw(const std::string& path) {
    RawImage raw;
    raw.valid = false;

    // Use stb_image (like legacy raylib LoadImage)
    int w = 0, h = 0, ch = 0;
    uint8_t* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4); // force RGBA
    if (!pixels || w <= 0 || h <= 0) return raw;

    raw.width = w;
    raw.height = h;
    raw.channels = 4;
    raw.format = ImageFormat::RGBA32;
    raw.valid = true;

    size_t buf_size = (size_t)w * h * 4;
    raw.pixels = (uint8_t*)malloc(buf_size);
    if (!raw.pixels) {
        stbi_image_free(pixels);
        raw.valid = false;
        return raw;
    }
    memcpy(raw.pixels, pixels, buf_size);
    stbi_image_free(pixels);
    return raw;
}

std::shared_ptr<ImageData> ImageLoader::load(const std::string& path) {
    auto result = std::make_shared<ImageData>();
    result->valid = false;

    int w = 0, h = 0, ch = 0;
    uint8_t* pixels = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!pixels || w <= 0 || h <= 0) {
        g_logger.error("stbi_load failed for: %s %s", path.c_str(), stbi_failure_reason());
        return result;
    }

    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        stbi_image_free(pixels);
        g_logger.error("SDL_CreateRGBSurfaceWithFormat failed for: %s", path.c_str());
        return result;
    }

    size_t psize = (size_t)w * h * 4;
    memcpy(surf->pixels, pixels, psize);
    stbi_image_free(pixels);

    int exif = read_exif_rotation(path.c_str());
    if (exif >= 2 && exif <= 8) {
        SDL_Surface* rotated = apply_exif_rotation(surf, exif);
        if (rotated) {
            SDL_FreeSurface(surf);
            surf = rotated;
        }
    }

    result->surface = surf;
    result->width = surf->w;
    result->height = surf->h;
    result->exif_rotation = exif;
    result->valid = true;

    GpuColor avg = Renderer::get_average_color(surf);
    result->avg_r = avg.r;
    result->avg_g = avg.g;
    result->avg_b = avg.b;

    return result;
}

void ImageLoader::load_texture(ImageData* data, SDL_Renderer* renderer) {
    g_logger.info("[TRACE] ImageLoader::load_texture data=%p surface=%p renderer=%p", (void*)data, data ? (void*)data->surface : nullptr, (void*)renderer);
    if (!data || !data->surface || !renderer || data->texture) return;

    const int MAX_DIM = 1920;
    if (data->width > MAX_DIM || data->height > MAX_DIM) {
        float scale = (float)MAX_DIM / (float)std::max(data->width, data->height);
        int nw = std::max(1, (int)(data->width * scale));
        int nh = std::max(1, (int)(data->height * scale));

        SDL_Surface* scaled = SDL_CreateRGBSurfaceWithFormat(0, nw, nh, 32, data->surface->format->format);
        if (scaled) {
            SDL_Rect src_rect = {0, 0, data->width, data->height};
            SDL_Rect dst_rect = {0, 0, nw, nh};
            SDL_BlitScaled(data->surface, &src_rect, scaled, &dst_rect);
            SDL_FreeSurface(data->surface);
            data->surface = scaled;
            data->width = nw;
            data->height = nh;
        }
    }

    data->texture = SDL_CreateTextureFromSurface(renderer, data->surface);
    if (!data->texture) {
        g_logger.error("Failed to create texture from surface: %s", SDL_GetError());
    } else {
        SDL_SetTextureScaleMode(data->texture, SDL_ScaleModeLinear);
    }

    SDL_FreeSurface(data->surface);
    data->surface = nullptr;
}

void ImageLoader::unload_texture(ImageData* data) {
    if (!data) return;
    if (data->texture) {
        SDL_DestroyTexture(data->texture);
        data->texture = nullptr;
    }
}

void ImageLoader::unload(ImageData* data) {
    if (!data) return;
    unload_texture(data);
    if (data->surface) {
        SDL_FreeSurface(data->surface);
        data->surface = nullptr;
    }
    data->valid = false;
}

int ImageLoader::read_exif_rotation(const char* path) {
    int rotation = 1;
    ExifData* ed = exif_data_new_from_file(path);
    if (!ed) return rotation;

    ExifEntry* entry = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
    if (!entry || entry->size < 2 || entry->format != EXIF_FORMAT_SHORT) {
        exif_data_unref(ed);
        return rotation;
    }

    unsigned short val = exif_get_short(entry->data, exif_data_get_byte_order(ed));
    if (val >= 1 && val <= 8) rotation = val;
    exif_data_unref(ed);
    return rotation;
}

SDL_Surface* ImageLoader::apply_exif_rotation(SDL_Surface* surface, int exif) {
    if (!surface || exif < 2 || exif > 8) return surface;
    SDL_Surface* rotated = nullptr;
    switch (exif) {
        case 2: {
            SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, surface->w, surface->h, 32, surface->format->format);
            if (dst) {
                int bpp = surface->format->BytesPerPixel;
                uint8_t* src_px = (uint8_t*)surface->pixels;
                uint8_t* dst_px = (uint8_t*)dst->pixels;
                for (int y = 0; y < surface->h; y++) {
                    uint8_t* src_row = src_px + y * surface->pitch;
                    uint8_t* dst_row = dst_px + y * dst->pitch;
                    for (int x = 0; x < surface->w; x++) {
                        memcpy(dst_row + x * bpp, src_row + (surface->w - 1 - x) * bpp, bpp);
                    }
                }
                rotated = dst;
            }
            break;
        }
        case 3: {
            SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, surface->w, surface->h, 32, surface->format->format);
            if (dst) {
                int bpp = surface->format->BytesPerPixel;
                uint8_t* src_px = (uint8_t*)surface->pixels;
                uint8_t* dst_px = (uint8_t*)dst->pixels;
                for (int y = 0; y < surface->h; y++) {
                    uint8_t* src_row = src_px + y * surface->pitch;
                    uint8_t* dst_row = dst_px + (surface->h - 1 - y) * dst->pitch;
                    for (int x = 0; x < surface->w; x++) {
                        memcpy(dst_row + (surface->w - 1 - x) * bpp, src_row + x * bpp, bpp);
                    }
                }
                rotated = dst;
            }
            break;
        }
        case 4: {
            SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, surface->w, surface->h, 32, surface->format->format);
            if (dst) {
                uint8_t* src_px = (uint8_t*)surface->pixels;
                uint8_t* dst_px = (uint8_t*)dst->pixels;
                for (int y = 0; y < surface->h; y++) {
                    memcpy(dst_px + y * dst->pitch, src_px + (surface->h - 1 - y) * surface->pitch, surface->pitch);
                }
                rotated = dst;
            }
            break;
        }
        case 5: {
            SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, surface->h, surface->w, 32, surface->format->format);
            if (dst) {
                int bpp = surface->format->BytesPerPixel;
                uint8_t* src_px = (uint8_t*)surface->pixels;
                uint8_t* dst_px = (uint8_t*)dst->pixels;
                for (int y = 0; y < surface->h; y++) {
                    uint8_t* src_row = src_px + y * surface->pitch;
                    for (int x = 0; x < surface->w; x++) {
                        uint8_t* dst_pixel = dst_px + x * dst->pitch + y * bpp;
                        memcpy(dst_pixel, src_row + x * bpp, bpp);
                    }
                }
                rotated = dst;
            }
            break;
        }
        case 6: {
            SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, surface->h, surface->w, 32, surface->format->format);
            if (dst) {
                int bpp = surface->format->BytesPerPixel;
                uint8_t* src_px = (uint8_t*)surface->pixels;
                uint8_t* dst_px = (uint8_t*)dst->pixels;
                for (int y = 0; y < surface->h; y++) {
                    uint8_t* src_row = src_px + y * surface->pitch;
                    for (int x = 0; x < surface->w; x++) {
                        uint8_t* dst_pixel = dst_px + x * dst->pitch + (surface->h - 1 - y) * bpp;
                        memcpy(dst_pixel, src_row + x * bpp, bpp);
                    }
                }
                rotated = dst;
            }
            break;
        }
        case 7: {
            SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, surface->h, surface->w, 32, surface->format->format);
            if (dst) {
                int bpp = surface->format->BytesPerPixel;
                uint8_t* src_px = (uint8_t*)surface->pixels;
                uint8_t* dst_px = (uint8_t*)dst->pixels;
                for (int y = 0; y < surface->h; y++) {
                    uint8_t* src_row = src_px + y * surface->pitch;
                    for (int x = 0; x < surface->w; x++) {
                        uint8_t* dst_pixel = dst_px + (surface->w - 1 - x) * dst->pitch + (surface->h - 1 - y) * bpp;
                        memcpy(dst_pixel, src_row + x * bpp, bpp);
                    }
                }
                rotated = dst;
            }
            break;
        }
        case 8: {
            SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, surface->h, surface->w, 32, surface->format->format);
            if (dst) {
                int bpp = surface->format->BytesPerPixel;
                uint8_t* src_px = (uint8_t*)surface->pixels;
                uint8_t* dst_px = (uint8_t*)dst->pixels;
                for (int y = 0; y < surface->h; y++) {
                    uint8_t* src_row = src_px + y * surface->pitch;
                    for (int x = 0; x < surface->w; x++) {
                        uint8_t* dst_pixel = dst_px + (surface->w - 1 - x) * dst->pitch + y * bpp;
                        memcpy(dst_pixel, src_row + x * bpp, bpp);
                    }
                }
                rotated = dst;
            }
            break;
        }
    }
    return rotated;
}

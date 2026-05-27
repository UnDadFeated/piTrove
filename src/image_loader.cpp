#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "image_loader.h"
#include "renderer.h"
#include "util.h"
#include <SDL3_image/SDL_image.h>
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

    SDL_Surface* surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        stbi_image_free(pixels);
        g_logger.error("SDL_CreateSurface failed for: %s", path.c_str());
        return result;
    }

    uint8_t* dst = (uint8_t*)surf->pixels;
    const uint8_t* src = pixels;
    for (int y = 0; y < h; y++) {
        memcpy(dst + y * surf->pitch, src + y * w * 4, w * 4);
    }
    stbi_image_free(pixels);

    int exif = read_exif_rotation(path.c_str());
    if (exif >= 2 && exif <= 8) {
        SDL_Surface* rotated = apply_exif_rotation(surf, exif);
        if (rotated) {
            SDL_DestroySurface(surf);
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

    // Compute matte color from center of image
    {
        int cx = result->width / 4, cy = result->height / 4;
        int cw = result->width / 2, ch = result->height / 2;
        if (cx >= 0 && cy >= 0 && cw > 0 && ch > 0) {
            uint8_t* px = (uint8_t*)surf->pixels;
            int bpp = SDL_BYTESPERPIXEL(surf->format);
            long sr = 0, sg = 0, sb = 0, n = 0;
            for (int y = cy; y < cy + ch && y < surf->h; y++) {
                for (int x = cx; x < cx + cw && x < surf->w; x++) {
                    const uint8_t* p = px + y * surf->pitch + x * bpp;
                    sr += p[0]; sg += p[1]; sb += p[2]; n++;
                }
            }
            if (n > 0) {
                result->matte_r = (uint8_t)(sr / n);
                result->matte_g = (uint8_t)(sg / n);
                result->matte_b = (uint8_t)(sb / n);
            }
        }
    }

    // Sample 4 edge colors for bias gradient (0=top, 1=bottom, 2=left, 3=right)
    for (int e = 0; e < 4; e++) {
        GpuColor ec = Renderer::get_edge_average_color(surf, 8, e);
        result->edge_r[e] = ec.r;
        result->edge_g[e] = ec.g;
        result->edge_b[e] = ec.b;
    }

    // Per-pixel edge strips: average 3px deep per position
    {
        uint8_t* px = (uint8_t*)surf->pixels;
        int bpp = SDL_BYTESPERPIXEL(surf->format);
        int sw = surf->w, sh = surf->h;
        int pitch = surf->pitch;

        // Top edge: one RGB per column
        result->edge_top_rgb.resize(sw * 3);
        for (int x = 0; x < sw; x++) {
            int ar = 0, ag = 0, ab = 0, ac = 0;
            int samples = sh < 3 ? sh : 3;
            for (int d = 0; d < samples; d++) {
                const uint8_t* dp = px + x * bpp + d * pitch;
                ar += dp[0]; ag += dp[1]; ab += dp[2]; ac++;
            }
            result->edge_top_rgb[x * 3 + 0] = (uint8_t)(ar / ac);
            result->edge_top_rgb[x * 3 + 1] = (uint8_t)(ag / ac);
            result->edge_top_rgb[x * 3 + 2] = (uint8_t)(ab / ac);
        }

        // Bottom edge
        result->edge_bot_rgb.resize(sw * 3);
        for (int x = 0; x < sw; x++) {
            int ar = 0, ag = 0, ab = 0, ac = 0;
            for (int d = -1; d <= 1; d++) {
                int ry = sh - 1 + d;
                if (ry >= 0 && ry < sh) {
                    const uint8_t* dp = px + x * bpp + ry * pitch;
                    ar += dp[0]; ag += dp[1]; ab += dp[2]; ac++;
                }
            }
            result->edge_bot_rgb[x * 3 + 0] = (uint8_t)(ar / ac);
            result->edge_bot_rgb[x * 3 + 1] = (uint8_t)(ag / ac);
            result->edge_bot_rgb[x * 3 + 2] = (uint8_t)(ab / ac);
        }

        // Left edge: one RGB per row
        result->edge_lft_rgb.resize(sh * 3);
        for (int y = 0; y < sh; y++) {
            const uint8_t* p = px + y * pitch;
            int ar = 0, ag = 0, ab = 0, ac = 0;
            for (int w = 0; w < 3 && w < sw; w++) {
                ar += p[w * bpp + 0]; ag += p[w * bpp + 1]; ab += p[w * bpp + 2]; ac++;
            }
            result->edge_lft_rgb[y * 3 + 0] = (uint8_t)(ar / ac);
            result->edge_lft_rgb[y * 3 + 1] = (uint8_t)(ag / ac);
            result->edge_lft_rgb[y * 3 + 2] = (uint8_t)(ab / ac);
        }

        // Right edge
        result->edge_rgt_rgb.resize(sh * 3);
        for (int y = 0; y < sh; y++) {
            const uint8_t* p = px + y * pitch;
            int ar = 0, ag = 0, ab = 0, ac = 0;
            for (int w = 0; w < 3; w++) {
                int wc = sw - 1 - w;
                if (wc >= 0) { ar += p[wc * bpp + 0]; ag += p[wc * bpp + 1]; ab += p[wc * bpp + 2]; ac++; }
            }
            result->edge_rgt_rgb[y * 3 + 0] = (uint8_t)(ar / ac);
            result->edge_rgt_rgb[y * 3 + 1] = (uint8_t)(ag / ac);
            result->edge_rgt_rgb[y * 3 + 2] = (uint8_t)(ab / ac);
        }
    }

    return result;
}

void ImageLoader::load_texture(ImageData* data, SDL_Renderer* renderer) {
    g_logger.info("[TRACE] ImageLoader::load_texture data=%p surface=%p renderer=%p", (void*)data, data ? (void*)data->surface : nullptr, (void*)renderer);
    if (!data || !data->surface || !renderer || data->texture || data->width <= 0 || data->height <= 0) return;

    const int MAX_DIM = 1920;
    if (data->width > MAX_DIM || data->height > MAX_DIM) {
        float scale = (float)MAX_DIM / (float)std::max(data->width, data->height);
        int nw = std::max(1, (int)(data->width * scale));
        int nh = std::max(1, (int)(data->height * scale));

        SDL_Surface* scaled = SDL_CreateSurface(nw, nh, data->surface->format);
        if (scaled) {
            SDL_Rect src_rect = {0, 0, data->width, data->height};
            SDL_Rect dst_rect = {0, 0, nw, nh};
            SDL_BlitSurfaceScaled(data->surface, &src_rect, scaled, &dst_rect, SDL_SCALEMODE_LINEAR);
            SDL_DestroySurface(data->surface);
            data->surface = scaled;
            data->width = nw;
            data->height = nh;
        }
    }

    data->texture = SDL_CreateTextureFromSurface(renderer, data->surface);
    if (!data->texture) {
        g_logger.error("Failed to create texture from surface: %s", SDL_GetError());
    } else {
        SDL_SetTextureScaleMode(data->texture, SDL_SCALEMODE_LINEAR);
    }

    if (data->blur_raw.valid && data->blur_raw.pixels) {
        SDL_Surface* bsurf = SDL_CreateSurface(data->blur_raw.width, data->blur_raw.height, SDL_PIXELFORMAT_RGBA32);
        if (bsurf) {
            memcpy(bsurf->pixels, data->blur_raw.pixels, (size_t)data->blur_raw.width * data->blur_raw.height * 4);
            data->blur_texture = SDL_CreateTextureFromSurface(renderer, bsurf);
            if (!data->blur_texture) {
                g_logger.error("Failed to create blur texture from surface: %s", SDL_GetError());
            } else {
                SDL_SetTextureScaleMode(data->blur_texture, SDL_SCALEMODE_LINEAR);
            }
            SDL_DestroySurface(bsurf);
        }
        free(data->blur_raw.pixels);
        data->blur_raw.pixels = nullptr;
        data->blur_raw.valid = false;
    }

    SDL_DestroySurface(data->surface);
    data->surface = nullptr;
}

void ImageLoader::unload_texture(ImageData* data) {
    if (!data) return;
    if (data->texture) {
        SDL_DestroyTexture(data->texture);
        data->texture = nullptr;
    }
    if (data->blur_texture) {
        SDL_DestroyTexture(data->blur_texture);
        data->blur_texture = nullptr;
    }
}

void ImageLoader::unload(ImageData* data) {
    if (!data) return;
    unload_texture(data);
    if (data->surface) {
        SDL_DestroySurface(data->surface);
        data->surface = nullptr;
    }
    data->valid = false;
}

int ImageLoader::read_exif_rotation(const char* path) {
    int rotation = 1;
    ExifData* ed = exif_data_new_from_file(path);
    if (!ed) return rotation;

    // Check IFD_0 first, then IFD_EXIF (some cameras write orientation there)
    ExifEntry* entry = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
    if (!entry || entry->size < 2 || entry->format != EXIF_FORMAT_SHORT) {
        entry = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_ORIENTATION);
    }
    if (!entry || entry->size < 2 || entry->format != EXIF_FORMAT_SHORT) {
        exif_data_unref(ed);
        return rotation;
    }

    unsigned short val = exif_get_short(entry->data, exif_data_get_byte_order(ed));
    if (val >= 1 && val <= 8) rotation = val;
    exif_data_unref(ed);
    return rotation;
}

bool ImageLoader::has_camera_exif(const char* path) {
    ExifData* ed = exif_data_new_from_file(path);
    if (!ed) return false;

    // Optical EXIF tags — only real cameras have these, screenshots never do
    ExifEntry* exposure = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_EXPOSURE_TIME);
    ExifEntry* fnumber = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_FNUMBER);
    ExifEntry* iso = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_ISO_SPEED_RATINGS);
    ExifEntry* focal = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_FOCAL_LENGTH);
    ExifEntry* datetime = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_ORIGINAL);

    // Require at least 2 optical tags. Phone screenshots have Make+Model but no optical data.
    int count = (exposure ? 1 : 0) + (fnumber ? 1 : 0) + (iso ? 1 : 0) + (focal ? 1 : 0) + (datetime ? 1 : 0);
    exif_data_unref(ed);
    return count >= 2;
}

SDL_Surface* ImageLoader::apply_exif_rotation(SDL_Surface* surface, int exif) {
    if (!surface || exif < 2 || exif > 8) return surface;
    SDL_Surface* rotated = nullptr;
    switch (exif) {
        case 2: {
            SDL_Surface* dst = SDL_CreateSurface(surface->w, surface->h, surface->format);
            if (dst) {
                int bpp = SDL_BYTESPERPIXEL(surface->format);
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
            SDL_Surface* dst = SDL_CreateSurface(surface->w, surface->h, surface->format);
            if (dst) {
                int bpp = SDL_BYTESPERPIXEL(surface->format);
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
            SDL_Surface* dst = SDL_CreateSurface(surface->w, surface->h, surface->format);
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
            SDL_Surface* dst = SDL_CreateSurface(surface->h, surface->w, surface->format);
            if (dst) {
                int bpp = SDL_BYTESPERPIXEL(surface->format);
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
            SDL_Surface* dst = SDL_CreateSurface(surface->h, surface->w, surface->format);
            if (dst) {
                int bpp = SDL_BYTESPERPIXEL(surface->format);
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
            SDL_Surface* dst = SDL_CreateSurface(surface->h, surface->w, surface->format);
            if (dst) {
                int bpp = SDL_BYTESPERPIXEL(surface->format);
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
            SDL_Surface* dst = SDL_CreateSurface(surface->h, surface->w, surface->format);
            if (dst) {
                int bpp = SDL_BYTESPERPIXEL(surface->format);
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

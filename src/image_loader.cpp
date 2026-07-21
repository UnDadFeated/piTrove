#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "image_loader.h"
#include <span>
extern "C" {
#include <jpeglib.h>
#include <setjmp.h>
}


struct JpegErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf jump;
};

static void jpeg_error_exit(j_common_ptr cinfo) {
    auto* err = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    longjmp(err->jump, 1);
}

SDL_Surface* load_jpeg_scaled(const std::string& path, int max_w, int max_h) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    [[unlikely]]
    if (!fp) return nullptr;

    jpeg_decompress_struct cinfo;
    JpegErrorMgr jerr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    if (setjmp(jerr.jump)) {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(fp);
        return nullptr;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_stdio_src(&cinfo, fp);
    jpeg_read_header(&cinfo, TRUE);

    int den = 1;
    while (den < 8 &&
           (static_cast<int>(cinfo.image_width) / den > max_w ||
            static_cast<int>(cinfo.image_height) / den > max_h)) {
        ++den;
    }

    cinfo.scale_num = 1;
    cinfo.scale_denom = den;
    cinfo.out_color_space = JCS_RGB;
    jpeg_calc_output_dimensions(&cinfo);

    SDL_Surface* surface = SDL_CreateSurface(
        static_cast<int>(cinfo.output_width),
        static_cast<int>(cinfo.output_height),
        SDL_PIXELFORMAT_RGBA32);

    if (!surface) {
        jpeg_destroy_decompress(&cinfo);
        std::fclose(fp);
        return nullptr;
    }

    jpeg_start_decompress(&cinfo);
    std::vector<JSAMPLE> row_buffer(cinfo.output_width * cinfo.output_components);

    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = row_buffer.data();
        jpeg_read_scanlines(&cinfo, &row, 1);
        Uint8* dst = static_cast<Uint8*>(surface->pixels) +
                     (cinfo.output_scanline - 1) * surface->pitch;
        for (unsigned int x = 0; x < cinfo.output_width; ++x) {
            dst[x * 4 + 0] = row_buffer[x * 3 + 0];
            dst[x * 4 + 1] = row_buffer[x * 3 + 1];
            dst[x * 4 + 2] = row_buffer[x * 3 + 2];
            dst[x * 4 + 3] = 255;
        }
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    std::fclose(fp);
    return surface;
}

#include "config.h"
#include "renderer.h"
#include "util.h"
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

std::vector<uint8_t> ImageLoader::read_file_to_buffer(const std::string& path) {
    std::vector<uint8_t> buffer;
    FILE* f = fopen(path.c_str(), "rb");
    [[unlikely]]
    if (!f) {
        return buffer;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return buffer;
    }
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return buffer;
    }
    // Safety check to prevent out-of-memory on invalid/massive files (e.g. > 200MB)
    if (size > 200 * 1024 * 1024) {
        fclose(f);
        return buffer;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return buffer;
    }
    buffer.resize(size);
    size_t read_bytes = fread(buffer.data(), 1, size, f);
    if (read_bytes != (size_t)size) {
        buffer.clear(); // Truncated read or read error, reject
    }
    fclose(f);
    return buffer;
}

int ImageLoader::read_exif_rotation_from_memory(std::span<const uint8_t> buffer) {
    int rotation = 1;
    if (buffer.empty()) return rotation;
    ExifData* ed = exif_data_new_from_data(buffer.data(), buffer.size());
    [[unlikely]]
    if (!ed) return rotation;

    ExifEntry* entry = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
    if (!entry || !entry->data || entry->size < 2 || entry->format != EXIF_FORMAT_SHORT) {
        entry = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_ORIENTATION);
    }
    if (!entry || !entry->data || entry->size < 2 || entry->format != EXIF_FORMAT_SHORT) {
        exif_data_unref(ed);
        return rotation;
    }

    unsigned short val = exif_get_short(entry->data, exif_data_get_byte_order(ed));
    if (val >= 1 && val <= 8) rotation = val;
    exif_data_unref(ed);
    return rotation;
}



std::shared_ptr<ImageData> ImageLoader::load(const std::string& path) {
    auto result = std::make_shared<ImageData>();
    size_t last_slash = path.find_last_of("/\\");
    result->filename = (last_slash == std::string::npos) ? path : path.substr(last_slash + 1);
    result->valid = false;
    result->transient_error = false;

    errno = 0;
    std::vector<uint8_t> buffer = read_file_to_buffer(path);
    if (buffer.empty()) {
        int err = errno;
        if (err != 0 && err != ENOENT) {
            result->transient_error = true;
            g_logger.warn("ImageLoader: Detected network/IO error (errno={}: {}) for path: {}", err, strerror(err), path.c_str());
            trigger_error(101); // E101: NAS_MOUNT_FAILED
        } else {
            trigger_error(201); // E201: IMAGE_LOAD_ERROR
        }
        return result;
    }

    int w = 0, h = 0, ch = 0;
    uint8_t* pixels = stbi_load_from_memory(buffer.data(), std::ssize(buffer), &w, &h, &ch, 4);
    [[unlikely]]
    if (!pixels || w <= 0 || h <= 0) {
        trigger_error(201); // E201: IMAGE_LOAD_ERROR
        return result;
    }

    SDL_Surface* surf = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        stbi_image_free(pixels);
        trigger_error(201); // E201: IMAGE_LOAD_ERROR
        return result;
    }

    uint8_t* dst = (uint8_t*)surf->pixels;
    const uint8_t* src = pixels;
    for (int y = 0; y < h; y++) {
        memcpy(dst + y * surf->pitch, src + y * w * 4, w * 4);
    }
    stbi_image_free(pixels);

    ImageMetadata meta = read_metadata_from_memory(buffer);
    int exif = meta.rotation;
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
    result->is_camera = meta.is_camera;
    result->creation_time = meta.creation_time;
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
    if (surf && surf->pixels && surf->w > 0 && surf->h > 0) {
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
            result->edge_top_rgb[x * 3 + 0] = (uint8_t)(ac > 0 ? (ar / ac) : 0);
            result->edge_top_rgb[x * 3 + 1] = (uint8_t)(ac > 0 ? (ag / ac) : 0);
            result->edge_top_rgb[x * 3 + 2] = (uint8_t)(ac > 0 ? (ab / ac) : 0);
        }

        // Bottom edge (3 rows: sh-3, sh-2, sh-1)
        result->edge_bot_rgb.resize(sw * 3);
        int bot_samples = sh < 3 ? sh : 3;
        for (int x = 0; x < sw; x++) {
            int ar = 0, ag = 0, ab = 0;
            for (int d = 0; d < bot_samples; d++) {
                int ry = sh - bot_samples + d;
                if (ry >= 0 && ry < sh) {
                    const uint8_t* dp = px + x * bpp + ry * pitch;
                    ar += dp[0]; ag += dp[1]; ab += dp[2];
                }
            }
            result->edge_bot_rgb[x * 3 + 0] = (uint8_t)(bot_samples > 0 ? (ar / bot_samples) : 0);
            result->edge_bot_rgb[x * 3 + 1] = (uint8_t)(bot_samples > 0 ? (ag / bot_samples) : 0);
            result->edge_bot_rgb[x * 3 + 2] = (uint8_t)(bot_samples > 0 ? (ab / bot_samples) : 0);
        }

        // Left edge: one RGB per row
        result->edge_lft_rgb.resize(sh * 3);
        for (int y = 0; y < sh; y++) {
            const uint8_t* p = px + y * pitch;
            int ar = 0, ag = 0, ab = 0, ac = 0;
            for (int w = 0; w < 3 && w < sw; w++) {
                ar += p[w * bpp + 0]; ag += p[w * bpp + 1]; ab += p[w * bpp + 2]; ac++;
            }
            result->edge_lft_rgb[y * 3 + 0] = (uint8_t)(ac > 0 ? (ar / ac) : 0);
            result->edge_lft_rgb[y * 3 + 1] = (uint8_t)(ac > 0 ? (ag / ac) : 0);
            result->edge_lft_rgb[y * 3 + 2] = (uint8_t)(ac > 0 ? (ab / ac) : 0);
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
            result->edge_rgt_rgb[y * 3 + 0] = (uint8_t)(ac > 0 ? (ar / ac) : 0);
            result->edge_rgt_rgb[y * 3 + 1] = (uint8_t)(ac > 0 ? (ag / ac) : 0);
            result->edge_rgt_rgb[y * 3 + 2] = (uint8_t)(ac > 0 ? (ab / ac) : 0);
        }
    }

    return result;
}

void ImageLoader::load_texture(ImageData* data, SDL_Renderer* renderer) {
    g_logger.info("[TRACE] ImageLoader::load_texture data={} surface={} renderer={}", (void*)data, data ? (void*)data->surface : nullptr, (void*)renderer);
    if (!data || !data->surface || !renderer || data->texture || data->width <= 0 || data->height <= 0) return;

    int max_dim = 1920;
    { std::lock_guard lk(g_config_mtx); max_dim = g_cfg.max_texture_dim; }
    if (data->width > max_dim || data->height > max_dim) {
        float scale = (float)max_dim / (float)std::max(data->width, data->height);
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
        } else {
            g_logger.warn("Failed to create scaled surface");
            SDL_DestroySurface(data->surface);
            data->surface = nullptr;
            return;
        }
    }

    data->texture = SDL_CreateTextureFromSurface(renderer, data->surface);
    if (!data->texture) {
        g_logger.error("Failed to create texture from surface: {}", SDL_GetError());
    } else {
        SDL_SetTextureScaleMode(data->texture, SDL_SCALEMODE_LINEAR);
    }

    if (data->blur_raw.valid && data->blur_raw.pixels) {
        SDL_Surface* bsurf = SDL_CreateSurface(data->blur_raw.width, data->blur_raw.height, SDL_PIXELFORMAT_RGBA32);
        if (bsurf) {
            memcpy(bsurf->pixels, data->blur_raw.pixels, (size_t)data->blur_raw.width * data->blur_raw.height * 4);
            if (data->exif_rotation >= 2 && data->exif_rotation <= 8) {
                SDL_Surface* rotated = apply_exif_rotation(bsurf, data->exif_rotation);
                if (rotated) {
                    SDL_DestroySurface(bsurf);
                    bsurf = rotated;
                }
            }
            data->blur_texture = SDL_CreateTextureFromSurface(renderer, bsurf);
            if (!data->blur_texture) {
                g_logger.error("Failed to create blur texture from surface: {}", SDL_GetError());
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







bool ImageLoader::has_camera_exif(const char* path) {
    ExifData* ed = exif_data_new_from_file(path);
    [[unlikely]]
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
    [[unlikely]]
    if (!surface || exif < 2 || exif > 8) return nullptr;
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

int64_t ImageLoader::get_creation_time(std::string_view path) {
    ExifData* ed = exif_data_new_from_file(std::string(path).c_str());
    [[unlikely]]
    if (!ed) return 0;

    ExifEntry* datetime = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_ORIGINAL);
    if (!datetime) {
        datetime = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_DATE_TIME);
    }
    if (!datetime) {
        datetime = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_DIGITIZED);
    }

    int64_t result = 0;
    if (datetime && datetime->data && datetime->size >= 19) {
        std::string_view date_sv(reinterpret_cast<const char*>(datetime->data), 19);
        if (date_sv.length() >= 19 && date_sv[4] == ':' && date_sv[7] == ':' && date_sv[10] == ' ' && date_sv[13] == ':' && date_sv[16] == ':') {
            auto parse_int = [](std::string_view s) -> int {
                int val = 0;
                for (char c : s) {
                    if (c < '0' || c > '9') return -1;
                    val = val * 10 + (c - '0');
                }
                return val;
            };

            int year  = parse_int(date_sv.substr(0, 4));
            int month = parse_int(date_sv.substr(5, 2));
            int day   = parse_int(date_sv.substr(8, 2));
            int hour  = parse_int(date_sv.substr(11, 2));
            int min   = parse_int(date_sv.substr(14, 2));
            int sec   = parse_int(date_sv.substr(17, 2));

            if (year >= 1900 && year <= 2100 && month >= 1 && month <= 12 && day >= 1 && day <= 31) {
                struct tm tm_dest = {};
                tm_dest.tm_year = year - 1900;
                tm_dest.tm_mon = month - 1;
                tm_dest.tm_mday = day;
                tm_dest.tm_hour = hour;
                tm_dest.tm_min = min;
                tm_dest.tm_sec = sec;
                tm_dest.tm_isdst = -1;
                time_t t = mktime(&tm_dest);
                if (t != -1) {
                    result = static_cast<int64_t>(t);
                }
            }
        }
    }
    exif_data_unref(ed);
    return result;
}

ImageMetadata ImageLoader::read_metadata_from_memory(std::span<const uint8_t> buffer) {
    ImageMetadata meta;
    if (buffer.empty()) return meta;

    ExifData* ed = exif_data_new_from_data(buffer.data(), buffer.size());
    [[unlikely]]
    if (!ed) return meta;

    // 1. Orientation/Rotation
    ExifEntry* orient = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
    if (!orient || !orient->data || orient->size < 2 || orient->format != EXIF_FORMAT_SHORT) {
        orient = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_ORIENTATION);
    }
    if (orient && orient->data && orient->size >= 2 && orient->format == EXIF_FORMAT_SHORT) {
        unsigned short val = exif_get_short(orient->data, exif_data_get_byte_order(ed));
        if (val >= 1 && val <= 8) meta.rotation = val;
    }

    // 2. Camera verification (optical hardware & lens tags)
    ExifEntry* make = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_MAKE);
    ExifEntry* model = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_MODEL);
    ExifEntry* exposure = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_EXPOSURE_TIME);
    ExifEntry* fnumber = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_FNUMBER);
    ExifEntry* iso = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_ISO_SPEED_RATINGS);
    ExifEntry* focal = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_FOCAL_LENGTH);
    ExifEntry* datetime = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_ORIGINAL);

    int count = (make ? 1 : 0) + (model ? 1 : 0) + (exposure ? 1 : 0) + (fnumber ? 1 : 0) + (iso ? 1 : 0) + (focal ? 1 : 0) + (datetime ? 1 : 0);
    meta.is_camera = (count >= 2);

    // 3. Creation / Capture time
    if (!datetime) {
        datetime = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_DATE_TIME);
    }
    if (!datetime) {
        datetime = exif_content_get_entry(ed->ifd[EXIF_IFD_EXIF], EXIF_TAG_DATE_TIME_DIGITIZED);
    }

    if (datetime && datetime->data && datetime->size >= 19) {
        std::string_view date_sv(reinterpret_cast<const char*>(datetime->data), 19);
        if (date_sv.length() >= 19 && date_sv[4] == ':' && date_sv[7] == ':' && date_sv[10] == ' ' && date_sv[13] == ':' && date_sv[16] == ':') {
            auto parse_int = [](std::string_view s) -> int {
                int val = 0;
                for (char c : s) {
                    if (c < '0' || c > '9') return -1;
                    val = val * 10 + (c - '0');
                }
                return val;
            };

            int year  = parse_int(date_sv.substr(0, 4));
            int month = parse_int(date_sv.substr(5, 2));
            int day   = parse_int(date_sv.substr(8, 2));
            int hour  = parse_int(date_sv.substr(11, 2));
            int min   = parse_int(date_sv.substr(14, 2));
            int sec   = parse_int(date_sv.substr(17, 2));

            if (year >= 1900 && year <= 2100 && month >= 1 && month <= 12 && day >= 1 && day <= 31) {
                struct tm tm_dest = {};
                tm_dest.tm_year = year - 1900;
                tm_dest.tm_mon = month - 1;
                tm_dest.tm_mday = day;
                tm_dest.tm_hour = hour;
                tm_dest.tm_min = min;
                tm_dest.tm_sec = sec;
                tm_dest.tm_isdst = -1;
                time_t t = mktime(&tm_dest);
                if (t != -1) {
                    meta.creation_time = static_cast<int64_t>(t);
                }
            }
        }
    }

    exif_data_unref(ed);
    return meta;
}

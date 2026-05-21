#define _GNU_SOURCE
#include "image_loader.h"
#include "renderer.h"
#include "util.h"
#include <SDL_image.h>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <cstdio>

// EXIF tags and library
#include <libexif/exif-data.h>
#include <libexif/exif-entry.h>
#include <libexif/exif-content.h>
#include <libexif/exif-tag.h>
#include <libexif/exif-utils.h>

// Low-level decoders
#include <jpeglib.h>
#include <jerror.h>
#include <png.h>
#include <tiffio.h>
#include <webp/decode.h>
#include <libheif/heif.h>

// JPEG custom error handler structure
struct jpeg_err_handler {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void jpeg_err_exit(j_common_ptr cinfo) {
    jpeg_err_handler *jerr = (jpeg_err_handler *)cinfo->err;
    longjmp(jerr->setjmp_buffer, 1);
}

// PNG memory read context structure
typedef struct {
    uint8_t* buf;
    size_t pos;
    size_t size;
} png_mem_ctx;

static void png_read_memory(png_structp png_ptr, png_bytep out, png_size_t want) {
    png_mem_ctx* ctx = (png_mem_ctx*)png_get_io_ptr(png_ptr);
    if (!ctx) { png_error(png_ptr, "png_read_memory: null context"); return; }
    if (ctx->pos + want > ctx->size) {
        png_error(png_ptr, "png_read_memory: truncated read");
        return;
    }
    if (want > 0 && ctx->buf) {
        memcpy(out, ctx->buf + ctx->pos, want);
        ctx->pos += want;
    }
}

// ============================================================
// Low-Level Decoders — return raw pixels (no SDL calls)
// ============================================================

static bool decode_jpeg_raw(const char* path, uint8_t*& out_pixels, int& out_w, int& out_h) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return false; }
    uint8_t* buf = (uint8_t*)malloc(fsize);
    if (!buf) { fclose(f); return false; }
    size_t nr = fread(buf, 1, (size_t)fsize, f);
    if (nr != (size_t)fsize) { fclose(f); free(buf); return false; }
    fclose(f);

    struct jpeg_decompress_struct cinfo;
    jpeg_err_handler jerr;
    jpeg_create_decompress(&cinfo);
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_err_exit;
    jpeg_mem_src(&cinfo, buf, (size_t)fsize);

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        free(buf);
        return false;
    }

    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_EXT_RGBA;
    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;
    uint8_t* raw = (uint8_t*)malloc((size_t)w * h * 4);
    if (!raw) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        free(buf);
        return false;
    }

    uint8_t* scanline = (uint8_t*)malloc((size_t)w * 4);
    if (scanline) {
        JSAMPROW row = {scanline};
        while (cinfo.output_scanline < (unsigned int)h) {
            unsigned int row_num = cinfo.output_scanline;
            jpeg_read_scanlines(&cinfo, &row, 1);
            memcpy(raw + row_num * w * 4, scanline, w * 4);
        }
        free(scanline);
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(buf);

    out_pixels = raw;
    out_w = w;
    out_h = h;
    return true;
}

static bool decode_png_raw(const char* path, uint8_t*& out_pixels, int& out_w, int& out_h) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return false; }
    uint8_t* buf = (uint8_t*)malloc(fsize);
    if (!buf) { fclose(f); return false; }
    size_t nr = fread(buf, 1, (size_t)fsize, f);
    if (nr != (size_t)fsize) { fclose(f); free(buf); return false; }
    fclose(f);

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) { free(buf); return false; }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        free(buf);
        return false;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        free(buf);
        return false;
    }

    png_mem_ctx png_ctx = {buf, 0, (size_t)fsize};
    png_set_read_fn(png_ptr, &png_ctx, png_read_memory);
    png_read_info(png_ptr, info_ptr);

    int width = png_get_image_width(png_ptr, info_ptr);
    int height = png_get_image_height(png_ptr, info_ptr);
    png_byte color_type = png_get_color_type(png_ptr, info_ptr);
    png_byte bit_depth = png_get_bit_depth(png_ptr, info_ptr);

    if (bit_depth == 16) png_set_strip_16(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png_ptr);
    if (color_type == PNG_COLOR_TYPE_GRAY || color_type == PNG_COLOR_TYPE_PALETTE) {
        png_set_expand(png_ptr);
    }
    if (color_type != PNG_COLOR_TYPE_RGB && color_type != PNG_COLOR_TYPE_RGBA) {
        png_set_rgb_to_gray(png_ptr, PNG_ERROR_ACTION_WARN, 0.0, 0.0);
    }

    png_read_update_info(png_ptr, info_ptr);
    color_type = png_get_color_type(png_ptr, info_ptr);

    uint8_t* raw = (uint8_t*)malloc((size_t)width * height * 4);
    if (!raw) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        free(buf);
        return false;
    }

    if (color_type == PNG_COLOR_TYPE_RGB) {
        uint8_t* tmp_rgb = (uint8_t*)malloc((size_t)width * height * 3);
        png_bytep* rows = (png_bytep*)malloc((size_t)height * sizeof(png_bytep));
        if (tmp_rgb && rows) {
            for (int y = 0; y < height; y++) {
                rows[y] = (png_bytep)(tmp_rgb + y * width * 3);
            }
            png_read_image(png_ptr, rows);
            for (int y = 0; y < height; y++) {
                uint8_t* dst_row = raw + y * width * 4;
                uint8_t* src_row = tmp_rgb + y * width * 3;
                for (int x = 0; x < width; x++) {
                    dst_row[x * 4 + 0] = src_row[x * 3 + 0];
                    dst_row[x * 4 + 1] = src_row[x * 3 + 1];
                    dst_row[x * 4 + 2] = src_row[x * 3 + 2];
                    dst_row[x * 4 + 3] = 255;
                }
            }
        }
        free(tmp_rgb);
        free(rows);
    } else {
        png_bytep* rows = (png_bytep*)malloc((size_t)height * sizeof(png_bytep));
        if (rows) {
            for (int y = 0; y < height; y++) {
                rows[y] = (png_bytep)(raw + y * width * 4);
            }
            png_read_image(png_ptr, rows);
        }
        free(rows);
    }

    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    free(buf);

    out_pixels = raw;
    out_w = width;
    out_h = height;
    return true;
}

static bool decode_tiff_raw(const char* path, uint8_t*& out_pixels, int& out_w, int& out_h) {
    TIFF* tif = TIFFOpen(path, "r");
    if (!tif) return false;

    uint32_t width = 0, height = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);

    if (width == 0 || height == 0) { TIFFClose(tif); return false; }

    uint16_t spp = 1;
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    if (spp < 1 || spp > 4) spp = 1;

    uint8_t* raw = (uint8_t*)malloc((size_t)width * height * 4);
    if (!raw) { TIFFClose(tif); return false; }
    uint8_t* scanline = (uint8_t*)malloc((size_t)width * spp);
    if (!scanline) { free(raw); TIFFClose(tif); return false; }

    uint32_t y = 0;
    while (y < height) {
        if (TIFFReadScanline(tif, scanline, y, 0) <= 0) {
            free(raw); free(scanline); TIFFClose(tif);
            return false;
        }
        uint8_t* dst_row = raw + y * width * 4;
        if (spp == 1) {
            for (uint32_t x = 0; x < width; x++) {
                dst_row[x * 4 + 0] = scanline[x];
                dst_row[x * 4 + 1] = scanline[x];
                dst_row[x * 4 + 2] = scanline[x];
                dst_row[x * 4 + 3] = 255;
            }
        } else if (spp == 3) {
            for (uint32_t x = 0; x < width; x++) {
                dst_row[x * 4 + 0] = scanline[x * 3 + 0];
                dst_row[x * 4 + 1] = scanline[x * 3 + 1];
                dst_row[x * 4 + 2] = scanline[x * 3 + 2];
                dst_row[x * 4 + 3] = 255;
            }
        } else if (spp == 4) {
            for (uint32_t x = 0; x < width; x++) {
                dst_row[x * 4 + 0] = scanline[x * 4 + 0];
                dst_row[x * 4 + 1] = scanline[x * 4 + 1];
                dst_row[x * 4 + 2] = scanline[x * 4 + 2];
                dst_row[x * 4 + 3] = scanline[x * 4 + 3];
            }
        } else {
            for (uint32_t x = 0; x < width; x++) {
                for (int c = 0; c < 4 && c < spp; c++) {
                    dst_row[x * 4 + c] = scanline[x * spp + c];
                }
                dst_row[x * 4 + 3] = 255;
            }
        }
        y++;
    }

    free(scanline);
    TIFFClose(tif);

    out_pixels = raw;
    out_w = (int)width;
    out_h = (int)height;
    return true;
}

static bool decode_heic_raw(const char* path, uint8_t*& out_pixels, int& out_w, int& out_h) {
    heif_context* ctx = heif_context_alloc();
    if (!ctx) return false;
    heif_error err = heif_context_read_from_file(ctx, path, nullptr);
    if (err.code != heif_error_Ok) { heif_context_free(ctx); return false; }

    heif_image_handle* handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok) { heif_context_free(ctx); return false; }

    heif_image* heif_img = nullptr;
    err = heif_decode_image(handle, &heif_img, heif_colorspace_RGB,
                            heif_chroma_interleaved_RGB, nullptr);
    if (err.code != heif_error_Ok) {
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return false;
    }

    int w = heif_image_get_width(heif_img, heif_channel_interleaved);
    int h = heif_image_get_height(heif_img, heif_channel_interleaved);
    int stride = 0;
    const uint8_t* data = heif_image_get_plane_readonly(heif_img,
                                                         heif_channel_interleaved,
                                                         &stride);

    uint8_t* raw = (uint8_t*)malloc((size_t)w * h * 4);
    if (raw) {
        for (int y = 0; y < h; y++) {
            const uint8_t* src_row = data + y * stride;
            uint8_t* dst_row = raw + y * w * 4;
            for (int x = 0; x < w; x++) {
                dst_row[x * 4 + 0] = src_row[x * 3 + 0];
                dst_row[x * 4 + 1] = src_row[x * 3 + 1];
                dst_row[x * 4 + 2] = src_row[x * 3 + 2];
                dst_row[x * 4 + 3] = 255;
            }
        }
    }

    heif_image_release(heif_img);
    heif_image_handle_release(handle);
    heif_context_free(ctx);

    if (raw) {
        out_pixels = raw;
        out_w = w;
        out_h = h;
    }
    return !!raw;
}

static bool decode_webp_raw(const char* path, uint8_t*& out_pixels, int& out_w, int& out_h) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return false; }
    uint8_t* buf = (uint8_t*)malloc(fsize);
    if (!buf) { fclose(f); return false; }
    size_t nr = fread(buf, 1, (size_t)fsize, f);
    if (nr != (size_t)fsize) { free(buf); fclose(f); return false; }
    fclose(f);

    int w = 0, h = 0;
    uint8_t* rgba = WebPDecodeRGBA(buf, (size_t)fsize, &w, &h);
    free(buf);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) WebPFree(rgba);
        return false;
    }

    out_pixels = rgba;
    out_w = w;
    out_h = h;
    return true;
}

// ============================================================
// Public API
// ============================================================

RawImage ImageLoader::load_raw(const std::string& path) {
    RawImage raw;
    raw.valid = false;

    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return raw;
    uint8_t magic[4];
    int magic_len = (int)fread(magic, 1, 4, f);
    fclose(f);

    if (magic_len < 4) return raw;

    // JPEG: FF D8 FF
    if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) {
        if (decode_jpeg_raw(path.c_str(), raw.pixels, raw.width, raw.height)) {
            raw.channels = 4;
            raw.format = ImageFormat::RGBA32;
            raw.valid = true;
        }
    }
    // PNG: 89 50 4E 47
    else if (magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47) {
        if (decode_png_raw(path.c_str(), raw.pixels, raw.width, raw.height)) {
            raw.channels = 4;
            raw.format = ImageFormat::RGBA32;
            raw.valid = true;
        }
    }
    // TIFF: LE or BE
    else if ((magic[0] == 0x49 && magic[1] == 0x49 && magic[2] == 0x2A && magic[3] == 0x00) ||
             (magic[0] == 0x4D && magic[1] == 0x4D && magic[2] == 0x00 && magic[3] == 0x2A)) {
        if (decode_tiff_raw(path.c_str(), raw.pixels, raw.width, raw.height)) {
            raw.channels = 4;
            raw.format = ImageFormat::RGBA32;
            raw.valid = true;
        }
    }

    // Fallback: try IMG_Load for formats we don't have raw decoders for (BMP, GIF, etc.)
    if (!raw.valid) {
        SDL_Surface* surf = IMG_Load(path.c_str());
        if (surf) {
            // Convert to RGBA32 raw
            if (surf->format->format == SDL_PIXELFORMAT_RGBA32 || surf->format->format == SDL_PIXELFORMAT_BGRA32) {
                raw.pixels = (uint8_t*)malloc((size_t)surf->w * surf->h * 4);
                if (raw.pixels) {
                    memcpy(raw.pixels, surf->pixels, (size_t)surf->w * surf->h * 4);
                    raw.width = surf->w;
                    raw.height = surf->h;
                    raw.channels = 4;
                    raw.format = ImageFormat::RGBA32;
                    raw.valid = true;
                }
            }
            SDL_FreeSurface(surf);
        }
    }

    // HEIC/WEBP fallback
    if (!raw.valid) {
        std::string ext = path.substr(path.find_last_of('.') + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == "heic" || ext == "heif") {
            if (decode_heic_raw(path.c_str(), raw.pixels, raw.width, raw.height)) {
                raw.channels = 4;
                raw.format = ImageFormat::RGBA32;
                raw.valid = true;
            }
        } else if (ext == "webp") {
            if (decode_webp_raw(path.c_str(), raw.pixels, raw.width, raw.height)) {
                raw.channels = 4;
                raw.format = ImageFormat::RGBA32;
                raw.valid = true;
            }
        }
    }

    return raw;
}

std::shared_ptr<ImageData> ImageLoader::load(const std::string& path) {
    auto data = std::make_shared<ImageData>();
    data->valid = false;

    // Load raw pixels on caller's thread (for preload workers)
    RawImage raw = load_raw(path);
    if (!raw.valid) {
        g_logger.error("Failed to load image: %s", path.c_str());
        return data;
    }

    // Create SDL surface from raw pixels (must be on main thread)
    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, raw.width, raw.height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        g_logger.error("Failed to create surface for: %s", path.c_str());
        free(raw.pixels);
        return data;
    }
    memcpy(surface->pixels, raw.pixels, (size_t)raw.width * raw.height * 4);
    free(raw.pixels);

    // Process EXIF rotation CPU-side
    int exif = read_exif_rotation(path.c_str());
    if (exif >= 2 && exif <= 8) {
        SDL_Surface* rotated = apply_exif_rotation(surface, exif);
        if (rotated) surface = rotated;
    }

    data->surface = surface;
    data->width = surface->w;
    data->height = surface->h;
    data->exif_rotation = exif;
    data->valid = true;

    // Extract average color
    GpuColor avg = Renderer::get_average_color(surface);
    data->avg_r = avg.r;
    data->avg_g = avg.g;
    data->avg_b = avg.b;

    return data;
}

void ImageLoader::load_texture(ImageData* data, SDL_Renderer* renderer) {
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
            SDL_Surface* tmp = nullptr;
            {
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
                    tmp = dst;
                }
            }
            if (tmp) {
                SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, tmp->w, tmp->h, 32, tmp->format->format);
                if (dst) {
                    uint8_t* src_px = (uint8_t*)tmp->pixels;
                    uint8_t* dst_px = (uint8_t*)dst->pixels;
                    for (int y = 0; y < tmp->h; y++) {
                        memcpy(dst_px + y * dst->pitch, src_px + (tmp->h - 1 - y) * tmp->pitch, tmp->pitch);
                    }
                    rotated = dst;
                }
                SDL_FreeSurface(tmp);
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
            // 90 CW + horizontal flip
            SDL_Surface* tmp = nullptr;
            {
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
                    tmp = dst;
                }
            }
            if (tmp) {
                SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, tmp->w, tmp->h, 32, tmp->format->format);
                if (dst) {
                    int bpp = tmp->format->BytesPerPixel;
                    uint8_t* src_px = (uint8_t*)tmp->pixels;
                    uint8_t* dst_px = (uint8_t*)dst->pixels;
                    for (int y = 0; y < tmp->h; y++) {
                        uint8_t* src_row = src_px + y * tmp->pitch;
                        uint8_t* dst_row = dst_px + y * dst->pitch;
                        for (int x = 0; x < tmp->w; x++) {
                            memcpy(dst_row + x * bpp, src_row + (tmp->w - 1 - x) * bpp, bpp);
                        }
                    }
                    rotated = dst;
                }
                SDL_FreeSurface(tmp);
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
            // 90 CW + vertical flip
            SDL_Surface* tmp = nullptr;
            {
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
                    tmp = dst;
                }
            }
            if (tmp) {
                SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, tmp->w, tmp->h, 32, tmp->format->format);
                if (dst) {
                    uint8_t* src_px = (uint8_t*)tmp->pixels;
                    uint8_t* dst_px = (uint8_t*)dst->pixels;
                    for (int y = 0; y < tmp->h; y++) {
                        memcpy(dst_px + y * dst->pitch, src_px + (tmp->h - 1 - y) * tmp->pitch, tmp->pitch);
                    }
                    rotated = dst;
                }
                SDL_FreeSurface(tmp);
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
    if (rotated) {
        SDL_FreeSurface(surface);
        return rotated;
    }
    return surface;
}

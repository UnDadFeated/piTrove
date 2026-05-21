#include "image_loader.h"
#include "util.h"
#include "renderer.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csetjmp>
#include <algorithm>
#include <stdexcept>

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

// Resilient Low-Level Decoders

SDL_Surface* ImageLoader::load_jpeg_surface(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return nullptr; }
    uint8_t* buf = (uint8_t*)malloc(fsize);
    if (!buf) { fclose(f); return nullptr; }
    size_t nr = fread(buf, 1, (size_t)fsize, f);
    if (nr != (size_t)fsize) { fclose(f); free(buf); return nullptr; }
    fclose(f);

    struct jpeg_decompress_struct cinfo;
    jpeg_err_handler jerr;

    jpeg_create_decompress(&cinfo);
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_err_exit;
    jpeg_mem_src(&cinfo, buf, (size_t)fsize);

    uint8_t* scanline = nullptr;
    uint8_t* rowbuf = nullptr;
    SDL_Surface* surface = nullptr;

    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        free(buf);
        if (scanline) free(scanline);
        if (rowbuf) free(rowbuf);
        if (surface) { SDL_FreeSurface(surface); surface = nullptr; }
        return nullptr;
    }

    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_EXT_RGBA;
    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;
    int channels = (int)cinfo.output_components;

    surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        free(buf);
        return nullptr;
    }

    uint8_t* dst = (uint8_t*)surface->pixels;
    int dst_pitch = surface->pitch;

    if (channels == 4) {
        scanline = (uint8_t*)malloc((size_t)w * 4);
        if (scanline) {
            JSAMPROW row = {scanline};
            while (cinfo.output_scanline < (unsigned int)h) {
                unsigned int row_num = cinfo.output_scanline;
                jpeg_read_scanlines(&cinfo, &row, 1);
                memcpy(dst + row_num * dst_pitch, scanline, w * 4);
            }
            free(scanline);
        } else {
            jpeg_abort_decompress(&cinfo);
            SDL_FreeSurface(surface);
            surface = nullptr;
        }
    } else {
        int rowbuf_stride = (channels == 1) ? w : (w * 3);
        rowbuf = (uint8_t*)malloc(rowbuf_stride);
        if (rowbuf) {
            JSAMPROW row = {rowbuf};
            while (cinfo.output_scanline < (unsigned int)h) {
                unsigned int row_num = cinfo.output_scanline;
                jpeg_read_scanlines(&cinfo, &row, 1);
                uint8_t* dst_row = dst + row_num * dst_pitch;
                if (channels == 1) {
                    for (int x = 0; x < w; x++) {
                        unsigned char g = rowbuf[x];
                        dst_row[x * 4 + 0] = g;
                        dst_row[x * 4 + 1] = g;
                        dst_row[x * 4 + 2] = g;
                        dst_row[x * 4 + 3] = 255;
                    }
                } else {
                    for (int x = 0; x < w; x++) {
                        dst_row[x * 4 + 0] = rowbuf[x * 3 + 0];
                        dst_row[x * 4 + 1] = rowbuf[x * 3 + 1];
                        dst_row[x * 4 + 2] = rowbuf[x * 3 + 2];
                        dst_row[x * 4 + 3] = 255;
                    }
                }
            }
            free(rowbuf);
        } else {
            jpeg_abort_decompress(&cinfo);
            SDL_FreeSurface(surface);
            surface = nullptr;
        }
    }

    if (surface) {
        jpeg_finish_decompress(&cinfo);
    }
    jpeg_destroy_decompress(&cinfo);
    free(buf);
    return surface;
}

SDL_Surface* ImageLoader::load_png_surface(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return nullptr; }
    uint8_t* buf = (uint8_t*)malloc(fsize);
    if (!buf) { fclose(f); return nullptr; }
    size_t nr = fread(buf, 1, (size_t)fsize, f);
    if (nr != (size_t)fsize) { fclose(f); free(buf); return nullptr; }
    fclose(f);

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) { free(buf); return nullptr; }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        free(buf);
        return nullptr;
    }

    uint8_t* tmp_rgb = nullptr;
    png_bytep* rows = nullptr;
    SDL_Surface* surface = nullptr;

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        free(buf);
        if (tmp_rgb) free(tmp_rgb);
        if (rows) free(rows);
        if (surface) { SDL_FreeSurface(surface); surface = nullptr; }
        return nullptr;
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

    surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        free(buf);
        return nullptr;
    }

    uint8_t* dst = (uint8_t*)surface->pixels;
    int dst_pitch = surface->pitch;

    if (color_type == PNG_COLOR_TYPE_RGB) {
        tmp_rgb = (uint8_t*)malloc((size_t)width * height * 3);
        rows = (png_bytep*)malloc((size_t)height * sizeof(png_bytep));
        if (tmp_rgb && rows) {
            for (int y = 0; y < height; y++) {
                rows[y] = (png_bytep)(tmp_rgb + y * width * 3);
            }
            png_read_image(png_ptr, rows);
            for (int y = 0; y < height; y++) {
                uint8_t* dst_row = dst + y * dst_pitch;
                uint8_t* src_row = tmp_rgb + y * width * 3;
                for (int x = 0; x < width; x++) {
                    dst_row[x * 4 + 0] = src_row[x * 3 + 0];
                    dst_row[x * 4 + 1] = src_row[x * 3 + 1];
                    dst_row[x * 4 + 2] = src_row[x * 3 + 2];
                    dst_row[x * 4 + 3] = 255;
                }
            }
        } else {
            SDL_FreeSurface(surface);
            surface = nullptr;
        }
        free(tmp_rgb);
        free(rows);
    } else {
        rows = (png_bytep*)malloc((size_t)height * sizeof(png_bytep));
        if (rows) {
            for (int y = 0; y < height; y++) {
                rows[y] = (png_bytep)((uint8_t*)dst + y * dst_pitch);
            }
            png_read_image(png_ptr, rows);
            free(rows);
        } else {
            SDL_FreeSurface(surface);
            surface = nullptr;
        }
    }

    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    free(buf);
    return surface;
}

SDL_Surface* ImageLoader::load_tiff_surface(const char* path) {
    TIFF* tif = TIFFOpen(path, "r");
    if (!tif) return nullptr;

    uint32_t width = 0, height = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);

    if (width == 0 || height == 0) {
        TIFFClose(tif);
        return nullptr;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        TIFFClose(tif);
        return nullptr;
    }

    uint16_t spp = 1;
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    if (spp < 1 || spp > 4) spp = 1;

    uint8_t* raw = (uint8_t*)malloc((size_t)width * spp);
    if (!raw) {
        SDL_FreeSurface(surface);
        TIFFClose(tif);
        return nullptr;
    }

    uint8_t* dst = (uint8_t*)surface->pixels;
    int dst_pitch = surface->pitch;

    uint32_t y = 0;
    while (y < height) {
        if (TIFFReadScanline(tif, raw, y, 0) <= 0) {
            g_logger.warn("TIFF partial read: row %u/%u — file may be corrupted", y, height);
            SDL_FreeSurface(surface);
            surface = nullptr;
            break;
        }

        uint8_t* dst_row = dst + y * dst_pitch;

        if (spp == 1) {
            for (uint32_t x = 0; x < width; x++) {
                uint8_t g = raw[x];
                dst_row[x * 4 + 0] = g;
                dst_row[x * 4 + 1] = g;
                dst_row[x * 4 + 2] = g;
                dst_row[x * 4 + 3] = 255;
            }
        } else if (spp == 3) {
            for (uint32_t x = 0; x < width; x++) {
                dst_row[x * 4 + 0] = raw[x * 3 + 0];
                dst_row[x * 4 + 1] = raw[x * 3 + 1];
                dst_row[x * 4 + 2] = raw[x * 3 + 2];
                dst_row[x * 4 + 3] = 255;
            }
        } else if (spp == 4) {
            for (uint32_t x = 0; x < width; x++) {
                dst_row[x * 4 + 0] = raw[x * 4 + 0];
                dst_row[x * 4 + 1] = raw[x * 4 + 1];
                dst_row[x * 4 + 2] = raw[x * 4 + 2];
                dst_row[x * 4 + 3] = raw[x * 4 + 3];
            }
        } else {
            for (uint32_t x = 0; x < width; x++) {
                for (int c = 0; c < 4 && c < spp; c++) {
                    dst_row[x * 4 + c] = raw[x * spp + c];
                }
                dst_row[x * 4 + 3] = 255;
            }
        }
        y++;
    }

    free(raw);
    TIFFClose(tif);
    return surface;
}

SDL_Surface* ImageLoader::load_heic_surface(const char* path) {
    heif_context* ctx = heif_context_alloc();
    if (!ctx) {
        g_logger.error("HEIC: Failed to allocate heif_context");
        return nullptr;
    }
    heif_error err = heif_context_read_from_file(ctx, path, nullptr);
    if (err.code != heif_error_Ok) { heif_context_free(ctx); return nullptr; }

    heif_image_handle* handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok) { heif_context_free(ctx); return nullptr; }

    heif_image* heif_img = nullptr;
    err = heif_decode_image(handle, &heif_img, heif_colorspace_RGB,
                            heif_chroma_interleaved_RGB, nullptr);
    if (err.code != heif_error_Ok) {
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return nullptr;
    }

    int w = heif_image_get_width(heif_img, heif_channel_interleaved);
    int h = heif_image_get_height(heif_img, heif_channel_interleaved);
    int stride = 0;
    const uint8_t* data = heif_image_get_plane_readonly(heif_img,
                                                        heif_channel_interleaved,
                                                        &stride);

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (surface) {
        uint8_t* dst = (uint8_t*)surface->pixels;
        int dst_pitch = surface->pitch;
        for (int y = 0; y < h; y++) {
            const uint8_t* src_row = data + y * stride;
            uint8_t* dst_row = dst + y * dst_pitch;
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
    return surface;
}

SDL_Surface* ImageLoader::load_webp_surface(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return nullptr; }
    uint8_t* buf = (uint8_t*)malloc(fsize);
    if (!buf) { fclose(f); return nullptr; }
    size_t nr = fread(buf, 1, (size_t)fsize, f);
    if (nr != (size_t)fsize) { free(buf); fclose(f); return nullptr; }
    fclose(f);

    int w = 0, h = 0;
    uint8_t* rgba = WebPDecodeRGBA(buf, (size_t)fsize, &w, &h);
    free(buf);
    if (!rgba || w <= 0 || h <= 0) {
        if (rgba) WebPFree(rgba);
        return nullptr;
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (surface) {
        uint8_t* dst = (uint8_t*)surface->pixels;
        int dst_pitch = surface->pitch;
        for (int y = 0; y < h; y++) {
            memcpy(dst + y * dst_pitch, rgba + y * w * 4, w * 4);
        }
    }
    WebPFree(rgba);
    return surface;
}

// CPU-Side Image Manipulations (Flips and Rotations)

SDL_Surface* ImageLoader::flip_horizontal(SDL_Surface* src) {
    if (!src) return nullptr;
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, src->w, src->h, 32, src->format->format);
    if (!dst) return nullptr;
    
    int bpp = src->format->BytesPerPixel;
    uint8_t* src_px = (uint8_t*)src->pixels;
    uint8_t* dst_px = (uint8_t*)dst->pixels;

    for (int y = 0; y < src->h; y++) {
        uint8_t* src_row = src_px + y * src->pitch;
        uint8_t* dst_row = dst_px + y * dst->pitch;
        for (int x = 0; x < src->w; x++) {
            memcpy(dst_row + x * bpp, src_row + (src->w - 1 - x) * bpp, bpp);
        }
    }
    return dst;
}

SDL_Surface* ImageLoader::flip_vertical(SDL_Surface* src) {
    if (!src) return nullptr;
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, src->w, src->h, 32, src->format->format);
    if (!dst) return nullptr;

    uint8_t* src_px = (uint8_t*)src->pixels;
    uint8_t* dst_px = (uint8_t*)dst->pixels;

    for (int y = 0; y < src->h; y++) {
        memcpy(dst_px + y * dst->pitch, src_px + (src->h - 1 - y) * src->pitch, src->pitch);
    }
    return dst;
}

SDL_Surface* ImageLoader::rotate_90_cw(SDL_Surface* src) {
    if (!src) return nullptr;
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, src->h, src->w, 32, src->format->format);
    if (!dst) return nullptr;

    int bpp = src->format->BytesPerPixel;
    uint8_t* src_px = (uint8_t*)src->pixels;
    uint8_t* dst_px = (uint8_t*)dst->pixels;

    for (int y = 0; y < src->h; y++) {
        uint8_t* src_row = src_px + y * src->pitch;
        for (int x = 0; x < src->w; x++) {
            uint8_t* dst_pixel = dst_px + x * dst->pitch + (src->h - 1 - y) * bpp;
            memcpy(dst_pixel, src_row + x * bpp, bpp);
        }
    }
    return dst;
}

SDL_Surface* ImageLoader::rotate_90_ccw(SDL_Surface* src) {
    if (!src) return nullptr;
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, src->h, src->w, 32, src->format->format);
    if (!dst) return nullptr;

    int bpp = src->format->BytesPerPixel;
    uint8_t* src_px = (uint8_t*)src->pixels;
    uint8_t* dst_px = (uint8_t*)dst->pixels;

    for (int y = 0; y < src->h; y++) {
        uint8_t* src_row = src_px + y * src->pitch;
        for (int x = 0; x < src->w; x++) {
            uint8_t* dst_pixel = dst_px + (src->w - 1 - x) * dst->pitch + y * bpp;
            memcpy(dst_pixel, src_row + x * bpp, bpp);
        }
    }
    return dst;
}

SDL_Surface* ImageLoader::rotate_180(SDL_Surface* src) {
    if (!src) return nullptr;
    SDL_Surface* dst = SDL_CreateRGBSurfaceWithFormat(0, src->w, src->h, 32, src->format->format);
    if (!dst) return nullptr;

    int bpp = src->format->BytesPerPixel;
    uint8_t* src_px = (uint8_t*)src->pixels;
    uint8_t* dst_px = (uint8_t*)dst->pixels;

    for (int y = 0; y < src->h; y++) {
        uint8_t* src_row = src_px + y * src->pitch;
        uint8_t* dst_row = dst_px + (src->h - 1 - y) * dst->pitch;
        for (int x = 0; x < src->w; x++) {
            memcpy(dst_row + (src->w - 1 - x) * bpp, src_row + x * bpp, bpp);
        }
    }
    return dst;
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
        case 2: rotated = flip_horizontal(surface); break;
        case 3: {
            SDL_Surface* temp = flip_horizontal(surface);
            if (temp) {
                rotated = flip_vertical(temp);
                SDL_FreeSurface(temp);
            }
            break;
        }
        case 4: rotated = flip_vertical(surface); break;
        case 5: {
            SDL_Surface* temp = rotate_90_cw(surface);
            if (temp) {
                rotated = flip_horizontal(temp);
                SDL_FreeSurface(temp);
            }
            break;
        }
        case 6: rotated = rotate_90_cw(surface); break;
        case 7: {
            SDL_Surface* temp = rotate_90_cw(surface);
            if (temp) {
                rotated = flip_vertical(temp);
                SDL_FreeSurface(temp);
            }
            break;
        }
        case 8: rotated = rotate_90_ccw(surface); break;
    }
    if (rotated) {
        SDL_FreeSurface(surface);
        return rotated;
    }
    return surface;
}

// Public Methods

std::shared_ptr<ImageData> ImageLoader::load(const std::string& path) {
    auto data = std::make_shared<ImageData>();
    SDL_Surface* surface = nullptr;

    // Check file magic bytes and try low-level decoders first to maintain robust fallback behaviour
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
        uint8_t magic[4];
        int magic_len = (int)fread(magic, 1, 4, f);
        fclose(f);

        if (magic_len >= 4) {
            // JPEG: FF D8 FF
            if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) {
                surface = load_jpeg_surface(path.c_str());
            }
            // PNG: 89 50 4E 47
            else if (magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47) {
                surface = load_png_surface(path.c_str());
            }
            // TIFF: LE or BE
            else if ((magic[0] == 0x49 && magic[1] == 0x49 && magic[2] == 0x2A && magic[3] == 0x00) ||
                     (magic[0] == 0x4D && magic[1] == 0x4D && magic[2] == 0x00 && magic[3] == 0x2A)) {
                surface = load_tiff_surface(path.c_str());
            }
        }
    }

    // If custom decoders didn't work or weren't used, try standard IMG_Load
    if (!surface) {
        surface = IMG_Load(path.c_str());
    }

    // Try custom HEIC / WEBP loader fallbacks
    if (!surface) {
        std::string ext = path.substr(path.find_last_of('.') + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == "heic" || ext == "heif") {
            surface = load_heic_surface(path.c_str());
        } else if (ext == "webp") {
            surface = load_webp_surface(path.c_str());
        }
    }

    if (!surface) {
        g_logger.error("Failed to load image: %s", path.c_str());
        data->valid = false;
        return data;
    }

    // Process EXIF rotation CPU-side
    int exif = read_exif_rotation(path.c_str());
    if (exif >= 2 && exif <= 8) {
        surface = apply_exif_rotation(surface, exif);
    }

    data->surface = surface;
    data->width = surface->w;
    data->height = surface->h;
    data->exif_rotation = exif;
    data->valid = true;

    // Extract average color safely in background thread before uploading/freeing
    GpuColor avg = Renderer::get_average_color(surface);
    data->avg_r = avg.r;
    data->avg_g = avg.g;
    data->avg_b = avg.b;

    return data;
}

void ImageLoader::load_texture(ImageData* data, SDL_Renderer* renderer) {
    if (!data || !data->surface || data->texture) return;
    
    // Scale image down on main thread if it exceeds screen dimensions to fit display or max VRAM size
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
        g_logger.error("Failed to upload texture to VRAM: %s", SDL_GetError());
    } else {
        // Set bilinear filtering by default for high quality rendering
        SDL_SetTextureScaleMode(data->texture, SDL_ScaleModeLinear);
    }

    // Free the CPU-side surface memory as it is now loaded in VRAM
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

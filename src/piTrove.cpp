/*
 * piTrove — Digital Picture Frame  (ARM64 / Pi5)
 *   • Config  – simple key=value with [sections]
 *   • Logger  – file + stdout, thread-safe
 *   • Scanner – getdents64 (CIFS-safe) with multi-threaded producer/consumer, timeout wrappers
 *   • EXIF    – libexif rotation
 *   • Loader  – stb_image + libjpeg-turbo + libpng + libtiff (robust fallback)
 *   • Cache   – SQLite3, WAL mode, 256 MB mmap, fast-path skip when DB exists
 *   • Splash  – logo + black-bottom UI overlay (P2/P3)
 *   • Slideshow – raylib, preload, crossfade, Ken Burns
 */

#define VERSION "7.10.1"
#define APP_NAME "piTrove"

// Global atomics for headless features
#include <atomic>
#include <algorithm>
#include <sys/prctl.h>
#include <sched.h>
std::atomic<bool> g_running{true};
std::atomic<int> g_remote_command{0}; // 1=Next, 2=Prev, 3=PauseToggle
std::atomic<float> g_weather_temp{-999.0f};
std::atomic<int> g_weather_code{-1};
std::atomic<bool> g_config_changed{false};

// v3.0.4: Global HTTP server fd for graceful shutdown (L2)
int g_http_server_fd = -1;

// ── raylib ─────────────────────────────────────────────────────────────
#include <raylib.h>
#include <raymath.h>
#include <rlgl.h>

// OpenGL ES — for glReadPixels in fb_flip()
// ── DRM master management ─────────────────────────────────────────────
#include <xf86drm.h>

// ── mpv render API (v3.0.0: in-process video) ─────────────────────────
#include <mpv/client.h>
#include <mpv/render_gl.h>
// ── EGL — for mpv render API context sharing ──
#include <EGL/egl.h>
#include <GLES2/gl2.h>   // glBindFramebuffer, GL_FRAMEBUFFER
// ── dlopen/dlsym for fresh GL function resolution ──
#include <dlfcn.h>
// ── SQLite ─────────────────────────────────────────────────────────────
#include <sqlite3.h>

   // ── EXIF (libexif uses subdirectory headers) ──────────────────────────
#include <libexif/exif-data.h>
#include <libexif/exif-entry.h>
#include <libexif/exif-content.h>
#include <libexif/exif-tag.h>
#include <libexif/exif-utils.h>

// ── JPEG (libjpeg-turbo) ───────────────────────────────────────────────
#include <cstdio>
#include <cstdlib>
#include <jpeglib.h>
#include <jerror.h>

// ── PNG ────────────────────────────────────────────────────────────────
#include <png.h>

// ── TIFF ───────────────────────────────────────────────────────────────
#include <tiffio.h>

// ── HEIF (libheif for HEIC/HEIF) ───────────────────────────────────────
#include <libheif/heif.h>

// ── POSIX / syscalls ───────────────────────────────────────────────────
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
/* termios.h removed — TUI uses system("stty") instead */
#include <dirent.h>
#include <time.h>
#include <cstdarg>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/wait.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/time.h>

// ── C++ standard ───────────────────────────────────────────────────────
#include <vector>
#include <string>
#include <string_view>
#include <filesystem>
#include <regex>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <atomic>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <queue>
#include <functional>
#include <memory>
#include <chrono>
#include <cmath>
#include <random>
#include <numeric>
#include <cstring>
// v16.2.0: <cstdio> and <cstdarg> already included above
#include <variant>
#include <array>
#include <stdexcept>
#include <iostream>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <sys/resource.h>

// Crash-safety tracking variables
std::string g_crash_cache_dir = "";
std::atomic<bool> g_database_complete{false};

static void crash_handler(int sig) {
    const char* msg = "\n[CRITICAL ERROR] piTrove intercepted a terminal fault / crash signal.\n";
    write(STDERR_FILENO, msg, strlen(msg));
    if (!g_database_complete.load() && !g_crash_cache_dir.empty()) {
        const char* purge_msg = "[CRITICAL] Database compilation was incomplete. Purging partial database records to protect state integrity...\n";
        write(STDERR_FILENO, purge_msg, strlen(purge_msg));
        std::string db_file = g_crash_cache_dir + "/cache.db";
        std::remove(db_file.c_str());
        std::remove((db_file + "-wal").c_str());
        std::remove((db_file + "-shm").c_str());
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void terminate_handler() {
    fprintf(stderr, "\n[CRITICAL ERROR] piTrove exited due to an unhandled C++ runtime exception.\n");
    if (!g_database_complete.load() && !g_crash_cache_dir.empty()) {
        fprintf(stderr, "[CRITICAL] Database compilation incomplete. Purging partial database records...\n");
        std::string db_file = g_crash_cache_dir + "/cache.db";
        std::remove(db_file.c_str());
        std::remove((db_file + "-wal").c_str());
        std::remove((db_file + "-shm").c_str());
    }
    std::abort();
}

// ── stb_image (raylib static lib already bundles these) ────────────────
// NOTE: Do NOT define STB_IMAGE_IMPLEMENTATION — raylib.a already includes it
// The raylib lib also bundles stb_image_resize2

static void* SafeMemAlloc(size_t size) {
    if (size > 0xFFFFFFFF) return nullptr;
    return MemAlloc((unsigned int)size);
}

// ── WebP decoder (libwebp) ─────────────────────────────────────────────
#include <webp/decode.h>
static Image LoadImageWebP(const std::string& path) {
    Image img = {0};
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return img;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return img; }
    uint8_t* buf = (uint8_t*)SafeMemAlloc(fsize);
    size_t nr = (buf) ? fread(buf, 1, (size_t)fsize, f) : 0;
    if (nr != (size_t)fsize) { MemFree(buf); buf = nullptr; }
    fclose(f);
    int w = 0, h = 0;
    if (!buf) return img;
    // FIX v1.6.6: copy rgba into MemAlloc'd buffer — WebPDecodeRGBA uses libwebp allocator,
    // not MemFree. After v1.6.5 UnloadImage fix, passing libwebp memory to RL_FREE would crash.
    uint8_t* rgba = WebPDecodeRGBA(buf, (size_t)fsize, &w, &h);
    MemFree(buf);
    if (rgba && w > 0 && h > 0) {
        // FIX v16.7.0: size_t cast prevents int * int * int overflow for large images
        size_t buf_sz = (size_t)w * (size_t)h * 4;
        img.data = SafeMemAlloc(buf_sz);
        if (img.data) {
            memcpy(img.data, rgba, buf_sz);
        }
        WebPFree(rgba);
    }
    if (img.data) {
        img.width = w;
        img.height = h;
        img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
        img.mipmaps = 1;
    }
    return img;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  JPEG custom error handler (for setjmp/longjmp error recovery)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
struct jpeg_err_handler {
    struct jpeg_error_mgr pub;
    jmp_buf setjmp_buffer;
};

static void jpeg_err_exit(j_common_ptr cinfo) {
    jpeg_err_handler *jerr = (jpeg_err_handler *)cinfo->err;
    longjmp(jerr->setjmp_buffer, 1);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  PNG memory read context (thread-safe - local per load, not global)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
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

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  ROBUST JPEG LOADER (libjpeg-turbo from memory)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
static Image LoadImageJPEG(const std::string& path) {
    Image img{0};
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return img;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return img; }
    uint8_t* buf = (uint8_t*)SafeMemAlloc(fsize);
    if (!buf) { fclose(f); return img; }
    size_t nr = fread(buf, 1, (size_t)fsize, f);
    if (nr != (size_t)fsize) { fclose(f); MemFree(buf); return img; }
    fclose(f);

    struct jpeg_decompress_struct cinfo;
    jpeg_err_handler jerr;

    jpeg_create_decompress(&cinfo);
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_err_exit;
    jpeg_mem_src(&cinfo, buf, (size_t)fsize);

    uint8_t* scanline = nullptr;
    uint8_t* rowbuf = nullptr;
    if (setjmp(jerr.setjmp_buffer)) {
        jpeg_destroy_decompress(&cinfo);
        MemFree(buf);
        if (scanline) MemFree(scanline);
        if (rowbuf) MemFree(rowbuf);
        if (img.data) { MemFree(img.data); img.data = nullptr; }
        return img;
    }
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_EXT_RGBA;
    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width;
    int h = (int)cinfo.output_height;
    int channels = (int)cinfo.output_components;

    img.width = w;
    img.height = h;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

    if (channels == 4) {
        // FIX v16.7.0: size_t cast prevents int overflow for large images
        img.data = SafeMemAlloc((size_t)w * h * 4);
        if (img.data) {
            uint8_t* scanline = (uint8_t*)SafeMemAlloc((size_t)w * 4);
            if (scanline) {
                JSAMPROW row = {scanline};
                int offset = 0;
                while (cinfo.output_scanline < h) {
                    jpeg_read_scanlines(&cinfo, &row, 1);
                    memcpy((uint8_t*)img.data + offset, scanline, w * 4);
                    offset += w * 4;
                }
                MemFree(scanline);
            } else {
                jpeg_abort_decompress(&cinfo);
                MemFree(img.data);
                img.data = nullptr;
            }
        }
    } else {
        // FIX v16.7.0: size_t cast prevents int overflow; channels==1 (grayscale) handled below
        img.data = SafeMemAlloc((size_t)w * h * 4);
        if (img.data) {
            uint8_t* rowbuf = nullptr;
            int rowbuf_stride = (channels == 1) ? w : (w * 3);
            rowbuf = (uint8_t*)SafeMemAlloc(rowbuf_stride);
            if (rowbuf) {
                while (cinfo.output_scanline < h) {
                    JSAMPROW row = {rowbuf};
                    jpeg_read_scanlines(&cinfo, &row, 1);
                    unsigned int row_num = cinfo.output_scanline;
                    uint8_t* dst = (uint8_t*)img.data + row_num * w * 4;
                    if (channels == 1) {
                        // Grayscale: libjpeg writes 1B/pixel, duplicate to RGB
                        for (int x = 0; x < w; x++) {
                            unsigned char g = rowbuf[x];
                            dst[x * 4 + 0] = g;
                            dst[x * 4 + 1] = g;
                            dst[x * 4 + 2] = g;
                            dst[x * 4 + 3] = 255;
                        }
                    } else {
                        // RGB: libjpeg writes 3B/pixel
                        for (int x = 0; x < w; x++) {
                            dst[x * 4 + 0] = rowbuf[x * 3 + 0];
                            dst[x * 4 + 1] = rowbuf[x * 3 + 1];
                            dst[x * 4 + 2] = rowbuf[x * 3 + 2];
                            dst[x * 4 + 3] = 255;
                        }
                    }
                }
                MemFree(rowbuf);
            } else {
                jpeg_abort_decompress(&cinfo);
                MemFree(img.data);
                img.data = nullptr;
            }
        }
    }

    if (img.data != nullptr) {
        jpeg_finish_decompress(&cinfo);
    }
    jpeg_destroy_decompress(&cinfo);
    MemFree(buf);
    return img;
}


// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  ROBUST PNG LOADER (libpng from memory)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
static Image LoadImagePNG(const std::string& path) {
    Image img{0};
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return img;
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0) { fclose(f); return img; }
    uint8_t* buf = (uint8_t*)SafeMemAlloc(fsize);
    if (!buf) { fclose(f); return img; }
    size_t nr = fread(buf, 1, (size_t)fsize, f);
    if (nr != (size_t)fsize) { fclose(f); MemFree(buf); return img; }
    fclose(f);

    png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    if (!png_ptr) { MemFree(buf); return img; }
    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, nullptr, nullptr);
        MemFree(buf);
        return img;
    }

    uint8_t* tmp_rgb = nullptr;
    png_bytep* rows = nullptr;
    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
        MemFree(buf);
        if (tmp_rgb) MemFree(tmp_rgb);
        if (rows) MemFree(rows);
        if (img.data) { MemFree(img.data); img.data = nullptr; }
        return img;
    }
    tmp_rgb = nullptr;
    rows = nullptr;

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

    img.width = width;
    img.height = height;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;

    if (color_type == PNG_COLOR_TYPE_RGB) {
        // FIX v16.7.0: size_t cast prevents int overflow for large images
        tmp_rgb = (uint8_t*)SafeMemAlloc((size_t)width * height * 3);
        img.data = SafeMemAlloc((size_t)width * height * 4);
        // v6.0.3: size_t cast prevents overflow for very large images
        rows = (png_bytep*)SafeMemAlloc((size_t)height * sizeof(png_bytep));
        if (tmp_rgb && img.data && rows) {
            for (int y = 0; y < height; y++) {
                rows[y] = (png_bytep)(tmp_rgb + y * width * 3);
            }
            png_read_image(png_ptr, rows);
            uint8_t* d = (uint8_t*)img.data;
            for (int i = 0; i < width * height; i++) {
                d[i * 4 + 0] = tmp_rgb[i * 3 + 0];
                d[i * 4 + 1] = tmp_rgb[i * 3 + 1];
                d[i * 4 + 2] = tmp_rgb[i * 3 + 2];
                d[i * 4 + 3] = 255;
            }
        } else {
            MemFree(img.data);
            img.data = nullptr;
        }
        MemFree(tmp_rgb);
        MemFree(rows);
    } else {
        // FIX v16.7.0: size_t cast prevents int overflow for large images
        img.data = SafeMemAlloc((size_t)width * height * 4);
        if (img.data) {
            // v6.0.3: size_t cast prevents overflow for very large images
            png_bytep* rows = (png_bytep*)SafeMemAlloc((size_t)height * sizeof(png_bytep));
            if (rows) {
                for (int y = 0; y < height; y++) {
                    rows[y] = (png_bytep)((uint8_t*)img.data + y * width * 4);
                }
                png_read_image(png_ptr, rows);
                MemFree(rows);
            } else {
                MemFree(img.data);
                img.data = nullptr;
            }
        }
    }

    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    MemFree(buf);
    return img;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  ROBUST TIFF LOADER (libtiff, file-backed since client I/O is tricky)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
static Image LoadImageTIFF(const std::string& path) {
    Image img{0};

    // For TIFF, try the file path directly first
    TIFF* tif = TIFFOpen(path.c_str(), "r");
    if (!tif) return img;

    uint32_t width = 0, height = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);

    if (width == 0 || height == 0) {
        TIFFClose(tif);
        return img;
    }

    img.width = (int)width;
    img.height = (int)height;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    img.mipmaps = 1;
    // FIX v16.7.0: size_t cast prevents int overflow for large images
    img.data = SafeMemAlloc((size_t)width * height * 4);
    if (!img.data) {
        TIFFClose(tif);
        return img;
    }

    uint16_t spp = 1;
    TIFFGetField(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
    // v6.0.3: Clamp spp to valid range (1-4) to prevent heap overflow from malicious TIFF
    if (spp < 1 || spp > 4) spp = 1;

    uint8_t* raw = (uint8_t*)SafeMemAlloc((size_t)width * spp);
    if (!raw) {

        MemFree(img.data);
        img.data = nullptr;
        TIFFClose(tif);
        return img;
    }

    uint32_t y = 0;
    int read_ok = 1;
    while (y < height) {
        if (TIFFReadScanline(tif, raw, y, 0) <= 0) {
            fprintf(stderr, "[WARN] TIFF partial read: row %u/%u — file may be corrupted\n", y, height);
            MemFree(img.data);
            img.data = nullptr;
            read_ok = 0;
            break;
        }

        uint8_t* dst = (uint8_t*)img.data + y * width * 4;

        if (spp == 1) {
            for (uint32_t x = 0; x < width; x++) {
                uint8_t g = raw[x];
                dst[x * 4 + 0] = g;
                dst[x * 4 + 1] = g;
                dst[x * 4 + 2] = g;
                dst[x * 4 + 3] = 255;
            }
        } else if (spp == 3) {
            for (uint32_t x = 0; x < width; x++) {
                dst[x * 4 + 0] = raw[x * 3 + 0];
                dst[x * 4 + 1] = raw[x * 3 + 1];
                dst[x * 4 + 2] = raw[x * 3 + 2];
                dst[x * 4 + 3] = 255;
            }
        } else if (spp == 4) {
            for (uint32_t x = 0; x < width; x++) {
                dst[x * 4 + 0] = raw[x * 4 + 0];
                dst[x * 4 + 1] = raw[x * 4 + 1];
                dst[x * 4 + 2] = raw[x * 4 + 2];
                dst[x * 4 + 3] = raw[x * 4 + 3];
            }
        } else {
            for (uint32_t x = 0; x < width; x++) {
                for (int c = 0; c < 4 && c < spp; c++) {
                    dst[x * 4 + c] = raw[x * spp + c];
                }
                dst[x * 4 + 3] = 255;
            }
        }
        y++;
    }

    MemFree(raw);
    TIFFClose(tif);
    return img;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  ROBUST IMAGE LOADER — wrapper with multi-format fallback
//  Tries stb_image first, then format-specific low-level loaders.
//  Extremely resilient to corruption.
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
static Image LoadImageRobust(const std::string& path) {
    // Strategy 1: Try stb_image (raylib's LoadImage)
    Image img = LoadImage(path.c_str());
    if (img.data != nullptr && img.width > 0 && img.height > 0) {
        return img;
    }
    // v16.2.0: Free potentially-allocated but invalid image from failed stb load
    if (img.data != nullptr) {
        UnloadImage(img);
        img = {0};
    }

    // Strategy 2: Check file magic bytes and try format-specific loader
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return img;

    uint8_t magic[4];
    int magic_len = (int)fread(magic, 1, 4, f);
    fclose(f);

    if (magic_len < 4) return img;

    // JPEG: starts with FF D8 FF
    if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) {
        img = LoadImageJPEG(path);
        if (img.data && img.width > 0 && img.height > 0) return img;
    }

    // PNG: starts with 89 50 4E 47
    if (magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47) {
        img = LoadImagePNG(path);
        if (img.data && img.width > 0 && img.height > 0) return img;
    }

    // TIFF: 49 49 2A 00 (LE) or 4D 4D 00 2A (BE)
    if ((magic[0] == 0x49 && magic[1] == 0x49 && magic[2] == 0x2A && magic[3] == 0x00) ||
        (magic[0] == 0x4D && magic[1] == 0x4D && magic[2] == 0x00 && magic[3] == 0x2A)) {
        img = LoadImageTIFF(path);
        if (img.data && img.width > 0 && img.height > 0) return img;
    }

    return img;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  Custom HEIC loader (stb_image doesn't support HEIC) ────────────────
static Image LoadImageHEIC(const std::string& path) {
    Image img = {0};
    heif_context* ctx = heif_context_alloc();
   if (!ctx) {
        TraceLog(LOG_ERROR, "HEIC: Failed to allocate heif_context");
        return img;
    }
    heif_error err = heif_context_read_from_file(ctx, path.c_str(), nullptr);
    if (err.code != heif_error_Ok) { heif_context_free(ctx); return img; }

    heif_image_handle* handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok) { heif_context_free(ctx); return img; }

    heif_image* heif_img = nullptr;
    err = heif_decode_image(handle, &heif_img, heif_colorspace_RGB,
                            heif_chroma_interleaved_RGB, nullptr);
    if (err.code != heif_error_Ok) {
        heif_image_handle_release(handle);
        heif_context_free(ctx);
        return img;
    }

    int w = heif_image_get_width(heif_img, heif_channel_interleaved);
    int h = heif_image_get_height(heif_img, heif_channel_interleaved);
    int stride = 0;
    const uint8_t* data = heif_image_get_plane_readonly(heif_img,
                                                        heif_channel_interleaved,
                                                        &stride);

    img.width = w;
    img.height = h;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    img.mipmaps = 1;
    img.data = SafeMemAlloc((size_t)w * h * 4);
 
    if (img.data) {
        uint8_t* dst = (uint8_t*)img.data;
        if (stride == w * 3) {
            for (size_t i = 0; i < (size_t)w * h; i++) {
                dst[i * 4 + 0] = data[i * 3 + 0];
                dst[i * 4 + 1] = data[i * 3 + 1];
                dst[i * 4 + 2] = data[i * 3 + 2];
                dst[i * 4 + 3] = 255;
            }
        } else {
            for (int y = 0; y < h; y++) {
                const uint8_t* row = data + y * stride;
                for (int x = 0; x < w; x++) {
                    dst[(y * w + x) * 4 + 0] = row[x * 3 + 0];
                    dst[(y * w + x) * 4 + 1] = row[x * 3 + 1];
                    dst[(y * w + x) * 4 + 2] = row[x * 3 + 2];
                    dst[(y * w + x) * 4 + 3] = 255;
                }
            }
        }
    }


    heif_image_release(heif_img);
    heif_image_handle_release(handle);
    heif_context_free(ctx);
    return img;
}

// ── NEON intrinsics (ARM64) ────────────────────────────────────────────

// ── GLSL Shaders ──────────────────────────────
const char* wipeShaderCode = R"(
    #version 100
    precision mediump float;
    varying vec2 fragTexCoord;
    uniform sampler2D texture0;
    uniform sampler2D texture1;
    uniform float progress;
    void main() {
        vec4 t0 = texture2D(texture0, fragTexCoord);
        vec4 t1 = texture2D(texture1, fragTexCoord);
        gl_FragColor = mix(t0, t1, progress);
    }
)";

const char* pixelateShaderCode = R"(
    #version 100
    precision mediump float;
    varying vec2 fragTexCoord;
    uniform sampler2D texture0;
    uniform sampler2D texture1;
    uniform float progress;
    void main() {
        float pixelSize = max(20.0 * (1.0 - progress), 0.001);
        vec2 uv = floor(fragTexCoord * (1.0 / pixelSize)) * pixelSize;
        vec4 t0 = texture2D(texture0, uv);
        vec4 t1 = texture2D(texture1, fragTexCoord);
        gl_FragColor = mix(t0, t1, progress);
    }
)";

// ── Fit + Ken Burns Shader (GPU-side scaling, native resolution, full image visible) ──
// Textures stay at native resolution — GPU handles fit scaling with letterboxing

// VRAM-safe texture loading — caps to display resolution, adds mipmaps + trilinear filtering
// Note: g_cfg isn't in scope at this point, so we use a sensible default.
// For 1080p displays this caps at 1920px; higher-res displays benefit from the
// same pixel budget since the screen can't resolve more anyway.

static Texture2D LoadTextureVRAMSafe(Image img) {
    if (img.data == nullptr || img.width <= 0 || img.height <= 0)
        return LoadTextureFromImage(img);

    const int MAX_DIM = 1920;
    Texture2D tex;

    if (img.width > MAX_DIM || img.height > MAX_DIM) {
        float scale = (float)MAX_DIM / (float)std::max(img.width, img.height);
        int nw = std::max(1, (int)(img.width  * scale));
        int nh = std::max(1, (int)(img.height * scale));

        // ImageCopy makes an independent deep copy so the caller's img.data
        // is never touched — without this, ImageResize frees the shared buffer
        // and the caller's pointer becomes dangling (-> double-free / SIGSEGV).
        Image resized = ImageCopy(img);
        ImageResize(&resized, nw, nh);
        tex = LoadTextureFromImage(resized);
        UnloadImage(resized);   // free the resized scratch copy - not the caller's data
    } else {
        tex = LoadTextureFromImage(img);
    }

    if (tex.id != 0) {
        GenTextureMipmaps(&tex);
        SetTextureFilter(tex, TEXTURE_FILTER_TRILINEAR);
    }

    return tex;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  CONFIG
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

struct Config {
    std::string media_dir;
    std::string cache_dir;
    std::string log_dir;
    std::string splash_file;
    int     screen_w{1920};
    int     screen_h{1080};
    bool    fullscreen{false};
    int     rotation{0};
    double  transition_delay{120.0};
    double  transition_duration{1.5};
    std::string transition_effect{"crossfade"};
    double  ken_burns_speed{0.1};
    bool    ken_burns{false};
    bool    matting{true};
    int     matting_size{48};
    bool    bias_lighting{true};
    float   bias_anim_speed{0.5f};
    std::string bias_anim_style{"edge_glow"};
     std::string bias_color_mode{"auto"};
    float   splash_overlay_y{0.5f};
    int     scan_depth{10};
    int     max_concurrent{4};
    bool    recursive{true};
    int     scan_window_days{15};
    long long cache_mmap_size{67108864};  // FIX v16.8.0: 64MB default (was 256MB) — safer on Pi 5 low-RAM
    bool    verbose{false};
    int     slideshow_fps{30};
    int     cooldown_days{330};

    // ---- Timer System ----
    bool    timer_enabled{true};
    float   timer_x{0.94f}, timer_y{0.03f};
    int     timer_font_size{12};
    std::string timer_color{"yellow"};
    bool    filename_enabled{true};
    float   filename_x{0.04f}, filename_y{0.966f};
    bool    count_enabled{false};
    float   count_x{0.5f}, count_y{0.02f};
    int     videos_per_photos{0};
      int     video_volume{0};
      int     video_probe_timeout{3};

     // [slideshow] advanced
     std::string sleep_time{""};      // e.g., "22:00"
    std::string wake_time{""};       // e.g., "07:00"

    // [dashboard]
    bool    weather_enabled{false};
    float   weather_lat{-999.0f};
    float   weather_lon{-999.0f};

    // [remote]
    bool    http_enabled{false};
    int     http_port{8080};

    // [date_overlay]
    bool    date_overlay_enabled{false};
    std::string date_text{"%Y-%m-%d"};
    float   date_x{0.1f}, date_y{0.08f};
    int     date_font_size{20};
    std::string date_color{"cyan"};

    // [brightness]
    bool    brightness_auto{false};
    int     brightness_auto_min{50};
    int     brightness_auto_max{100};

    // [touch]
    bool    touch_enabled{false};

    // [collage]
    bool    collage_enabled{false};
    int     collage_cols{2};
    int     collage_rows{2};

    // [display] advanced
    bool    auto_display_rotation{false};

    // [display] border
    bool    border_enabled{true};
    int     border_width{10};
    bool    vignette_enabled{true};

    // [slideshow] extended
    bool    shuffle{true};
    float   ken_burns_zoom{0.15f};
    int     bias_strength{110};

    // [overlay] clock
    bool    clock_enabled{false};
    float   clock_x{0.5f}, clock_y{0.96f};
    int     clock_font_size{18};
    std::string clock_color{"white"};
    bool    clock_24h{true};

    // [overlay] font sizes (were hardcoded)
    int     filename_font_size{12};
    int     count_font_size{20};

    std::vector<std::string> ignore_folders;
};

Config g_cfg;
static std::mutex g_config_mtx;  // v6.0.10: protects g_cfg from HTTP/write races (B4/B199)

static std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ── Safe string conversion helpers (v11.9.0 audit fix) ──
// v16.7.0: warn on parse failure so config typos are detectable
// (use fprintf since these functions are defined before g_logger declaration)
static int safe_stoi(const std::string& s, int def) {
    try { return std::stoi(s); } catch(...) { fprintf(stderr, "[WARN] Invalid integer in config: '%s' (default %d)\n", s.c_str(), def); return def; }
}
static float safe_stof(const std::string& s, float def) {
    try { return std::stof(s); } catch(...) { fprintf(stderr, "[WARN] Invalid float in config: '%s' (default %.2f)\n", s.c_str(), def); return def; }
}
static double safe_stod(const std::string& s, double def) {
    try { return std::stod(s); } catch(...) { fprintf(stderr, "[WARN] Invalid double in config: '%s' (default %.2f)\n", s.c_str(), def); return def; }
}
static long safe_stol(const std::string& s, long def) {
    try { return std::stol(s); } catch(...) { fprintf(stderr, "[WARN] Invalid long in config: '%s' (default %ld)\n", s.c_str(), (long)def); return def; }
}
static long long safe_stoll(const std::string& s, long long def) {
    try { return std::stoll(s); } catch(...) { fprintf(stderr, "[WARN] Invalid long long in config: '%s' (default %lld)\n", s.c_str(), def); return def; }
}

Config load_config(const char* config_path) {
    Config c;
    std::string path = std::string(config_path);
    std::ifstream f(path);
    if (!f.is_open()) return c;

    auto trim = [](std::string s) -> std::string {
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        if (s.empty()) return s;
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
        return s;
    };
    auto safe_stoi = [](const std::string& v, int def) -> int {
        try { return std::stoi(v); } catch (...) { return def; }
    };
    auto safe_stof = [](const std::string& v, float def) -> float {
        try { return std::stof(v); } catch (...) { return def; }
    };
    auto safe_stod = [](const std::string& v, double def) -> double {
        try { return std::stod(v); } catch (...) { return def; }
    };
    auto safe_stol = [](const std::string& v, long def) -> long {
        try { return std::stol(v); } catch (...) { return def; }
    };
    auto safe_stoll = [](const std::string& v, long long def) -> long long {
        try { return std::stoll(v); } catch (...) { return def; }
    };

    std::string section;
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }

        if (key == "media_dir")              c.media_dir = val;
        else if (key == "cache_dir")         c.cache_dir = val;
        else if (key == "log_dir")           c.log_dir = val;
        else if (key == "splash_file")       c.splash_file = val;
        else if (key == "fullscreen")        c.fullscreen = (val == "1" || val == "true");
        else if (key == "rotation")          c.rotation = safe_stoi(val, c.rotation);
        else if (key == "splash_overlay_y")  c.splash_overlay_y = safe_stof(val, c.splash_overlay_y);
        else if (key == "timer_enabled")     c.timer_enabled = (val == "1" || val == "true");
        else if (key == "timer_x")           c.timer_x = safe_stof(val, c.timer_x);
        else if (key == "timer_y")           c.timer_y = safe_stof(val, c.timer_y);
        else if (key == "timer_font_size")   c.timer_font_size = safe_stoi(val, c.timer_font_size);
        else if (key == "timer_color")        c.timer_color = val;
        else if (key == "filename_enabled")  c.filename_enabled = (val == "1" || val == "true");
        else if (key == "filename_x")        c.filename_x = safe_stof(val, c.filename_x);
        else if (key == "filename_y")        c.filename_y = safe_stof(val, c.filename_y);
        else if (key == "count_enabled")     c.count_enabled = (val == "1" || val == "true");
        else if (key == "count_x")           c.count_x = safe_stof(val, c.count_x);
        else if (key == "count_y")           c.count_y = safe_stof(val, c.count_y);
        else if (key == "videos_per_photos") c.videos_per_photos = safe_stoi(val, c.videos_per_photos);
        else if (key == "sleep_time")        c.sleep_time = val;
        else if (key == "wake_time")         c.wake_time = val;
        else if (key == "weather_enabled")   c.weather_enabled = (val == "1" || val == "true");
        else if (key == "weather_lat")       c.weather_lat = safe_stof(val, c.weather_lat);
        else if (key == "weather_lon")       c.weather_lon = safe_stof(val, c.weather_lon);
        else if (key == "http_enabled")      c.http_enabled = (val == "1" || val == "true");
        else if (key == "http_port") {
            int p = safe_stoi(val, c.http_port);
            c.http_port = (p >= 1 && p <= 65535) ? p : 8080;
        }
        else if (key == "volume")            c.video_volume = safe_stoi(val, c.video_volume);
        else if (key == "probe_timeout")     c.video_probe_timeout = safe_stoi(val, c.video_probe_timeout);
        else if (key == "enabled" && section == "date_overlay") c.date_overlay_enabled = (val == "1" || val == "true");
        else if (key == "text" && section == "date_overlay")    c.date_text = val;
        else if (key == "x" && section == "date_overlay")       c.date_x = safe_stof(val, c.date_x);
        else if (key == "y" && section == "date_overlay")       c.date_y = safe_stof(val, c.date_y);
        else if (key == "font_size" && section == "date_overlay") c.date_font_size = safe_stoi(val, c.date_font_size);
        else if (key == "color" && section == "date_overlay")   c.date_color = val;
        else if (key == "enabled" && section == "touch")        c.touch_enabled = (val == "1" || val == "true");
        else if (key == "enabled" && section == "collage")      c.collage_enabled = (val == "1" || val == "true");
        else if (key == "cols")              c.collage_cols = safe_stoi(val, c.collage_cols);
        else if (key == "rows")              c.collage_rows = safe_stoi(val, c.collage_rows);
        else if (key == "transition_delay")  c.transition_delay = safe_stod(val, c.transition_delay);
        else if (key == "transition_duration") c.transition_duration = safe_stod(val, c.transition_duration);
        else if (key == "slideshow_fps")     c.slideshow_fps = safe_stoi(val, c.slideshow_fps);
        else if (key == "transition_effect") c.transition_effect = val;
        else if (key == "ken_burns_speed")   c.ken_burns_speed = safe_stod(val, c.ken_burns_speed);
        else if (key == "ken_burns")         c.ken_burns = (val == "1" || val == "true");
        else if (key == "matting")           c.matting = (val == "1" || val == "true");
        else if (key == "matting_size")      c.matting_size = safe_stoi(val, c.matting_size);
        else if (key == "bias_lighting")     c.bias_lighting = (val == "1" || val == "true");
        else if (key == "bias_anim_speed")   c.bias_anim_speed = safe_stof(val, c.bias_anim_speed);
        else if (key == "bias_anim_style")   c.bias_anim_style = val;
        else if (key == "bias_color_mode")   c.bias_color_mode = val;
        else if (key == "cooldown_days")     c.cooldown_days = safe_stoi(val, c.cooldown_days);
        else if (key == "auto" || key == "brightness_auto") c.brightness_auto = (val == "1" || val == "true");
        else if (key == "auto_min" || key == "brightness_auto_min") c.brightness_auto_min = safe_stoi(val, c.brightness_auto_min);
        else if (key == "auto_max" || key == "brightness_auto_max") c.brightness_auto_max = safe_stoi(val, c.brightness_auto_max);
        else if (key == "border_enabled")    c.border_enabled = (val == "1" || val == "true");
        else if (key == "border_width")      c.border_width = safe_stoi(val, c.border_width);
        else if (key == "vignette_enabled")  c.vignette_enabled = (val == "1" || val == "true");
        else if (key == "shuffle")           c.shuffle = !(val == "0" || val == "false");
        else if (key == "ken_burns_zoom")    c.ken_burns_zoom = safe_stof(val, c.ken_burns_zoom);
        else if (key == "bias_strength")     c.bias_strength = safe_stoi(val, c.bias_strength);
        else if (key == "clock_enabled")     c.clock_enabled = (val == "1" || val == "true");
        else if (key == "clock_x")           c.clock_x = safe_stof(val, c.clock_x);
        else if (key == "clock_y")           c.clock_y = safe_stof(val, c.clock_y);
        else if (key == "clock_font_size")   c.clock_font_size = safe_stoi(val, c.clock_font_size);
        else if (key == "clock_color")       c.clock_color = val;
        else if (key == "clock_24h")         c.clock_24h = (val == "1" || val == "true");
        else if (key == "filename_font_size") c.filename_font_size = safe_stoi(val, c.filename_font_size);
        else if (key == "count_font_size")    c.count_font_size = safe_stoi(val, c.count_font_size);
        else if (key == "recursive")         c.recursive = (val == "1" || val == "true");
        else if (key == "depth")             c.scan_depth = safe_stoi(val, c.scan_depth);
        else if (key == "max_concurrent")    c.max_concurrent = safe_stoi(val, c.max_concurrent);
        else if (key == "window_days")       c.scan_window_days = safe_stoi(val, c.scan_window_days);
        else if (key == "mmap_size")         c.cache_mmap_size = safe_stoll(val, c.cache_mmap_size);
        else if (key == "level")             c.verbose = (val == "debug");
        else if (key == "resolution") {
            auto comma = val.find(',');
            if (comma != std::string::npos) {
                c.screen_w = safe_stoi(val.substr(0, comma), c.screen_w);
                c.screen_h = safe_stoi(val.substr(comma + 1), c.screen_h);
            }
        }
    }

    return c;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  FRAMEBUFFER BUFFER MANAGER (onscreen/offscreen control)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// v16.2.0: removed duplicate includes (fcntl.h, sys/file.h,
//          dirent.h, unistd.h, cstring already included above)
// v16.3.0: slide_debug thread-safety, spawn zombie reaping,
//           strncpy null-terminator, buffer overflow fixes
// v16.4.0: va_list UAF fix (C1), of data race (C2), localtime_r (C3-C5),
//           delete dead code (H2-H3), weather timeout (H4), slide_debug scope (M1),
//           execv dangling c_str (L5-L6), duplicate brace (L2), indent (L3)
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <future>
#include <cstdint>
#include <vector>

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  LOGGER
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

enum class LogLevel { INFO, WARN, ERROR, DEBUG };

static Color overlay_color_from_str(const std::string& name) {
    if (name == "red")    return (Color){255, 0, 0, 255};
    if (name == "green")  return (Color){0, 255, 0, 255};
    if (name == "blue")   return (Color){0, 0, 255, 255};
    if (name == "yellow") return (Color){255, 255, 0, 255};
    if (name == "cyan")   return (Color){0, 255, 255, 255};
    if (name == "white")  return WHITE;
    if (name == "gray")   return DARKGRAY;
    return (Color){200, 200, 200, 255};
}

struct Logger {
    LogLevel   level{LogLevel::INFO};
    std::string log_dir;
    std::string log_file_path;

    // Async queue
    std::mutex   queue_mtx;
    std::condition_variable cv;
    std::vector<std::string> front_queue;
    std::vector<std::string> back_queue;
    std::thread flush_thread;
    std::atomic<bool> flush_running{true};

    void flush_loop() {
        while (flush_running.load() || !front_queue.empty()) {
            {
                std::unique_lock<std::mutex> lock(queue_mtx);
                cv.wait(lock, [this] { return !front_queue.empty() || !flush_running.load(); });
                std::swap(front_queue, back_queue);
            }
            if (!back_queue.empty()) {
                FILE* f = fopen(log_file_path.c_str(), "a");
                for (const auto& msg : back_queue) {
                    write(STDOUT_FILENO, msg.c_str(), msg.size());
                    if (f) fprintf(f, "%s", msg.c_str());
                }
                if (f) { fclose(f); fflush(stdout); }
                back_queue.clear();
            }
        }
    }

    void init(const std::string& path, LogLevel lvl) {
        level = lvl;
        log_dir = path;
        std::filesystem::create_directories(path);

        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        char fname[128];
        struct tm tm_buf;
        std::strftime(fname, sizeof(fname), "piTrove_%Y%m%d_%H%M%S.log", localtime_r(&t, &tm_buf));
        log_file_path = path + "/" + fname;

        // Rotate: keep only last 3 log files
        rotate_logs(path, 3);

        flush_thread = std::thread(&Logger::flush_loop, this);
    }

    ~Logger() {
        flush_running.store(false);
        cv.notify_one();
        if (flush_thread.joinable()) flush_thread.join();
    }

    void rotate_logs(const std::string& dir, int keep) {
        std::vector<std::string> files;
        for (const auto& entry : std::filesystem::directory_iterator(dir)) {
            std::string fn = entry.path().filename().string();
            if (fn.find("piTrove_") == 0 && fn.find(".log") != std::string::npos) {
                files.push_back(entry.path().string());
            }
        }
        std::sort(files.begin(), files.end());
        while ((int)files.size() > keep) {
            std::filesystem::remove(files.front());
            files.erase(files.begin());
        }
    }

    void log(LogLevel lvl, const char* fmt, ...) {
        if (lvl < level) return;

        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        const char* tag = (lvl == LogLevel::WARN)  ? "WARN"
                          : (lvl == LogLevel::ERROR) ? "ERROR"
                          : (lvl == LogLevel::DEBUG) ? "DEBUG"
                                                     : "INFO";

        char header[64];
        struct tm tm_buf2;
        std::strftime(header, sizeof(header), "%Y-%m-%d %H:%M:%S", localtime_r(&t, &tm_buf2));

        char line[512];
        int n = std::snprintf(line, sizeof(line), "%s.%03ld [%s] ", header, (long)ms.count(), tag);

        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(line + n, sizeof(line) - (size_t)n, fmt, ap);
        va_end(ap);

        std::string final_line;
        final_line.reserve(n + 512);
        final_line += line;
        final_line += '\n';

        {
            std::lock_guard<std::mutex> lock(queue_mtx);
            front_queue.push_back(std::move(final_line));
        }
        cv.notify_one();
    }

    void info(const char* fmt, ...) {
        char buf[4096];
        va_list ap; va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        log(LogLevel::INFO, "%s", buf);
    }
    void warn(const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        char buf[4096];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        log(LogLevel::WARN, "%s", buf);
    }
    void error(const char* fmt, ...) {
        va_list ap; va_start(ap, fmt);
        char buf[4096];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        log(LogLevel::ERROR, "%s", buf);
    }
    void debug(const char* fmt, ...) {
        if (level < LogLevel::DEBUG) return;
        va_list ap; va_start(ap, fmt);
        char buf[4096];
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        log(LogLevel::DEBUG, "%s", buf);
    }
};

Logger g_logger;

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  v3.0.0: MPVPlayer — in-process mpv render API (merged from MPVPlayer.h/.cpp)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Logging interface — implemented via _mpv_log_error/_mpv_log_info below
typedef void (*mpv_log_fn)(const char* fmt, ...);

extern Config g_cfg;

struct MPVPlayer {
     mpv_handle *ctx = nullptr;
     mpv_render_context *gl_ctx = nullptr;
     bool initialized = false;
     std::atomic<bool> playing{false};
     std::atomic<bool> eof{false};
     std::string current_file;
     std::mutex play_mutex;
     int surface_w{1920};
     int surface_h{1080};
     int video_volume{0};
     RenderTexture2D video_rt{};
     
     // Smooth EGL tracking
     EGLDisplay egl_dpy = EGL_NO_DISPLAY;
     EGLContext egl_ctx = EGL_NO_CONTEXT;
     EGLSurface egl_surf_draw = EGL_NO_SURFACE;
     EGLSurface egl_surf_read = EGL_NO_SURFACE;

     // Asynchronous property state counters
     std::atomic<double> video_time_remaining{0.0};
     std::atomic<double> video_duration{0.0};

     // Event drain thread — keeps mpv's event queue empty so commands don't block.
      // Without this, mpv_command() blocks when the queue fills (~1000 events) →
      // main thread freezes → process killed → green screen on restart.
      std::thread event_thread;
      std::atomic<bool> event_thread_stop{false};

      bool init();
      void destroy();
      bool play(const std::string &path);
      void stop();
      bool update_frame();
      void make_egl_current();
      void release_egl_current();
      bool is_playing() const { return playing.load(); }
      bool is_initialized() const { return initialized; }
      bool has_eof() const { return eof.load(); }
  };

static MPVPlayer g_mpv;
std::atomic<bool> g_mpv_frame_available{false};


// Update callback — called from mpv's internal thread when a frame is available
// Must be fast — just set an atomic flag (no libmpv API calls)
static void mpv_update_callback(void *ctx) {
    (void)ctx;
    g_mpv_frame_available.store(true);
}

// v3.0.0: mpv logging function pointers — bridge to g_logger
static void _mpv_log_error(const char* fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    g_logger.error("%s", buf);
}
static void _mpv_log_info(const char* fmt, ...) {
    char buf[512]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    g_logger.info("%s", buf);
}
mpv_log_fn g_mpv_log_error = _mpv_log_error;
mpv_log_fn g_mpv_log_info = _mpv_log_info;

// Resolve GL function pointers via eglGetProcAddress.
// dlsym(RTLD_DEFAULT) misses GLES2 extension functions that are only exposed
// through EGL — mpv gets NULL pointers and crashes on first use.
// eglGetProcAddress is safe here because init() guards on eglGetCurrentContext()
// succeeding before this callback is ever registered.
static void* mpv_get_proc_address(void* ctx, const char* name) {
    (void)ctx;
    void* ptr = (void*)eglGetProcAddress(name);
    if (!ptr) ptr = dlsym(RTLD_DEFAULT, name);  // fallback for core symbols
    return ptr;
}

// ── MPVPlayer method implementations ──

bool MPVPlayer::init() {
    if (initialized) return true;

    egl_dpy = eglGetCurrentDisplay();
    egl_ctx = eglGetCurrentContext();
    egl_surf_draw = eglGetCurrentSurface(EGL_DRAW);
    egl_surf_read = eglGetCurrentSurface(EGL_READ);

    if (egl_dpy == EGL_NO_DISPLAY || egl_ctx == EGL_NO_CONTEXT) {
        g_logger.error("MPV_INIT: No current EGL context available.");
        return false;
    }

    ctx = mpv_create();
    if (!ctx) return false;

    mpv_set_option_string(ctx, "msg-level", "all=warn");
    mpv_set_option_string(ctx, "gpu-api", "opengl");
    mpv_set_option_string(ctx, "opengl-es", "yes");
    mpv_set_option_string(ctx, "vo", "libmpv"); // Required for mpv_render_context API
    
    // Pi 5 FBO Compatibility Optimization
    mpv_set_option_string(ctx, "hwdec", "v4l2m2m-copy"); 
    mpv_set_option_string(ctx, "audio", "no");
    mpv_set_option_string(ctx, "vd-lavc-skiploopfilter", "nonref");
    mpv_set_option_string(ctx, "vd-lavc-threads", "4");
    mpv_set_option_string(ctx, "sws-scaler", "fast-bilinear");
    mpv_set_option_string(ctx, "video-output-levels", "full");

    char vol[16];
    snprintf(vol, sizeof(vol), "%d", video_volume);
    mpv_set_option_string(ctx, "volume", vol);

    if (mpv_initialize(ctx) < 0) {
        mpv_destroy(ctx);
        ctx = nullptr;
        return false;
    }

    // Register async property listeners before spinning loop up
    mpv_observe_property(ctx, 0, "time-remaining", MPV_FORMAT_DOUBLE);
    mpv_observe_property(ctx, 0, "duration", MPV_FORMAT_DOUBLE);

    mpv_opengl_init_params gl_init = { mpv_get_proc_address, nullptr };
    mpv_render_param mpv_params[] = {
        {MPV_RENDER_PARAM_API_TYPE,           (void*)MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init},
        {MPV_RENDER_PARAM_ADVANCED_CONTROL,   (int[]){1}},
        {MPV_RENDER_PARAM_INVALID,            nullptr}
    };

    if (mpv_render_context_create(&gl_ctx, ctx, mpv_params) < 0) {
        mpv_destroy(ctx);
        ctx = nullptr;
        return false;
    }

    video_rt = LoadRenderTexture(surface_w, surface_h);
    mpv_render_context_set_update_callback(gl_ctx, mpv_update_callback, nullptr);

    event_thread_stop.store(false);
    try {
        event_thread = std::thread([this]() {
            while (!event_thread_stop.load()) {
                mpv_event *ev = mpv_wait_event(ctx, 0.02);
                if (!ev || ev->event_id == MPV_EVENT_NONE) continue;
                if (ev->event_id == MPV_EVENT_SHUTDOWN) break;
                
                // Track streaming properties asynchronously
                if (ev->event_id == MPV_EVENT_PROPERTY_CHANGE) {
                    mpv_event_property *prop = (mpv_event_property*)ev->data;
                    if (prop && prop->name) {
                        if (strcmp(prop->name, "time-remaining") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                            double *val = (double*)prop->data;
                            if (val) video_time_remaining.store(*val);
                        } else if (strcmp(prop->name, "duration") == 0 && prop->format == MPV_FORMAT_DOUBLE) {
                            double *val = (double*)prop->data;
                            if (val) video_duration.store(*val);
                        }
                    }
                }

                if (ev->event_id == MPV_EVENT_END_FILE) {
                    playing.store(false);
                    eof.store(true);
                }
            }
        });
    } catch (...) {
        if (video_rt.id != 0) { UnloadRenderTexture(video_rt); video_rt = {}; }
        return false;
    }

    initialized = true;
    return true;
}

void MPVPlayer::destroy() {
     // Stop event thread first — it holds a reference to ctx
     event_thread_stop.store(true);
     if (ctx) mpv_wakeup(ctx); // unblock mpv_wait_event immediately
     if (event_thread.joinable()) event_thread.join();

     if (gl_ctx) {
         mpv_render_context_free(gl_ctx);
         gl_ctx = nullptr;
     }
     if (ctx) {
         mpv_destroy(ctx);
         ctx = nullptr;
     }
     if (video_rt.id != 0) {
         UnloadRenderTexture(video_rt);
         video_rt = {};
     }
      initialized = false;
      playing.store(false);
      eof.store(false);
      {
          std::lock_guard<std::mutex> lock(play_mutex);
          current_file.clear();
      }
      if (g_mpv_log_info) g_mpv_log_info("MPVPlayer destroyed");

  }

// Make Raylib's EGL context current before mpv render.
// mpv's render context creates its own internal EGL context in mpv_render_context_create().
// When mpv_render_context_render() is called, it internally calls eglMakeCurrent() with
// its own EGL context. On GLES2/DRM this conflicts with Raylib's active EGL context,
// causing a segfault. By explicitly making Raylib's context current first, we ensure
// the EGL state is consistent before mpv does its internal context switching.
void MPVPlayer::make_egl_current() {
    if (egl_dpy != EGL_NO_DISPLAY && egl_ctx != EGL_NO_CONTEXT) {
        eglMakeCurrent(egl_dpy, egl_surf_draw, egl_surf_read, egl_ctx);
    }
}

void MPVPlayer::release_egl_current() {
    if (egl_dpy != EGL_NO_DISPLAY && egl_ctx != EGL_NO_CONTEXT) {
        eglMakeCurrent(egl_dpy, egl_surf_draw, egl_surf_read, egl_ctx);
    }
}

bool MPVPlayer::play(const std::string &path) {
     if (!initialized) return false;

     // CRITICAL: Force Raylib's EGL context current before ANY mpv command.
     // mpv_command_string("loadfile ... play-now") synchronously spins up
     // mpv's playback pipeline which probes GL/EGL state to allocate textures,
     // compile shaders, and configure FBOs. Without Raylib's EGL context current,
     // mpv's internal EGL calls dereference stale/null function pointers → SEGV.
     make_egl_current();

     // Use mpv_command() (array form) — more robust than mpv_command_string()
     // when mpv's GL state is being queried synchronously.
        // Use the path directly; mpv_command (array form) does not require shell escaping
        std::string path_buf = path;
        
        int rc = -1;
        if (ctx) {
            const char *args[] = {"loadfile", path.c_str(), nullptr};
            rc = mpv_command(ctx, args);
            if (rc < 0) {
                if (g_mpv_log_error) g_mpv_log_error("MPV_PLAY: mpv_command loadfile failed: %d ctx=%p", rc, (void*)ctx);
            } else {
                if (g_mpv_log_info) g_mpv_log_info("MPV_PLAY: loadfile succeeded, loading='%s'", path_buf.c_str());
                mpv_set_property_string(ctx, "pause", "no");
            }
        } else {
            if (g_mpv_log_error) g_mpv_log_error("MPV_PLAY: mpv_command ctx is NULL");
        }


        // Restore Raylib's EGL context after mpv's synchronous command.
        release_egl_current();

      // Verify mpv loaded the file
      if (ctx) {
          const char *fname = mpv_get_property_string(ctx, "filename");
          if (fname) {
              if (g_mpv_log_info) g_mpv_log_info("MPV_VERIFY: filename='%s'", fname);
              mpv_free((void*)fname);
          } else {
              if (g_mpv_log_info) g_mpv_log_info("MPV_VERIFY: filename property returned null (ctx=%p)", (void*)ctx);
          }
      } else {
          if (g_mpv_log_error) g_mpv_log_error("MPV_VERIFY: ctx is NULL");
      }

      playing.store(true);
      eof.store(false);
      {
          std::lock_guard<std::mutex> lock(play_mutex);
          current_file = path;
      }
      if (g_mpv_log_info) g_mpv_log_info("MPV_PLAY: '%s'", path.substr(0, 80).c_str());

    return (rc >= 0);
}

void MPVPlayer::stop() {
    std::lock_guard<std::mutex> lock(play_mutex);
    if (playing) {
        mpv_command_string(ctx, "stop");
        playing.store(false);
        eof.store(true);
        if (g_mpv_log_info) g_mpv_log_info("MPV_STOP: playback stopped");
    }
}

bool MPVPlayer::update_frame() {
      if (!initialized || !playing) return false;

      // Make sure Raylib's EGL context is current before any mpv render API call
      make_egl_current();

      // Check if mpv has a new frame ready
      uint64_t update_flags = mpv_render_context_update(gl_ctx);
      if (!(update_flags & MPV_RENDER_UPDATE_FRAME)) {
          release_egl_current();
          return false;
      }

      mpv_opengl_fbo fbo = {0};
      fbo.fbo = (int)video_rt.id;   
      fbo.w   = surface_w;
      fbo.h   = surface_h;
      fbo.internal_format = 0x1908; // GLES2: explicit GL_RGBA (0 = auto-detect fails silently on DRM)

      int flip_y = 1;
      // FIX: Removed BLOCK_FOR_TARGET_TIME — 32-bit int causes stack corruption with mpv's 64-bit uint64_t* on ARM64
      mpv_render_param render_params[] = {
              {MPV_RENDER_PARAM_OPENGL_FBO,            &fbo},
              {MPV_RENDER_PARAM_FLIP_Y,                &flip_y},
              {MPV_RENDER_PARAM_INVALID,               nullptr}
      };

       int ret = mpv_render_context_render(gl_ctx, render_params);
       if (ret < 0) {
           g_logger.error("MPV_RENDER: render failed ret=%d fbo=%d w=%d h=%d", ret, (int)video_rt.id, surface_w, surface_h);
       } else {
           // Verify texture was written by checking for non-zero pixels (best-effort)
           g_logger.info("MPV_RENDER: frame written fbo=%d w=%d h=%d", (int)video_rt.id, surface_w, surface_h);
       }
       
       // CRITICAL: Always reset FBO and Viewport before checking return value,
       // as mpv_render_context_render leaves the FBO bound regardless of success.
       glBindFramebuffer(GL_FRAMEBUFFER, 0);
       rlViewport(0, 0, GetScreenWidth(), GetScreenHeight());
       
       if (ret < 0) {
           if (g_mpv_log_error) g_mpv_log_error("MPV_RENDER: mpv_render_context_render failed: %d", ret);
           release_egl_current();
           return false;
       }
       
       // Report swap — tells mpv the frame was presented, for A/V sync timing
       mpv_render_context_report_swap(gl_ctx);
       
       // ── FIX v7.0.5: Safe OpenGL state reset via rlgl APIs ──

      // mpv alters internal VBOs, shaders, and texture bindings.
      // glActiveTexture + glBindTexture resets OpenGL texture state.
      // rlDisableShader() safely restores Raylib's default shader via rlgl.
      // REMOVED: raw glBindBuffer calls — desynced rlgl's VBO cache (v7.0.3).
      // REMOVED: rlBindTexture — not available on Pi's Raylib (GLES2) build.
      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, 0);
      rlDisableShader();
      // ─────────────────────────────────────────────────────────

      release_egl_current();

      // FIX: Removed mpv_get_property("eof-reached") — 60fps polling floods IPC, causes deadlock/crash.
      // EOF is already safely handled asynchronously by the event_thread.

      return true;
  }


// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  NEON HELPERS
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static bool has_extension(std::string_view path, std::string_view ext) {
    // The scanner extracts the extension first, so sizes must match exactly.
    if (path.size() != ext.size()) return false;
    for (size_t i = 0; i < ext.size(); ++i) {
        if (tolower((unsigned char)path[i]) != tolower((unsigned char)ext[i])) return false;
    }
    return true;
}

static const char* IMAGE_EXTS[] = {"jpg", "jpeg", "png", "bmp", "tga", "gif", "webp", "tiff", "tif", "heic", "heif"};
static const char* VIDEO_EXTS[] = {"mp4", "mkv", "avi", "mov", "webm", "m4v"};

static bool is_image(std::string_view path) {
    for (auto ext : IMAGE_EXTS)
        if (has_extension(path, ext)) return true;
    return false;
}

static bool is_video(std::string_view path) {
    for (auto ext : VIDEO_EXTS)
        if (has_extension(path, ext)) return true;
    return false;
}

static bool is_media(std::string_view path) {
    return is_image(path) || is_video(path);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  VIDEO DURATION PROBE (ffprobe, v1.8.5)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// v6.0.3: Forward declaration — run_ffprobe is defined later in this file
static std::string run_ffprobe(const std::vector<std::string>& args, int timeout_ms);

static double probe_video_duration(const std::string& path, int timeout_ms) {
    if (!std::filesystem::exists(path)) return 0.0;

    // v6.0.3: Use run_ffprobe() instead of popen() — fork+exec+poll+SIGKILL
    // popen() cannot be killed mid-operation on CIFS; run_ffprobe has proper watchdog
    std::string json = run_ffprobe({"-v","quiet","-print_format","json","-show_format","-show_streams", path}, timeout_ms);

    // Find "duration" field in the top-level format object
    // Format: "duration":"245.123456"
    std::string dur_key = "\"duration\":";
    size_t dur_pos = json.find(dur_key);
    if (dur_pos == std::string::npos) return 0.0;
    dur_pos += dur_key.size();

    // Skip whitespace
    while (dur_pos < json.size() && (json[dur_pos] == ' ' || json[dur_pos] == '\t')) dur_pos++;

    // Extract numeric string
    std::string dur_str;
    while (dur_pos < json.size() && (isdigit(json[dur_pos]) || json[dur_pos] == '.')) {
        dur_str += json[dur_pos++];
    }

    if (dur_str.empty()) return 0.0;

    try {
        double dur = std::stod(dur_str);
        return (dur > 0.1) ? dur : 0.0;
    } catch (...) {
        return 0.0;
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  MEDIA ITEM
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

struct MediaItem {
    std::string path;
    std::string filename;
    std::string ext;
    std::string type{"image"};
    int64_t     width{0};
    int64_t     height{0};
    double      duration{0.0};
    int         exif_rotation{0};
    int64_t     file_size{0};
    int64_t     modified_time{0};
    bool        cached{false};
    int64_t     last_shown{0};
};

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  EXIF READER (libexif)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static int read_exif_rotation(const char* path) {
    int rotation = 1;
    ExifData* ed = exif_data_new_from_file(path);
    if (!ed) return rotation;

    ExifEntry* entry = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
    if (!entry || entry->size < 2 || entry->format != EXIF_FORMAT_SHORT) {
        exif_data_unref(ed);
        return rotation;
    }

    unsigned short val = exif_get_short(entry->data,
                                        exif_data_get_byte_order(ed));
    if (val >= 1 && val <= 8) rotation = val;
    exif_data_unref(ed);
    return rotation;
}

// Timeout wrapper for EXIF rotation - prevents CIFS hangs

// v3.0.4: Full EXIF orientation handling (all 8 cases with flips)
 static bool apply_exif_rotation(Image& img, int exif) {
     if (exif < 2 || exif > 8) return false;
     switch (exif) {
         case 2: ImageFlipHorizontal(&img); break;
         case 3: ImageFlipHorizontal(&img); ImageFlipVertical(&img); break;
         case 4: ImageFlipVertical(&img); break;
         case 5: ImageRotateCW(&img); ImageFlipHorizontal(&img); break;
         case 6: ImageRotateCW(&img); break;
         case 7: ImageRotateCW(&img); ImageFlipVertical(&img); break;
         case 8: ImageRotateCCW(&img); break;
     }
     return true;
 }

  // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
  //  IMAGE LOADER (uses raylib's LoadImage which bundles stb_image)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  GETDENTS64 SCANNER (CIFS-safe, multi-threaded)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static ssize_t get_dents64(int fd, char* buf, size_t bufsz) {
    return static_cast<ssize_t>(syscall(SYS_getdents64, fd, buf, bufsz));
}

static std::vector<std::string> read_dir(const std::string& path) {
    std::vector<std::string> entries;
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NONBLOCK);
    if (fd < 0) return entries;

    char buf[65536];
    ssize_t n;
    while ((n = get_dents64(fd, buf, sizeof(buf))) > 0) {
        char* p = buf;
        while (p < buf + n) {
            auto* de = reinterpret_cast<struct dirent64*>(p);
            if (de->d_name[0] != '.' || de->d_name[1] == '\0') {
                entries.emplace_back(de->d_name);
            }
            p += de->d_reclen;
        }
    }
    close(fd);
    return entries;
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  v3.0.3: TimeoutState — shared lifetime for detached threads
//  Prevents stack UAF: when wait_for times out and the thread is
//  pthread_detached, the function returns and all local vars are
//  destroyed. If the detached thread later wakes from the blocked
//  syscall, it writes to destroyed memory → SIGSEGV / heap corruption.
//  Bundling into shared_ptr ensures lifetime extends until the thread
//  actually completes.
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
struct TimeoutState {
    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;
};
static int read_exif_rotation_timeout(const std::string& path, int timeout_ms = 5000) {
    auto result = std::make_shared<int>(1);
    auto state = std::make_shared<TimeoutState>();
    std::thread t([path, result, state]() {
        *result = read_exif_rotation(path.c_str());
        {
            std::lock_guard<std::mutex> lk(state->mtx);
            state->done = true;
        }
        state->cv.notify_one();
    });
    std::unique_lock<std::mutex> lk2(state->mtx);
    state->cv.wait_for(lk2, std::chrono::milliseconds(timeout_ms), [&]() { return state->done; });
    if (!state->done) {
        std::string msg="exif rotation timeout for: "+path;
        g_logger.warn(msg.c_str());
        t.detach();
        return 1; // Return default safe fallback directly, avoiding racy read on result
    } else {
        t.join();
        return *result;
    }
}

static std::vector<std::string> read_dir_timeout(const std::string& path, int timeout_ms = 15000) {
     auto entries = std::make_shared<std::vector<std::string>>();
     auto state = std::make_shared<TimeoutState>();

     std::thread t([entries, path, state]() {
         *entries = read_dir(path);
         {
             std::lock_guard<std::mutex> lk(state->mtx);
             state->done = true;
         }
         state->cv.notify_one();
     });

     std::unique_lock<std::mutex> lk(state->mtx);
     state->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&]() { return state->done; });

     if (!state->done) {
         g_logger.warn("read_dir timeout after %dms for '%s'", timeout_ms, path.c_str());
         t.detach();
         return {}; // Return empty vector directly, avoiding racy read/copy on entries
     } else {
         t.join();
         return *entries;
     }
 }

// Timeout wrapper for stat() — prevents CIFS hangs on individual files
static bool stat_timeout(const std::string& path, struct stat& st, int timeout_ms = 5000) {
     auto result = std::make_shared<int>(-1);
     auto st_ptr = std::make_shared<struct stat>();
     auto state = std::make_shared<TimeoutState>();

     std::thread t([path, st_ptr, result, state]() {
         *result = stat(path.c_str(), st_ptr.get());
         {
             std::lock_guard<std::mutex> lk(state->mtx);
             state->done = true;
         }
         state->cv.notify_one();
     });

     std::unique_lock<std::mutex> lk(state->mtx);
     state->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&]() { return state->done; });

     if (!state->done) {
         g_logger.debug("stat timeout after %dms for '%s'", timeout_ms, path.c_str());
        t.detach();

         return false;  // Treat as "file not found" — safe to skip
     }
     t.join();
     if (*result == 0) st = *st_ptr;
     return *result == 0;
 }

static std::string file_ext(const std::string& path) {
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos || dot == path.size() - 1) return "";
    return path.substr(dot + 1);
}

static std::string file_name(const std::string& path) {
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

// Year-agnostic scanner. Tests current/prev/next year for Dec/Jan wrap-around.
// Year-agnostic seasonal window check: extract MM-DD from filename and
// compare against today's MM-DD, with wrap-around across year boundary.
static bool is_in_seasonal_window(const std::string& filename, int window_days) {
    if (window_days <= 0) return true;

    // Extract MM-DD from formats like YYYY-MM-DD_ or YYYYMMDD_
    static const std::regex date_regex(R"((\d{4})[-_]?(\d{2})[-_]?(\d{2})_)");
    std::smatch match;

    if (std::regex_search(filename, match, date_regex)) {
        try {
            int file_m = std::stoi(match[2]);
            int file_d = std::stoi(match[3]);
 
            time_t t = std::time(nullptr);
            tm tm_buf;
            tm* now = localtime_r(&t, &tm_buf);
            int curr_m = now->tm_mon + 1;
            int curr_d = now->tm_mday;
 
            // Approximate day-of-year (works well enough for seasonal window)
            int file_doy = file_m * 30 + file_d;
            int curr_doy = curr_m * 30 + curr_d;
 
            int diff = std::abs(curr_doy - file_doy);
            // Handle wrap-around (e.g., Dec 31 to Jan 1)
            if (diff > 365 / 2) diff = 365 - diff;
 
            return diff <= window_days;
        } catch (...) {
            return false;
        }
    }

    return false;
}

// v16.4.0: deleted get_video_duration() — dead code + unbounded popen (H2, H3)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// MediaScanner v5.5.0 — Smart month-folder filter + std::filesystem scanner
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
class MediaScanner {
public:
    std::atomic<int> live_found_count{0};

    // SMART FOLDER FILTER: Drastically reduces disk I/O by skipping irrelevant months
    bool is_month_in_window(const std::string& dirname, int window_days) {
        if (window_days <= 0) return true; // 0 means show all

        // Match YYYY-MM or YYYYMM in the folder name
        static const std::regex folder_regex(R"((\d{4})[-_]?(\d{2}))");
        std::smatch match;

        if (std::regex_search(dirname, match, folder_regex)) {
            int folder_m = 0;
            try {
                folder_m = std::stoi(match[2]);
            } catch (...) {
                return true; // Fallback to scan if parsing fails
            }

            // SAFE GUARD RANGE BOUNDS: Discard garbage month tokens cleanly
            if (folder_m < 1 || folder_m > 12) {
                return true; // Treat as un-dated custom folder and scan safely
            }

            time_t t = std::time(nullptr);
            tm tm_buf;
            tm* now = localtime_r(&t, &tm_buf);
            int curr_m = now->tm_mon + 1;

            // Calculate how many months the temporal window spills over
            // e.g., 45 days / 30.0 = 1.5 -> spills into 2 months
            int max_month_spread = std::ceil(window_days / 30.0);

            int diff = std::abs(curr_m - folder_m);
            if (diff > 6) diff = 12 - diff; // Safe wrap-around for Dec/Jan

            return diff <= max_month_spread;
        }

        // If the folder has no date format (e.g. "Favorites"), scan it anyway
        return true;
    }

    std::vector<std::string> scan(const std::string& directory,
                                    const std::vector<std::string>& exts,
                                    int window_days,
                                    int max_depth) {
        live_found_count.store(0);
        std::vector<std::string> all_files;
        std::mutex list_mutex;

        // Helper for recursive scanning using safe read_dir
        std::function<void(const std::string&, int)> scan_rec = [&](const std::string& current_dir, int depth) {
            if (depth > max_depth) return;

            // 1. Scan files in this directory
            std::vector<std::string> entries = read_dir(current_dir);
            for (const auto& name : entries) {
                std::string full_path = current_dir + "/" + name;
                
                // Check if it's a directory
                struct stat st;
                if (stat(full_path.c_str(), &st) != 0) continue;
                if (S_ISDIR(st.st_mode)) {
                    // Skip hidden dirs or apply month filter if it's not the root
                    if (name[0] == '.') continue;
                    if (depth > 0 && !is_month_in_window(name, window_days)) continue;
                    
                    scan_rec(full_path, depth + 1);
                } else if (S_ISREG(st.st_mode)) {
                    process_entry(full_path, exts, window_days, list_mutex, all_files);
                }
            }
        };

        // To maintain the 3-thread parallel structure, we can't easily use a simple recursive function.
        // Instead, we'll use the "smart folder" approach but replace the recursive iterator.
        
        std::vector<std::string> subdirs;
        std::vector<std::string> root_files;
        std::vector<std::string> root_entries = read_dir_timeout(directory, 15000);
        for (const auto& name : root_entries) {
            std::string p = directory + "/" + name;
            struct stat st;
            if (stat_timeout(p, st, 5000)) {
                if (S_ISDIR(st.st_mode)) {
                    if (is_month_in_window(name, window_days)) {
                        subdirs.push_back(p);
                    }
                } else if (S_ISREG(st.st_mode)) {
                    root_files.push_back(p);
                }
            }
        }

        auto worker = [&](int start_idx, int step) {
            try {
                for (size_t i = start_idx; i < subdirs.size(); i += step) {
                    std::string target_dir = subdirs[i];
                    // Use manual recursion with depth limit and read_dir
                    std::function<void(const std::string&, int)> rec = [&](const std::string& dir, int d) {
                        if (d > max_depth) return;
                        std::vector<std::string> entries = read_dir(dir);
                        for (const auto& name : entries) {
                            std::string p = dir + "/" + name;
                            struct stat st;
                            if (stat(p.c_str(), &st) != 0) continue;
                            if (S_ISDIR(st.st_mode)) {
                                if (name[0] == '.') continue;
                                rec(p, d + 1);
                            } else if (S_ISREG(st.st_mode)) {
                                process_entry(p, exts, window_days, list_mutex, all_files);
                            }
                        }
                    };
                    rec(target_dir, 1);
                }
            } catch (...) {}
        };

        // Scan root files once before starting workers
        for (const auto& p : root_files) {
            process_entry(p, exts, window_days, list_mutex, all_files);
        }

        std::thread t1(worker, 0, 3);
        std::thread t2(worker, 1, 3);
        std::thread t3(worker, 2, 3);
        t1.join(); t2.join(); t3.join();

        return all_files;
    }


    int get_count() { return live_found_count.load(std::memory_order_relaxed); }

private:
    void process_entry(const std::filesystem::path& path_obj, const std::vector<std::string>& exts, int window_days, std::mutex& list_mutex, std::vector<std::string>& all_files) {
        if (path_obj.stem().string().empty()) return; // Skip hidden .dotfiles

        std::string ext = path_obj.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

        if (std::find(exts.begin(), exts.end(), ext) != exts.end()) {
            if (is_in_seasonal_window(path_obj.filename().string(), window_days)) {
                std::lock_guard<std::mutex> lock(list_mutex);
                all_files.push_back(path_obj.string());
                live_found_count.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
};
struct CacheManager {
    sqlite3* db{nullptr};
    std::mutex db_mutex;
    sqlite3_stmt* stmt_upsert{nullptr};
    sqlite3_stmt* stmt_load{nullptr};
    sqlite3_stmt* stmt_mark{nullptr};

    bool open(const std::string& dir) {
        // FIX: Ensure cache directory exists before SQLite tries to write
        // v16.2.0: use std::filesystem instead of blocking system("mkdir -p")
        std::filesystem::create_directories(dir);
 
        std::string path = dir + "/cache.db";
        int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
         if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
             if (db) { sqlite3_close(db); db = nullptr; }
             return false;
         }
         sqlite3_busy_timeout(db, 5000);
 
         // v3.0.4: Proactive SQLite integrity check (F4)
         sqlite3_stmt* stmt = nullptr;
         if (sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &stmt, nullptr) == SQLITE_OK
             && sqlite3_step(stmt) == SQLITE_ROW) {
             const char* result = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
             if (result && std::string(result) != "ok") {
                 g_logger.error("SQLite integrity check failed (%s). Purging corrupted cache...", result);
                 sqlite3_finalize(stmt);
                 sqlite3_close(db);
                 db = nullptr;
                 std::remove(path.c_str());
                 std::string wal = path + "-wal";
                 std::string shm = path + "-shm";
                 std::remove(wal.c_str());
                 std::remove(shm.c_str());
                 if (sqlite3_open_v2(path.c_str(), &db, flags, nullptr) != SQLITE_OK) {
                     if (db) { sqlite3_close(db); db = nullptr; }
                     return false;
                 }
                 // DDL will be re-executed below
             } else {
                 sqlite3_finalize(stmt);
             }
         }

          sqlite3_busy_timeout(db, 5000);


        // v16.2.0: log PRAGMA results for easier debugging
        char* err = nullptr;
        if (sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err) != SQLITE_OK) {
            g_logger.warn("Failed to set WAL mode: %s", err ? err : "unknown");
            if (err) sqlite3_free(err);
        }
        if (sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, &err) != SQLITE_OK) {
            g_logger.warn("Failed to set synchronous=NORMAL: %s", err ? err : "unknown");
            if (err) sqlite3_free(err);
        }
        char mmap_sql[64];
        snprintf(mmap_sql, sizeof(mmap_sql), "PRAGMA mmap_size=%lld", g_cfg.cache_mmap_size);
        if (sqlite3_exec(db, mmap_sql, nullptr, nullptr, &err) != SQLITE_OK) {
            g_logger.warn("Failed to set mmap_size=%lld: %s", g_cfg.cache_mmap_size, err ? err : "unknown");
            if (err) sqlite3_free(err);
        }

        // v16.2.0: log DDL errors
         if (sqlite3_exec(db,
             "CREATE TABLE IF NOT EXISTS cache ("
             "path TEXT PRIMARY KEY, type TEXT, w INT, h INT, duration REAL, "
             "exif INT, bad INT DEFAULT 0, last_shown INTEGER DEFAULT 0, timestamp INTEGER DEFAULT 0"
             ")", nullptr, nullptr, &err) != SQLITE_OK) {
             g_logger.error("Failed to create cache table: %s", err ? err : "unknown");
             if (err) sqlite3_free(err);
             close();
             return false;
         }


       // Safe migration for existing databases
        if (sqlite3_exec(db, "ALTER TABLE cache ADD COLUMN last_shown INTEGER DEFAULT 0",
                      nullptr, nullptr, &err) != SQLITE_OK) {
            // Column may already exist — harmless
            if (err) sqlite3_free(err);
        }
        if (sqlite3_exec(db, "ALTER TABLE cache ADD COLUMN bad INT DEFAULT 0",
                      nullptr, nullptr, &err) != SQLITE_OK) {
            // Column may already exist — harmless
            if (err) sqlite3_free(err);
        }

        // Ensure we don't leak on earlier failures: reset any partially prepared statements
        sqlite3_finalize(stmt_upsert); stmt_upsert = nullptr;
        sqlite3_finalize(stmt_load); stmt_load = nullptr;
        sqlite3_finalize(stmt_mark); stmt_mark = nullptr;

        // Pre-compile statements for efficiency
         if (sqlite3_prepare_v2(db,
             "INSERT INTO cache (path, type, w, h, exif, duration, bad, last_shown, timestamp) "
             "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
             "ON CONFLICT(path) DO UPDATE SET "
             "w=excluded.w, h=excluded.h, exif=excluded.exif, "
             "duration=excluded.duration, bad=excluded.bad, "
             "last_shown=excluded.last_shown, timestamp=excluded.timestamp",
             -1, &stmt_upsert, nullptr) != SQLITE_OK) {
             g_logger.error("Failed to prepare upsert statement.");
             close();
             return false;
         }
 
         if (sqlite3_prepare_v2(db,
             "SELECT w, h, duration, exif, bad, last_shown, timestamp FROM cache WHERE path = ?",
             -1, &stmt_load, nullptr) != SQLITE_OK) {
             g_logger.error("Failed to prepare load statement.");
             close();
             return false;
         }
 
         if (sqlite3_prepare_v2(db,
             "UPDATE cache SET last_shown = ? WHERE path = ?",
             -1, &stmt_mark, nullptr) != SQLITE_OK) {
             g_logger.error("Failed to prepare mark statement.");
             close();
             return false;
         }


        return true;
    }

    ~CacheManager() { close(); }

    void close() {
         if (stmt_upsert) { sqlite3_finalize(stmt_upsert); stmt_upsert = nullptr; }
         if (stmt_load) { sqlite3_finalize(stmt_load); stmt_load = nullptr; }
         if (stmt_mark) { sqlite3_finalize(stmt_mark); stmt_mark = nullptr; }
         if (db) { sqlite3_close(db); db = nullptr; }
     }

    bool load_cached(MediaItem& mi) {
        if (!stmt_load) return false;
        std::lock_guard<std::mutex> lk(db_mutex);
        bool found = false;
        sqlite3_bind_text(stmt_load, 1, mi.path.c_str(), -1, SQLITE_STATIC);
        if (sqlite3_step(stmt_load) == SQLITE_ROW) {
            mi.width      = sqlite3_column_int64(stmt_load, 0);
            mi.height     = sqlite3_column_int64(stmt_load, 1);
            mi.duration   = sqlite3_column_double(stmt_load, 2);
            mi.exif_rotation = sqlite3_column_int(stmt_load, 3);
            int bad = sqlite3_column_int(stmt_load, 4);
            mi.last_shown = sqlite3_column_int64(stmt_load, 5);
            mi.modified_time = sqlite3_column_int64(stmt_load, 6);
            if (bad == 0) found = true;
        }
        sqlite3_reset(stmt_load);
        return found;
    }

    void upsert(const MediaItem& mi, int bad) {
        if (!stmt_upsert) return;
        std::lock_guard<std::mutex> lk(db_mutex);
        sqlite3_bind_text(stmt_upsert, 1, mi.path.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt_upsert, 2, mi.type.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt_upsert, 3, mi.width);
        sqlite3_bind_int64(stmt_upsert, 4, mi.height);
        sqlite3_bind_int(stmt_upsert, 5, mi.exif_rotation);
        sqlite3_bind_double(stmt_upsert, 6, mi.duration);
        sqlite3_bind_int(stmt_upsert, 7, bad);
        sqlite3_bind_int64(stmt_upsert, 8, mi.last_shown);
        sqlite3_bind_int64(stmt_upsert, 9, mi.modified_time);
        int step_ret = sqlite3_step(stmt_upsert);
        if (step_ret != SQLITE_DONE) {
            g_logger.error("Failed to execute upsert for: %s", mi.path.c_str());
        }
        sqlite3_reset(stmt_upsert);
    }

    void mark_shown(const std::string& path) {
        if (!stmt_mark) return;
        std::lock_guard<std::mutex> lk(db_mutex);
        sqlite3_bind_int64(stmt_mark, 1, time(nullptr));
        // v6.0.3: Use SQLITE_TRANSIENT so SQLite copies the data immediately
        // SQLITE_STATIC would dangle if path goes out of scope before sqlite3_reset
        sqlite3_bind_text(stmt_mark, 2, path.c_str(), -1, SQLITE_TRANSIENT);
        int step_ret = sqlite3_step(stmt_mark);
        if (step_ret != SQLITE_DONE) {
            g_logger.error("Failed to execute mark_shown for: %s", path.c_str());
        }
        sqlite3_reset(stmt_mark);
    }

    // ── NEW FIX: Persist bad files to SQLite so they are skipped permanently ──
    void mark_bad(const std::string& filepath) {
        if (!db) return;
        std::lock_guard<std::mutex> lk(db_mutex);
        const char* sql = "UPDATE cache SET bad = 1 WHERE path = ?;";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, filepath.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }
    // ──────────────────────────────────────────────────────────────────────────

    // Bulk transaction support — synchronized for thread safety
    void begin_transaction() {
        if (!db) return;
        std::lock_guard<std::mutex> lk(db_mutex);
        sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
    }

    void commit_transaction() {
        if (!db) return;
        std::lock_guard<std::mutex> lk(db_mutex);
        sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
    }
};

CacheManager* g_cache = nullptr;
static void scan_directory(const std::string& dir, int depth,
                           std::vector<MediaItem>& items, std::atomic<int64_t>& count) {
    g_logger.info("scan_directory: dir=%s depth=%d",
        dir.c_str(), depth);
    MediaScanner scanner;
    std::vector<std::string> exts = {".jpg", ".jpeg", ".png", ".webp", ".heic", ".heif", ".gif", ".bmp", ".tiff", ".mp4", ".mov", ".mkv", ".avi", ".webm"};
    int scan_days;  // v6.0.10: copy g_cfg under lock (B199)
    std::vector<std::string> ignore_f;
    {
        std::lock_guard<std::mutex> lk(g_config_mtx);
        scan_days = g_cfg.scan_window_days;
        ignore_f = g_cfg.ignore_folders;
    }
    auto media_files = scanner.scan(dir, exts, scan_days, depth);

    for (auto& filepath : media_files) {
        auto fname = filepath.substr(filepath.find_last_of('/') + 1);
        if (fname.empty() || fname[0] == '.') continue;

        // Check ALL path components against ignore_folders, not just the leaf filename.
        // A file at /media/@eaDir/photo.jpg should be skipped when "@eaDir" is ignored,
        // but the old code compared "photo.jpg" == "@eaDir" which never matched.
        bool skip = false;
        for (const auto& ign : ignore_f) {
            if (filepath.find("/" + ign + "/") != std::string::npos ||
                (filepath.size() >= ign.size() + 1 &&
                 filepath.substr(filepath.size() - ign.size() - 1) == "/" + ign)) {
                skip = true;
                break;
            }
        }
        if (skip) continue;

        std::string ext = file_ext(filepath);
        if (ext.empty()) continue;
        if (!is_image(ext) && !is_video(ext)) continue;

        struct stat st;
        if (stat(filepath.c_str(), &st) != 0) continue;

        MediaItem mi;
        mi.path = filepath;
        mi.filename = file_name(filepath);
        mi.ext = ext;
        mi.file_size = st.st_size;
        mi.modified_time = st.st_mtime;
        mi.type = is_image(ext) ? "image" : "video";

        // Load cached metadata (especially last_shown) for cooldown filter
        if (g_cache) g_cache->load_cached(mi);

        items.push_back(std::move(mi));
        count.fetch_add(1, std::memory_order_relaxed);
    }
}


// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  CACHE MANAGER (SQLite3, WAL, mmap)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Probe video metadata — fork+execvp, SIGKILL watchdog, poll() timeout
// Replaces popen()+timeout which cannot interrupt a CIFS hang:
// popen() blocks in fork() or shell stat() before timeout even starts.
// Direct fork+execvp gives us the child PID and SIGKILL at any point.
static std::string run_ffprobe(const std::vector<std::string>& args, int timeout_ms) {
    std::vector<const char*> argv;
    argv.push_back("ffprobe");
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return "";
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return ""; }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        close(pipefd[0]); close(pipefd[1]);
        setsid();
        struct rlimit rl{ 256u*1024*1024, 256u*1024*1024 };
        setrlimit(RLIMIT_AS, &rl);
        execvp("ffprobe", const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    close(pipefd[1]);
    std::string out; out.reserve(512);
    char buf[4096];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    bool eof_reached = false;
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) { break; }
        struct pollfd pfd{ pipefd[0], POLLIN, 0 };
        int ret = poll(&pfd, 1, (int)std::min<long>(remaining, 500));
        if (ret < 0) { if (errno == EINTR) continue; break; }
        if (ret == 0) continue;
        if (pfd.revents & (POLLIN|POLLHUP)) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n > 0) {
                out.append(buf, n);
                if (out.size() > 65536) break;
            }
            else { eof_reached = true; break; }
        }
    }
    if (!eof_reached) {
        kill(pid, SIGKILL);
    }
    close(pipefd[0]);
    waitpid(pid, nullptr, 0);
    return out;
}

static std::string ffprobe_field(const std::string& out, const std::string& key) {
    // v6.0.4: Search for "\nkey=" to avoid matching partial key names like "format_duration" when searching for "duration"
    std::string search = "\n" + key + "=";
    auto pos = out.find(search);
    if (pos == std::string::npos) {
        // Fallback: try without newline prefix (for first field in output)
        search = key + "=";
        pos = out.find(search);
    }
    if (pos == std::string::npos) return "";
    pos += search.size();
    auto end2 = out.find("\n", pos);
    std::string val = (end2 == std::string::npos) ? out.substr(pos) : out.substr(pos, end2 - pos);
    if (!val.empty() && val.back() == '\r') val.pop_back();
    return val;
}

bool probe_video_meta(const std::string& filepath, int& width, int& height, float& duration, time_t& creation_time) {
    if (filepath.empty()) return false;
    auto out = run_ffprobe({"-v","quiet","-print_format","default=noprint_wrappers=1:nokey=0",
        "-select_streams","v:0","-show_entries","stream=width,height:format=duration:format_tags=creation_time",
        filepath}, 8000);
    if (out.empty()) return false;
    auto sw = ffprobe_field(out, "width"), sh = ffprobe_field(out, "height"),
        sd = ffprobe_field(out, "duration"), sts = ffprobe_field(out, "TAG:creation_time");
    if (sw.empty() || sh.empty() || sd.empty()) return false;
    try { width = std::stoi(sw); height = std::stoi(sh); duration = std::stof(sd); }
    catch (...) { return false; }
    if (!sts.empty()) { struct tm t={}; std::istringstream s(sts);
        s >> std::get_time(&t, "%Y-%m-%dT%H:%M:%S"); if (!s.fail())
        creation_time = timegm(&t); }
    return (width > 0 && height > 0 && duration > 0.0f);
}

// FIX v4.1.5: Detect and auto-remove corrupted SQLite database
static bool verify_database(const std::string& path) {
    sqlite3* db = nullptr;
    sqlite3_stmt* stmt = nullptr;
    bool ok = false;

    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK) {
        return false;
    }

    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master WHERE type='table' AND name='cache';", -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* table_name = (const char*)sqlite3_column_text(stmt, 0);
            if (table_name && std::string(table_name) == "cache") {
                ok = true;
            }
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    return ok;
}



// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  SPLASH SCREEN
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

struct SplashScreen {
    Texture2D logo{};
    bool      logo_loaded{false};
    bool      font_loaded{false};
    int       w{0}, h{0};
    Font      crt_font{};
    float     anim_time{0.0f};
    float     telemetry_temp{0.0f};
    float     telemetry_freq{0.0f};
    time_t    telemetry_ts{0};
    std::string current_cache_file;

    std::vector<std::string> log_buffer;
    std::mutex log_mutex;
    // FIX: Robust string parsing. Ignores sysfs newlines that break fscanf/cin.
    static float read_sys_f(const char* path, float divisor) {
        std::ifstream file(path);
        if (!file.is_open()) return 0.0f;
        std::string line;
        if (std::getline(file, line)) {
            if (line.empty()) return 0.0f;
            try {
                size_t pos;
                long long v = std::stoll(line, &pos);
                if (pos == 0) return 0.0f; // No digits found
                return (float)v / divisor;
            } catch (const std::invalid_argument&) {
                g_logger.debug("sysfs invalid arg: %s", path);
                return 0.0f;
            } catch (const std::out_of_range&) {
                g_logger.debug("sysfs out of range: %s", path);
                return 0.0f;
            } catch (...) {
                return 0.0f;
            }
        }
        return 0.0f;
    }

    static std::string folder_and_file(const std::string& path) {
        auto slash2 = path.find_last_of('/');
        if (slash2 == std::string::npos) return path;
        auto slash1 = path.find_last_of('/', slash2 - 1);
        if (slash1 == std::string::npos) return path.substr(slash2 + 1);
        return path.substr(slash1 + 1);
    }

    void load(const std::string& path) {
        if (logo_loaded) {
            UnloadTexture(logo);
            logo_loaded = false;
        }

        // Try provided path -> src/splash.png -> exe_dir/splash.png
        std::string exe_dir = GetApplicationDirectory();
        std::string src_dir = exe_dir + "/src";
        std::string splash_path = path;
        if (!std::filesystem::exists(path)) {
            std::string src_path = src_dir + "/" + path;
            if (std::filesystem::exists(src_path)) {
                splash_path = src_path;
                g_logger.info("Splash loaded from src dir: %s", splash_path.c_str());
            } else {
                std::string exe_path = exe_dir + "/" + path;
                if (std::filesystem::exists(exe_path)) {
                    splash_path = exe_path;
                    g_logger.info("Splash loaded from exe dir: %s", splash_path.c_str());
                } else {
                    // FIX v5.2.0: GetApplicationDirectory() returns XDG data dir, not binary parent.
                    // Resolve /proc/self/exe to find the actual binary location.
                    char exe_buf[4096];
                    ssize_t len = readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
                    if (len > 0) {
                        exe_buf[len] = '\0';
                        std::string real_exe = exe_buf;
                        std::string real_dir = std::filesystem::path(real_exe).parent_path().string();
                        std::string real_src = real_dir + "/src/" + path;
                        if (std::filesystem::exists(real_src)) {
                            splash_path = real_src;
                            g_logger.info("Splash loaded from real exe dir: %s", splash_path.c_str());
                        }
                    }
                }
            }
        }

        // FIX: check path exists before attempting LoadImage
        if (!std::filesystem::exists(splash_path)) {
            g_logger.warn("Splash file not found: %s, generating fallback background", splash_path.c_str());
            // Generate a solid dark background image as fallback
            int fb_w = GetScreenWidth();
            int fb_h = GetScreenHeight();
            Image fb_img = GenImageColor(fb_w, fb_h, (Color){8, 10, 8, 255});
            if (fb_img.data) {
                logo = LoadTextureFromImage(fb_img);
                if (logo.id != 0) {
                    w = fb_w;
                    h = fb_h;
                    logo_loaded = true;                    SetTextureFilter(logo, TEXTURE_FILTER_BILINEAR);
                    g_logger.info("Splash fallback background generated (%dx%d)", w, h);
                }
                UnloadImage(fb_img);
            }
        } else {
            Image img = LoadImage(splash_path.c_str());
            if (img.data && img.width > 0 && img.height > 0) {
                logo = LoadTextureFromImage(img);
                if (logo.id != 0) {
                    w = img.width;
                    h = img.height;
                    logo_loaded = true;                    SetTextureFilter(logo, TEXTURE_FILTER_BILINEAR);
                    g_logger.info("Splash loaded: %s (%dx%d)", splash_path.c_str(), w, h);
                } else {
                    g_logger.error("Splash texture creation FAILED for %s (GL error)", path.c_str());
                }
                UnloadImage(img);
            } else {
                g_logger.error("Failed to load splash image: %s (img.data=%p, %dx%d)", path.c_str(), (void*)img.data, img.width, img.height);
            }
        }
        crt_font = LoadFont("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf");
        if (crt_font.texture.id == 0) {
            crt_font = GetFontDefault();
            font_loaded = false;
        } else {
            font_loaded = true;
        }
        if (crt_font.texture.id != 0) SetTextureFilter(crt_font.texture, TEXTURE_FILTER_POINT);
    }

    void cleanup() {
        if (logo.id != 0) {
            UnloadTexture(logo);
            logo.id = 0;
            logo_loaded = false;
        }
        if (font_loaded) UnloadFont(crt_font);
    }

   void render(int phase, int progress, int total, int done, const char* label) {
        // FIX v16.9.0: use dt parameter for frame-rate-independent animation
        anim_time += GetFrameTime();

    if (logo_loaded) {
        float ratio = (float)w / h;
        int draw_w=GetScreenWidth();
        int draw_h=(int)(draw_w/ratio);
        int draw_x=(GetScreenWidth()-draw_w)/2;
        int draw_y=(GetScreenHeight()-draw_h)/2;
        Rectangle src{0,0,(float)w,(float)h};
        Rectangle dst{(float)draw_x,(float)draw_y,(float)draw_w,(float)draw_h};
        DrawTexturePro(logo,src,dst,{0,0},0,WHITE);
    } else {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), (Color){8, 10, 8, 255});
        std::string brand = "piTrove";
        int brand_fs = 36;
        Vector2 brand_size = MeasureTextEx(crt_font, brand.c_str(), brand_fs, 1);
        int brand_x = (GetScreenWidth() - (int)brand_size.x) / 2;
        int brand_y = (GetScreenHeight() - (int)brand_size.y) / 2;
        DrawTextEx(crt_font, brand.c_str(), {(float)brand_x, (float)brand_y}, brand_fs, 1, (Color){180, 180, 180, 240});
        DrawText("piTrove", brand_x + 1, brand_y + 1, brand_fs, (Color){0, 0, 0, 100});
    }


        int overlay_y = (int)(GetScreenHeight() * g_cfg.splash_overlay_y);
        int panel_h = GetScreenHeight() - overlay_y;

     DrawRectangle(0, overlay_y, GetScreenWidth(), panel_h, (Color){10, 12, 10, 230});
        DrawRectangle(0, overlay_y, GetScreenWidth(), panel_h, (Color){0, 20, 0, 60});

        // Scanlines drawn over the green console PANEL area only (below overlay_y).
        // Previous loop iterated sy < overlay_y which drew over the logo area.
        float scan_offset = fmodf(anim_time * 15.0f, 4.0f);
        int screen_h = GetScreenHeight();
        for (int sy = overlay_y - 4; sy < screen_h; sy += 4) {
            int draw_y = sy + (int)scan_offset;
            if (draw_y >= overlay_y && draw_y < screen_h) {
                DrawRectangle(0, draw_y, GetScreenWidth(), 1, (Color){0, 0, 0, 50});
            }
        }

        Color green  = {0, 200, 0, 240};
        Color bgreen = {0, 255, 0, 255};
        Color dimg   = {0, 130, 0, 220};
        Color gray   = {160, 160, 160, 220};

      // ── Phase 1-2: Scanning ──
        if (phase <= 2) {
            int fs = 15;
            int lh = 20;
            int col_w = 80;
            std::string col_w_str(col_w, 'X');
            int box_w_px = (int)MeasureTextEx(crt_font, col_w_str.c_str(), fs, 1).x;
            int mx = (GetScreenWidth() - box_w_px) / 2;
            int y = overlay_y + 10;
            int bh = 22 * lh;
            int pad = 3;

            // Double-line outer border: two nested single-line rectangles, equal gap on all sides
            int outer_x = mx - pad;
            int outer_y = overlay_y + 10;
            int outer_w = box_w_px + pad * 2;
            int outer_h = bh;
            int inner_x = mx;
            int inner_y = outer_y + pad;
            int inner_w = box_w_px;
            int inner_h = outer_h - pad * 2;

            DrawRectangle(outer_x, outer_y, outer_w, 1, bgreen);
            DrawRectangle(outer_x, outer_y + outer_h - 1, outer_w, 1, bgreen);
            DrawRectangle(outer_x, outer_y, 1, outer_h, bgreen);
            DrawRectangle(outer_x + outer_w - 1, outer_y, 1, outer_h, bgreen);
            DrawRectangle(inner_x, inner_y, inner_w, 1, bgreen);
            DrawRectangle(inner_x, inner_y + inner_h - 1, inner_w, 1, bgreen);
            DrawRectangle(inner_x, inner_y, 1, inner_h, bgreen);
            DrawRectangle(inner_x + inner_w - 1, inner_y, 1, inner_h, bgreen);

        const char* dots[] = {"   ", ".  ", ".. ", "..."};
            const char* dot = dots[(int)(anim_time / 0.3f) % 4];

            auto draw_row = [&](const std::string& l, const std::string& r, Color c) {
                int spaces = (col_w - 4) - (int)l.length() - (int)r.length();
                if (spaces < 0) spaces = 0;
                std::string final_str = l + std::string(spaces, ' ') + r;
                DrawTextEx(crt_font, final_str.c_str(), {(float)(inner_x + 4), (float)y}, fs, 1, c);
                y += lh;
            };
            auto draw_border_row = [&]() {
                DrawRectangle(outer_x, y, outer_w, 1, bgreen);
                DrawRectangle(inner_x, y, inner_w, 1, bgreen);
                y += lh;
            };
            auto draw_div_row = [&]() {
                DrawRectangle(inner_x, y, inner_w, 1, bgreen);
                y += lh;
            };

            draw_border_row();
            draw_row(std::string("piTrove KERNEL INTERFACE v") + VERSION, "", bgreen);
            draw_div_row();
            draw_row("AArch64 TELEMETRY [BCM2712]", "", bgreen);

            time_t now = time(nullptr);
            if (now != telemetry_ts) {
                telemetry_temp = read_sys_f("/sys/class/thermal/thermal_zone0/temp", 1000.0f);
                telemetry_freq = read_sys_f("/sys/class/cpufreq/policy0/scaling_cur_freq", 1000.0f);
                if (telemetry_freq == 0.0f) {
                    telemetry_freq = read_sys_f("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", 1000.0f);
                }
                telemetry_ts = now;
            }
            char buf[128], buf2[64];
            std::snprintf(buf, sizeof(buf), "SOC TEMP : %.1f C", telemetry_temp);
            std::snprintf(buf2, sizeof(buf2), "CLOCK : %.0f MHz", telemetry_freq);
            draw_row(buf, buf2, gray);

            draw_div_row();
            draw_row("PHASE 2: TEMPORAL SCANNING", "", bgreen);
            draw_row("", "", bgreen);

            draw_row(std::string("NETWORK TARGET : ") + g_cfg.media_dir, "", gray);
            draw_row("ACCESS PROTOCOL: READ-ONLY", "", gray);
            draw_row(std::string("SCAN STATUS    : ACTIVE") + dot, "", bgreen);

            std::snprintf(buf, sizeof(buf), "FILES COUNTED : %d", done);
            draw_row(buf, "", bgreen);

            draw_div_row();
            draw_row("NEON SIMD VECTOR PIPELINE", "", bgreen);

            int bar_cols = col_w - 12;
            int neon_pct = 40 + (int)(fabsf(sinf(anim_time * 1.7f)) * 35.0f);
            int filled = bar_cols * neon_pct / 100;
            if (filled > bar_cols) filled = bar_cols;
            std::string bar = "[" + std::string(filled, 'X') + std::string(bar_cols - filled, '.') + "]";
            draw_row(" " + bar, "", gray);

            draw_row(" Optimizing YYYY-MM-DD prefix...", "", gray);
            draw_div_row();
            draw_row("SYSTEM LOGS", "", bgreen);
            draw_row(" > Initializing scanner...", "", gray);
            draw_row(" > Mounting remote archive...", "", gray);
            draw_row(" > Filtering temporal window...", "", gray);
             DrawRectangle(inner_x, y, inner_w, 1, bgreen);
             y += lh;

             // CRT phosphor glow centered on actual content area
             {
                 int content_top = inner_y + pad;
                 int content_bottom = y;
                 int content_h = content_bottom - content_top;
                 if (content_h > 0) {
                     for (int gy = content_top; gy < content_bottom; gy += 2) {
                         float cy = (float)(gy - content_top) / content_h;
                         float center = 1.0f - (2.0f * cy - 1.0f) * (2.0f * cy - 1.0f);
                         center = center * center;
                         if (center > 0.4f) {
                             DrawRectangle(inner_x + 2, gy, inner_w - 4, 1, (Color){0, (unsigned char)(std::min(255, (int)(50 * center))), 0, (unsigned char)(std::min(255, (int)(40 * center)))});
                         }
                     }
                 }
             }
         }
        // ── Phase 3: Caching ──
        else if (phase == 3) {
            int fs = 15;
            int lh = 20;
            int col_w = 80;
            std::string col_w_str(col_w, 'X');
            int box_w_px = (int)MeasureTextEx(crt_font, col_w_str.c_str(), fs, 1).x;
            int mx = (GetScreenWidth() - box_w_px) / 2;
            int y = overlay_y + 10;
            int bh = 24 * lh;
            int pad = 3;

            // Double-line outer border: two nested single-line rectangles, equal gap on all sides
            int outer_x = mx - pad;
            int outer_y = overlay_y + 10;
            int outer_w = box_w_px + pad * 2;
            int outer_h = bh;
            int inner_x = mx;
            int inner_y = outer_y + pad;
            int inner_w = box_w_px;
            int inner_h = outer_h - pad * 2;

            DrawRectangle(outer_x, outer_y, outer_w, 1, bgreen);
            DrawRectangle(outer_x, outer_y + outer_h - 1, outer_w, 1, bgreen);
            DrawRectangle(outer_x, outer_y, 1, outer_h, bgreen);
            DrawRectangle(outer_x + outer_w - 1, outer_y, 1, outer_h, bgreen);
            DrawRectangle(inner_x, inner_y, inner_w, 1, bgreen);
            DrawRectangle(inner_x, inner_y + inner_h - 1, inner_w, 1, bgreen);
            DrawRectangle(inner_x, inner_y, 1, inner_h, bgreen);
            DrawRectangle(inner_x + inner_w - 1, inner_y, 1, inner_h, bgreen);

            auto draw_row = [&](const std::string& l, const std::string& r, Color c) {
                int spaces = (col_w - 4) - (int)l.length() - (int)r.length();
                if (spaces < 0) spaces = 0;
                std::string final_str = l + std::string(spaces, ' ') + r;
                DrawTextEx(crt_font, final_str.c_str(), {(float)(inner_x + 4), (float)y}, fs, 1, c);
                y += lh;
            };
            auto draw_border_row = [&]() {
                DrawRectangle(outer_x, y, outer_w, 1, bgreen);
                DrawRectangle(inner_x, y, inner_w, 1, bgreen);
                y += lh;
            };
            auto draw_div_row = [&]() {
                DrawRectangle(inner_x, y, inner_w, 1, bgreen);
                y += lh;
            };

            draw_border_row();
            draw_row(std::string("piTrove KERNEL INTERFACE v") + VERSION, "", bgreen);
            draw_row("AArch64 TELEMETRY [BCM2712]", "", green);

            time_t now = time(nullptr);
            if (now != telemetry_ts) {
                telemetry_temp = read_sys_f("/sys/class/thermal/thermal_zone0/temp", 1000.0f);
                telemetry_ts = now;
            }
            char buf[128];
            std::snprintf(buf, sizeof(buf), "SOC TEMP : %.1f C", telemetry_temp);
            draw_row(buf, "STATUS : PROCESSING", gray);

            draw_div_row();
            draw_row("PHASE 3: SQLite PERSISTENCE ENGINE (Caching)", "", green);
            draw_row("", "", green);

            std::snprintf(buf, sizeof(buf), "MEDIA TOTAL : %d files", total);
            draw_row(buf, "", gray);
            draw_row(("DATABASE : " + g_cfg.cache_dir).c_str(), "", gray);
            draw_row("METADATA : EXIF Rotation + Stb_image Analysis", "", gray);

            std::snprintf(buf, sizeof(buf), "PROGRESS : %d / %d FILES", done, total);
            draw_row(buf, "", bgreen);

            std::string cf = current_cache_file.empty() ? "---" : current_cache_file;
            if ((int)cf.size() > col_w - 18) cf = "..." + cf.substr(cf.size() - (col_w - 21));
            draw_row("FILE     : " + cf, "", bgreen);

            draw_row("", "", bgreen);

            // Progress bar
            {
                int bw = box_w_px - 8;
                int filled = (bw * (total > 0 ? done : 0)) / (total > 0 ? total : 1);
                if (filled > bw) filled = bw;
                DrawRectangle(mx + 4, y - lh + 4, bw, lh - 8, (Color){0, 40, 0, 255});
                DrawRectangle(mx + 4, y - lh + 4, filled, lh - 8, bgreen);
            }

            draw_div_row();
            draw_row("DATABASE OPTIMIZATION STRATEGY", "", green);
            draw_row("JOURNAL MODE : [WAL] Write-Ahead Logging", "", gray);
            draw_row("MEMORY MAP   : 256MB (mmap_size)", "", gray);
            draw_row("SYNC STATUS  : Synchronizing background I/O...", "", gray);

            draw_div_row();
            draw_row("IO STATUS LOGS", "", green);
            draw_row(" > Initializing SQLite VFS... OK", "", gray);
            draw_row(" > Analyzing headers and validating formats...", "", gray);
            draw_row(" > Writing temporal indices to local cache...", "", gray);
            draw_row(" [ CACHE SYNC IN PROGRESS - DO NOT POWER OFF ]", "", dimg);

            // CRT phosphor glow centered on actual content area
            {
                int content_top = inner_y + pad;
                int content_bottom = y;
                int content_h = content_bottom - content_top;
                if (content_h > 0) {
                    for (int gy = content_top; gy < content_bottom; gy += 2) {
                        float cy = (float)(gy - content_top) / content_h;
                        float center = 1.0f - (2.0f * cy - 1.0f) * (2.0f * cy - 1.0f);
                        center = center * center;
                        if (center > 0.4f) {
                            DrawRectangle(inner_x + 2, gy, inner_w - 4, 1, (Color){0, (unsigned char)(std::min(255, (int)(50 * center))), 0, (unsigned char)(std::min(255, (int)(40 * center)))});
                        }
                    }
                }
            }
        }

       // CRT scanlines (drawn LAST so they go through all borders)
         for (int sy = 0; sy < GetScreenHeight(); sy += 2) {
             DrawRectangle(0, sy, GetScreenWidth(), 1, (Color){0, 255, 0, 30});
         }

         // CRT screen curvature vignette (bright center, dark edges)
        {
            float screen_h = (float)GetScreenHeight();
            for (int y = 0; y < GetScreenHeight(); y += 4) {
                float edge = 1.0f - 0.3f * (1.0f - (2.0f * y / screen_h - 1.0f) * (2.0f * y / screen_h - 1.0f));
                if (edge < 0.7f) {
                    DrawRectangle(0, y, GetScreenWidth(), 4, (Color){0, 0, 0, (unsigned char)(std::min(255, (int)(255 * (1.0f - edge))))});
                }
            }
        }
     }

    void add_log(const std::string& line) {
        {
            std::lock_guard<std::mutex> lock(log_mutex);
            log_buffer.push_back(line);
            if ((int)log_buffer.size() > 50) log_buffer.erase(log_buffer.begin());
        }
    }

    void draw_box(int x, int y, int w, int h) {
        // Outer double-line simulation
        DrawRectangleLines(x, y, w, h, GREEN);
        DrawRectangleLines(x + 4, y + 4, w - 8, h - 8, (Color){0, 130, 0, 220});

        // Break top line for Title
        int title_w = MeasureText(APP_NAME, 20);
        DrawRectangle(x + 15, y - 2, title_w + 10, 6, BLACK);
        draw_text(APP_NAME, x + 20, y - 10, 20, (Color){0, 200, 0, 240});

        // Move Version number safely inside the inner-outer box
        int ver_w = MeasureText("v" VERSION, 14);
        draw_text("v" VERSION, x + w - ver_w - 28, y + 10, 14, (Color){0, 130, 0, 220});
    }

    void draw_unified_screen(int scan_count, bool scan_active, int cache_current, int cache_total, bool cache_active, int dot_counter) {
        BeginDrawing();
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();

        // 1. Draw Splash cleanly with Aspect Ratio Scaling
        if (logo_loaded) {
            float scale = std::min((float)sw / (float)w, (float)sh / (float)h);
            float draw_w = (float)w * scale;
            float draw_h = (float)h * scale;
            float draw_x = (sw - draw_w) / 2.0f;
            float draw_y = (sh - draw_h) / 2.0f;

            Rectangle source = { 0.0f, 0.0f, (float)w, (float)h };
            Rectangle dest = { draw_x, draw_y, draw_w, draw_h };
            DrawTexturePro(logo, source, dest, {0.0f, 0.0f}, 0.0f, WHITE);
        } else {
            ClearBackground(BLACK);
        }

        // Live Uptime tracking — use double to avoid float precision loss after days of uptime
        static double boot_time = (double)GetTime();
        double uptime = (double)GetTime() - boot_time;

        // FIXED: Shifted down to 56% to clear the white splash graphic
        int terminal_start_y = sh * 0.56f;
        int bottom_matte_padding = sh * 0.06f;
        int black_area_end = sh - bottom_matte_padding;

        // 2. Animated Scanlines for the TOP white graphical area
        for (int y = 0; y < terminal_start_y; y += 4) {
            float wave = sinf(y * 0.02f - uptime * 3.0f) * 0.5f + 0.5f;
            DrawRectangle(0, y, sw, 2, Fade(BLACK, 0.08f + (wave * 0.12f)));
        }

        // 3. UI Box dimensions
        int box_w = sw * 0.90f;
        int box_x = (sw - box_w) / 2;
        int box_y = terminal_start_y + (sh * 0.015f);
        int box_h = black_area_end - box_y - (sh * 0.015f);

        // Draw main background box
        draw_box(box_x, box_y, box_w, box_h);

        // 4. Dense Phosphorous Green Scanlines (Thin, uniform layout)
        // Moved scanline start UP by 5% to cover the upper intersection
        int scanline_start = terminal_start_y - (sh * 0.02f);
        int scanline_end = black_area_end + (sh * 0.03f);
        
        for (int sy = scanline_start; sy < scanline_end; sy += 3) {
            // Lowered background phosphor bleed (reduced alpha to 0.04f)
            DrawRectangle(0, sy - 1, sw, 2, Fade(GREEN, 0.04f));
            // Lowered sharp core line glow (reduced alpha to 0.15f)
            DrawRectangle(0, sy, sw, 1, Fade(GREEN, 0.15f));
        }

        // Main Horizontal Center Box Divider
        int mid_y = box_y + (box_h / 2);
        DrawRectangle(box_x, mid_y - 1, box_w, 2, (Color){0, 200, 0, 240});
        DrawRectangle(box_x + 4, mid_y - 5, box_w - 8, 1, (Color){0, 130, 0, 220});
        DrawRectangle(box_x + 4, mid_y + 4, box_w - 8, 1, (Color){0, 130, 0, 220});

        // Grid Math for Columns
        int col_w = box_w / 4;
        int text_x = box_x + 20;
        int col2_x = box_x + col_w + 15;
        int col3_x = box_x + col_w * 2 + 15;
        int col4_x = box_x + col_w * 3 + 15;

        // Inner Grid Lines (Top Section)
        int inner_y_offset = box_y + 38;
        int top_inner_h = (box_h / 2) - 41;
        DrawRectangle(box_x + 10, inner_y_offset, box_w - 20, 1, Fade(GREEN, 0.3f));
        DrawRectangle(box_x + col_w, inner_y_offset, 1, top_inner_h, Fade(GREEN, 0.3f));
        DrawRectangle(box_x + col_w * 2, inner_y_offset, 1, top_inner_h, Fade(GREEN, 0.3f));
        DrawRectangle(box_x + col_w * 3, inner_y_offset, 1, top_inner_h, Fade(GREEN, 0.3f));

        int dots = (dot_counter / 15) % 4;
        char _ds[4] = {0}; memset(_ds, 46, dots); std::string dot_str(_ds);
        char cursor_chars[3] = {0xE2, 0x96, 0x88};
        std::string cursor(cursor_chars, 3);

        // ── TECHNICAL DATA SIMULATIONS ──
        int mem_mb = 142 + (int)(sin(uptime * 2.0) * 12) + (scan_count % 15);
        int speed = uptime > 0.001 ? (int)(scan_count / uptime) : 0;
        int active_threads = std::thread::hardware_concurrency();
        int latency = scan_active ? GetRandomValue(2, 14) : 0;
        int syscalls = scan_active ? GetRandomValue(1200, 3400) : 0;
        float l2_hit = scan_active ? 94.0f + ((float)GetRandomValue(0, 50) / 10.0f) : 100.0f;

        const char* hex_chars = "0123456789ABCDEF";
        std::string rand_hex = "0x";
        std::string rand_hash = "";
        for(int i = 0; i < 8; i++) rand_hex += hex_chars[GetRandomValue(0, 15)];
        for(int i = 0; i < 12; i++) rand_hash += hex_chars[GetRandomValue(0, 15)];

        // ── TOP HALF: SCANNER ──
        draw_text("PHASE 2: DIRECTORY SCANNER", text_x, box_y + 12, 20, (Color){0, 200, 0, 240});

        int row_start_y = inner_y_offset + 10;
        float row_space = (box_h * 0.08f);

        // Column 1: Core Status & Files Found
        if (scan_active) {
            draw_text("SYS_STAT : SCAN_ACTIVE" + dot_str, text_x, row_start_y, 16, (Color){0, 200, 0, 240});
            draw_text(TextFormat("FILES FND: %06d", scan_count), text_x, row_start_y + row_space * 1.2f, 18, (Color){0, 200, 0, 240});
            draw_text(TextFormat("I/O SPEED: %d nodes/s", speed), text_x, row_start_y + row_space * 2.5f, 14, (Color){0, 130, 0, 220});
            draw_text(TextFormat("LATENCY  : %d ms", latency), text_x, row_start_y + row_space * 3.5f, 14, (Color){0, 130, 0, 220});
        } else {
            draw_text("SYS_STAT : SCAN_COMPLETE", text_x, row_start_y, 16, (Color){0, 130, 0, 220});
            draw_text(TextFormat("FILES FND: %06d", scan_count), text_x, row_start_y + row_space * 1.2f, 18, (Color){0, 130, 0, 220});
            draw_text("I/O SPEED: 0 nodes/s", text_x, row_start_y + row_space * 2.5f, 14, (Color){0, 130, 0, 220});
            draw_text("LATENCY  : 0 ms", text_x, row_start_y + row_space * 3.5f, 14, (Color){0, 130, 0, 220});
        }

        // Column 2: System Memory
        draw_text(TextFormat("PID      : %d", getpid()), col2_x, row_start_y, 14, (Color){0, 130, 0, 220});
        draw_text(TextFormat("MEM ALLOC: %d MB", mem_mb), col2_x, row_start_y + row_space, 14, (Color){0, 130, 0, 220});
        draw_text(TextFormat("PAGE FLTS: %d", (scan_count / 12) + GetRandomValue(0, 3)), col2_x, row_start_y + row_space * 2.2f, 14, (Color){0, 130, 0, 220});
        draw_text(TextFormat("DIR_HASH : %s", rand_hash.c_str()), col2_x, row_start_y + row_space * 3.5f, 14, (Color){0, 130, 0, 220});

        // Column 3: Processing & CPU
        draw_text(TextFormat("THREADS  : %d ACTIVE", std::max(1, (int)std::thread::hardware_concurrency() - 1)), col3_x, row_start_y + row_space, 14, (Color){0, 130, 0, 220});
        draw_text(TextFormat("SYSCALLS : %d/s", syscalls), col3_x, row_start_y + row_space * 2.2f, 14, (Color){0, 130, 0, 220});
        draw_text(TextFormat("LAST PTR : %s", scan_active ? rand_hex.c_str() : "0x00000000"), col3_x, row_start_y + row_space * 3.5f, 14, (Color){0, 130, 0, 220});

        // Column 4: Storage & Buffers
        draw_text("HW_DECODE: READY", col4_x, row_start_y, 14, (Color){0, 130, 0, 220});
        draw_text("EGL_CTX  : GLES2", col4_x, row_start_y + row_space, 14, (Color){0, 130, 0, 220});
        draw_text(TextFormat("VFS BUF  : %d KB", 4096 + (scan_count % 1024)), col4_x, row_start_y + row_space * 2.2f, 14, (Color){0, 130, 0, 220});
        draw_text(TextFormat("INODE Q  : %04d PEND", scan_active ? GetRandomValue(12, 105) : 0), col4_x, row_start_y + row_space * 3.5f, 14, (Color){0, 130, 0, 220});

        Color theme_color = (Color){0, 200, 0, 240};
        Color theme_dim = (Color){0, 130, 0, 220};
        // ── BOTTOM HALF: CACHER ──
        draw_text("PHASE 3: SQLITE CACHE DB", text_x, mid_y + 12, 20, theme_color);

        // Inner Grid Lines (Bottom Section)
        int bot_inner_y_offset = mid_y + 38;
        DrawRectangle(box_x + 10, bot_inner_y_offset, box_w - 20, 1, Fade(theme_color, 0.3f)); // Horizontal Under-Title

        // SHORTER Vertical Dividers to leave room for the live file path logs at the bottom
        int bot_inner_h = 75;
        DrawRectangle(box_x + col_w, bot_inner_y_offset, 1, bot_inner_h, Fade(theme_color, 0.3f));
        DrawRectangle(box_x + col_w * 2, bot_inner_y_offset, 1, bot_inner_h, Fade(theme_color, 0.3f));
        DrawRectangle(box_x + col_w * 3, bot_inner_y_offset, 1, bot_inner_h, Fade(theme_color, 0.3f));

        int bot_row_start_y = bot_inner_y_offset + 10;
        int bot_row_space = 22;

        if (!scan_active && !cache_active && cache_total > 0) {
            draw_text("DB_STAT  : COMMIT_SUCCESS " + cursor, text_x, bot_row_start_y, 16, theme_dim);

            // Full Progress Bar
            int bar_y = mid_y + 130;
            int bar_w = box_w - 120;
            DrawRectangleLines(text_x, bar_y, bar_w, 18, theme_dim);
            DrawRectangle(text_x + 2, bar_y + 2, bar_w - 4, 14, theme_dim);
            draw_text("[100%]", text_x + bar_w + 15, bar_y, 16, theme_dim);

        } else if (cache_active) {
            // Column 1: DB Status
            draw_text("DB_STAT  : BULK_INSERT" + dot_str, text_x, bot_row_start_y, 16, theme_color);
            draw_text(TextFormat("CACHED   : %06d", cache_current), text_x, bot_row_start_y + bot_row_space * 1.2f, 18, theme_color);
            draw_text(TextFormat("I/O SPEED: %d ops/s", speed + GetRandomValue(10, 45)), text_x, bot_row_start_y + bot_row_space * 2.5f, 14, theme_dim);

            // Column 2: Storage Engine
            draw_text("VFS_MODE : WAL | NORMAL", col2_x, bot_row_start_y, 14, theme_dim);
            draw_text(TextFormat("PG_CACHE : %d KB", 4096 + (cache_current % 1024)), col2_x, bot_row_start_y + bot_row_space, 14, theme_dim);
            draw_text(TextFormat("WAL_SIZE : %.2f MB", 1.2f + (cache_current * 0.005f)), col2_x, bot_row_start_y + bot_row_space * 2, 14, theme_dim);

            // Column 3: Telemetry Extractor
            const char* ext_op = (cache_current % 3 == 0) ? "ffprobe -v quiet" : "libexif_rotate";
            const char* parse_op = (cache_current % 2 == 0) ? "JPEG_MARKER" : "MP4_MOOV_ATOM";
            draw_text("EXTRACTOR: ACTIVE", col3_x, bot_row_start_y, 14, theme_dim);
            draw_text(TextFormat("EXEC     : %s", ext_op), col3_x, bot_row_start_y + bot_row_space, 14, theme_dim);
            draw_text(TextFormat("META_TAG : %s", parse_op), col3_x, bot_row_start_y + bot_row_space * 2, 14, theme_dim);

            // Column 4: Transaction Pipeline
            draw_text("PIPE_STAT: BUFFER_FILL", col4_x, bot_row_start_y, 14, theme_dim);
            draw_text(TextFormat("TRANSACT : PENDING Q=%d", GetRandomValue(1, 15)), col4_x, bot_row_start_y + bot_row_space, 14, theme_dim);
            draw_text(TextFormat("COMMIT_ID: %s", rand_hex.c_str()), col4_x, bot_row_start_y + bot_row_space * 2, 14, theme_dim);

             // LOG BUFFER: Full width, below columns, above progress bar
             int log_y = bot_inner_y_offset + bot_inner_h + 8;
 
             std::vector<std::string> local_logs;
             {
                 std::lock_guard<std::mutex> lock(log_mutex);
                 int logs_to_show = 2;
                 int start_log = log_buffer.size() > (size_t)logs_to_show ? log_buffer.size() - logs_to_show : 0;
                 for (size_t i = start_log; i < log_buffer.size(); i++) {
                     local_logs.push_back(log_buffer[i]);
                 }
             }
 
             for (const auto& line : local_logs) {
                 draw_text(TextFormat("> [DATA_STREAM] %s", line.c_str()), text_x, log_y, 14, theme_dim);
                 log_y += 18;
             }
             if (!local_logs.empty()) {
                 draw_text(cursor, text_x + MeasureText(TextFormat("> [DATA_STREAM] %s", local_logs.back().c_str()), 14), log_y - 18, 14, theme_color);
             }


            // Segmented Progress Bar
            if (cache_total > 0) {
                float pct = (float)cache_current / (float)cache_total;
                int bar_y = mid_y + 130;
                int bar_w = box_w - 120;
                DrawRectangleLines(text_x, bar_y, bar_w, 18, theme_dim);

                int fill_w = (int)((bar_w - 4) * pct);
                for (int bx = 2; bx < fill_w; bx += 10) {
                    int w = std::min(8, fill_w - bx);
                    DrawRectangle(text_x + bx, bar_y + 2, w, 14, theme_color);
                }

                draw_text(TextFormat("[%3d%%]", (int)(pct * 100)), text_x + bar_w + 15, bar_y, 16, theme_color);

                // Add the current file being processed exactly below the progress bar
                if (!current_cache_file.empty()) {
                    draw_text(current_cache_file, text_x, bar_y + 28, 14, theme_dim);
                }
            }
        } else {
            draw_text("DB_STAT  : AWAITING_I/O_PIPELINE... " + cursor, text_x, bot_row_start_y, 16, theme_dim);
            draw_text("QUEUE    : BLOCKED", text_x, bot_row_start_y + 22, 14, theme_dim);

            // Empty Progress Bar
            int bar_y = mid_y + 130;
            int bar_w = box_w - 120;
            DrawRectangleLines(text_x, bar_y, bar_w, 18, Fade(theme_dim, 0.3f));
            draw_text("[000%]", text_x + bar_w + 15, bar_y, 16, Fade(theme_dim, 0.5f));
        }

        EndDrawing();
    }










    void draw_text(const std::string& text, int x, int y, int size, Color color) {
        if (crt_font.texture.id == 0) return;

        // Phosphor Glow Effect (1980s CRT bleed)
        Color glow = Fade(color, 0.4f);
        DrawTextEx(crt_font, text.c_str(), {(float)x - 1, (float)y}, size, 1, glow);
        DrawTextEx(crt_font, text.c_str(), {(float)x + 1, (float)y}, size, 1, glow);
        DrawTextEx(crt_font, text.c_str(), {(float)x, (float)y - 1}, size, 1, glow);
        DrawTextEx(crt_font, text.c_str(), {(float)x, (float)y + 1}, size, 1, glow);

        // Core crisp text
        DrawTextEx(crt_font, text.c_str(), {(float)x, (float)y}, size, 1, color);
    }

    void draw_progress_bar(int x, int y, int w, int h, float pct) {
        pct = std::max(0.0f, std::min(1.0f, pct));
        int bar_w = (int)(w * 0.9f);
        int filled = (int)(bar_w * pct);
        if (filled > bar_w) filled = bar_w;
        DrawRectangle(x, y, w, h, (Color){0, 40, 0, 200});
        DrawRectangle(x, y, filled, h, (Color){0, 200, 0, 240});
    }

     };

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  SLIDESHOW
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static Color GetAverageColor(Image img) {
     if (img.data == nullptr || img.width <= 0 || img.height <= 0) return (Color){220, 210, 195, 255};
     const int MAX_STEPS = 64;
     int step_x = std::max(1, img.width  / MAX_STEPS);
     int step_y = std::max(1, img.height / MAX_STEPS);
     long r = 0, g = 0, b = 0, samples = 0;
     for (int y = 0; y < img.height; y += step_y) {
         for (int x = 0; x < img.width; x += step_x) {
             Color c = GetImageColor(img, x, y);
             r += c.r; g += c.g; b += c.b;
             ++samples;
         }
     }
     if (samples == 0) return (Color){220, 210, 195, 255};
     return (Color){
         (unsigned char)(r / samples),
         (unsigned char)(g / samples),
         (unsigned char)(b / samples),
         255
     };
 }

static Color GetEdgeAvgColor(Image img, int depth, int which) {
    // which: 0=top 1=bottom 2=left 3=right
    if (img.data == nullptr || img.width <= 0 || img.height <= 0) return BLACK;
    int x0, y0, x1, y1;
    depth = std::max(1, depth);
    if      (which == 0) { x0=0; y0=0;               x1=img.width; y1=std::min(depth,img.height); }
    else if (which == 1) { x0=0; y1=img.height;       x1=img.width; y0=std::max(0,img.height-depth); }
    else if (which == 2) { y0=0; x0=0;                y1=img.height; x1=std::min(depth,img.width); }
    else                 { y0=0; x1=img.width;         y1=img.height; x0=std::max(0,img.width-depth); }
    const int STEPS = 32;
    int sx = std::max(1,(x1-x0)/STEPS), sy = std::max(1,(y1-y0)/STEPS);
    long r=0,g=0,b=0,n=0;
    for (int y=y0; y<y1; y+=sy)
        for (int x=x0; x<x1; x+=sx) {
            Color c=GetImageColor(img,x,y);
            r+=c.r; g+=c.g; b+=c.b; n++;
        }
    if (n==0) return BLACK;
    return {(unsigned char)(r/n),(unsigned char)(g/n),(unsigned char)(b/n),255};
}


// --- Debug logger for slideshow (thread-safe, timestamped, rotated) ---
// v16.3.0: rewritten for thread safety (mutex-protected FILE*),
//           no per-frame directory scans, robust re-open on failure.
// v16.4.0: fixed of data race (C2), localtime_r (C3), va_list UAF (C1)
// v16.7.0: uses g_cfg.log_dir instead of hardcoded path
// v1.9.5: file-scope statics for slide_debug_close() (W3)
static std::string _slide_log_dir() {
    if (!g_cfg.log_dir.empty()) return g_cfg.log_dir;
    std::string h = getenv("HOME") ? getenv("HOME") : "/home/pi";
    return h + "/piTrove/logs";
}

// File-scope statics for slide_debug (accessible by slide_debug_close)
static std::mutex __slide_debug_mtx;
static FILE* __slide_debug_f = nullptr;
static std::atomic<bool> __slide_debug_of{false};
static time_t __slide_debug_last_rotate = 0;
static bool __slide_debug_first = true;
static std::string __slide_debug_fname;

static void slide_debug(const char* fmt, ...)
{
    // v16.4.0: moved of check + fopen inside lock to prevent data race (C2)
    // v16.5.0: early return moved outside lock_guard to avoid UB on return-from-lock
    {
        std::lock_guard<std::mutex> lk(__slide_debug_mtx);
        if (!__slide_debug_of) {
            // FIX v16.7.0: build path from log_dir + timestamped filename
            auto now = std::chrono::system_clock::now();
            auto ts = std::chrono::system_clock::to_time_t(now);
            struct tm tmb;
            char datestr[32];
            strftime(datestr, sizeof(datestr), "%Y%m%d_%H%M%S", localtime_r(&ts, &tmb));
            __slide_debug_fname = _slide_log_dir() + "/slide_debug_" + std::string(datestr) + ".log";
            __slide_debug_f = fopen(__slide_debug_fname.c_str(), "a");
            if (__slide_debug_f) {
                __slide_debug_of = true;
            } else {
                __slide_debug_of = false;
            }
        }
    }
    if (!__slide_debug_of.load()) return;

    // v16.5.0: rotation check fully inside lock — f + last_rotate_ts both protected
    bool do_rotate = false;
    {
        std::lock_guard<std::mutex> lk(__slide_debug_mtx);
        time_t now = time(nullptr);
        struct stat szWcheck;
        if (__slide_debug_f && fstat(fileno(__slide_debug_f), &szWcheck) == 0 && szWcheck.st_size > 5 * 1024 * 1024) {
            do_rotate = true;
        } else if (!__slide_debug_first && now - __slide_debug_last_rotate > 300) {
            __slide_debug_last_rotate = now;
            do_rotate = true;
        }
        if (__slide_debug_first) __slide_debug_first = false;
    }

    if (do_rotate) {
        std::lock_guard<std::mutex> lk(__slide_debug_mtx);
        // Rotate old files
        try {
            std::vector<std::string> files;
            std::string logdir = _slide_log_dir();
            for (const auto& entry : std::filesystem::directory_iterator(logdir)) {
                std::string fn = entry.path().filename().string();
                if (fn.find("slide_debug_") == 0 && fn.find(".log") != std::string::npos) {
                    files.push_back(entry.path().string());
                }
            }
            std::sort(files.begin(), files.end());
            while ((int)files.size() > 3) {
                std::filesystem::remove(files.front());
                files.erase(files.begin());
            }
        } catch (...) {}
        // Re-open current file
        if (__slide_debug_f) {
            fclose(__slide_debug_f);
            __slide_debug_f = nullptr;
        }
        __slide_debug_fname.clear();
        __slide_debug_of = false;
    }

    // v16.3.0: try to re-open if f became NULL (e.g. file was deleted)
    // v16.5.0: lock protects against race with other threads + double-check pattern
    {
       std::lock_guard<std::mutex> lk(__slide_debug_mtx);
        if (!__slide_debug_f) {
            __slide_debug_f = fopen(__slide_debug_fname.empty() ? (_slide_log_dir() + "/slide_debug.log").c_str() : __slide_debug_fname.c_str(), "a");
            if (__slide_debug_f) {
                ftruncate(fileno(__slide_debug_f), 0);  // Truncate stale content after rotation
                __slide_debug_of = true;
            }
            else return;
        }
    }

    va_list ap;
    va_start(ap, fmt);
    char line[1024];
    int n = std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    // v16.5.0: removed gettimeofday() syscall — cache seconds-level timestamp only
    // v6.0.10: moved timestamp update inside lock to prevent data race (B3)
    char tb[64];
    {
        std::lock_guard<std::mutex> lk(__slide_debug_mtx);
        static time_t cached_sec = -1;
        static struct tm cached_tm;
        static char cached_tb[64];
        time_t tv = time(nullptr);
        struct tm* tm = localtime_r(&tv, &cached_tm);
        if (!tm) return;
        if (tv != cached_sec) {
            cached_sec = tv;
            snprintf(cached_tb, sizeof(cached_tb), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
        }
        snprintf(tb, sizeof(tb), "%s", cached_tb);
        fprintf(__slide_debug_f, "[%s] %s\n", tb, line);
        fflush(__slide_debug_f);
    }
}

// v1.9.5: Close slide_debug FILE* to prevent resource leak (W3)
void slide_debug_close() {
    std::lock_guard<std::mutex> lk(__slide_debug_mtx);
    if (__slide_debug_f) {
        fclose(__slide_debug_f);
        __slide_debug_f = nullptr;
        __slide_debug_of = false;
    }
}
struct Slideshow;
void treadmill_worker(const Config&, Slideshow&);
struct Slideshow {
    Font      hud_font{};
    bool      hud_font_loaded{false};
    std::shared_ptr<std::vector<MediaItem>> items;
    std::atomic<int> current_index{0};
    std::atomic<int> next_index{-1};
    int frame_current_index{0};
    int frame_next_index{-1};

    Texture2D current_tex{};
    Texture2D loaded_tex{};

    int current_w{0}, current_h{0};
    std::atomic<int> next_w{0}, next_h{0};

    double transition_timer{0.0};
    double transition_progress{0.0};
    double item_timer{0.0};
    bool   transitioning{false};
     bool   fading_in{false};
     float  fade_in_timer{0.0f};
     float    scan_time{0.0f};
     bool   done{false};

    double kb_timer{0.0};
    float  kb_zoom{1.0f};
    float  kb_pan_x{0.0f}, kb_pan_y{0.0f};

  bool   shuffle{true};
    std::mutex shuffle_mutex;
    std::shared_ptr<std::vector<MediaItem>> get_items() {
        std::lock_guard<std::mutex> lk(shuffle_mutex);
        return items;
    }
std::atomic<bool> current_is_video{false};  // v6.0.10: atomic for HTTP thread safety (B7)
      // v3.0.0: g_mpv is a global defined inline above; subprocess fields below kept for mpv_video_play() fallback only
      std::mt19937 rng{std::random_device{}() ^ static_cast<unsigned int>(time(nullptr))};
        std::thread treadmill_thread;

    // Async preload
    std::thread preload_thread;
    std::mutex preload_mutex;
    std::mutex preload_lifecycle_mtx;  // v1.9.6: protects preload_running + thread lifecycle
    std::mutex drm_mutex;  // v4.5.8: protects DRM master drop to prevent race with preload VRAM ops
    std::atomic<bool> preload_ready{false};
    std::atomic<bool> preload_running{false};
    std::atomic<bool> stop_preload{false};  // FIX v16.8.0: forces preload thread to exit immediately
      Image preloaded_img{0};
      std::atomic<bool> preloaded_img_valid{false};

      // Corrupted files cache — prevents repeated NFS I/O on bad files
      uint64_t corrupted_cache_seq{0};
      std::unordered_map<std::string, std::pair<int, uint64_t>> corrupted_cache;
      std::mutex corrupted_cache_mtx;
      static const int MAX_CORRUPTED_CACHE = 50;

    // Console font for loading screen (same as P2/P3)
      Font console_font{};
      bool console_font_loaded{false};

    // Shaders
      Shader wipe_shader{0};
      Shader pixelate_shader{0};
      bool shaders_loaded{false};

       // 3D frame calculation (recompute fit dimensions CPU-side for frame drawing)
       float frame_x{0}, frame_y{0}, frame_w{0}, frame_h{0};

  // EFF-8: HUD text caching
      std::string cached_hud_text;
      Vector2 cached_hud_size{};
      int last_render_idx{-1};

std::atomic<int> preload_progress{0};
        std::atomic<int> preload_max{0};
      std::atomic<bool> preload_initial_phase{true};
     std::atomic<bool> preload_cancel{false};

        // First image preloaded during Phase 3 — skip disk load on startup
         int first_idx{-1};
         std::mutex first_img_mtx;
         std::condition_variable first_img_cv;
         // preload_limit removed v6.0.10: atomic declared but never read (B8)
        Texture2D first_img_tex{0};
        Color first_img_color{BLACK};
        std::atomic<bool> first_img_ready{false};
        std::atomic<bool> first_img_thread_done{false};
        std::thread first_img_thread;

      Color current_bg_color{BLACK};
      std::atomic<unsigned int> next_bg_color_hex{0xFF000000};
      std::atomic<unsigned int> next_bias_top_hex{0xFF000000};
      std::atomic<unsigned int> next_bias_bot_hex{0xFF000000};
      std::atomic<unsigned int> next_bias_lft_hex{0xFF000000};
      std::atomic<unsigned int> next_bias_rgt_hex{0xFF000000};
      Color current_bias_top{210, 195, 165, 255};
      Color current_bias_bot{210, 195, 165, 255};
      Color current_bias_lft{210, 195, 165, 255};
      Color current_bias_rgt{210, 195, 165, 255};

      // FIX v16.0.0: prevent reentrant remote command (advance -> load_item -> advance loop)
       // v6.0.3: Changed to atomic<bool> to prevent deadlock if interrupted by signal/nested call
       std::atomic<bool> reentrant_command{false};

    void load_item(const MediaItem& item, std::shared_ptr<std::vector<MediaItem>> items_ptr = nullptr) {
        // v6.0.11: Accept items_ptr for duration write-back to prevent race with treadmill worker
         // Remote commands consumed in main loop (KEY_RIGHT/KEY_LEFT)
         // to avoid reentrant advance() -> load_item() loops
         // FIX v16.1.0

         if (g_cache) g_cache->mark_shown(item.path);

         if (current_tex.id != 0) {
             UnloadTexture(current_tex);
             current_tex.id = 0;
         }

         // Use preloaded first image — instant startup, zero disk I/O
          bool use_preloaded = false;
          {
              std::lock_guard<std::mutex> lk(first_img_mtx);
              if (first_img_ready.load() && first_idx >= 0 && current_index.load() == first_idx && first_img_tex.id != 0) {
                  current_tex = first_img_tex;
                  current_bg_color = first_img_color;
                  first_img_tex.id = 0;
                  use_preloaded = true;
              }
          }
          if (use_preloaded) {
               first_img_ready.store(false);
               current_w = current_tex.width;
               current_h = current_tex.height;
               current_is_video.store(false);
               return;
           }

    if (item.type == "video") {
                  g_logger.info("LOAD_ITEM: video idx=%d path=%s", current_index.load(), item.path.substr(0, 80).c_str());
                  // Probe duration if not already set (preload missed it)
                  if (item.duration <= 0.0) {
                       double dur = probe_video_duration(item.path, g_cfg.video_probe_timeout * 1000);
                       if (dur > 0) {
                           std::lock_guard<std::mutex> lk(shuffle_mutex);
                                  int ci = current_index.load();
                                  auto target = items_ptr ? items_ptr : get_items();  // v6.0.11: use passed items_ptr
                                  if (ci >= 0 && ci < (int)target->size())
                              (*target)[ci].duration = dur;
                      }
                     g_logger.info("LOAD_ITEM: probed duration=%.1fs", dur);
                 }
                 // CRITICAL: Prevent VRAM leak when transitioning to video
                 if (current_tex.id != 0) {
                     UnloadTexture(current_tex);
                     current_tex.id = 0;
                 }
                 current_is_video.store(true);
                   current_w = g_cfg.screen_w;
                  current_h = g_cfg.screen_h;

                  // In-process libmpv render API — shares Raylib's EGL context.
                   if (!g_mpv.is_initialized()) {
                      g_mpv.surface_w    = g_cfg.screen_w;
                      g_mpv.surface_h    = g_cfg.screen_h;
                      g_mpv.video_volume = g_cfg.video_volume;
                     if (!g_mpv.init()) {
                           g_logger.error("LOAD_ITEM: g_mpv.init() failed — skipping video");
                           current_is_video.store(false);
                           return;
                       }
                  }
                  g_mpv.play(item.path);
      return;
              }
       current_is_video.store(false);

           // Check preload thread's corrupted cache before loading
          {
              std::lock_guard<std::mutex> lk(corrupted_cache_mtx);
              auto it = corrupted_cache.find(item.path);
              if (it != corrupted_cache.end() && it->second.first >= 1) {
                  // Mark as corrupted and skip
                  g_logger.warn("Skipping corrupted file (preloaded): %s", item.path.c_str());
                  return;
              }
          }

          try {
              Image img;
             std::string ext_lower = item.ext;
             for (auto& c : ext_lower) c = tolower(c);

             if (ext_lower == "heic" || ext_lower == "heif") {
                 img = LoadImageHEIC(item.path);
             } else if (ext_lower == "webp") {
                 img = LoadImageWebP(item.path);
             } else {
                 img = LoadImageRobust(item.path);
             }

if (img.data && img.width > 0 && img.height > 0) {
                          // v7.8.1: Read EXIF at load time (cache only has placeholder=1)
                          int exif_rot = read_exif_rotation_timeout(item.path, 3000);
                          bool rotated = apply_exif_rotation(img, exif_rot);
                          current_bg_color = GetAverageColor(img);

                        current_tex = LoadTextureVRAMSafe(img);
                         if (current_tex.id != 0) {
                             current_w = current_tex.width;
                             current_h = current_tex.height;
                             g_logger.info("LOAD_ITEM: image idx=%d loaded %dx%d tex.id=%d%s",
                                 current_index.load(), current_tex.width, current_tex.height, current_tex.id,
                                 rotated ? " (rotated)" : "");
                        } else {
                            g_logger.error("LOAD_ITEM: image idx=%d failed to create texture %s",
                                current_index.load(), item.path.substr(0, 80).c_str());
                        }
                        UnloadImage(img);
                } else {
                    g_logger.warn("LOAD_ITEM: image load returned empty/invalid idx=%d %s",
                        current_index.load(), item.path.substr(0, 80).c_str());
                }
          } catch (const std::exception& e) {
             g_logger.error("load_item crashed for %s: %s", item.path.c_str(), e.what());
         } catch (...) {
             g_logger.error("load_item crashed for %s (unknown)", item.path.c_str());
         }
     }

    void preload_next() {
        // v6.0.6: Capture items shared_ptr to prevent treadmill worker from replacing it
        auto items_ptr = get_items();
        if (items_ptr->empty()) return;

        int ni = next_index.load();
        if (ni < 0) { next_index.store((current_index.load() + 1) % (int)items_ptr->size()); }
        slide_debug("PRELOAD_NEXT: cur=%d next=%d items_ptr=%d", current_index.load(), next_index.load(), (int)items_ptr->size());

        {
            std::lock_guard<std::mutex> lk(preload_lifecycle_mtx);
            if (preload_running.load(std::memory_order_relaxed)) return; // v7.8.0: Already running
            if (preload_thread.joinable()) {
                preload_cancel.store(true);
                preload_thread.detach();
            }
            preload_cancel.store(false);
            preload_ready.store(false); // v7.8.0: Reset BEFORE setting running=true to prevent race
            preload_running.store(true, std::memory_order_relaxed);
        }

        preload_thread = std::thread([this, items_ptr]() {
            try {
                if (items_ptr->empty()) { preload_ready.store(true); preload_running.store(false); return; }
                g_logger.info("PRELOAD_START: idx=%d path=%s", next_index.load(),
                    (*items_ptr)[next_index.load()].path.substr(0, 60).c_str());
                bool found_valid = false;
                int attempts = 0;
                int corrupted_count = 0;
                int max_corrupted = preload_initial_phase.load() ? 10 : 20;
                int max_attempts = preload_initial_phase.load() ? 15 : 30;
                max_attempts = std::min(max_attempts, (int)items_ptr->size());
                preload_max.store(max_attempts);

                while (!found_valid && attempts < max_attempts && corrupted_count < max_corrupted && !preload_cancel.load() && !stop_preload.load()) {
                    attempts++;
                    preload_progress.store(attempts);
                    int idx = next_index.load();
                    if (idx < 0 || idx >= (int)items_ptr->size()) { 
                        next_index.store((current_index.load() + 1) % (int)items_ptr->size()); 
                        idx = next_index.load(); 
                    }
                    slide_debug("PRELOAD_THREAD: idx=%d current=%d items_ptr=%d", idx, current_index.load(), (int)items_ptr->size());
                    auto next_item = (*items_ptr)[idx];
                    slide_debug("PRELOAD_THREAD: path=%s ext=%s", next_item.path.substr(0,60).c_str(), next_item.ext.c_str());

                    if (next_item.type == "image") {
                        {
                            std::lock_guard<std::mutex> lk(corrupted_cache_mtx);
                            auto it = corrupted_cache.find(next_item.path);
                            if (it != corrupted_cache.end() && it->second.first >= 1) {
                                g_logger.info("PRELOAD_SKIP: corrupted cache hit idx=%d path=%s", idx, next_item.path.substr(0, 60).c_str());
                                int ni_curr = next_index.fetch_add(1, std::memory_order_relaxed);
                                int ni_next = (ni_curr + 1 >= (int)items_ptr->size()) ? 0 : (ni_curr + 1);
                                next_index.store(ni_next);
                                if (next_index.load() == current_index.load()) break;
                                corrupted_count++;
                                continue;
                            }
                        }

                        std::string ext_lower = next_item.ext;
                        std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
                        Image local_img = {};
                        if (ext_lower == "heic" || ext_lower == "heif") local_img = LoadImageHEIC(next_item.path);
                        else if (ext_lower == "webp") local_img = LoadImageWebP(next_item.path);
                        else local_img = LoadImageRobust(next_item.path);

                        if (local_img.data && local_img.width > 0 && local_img.height > 0) {
                            std::lock_guard<std::mutex> lk(preload_mutex);
                            // v7.8.1: Read EXIF at preload time (cache only has placeholder=1)
                            int exif_rot = read_exif_rotation_timeout(next_item.path, 3000);
                            apply_exif_rotation(local_img, exif_rot);
                            preloaded_img = local_img;
                            next_w.store(preloaded_img.width);
                            next_h.store(preloaded_img.height);
                            Color c = GetAverageColor(preloaded_img);
                            next_bg_color_hex.store(((unsigned int)c.r << 24) | ((unsigned int)c.g << 16) | ((unsigned int)c.b << 8) | (unsigned int)c.a);
                            int depth = std::max(1, std::min(preloaded_img.width, preloaded_img.height) / 6);
                            auto pack = [](Color c) -> unsigned int { return ((unsigned int)c.r<<24)|((unsigned int)c.g<<16)|((unsigned int)c.b<<8)|(unsigned int)c.a; };
                            next_bias_top_hex.store(pack(GetEdgeAvgColor(preloaded_img, depth, 0)));
                            next_bias_bot_hex.store(pack(GetEdgeAvgColor(preloaded_img, depth, 1)));
                            next_bias_lft_hex.store(pack(GetEdgeAvgColor(preloaded_img, depth, 2)));
                            next_bias_rgt_hex.store(pack(GetEdgeAvgColor(preloaded_img, depth, 3)));
                            preloaded_img_valid.store(true);
                            found_valid = true;
                        } else {
                            g_logger.info("PRELOAD_FAIL: image load failed idx=%d ext=%s %s", idx, next_item.ext.c_str(), next_item.path.substr(0, 60).c_str());
                            {
                                std::lock_guard<std::mutex> lk(corrupted_cache_mtx);
                                if (corrupted_cache.find(next_item.path) == corrupted_cache.end()) corrupted_cache_seq++;
                                corrupted_cache[next_item.path] = {1, corrupted_cache_seq};
                                if ((int)corrupted_cache.size() > MAX_CORRUPTED_CACHE) {
                                    auto oldest = corrupted_cache.begin();
                                    for (auto it = corrupted_cache.begin(); it != corrupted_cache.end(); ++it)
                                        if (it->second.second < oldest->second.second) oldest = it;
                                    corrupted_cache.erase(oldest);
                                }
                            }
                            if (next_index.load() == current_index.load()) break;
                            corrupted_count++;
                        }
                    } else {
                        double dur = probe_video_duration(next_item.path, g_cfg.video_probe_timeout * 1000);
                        if (dur > 0) {
                            std::lock_guard<std::mutex> lk(shuffle_mutex);
                            if (idx >= 0 && idx < (int)items_ptr->size())
                                (*items_ptr)[idx].duration = dur;
                        }
                        g_logger.info("PRELOAD_VID: probed %s duration=%.1fs", next_item.path.substr(0, 60).c_str(), dur);
                        found_valid = true;
                    }
                }
                bool was_initial = preload_initial_phase.exchange(false);
                if (found_valid) {
                    g_logger.info("PRELOAD_DONE: attempts=%d corrupted=%d found=yes phase=%s",
                        attempts, corrupted_count, was_initial ? "initial" : "remaining");
                    preload_ready.store(true);
                    // v7.8.0: Do NOT reset preload_running on success — only advance() resets it
                } else {
                    preload_ready.store(true);
                    {
                        std::lock_guard<std::mutex> lk(preload_lifecycle_mtx);
                        preload_running.store(false);
                    }
                }
            } catch (const std::exception& e) {
                g_logger.error("preload_next crashed: %s", e.what());
                preload_ready.store(true);
                { std::lock_guard<std::mutex> lk(preload_lifecycle_mtx); preload_running.store(false); }
            } catch (...) {
                g_logger.error("preload_next crashed (unknown)");
                preload_ready.store(true);
                { std::lock_guard<std::mutex> lk(preload_lifecycle_mtx); preload_running.store(false); }
            }
        });
    }


    void clear_tex_refs() {
             // FIX v16.9.0: dedicated cleanup — unloads any dangling textures before slide advance
             // v6.0.5: Hold first_img_mtx when accessing first_img_tex to prevent race with first_img_thread
             if (current_tex.id != 0) { UnloadTexture(current_tex); current_tex.id = 0; }
             if (loaded_tex.id != 0) { UnloadTexture(loaded_tex); loaded_tex.id = 0; }
             {
                 std::lock_guard<std::mutex> lk(first_img_mtx);
                 if (first_img_tex.id != 0) { UnloadTexture(first_img_tex); first_img_tex.id = 0; }
             }
         }

    bool advance(bool forward = true) {
         // FIX v16.1.0: Guard against reentrant advance() calls from remote commands
         // Check BEFORE clearing references to prevent permanent black screen on double-advance
         if (reentrant_command.load()) return false;
         // v6.0.6: Capture items_ptr shared_ptr to prevent treadmill worker from replacing it
         auto items_ptr = get_items();
         if (items_ptr->empty()) return false;
         reentrant_command.store(true);
         // Now safe to clear active references since guards have passed
         clear_tex_refs();
         // v6.0.10: RAII guard — ensures reentrant_command reset even if load_item() throws (B6)
         struct ReentrantGuard { Slideshow* s; ~ReentrantGuard() { s->reentrant_command.store(false); } } guard{this};

slide_debug("ADVANCE: fwd=%d cur=%d items_ptr=%d", forward ? 1 : 0, current_index.load(), (int)items_ptr->size());
         g_logger.info("ADVANCE: forward=%d prev_idx=%d items_ptr=%d shuffle=%d",
             forward ? 1 : 0, current_index.load(), (int)items_ptr->size(), shuffle ? 1 : 0);

       int prev_idx = current_index.load();
        if (shuffle) {
            // v6.0.3: Lock shuffle_mutex to protect rng state and current_index
            std::lock_guard<std::mutex> lk(shuffle_mutex);
            std::uniform_int_distribution<int> dist(0, (int)items_ptr->size() - 1);
            do { current_index.store(dist(rng)); }
            while (current_index.load() == prev_idx   && items_ptr->size() > 1);
        } else {
            current_index.store((current_index.load() + (forward ? 1 : -1) + (int)items_ptr->size()) % (int)items_ptr->size());
        }

       current_w = 0;
        current_h = 0;
         int ci = current_index.load();
         load_item((*items_ptr)[ci], items_ptr);  // v6.0.12: pass items_ptr for duration write-back

        // FIX v1.9.8: skip corrupted images directly in advance() instead of waiting
        // for the transition guard to stall on a black screen.
        if (!current_is_video && current_tex.id == 0 && !first_img_ready.load()) {
             int skip_limit = (int)items_ptr->size();
              int skip_count = 0;
              for (int skipped = 0; skipped < skip_limit; skipped++) {
                  {
                      std::lock_guard<std::mutex> lk(corrupted_cache_mtx);
                      if (ci >= 0 && ci < (int)items_ptr->size() && corrupted_cache.find((*items_ptr)[ci].path) == corrupted_cache.end())
                          corrupted_cache_seq++;
                      if (ci >= 0 && ci < (int)items_ptr->size())
                          corrupted_cache[(*items_ptr)[ci].path] = {1, corrupted_cache_seq};
                  }
                 // ── NEW FIX: Mark bad=1 in cache DB so Phase 2 skips it forever ──
                  if (g_cache && ci >= 0 && ci < (int)items_ptr->size())
                      g_cache->mark_bad((*items_ptr)[ci].path);
                  // ──────────────────────────────────────────────────────────────
                  if (ci >= 0 && ci < (int)items_ptr->size())
                       g_logger.warn("ADVANCE_SKIP: corrupted file #%d/%d %s", skipped+1, skip_limit, (*items_ptr)[ci].path.c_str());
                  skip_count++;
   int prev = current_index.load();
                   if (shuffle) {
                      // v6.0.3: Lock shuffle_mutex to protect rng state
                      std::lock_guard<std::mutex> lk(shuffle_mutex);
                      std::uniform_int_distribution<int> dist(0, (int)items_ptr->size() - 1);
                      do { current_index.store(dist(rng)); }
                      while (current_index.load() == prev   && items_ptr->size() > 1);
                  } else {
                     current_index.store((prev + (forward ? 1 : -1) + (int)items_ptr->size()) % (int)items_ptr->size());
                 }
                 ci = current_index.load();
                 if (ci >= 0 && ci < (int)items_ptr->size())
      load_item((*items_ptr)[ci], items_ptr);  // v6.0.11: pass items_ptr for duration write-back
                if (current_is_video || current_tex.id != 0) break;
              }
             g_logger.info("ADVANCE_SKIP_DONE: skipped %d corrupted files, now at idx=%d is_video=%d tex.id=%d",
                 skip_count, current_index.load(), current_is_video ? 1 : 0, current_tex.id);
         }

        kb_timer = 0;
         kb_zoom = 1.0f;
         kb_pan_x = 0;
         kb_pan_y = 0;
         // v7.8.0: Wait for in-flight preload thread, then reset flag (prevents race with advance())
         if (preload_thread.joinable()) {
             preload_thread.join();
         }
         {
             std::lock_guard<std::mutex> lk(preload_lifecycle_mtx);
             preload_running.store(false, std::memory_order_relaxed);
         }
          // reentrant_command.reset handled by ReentrantGuard destructor (B6 fix)

          g_logger.info("ADVANCE_COMPLETE: cur=%d type=%s tex.id=%d",
              current_index.load(), current_is_video ? "video" : "image", current_tex.id);
        item_timer = 0;
        transitioning = false;
        transition_timer = 0;
        transition_progress = 0.0;
        fading_in = false;
        fade_in_timer = 0.0f;
        return true;
    }

    static float ease_in_out(float t) {
        return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * (1.0f - t) * (1.0f - t);
    }

    void update(float dt) {
        // v6.0.6: Capture shared_ptr and indices to prevent treadmill worker from replacing them mid-frame (B276)
        std::shared_ptr<std::vector<MediaItem>> items_ptr;
        {
            std::lock_guard<std::mutex> lk(shuffle_mutex);
            items_ptr = items;
            frame_current_index = current_index.load();
            frame_next_index = next_index.load();
        }
        if (items_ptr->empty()) return;

        // Fix B271: Ensure current_index is within bounds immediately
        if (frame_current_index < 0 || frame_current_index >= (int)items_ptr->size()) {
            current_index.store(0);
            frame_current_index = 0;
        }

        // Capture config locally to avoid data races with HTTP thread (B264)
        Config cfg;
        {
            std::lock_guard<std::mutex> lk(g_config_mtx);
            cfg = g_cfg;
        }

        // Ensure preload is always active if not ready
        if (!preload_running.load() && !preload_ready.load()) {
            preload_next();
        }


         if (preload_ready.load()) {
        slide_debug("PRELOAD_READY: valid=%s data=%s", preloaded_img_valid.load() ? "Y" : "N", preloaded_img.data ? "Y" : "N");
         if (preloaded_img_valid.load() && preloaded_img.data) {
             g_logger.info("PRELOAD_READY: valid image received");
         }
             bool img_valid = false;
            Image img_to_load;
            if (preload_cancel.exchange(false)) { 
                slide_debug("PRELOAD_READY: cancelled, discarding"); 
                preload_ready.store(false); 
            } else {
                {
                    std::lock_guard<std::mutex> lk(preload_mutex);
                    img_valid = preloaded_img_valid.load();
                    if (img_valid) {
                        preloaded_img_valid.store(false);
                        img_to_load = preloaded_img;
                        preloaded_img = {};
                    }
                }
                // Fix B4 (v1.8.8): Copy Image struct under mutex, then do GPU ops outside
                if (img_valid && img_to_load.data != nullptr) {
                    if (loaded_tex.id != 0) UnloadTexture(loaded_tex);
                    loaded_tex = LoadTextureVRAMSafe(img_to_load);
                    slide_debug("PRELOAD_LOADED: tex id=%d w=%d h=%d", loaded_tex.id, loaded_tex.width, loaded_tex.height);
                    g_logger.info("PRELOAD_LOADED: tex id=%d w=%d h=%d", loaded_tex.id, loaded_tex.width, loaded_tex.height);
                    UnloadImage(img_to_load);
                    slide_debug("PRELOAD_FREED: tex id=%d", loaded_tex.id);
                }
                preload_ready.store(false);
            }
        }

    if (fading_in) {
        slide_debug("FADE_CHECK: fading=%d timer=%f dur=%f", fading_in, fade_in_timer, cfg.transition_duration);
          fade_in_timer += dt;
          if (fade_in_timer >= (float)cfg.transition_duration)
              fading_in = false;
         slide_debug("FADE_IN_DONE: timer=%f", fade_in_timer);
      } else {
          item_timer += dt;  // only count display time after fade-in completes
         slide_debug("ITEM_TIMER: t=%f delay=%f", item_timer, cfg.transition_delay);
      }

    bool video_playing = false;
              if (current_is_video && g_mpv.is_playing()) {
                  video_playing = true;
              }
              g_logger.debug("UPDATE_STATE: is_video=%d mpv_playing=%d video_playing=%d transitioning=%d",
                  current_is_video ? 1 : 0, g_mpv.is_playing() ? 1 : 0, video_playing ? 1 : 0, transitioning ? 1 : 0);

             if (!transitioning) {
                bool time_up = false;
                if (current_is_video) {
                      // time_up when in-process mpv finished (EOF) or stalled >2s without starting
                      time_up = (g_mpv.has_eof() || (!video_playing && item_timer > 2.0));
              } else {
                 time_up = (item_timer >= cfg.transition_delay);
             slide_debug("TIME_UP: t=%f del=%f up=%d", item_timer, cfg.transition_delay, item_timer >= cfg.transition_delay);
             }

            if (time_up) {
                transitioning = true;
            slide_debug("TRANS_START: transitioning=true");
                transition_timer = 0;
            }
        }

        if (transitioning) {
            transition_timer += dt;
            double safe_dur = std::max(cfg.transition_duration, 0.001);
            transition_progress = ease_in_out(
                (float)std::min(transition_timer / safe_dur, 1.0));

            if (transition_progress >= 1.0f) {
          // Guard: never swap to an empty texture — that causes a permanent black screen.
            // FIX v1.9.8: exempt video-next items_ptr; they intentionally carry no texture.
                 int _guard_ni = frame_next_index;
                 bool _next_is_video = (_guard_ni >= 0 && _guard_ni < (int)items_ptr->size()
                                        && (*items_ptr)[_guard_ni].type == "video");
             if (loaded_tex.id == 0 && !_next_is_video) {
                      // FIX v16.8.0: unload any leftover texture to prevent VRAM leak on failed preload
                      slide_debug("TRANS_GUARD: loaded_tex.id==0! cur=%d next=%d", frame_current_index, frame_next_index);
                      if (!preload_running.load()) {
                          int failed_idx = frame_next_index;
                          // FIX v1.9.9: fetch_add returns the OLD value; compute
                          // the incremented value explicitly before storing.
                          int ni3 = next_index.fetch_add(1, std::memory_order_relaxed);
                          int ni3_next = (ni3 + 1 >= (int)items_ptr->size()) ? 0 : (ni3 + 1);
                          next_index.store(ni3_next);
                          // CRITICAL FIX: Break state out of active transition immediately to halt frame-by-frame thread spawning
                          transitioning = false;
                          transition_timer = 0;
                          transition_progress = 0.0;
                          item_timer = 0;
                          if (ni3_next != failed_idx && ni3_next != frame_current_index) {
                              preload_next();
                          }
                      }
                      return;
                  }


                 UnloadTexture(current_tex);
                   current_tex = loaded_tex;
                   slide_debug("SWAP: tex id=%d w=%d h=%d idx=%d", current_tex.id, current_tex.width, current_tex.height, frame_current_index);
                   g_logger.info("SWAP: loaded tex id=%d w=%d h=%d idx=%d -> idx=%d",
                       current_tex.id, current_tex.width, current_tex.height, frame_current_index, frame_current_index + 1);
                   loaded_tex.id = 0;
                   { std::lock_guard<std::mutex> lk(preload_lifecycle_mtx); preload_running.store(false, std::memory_order_relaxed); } // v7.8.0: Reset after swap so guard block can trigger next preload


                 current_w = current_tex.width;
                 current_h = current_tex.height;
            {
                     std::lock_guard<std::mutex> lk(preload_mutex);
                     unsigned int hex = next_bg_color_hex.load();
                     current_bg_color = {(unsigned char)(hex >> 24), (unsigned char)(hex >> 16), (unsigned char)(hex >> 8), (unsigned char)hex};
                    {
                        auto unpack = [](unsigned int h) -> Color {
                            return {(unsigned char)(h>>24),(unsigned char)(h>>16),(unsigned char)(h>>8),(unsigned char)h};
                        };
                        current_bias_top = unpack(next_bias_top_hex.load());
                        current_bias_bot = unpack(next_bias_bot_hex.load());
                        current_bias_lft = unpack(next_bias_lft_hex.load());
                        current_bias_rgt = unpack(next_bias_rgt_hex.load());
                    }
                 }
                 current_index.store(frame_next_index);
                 next_index.store(-1);
 
                int _swap_ci = frame_next_index;
                  if (_swap_ci >= 0 && _swap_ci < (int)items_ptr->size() && (*items_ptr)[_swap_ci].type == "video") {

                        g_logger.info("SWAP_TO_VIDEO: transitioning to video idx=%d", _swap_ci);
                        // CRITICAL: Prevent VRAM leak when transitioning to video
                        if (current_tex.id != 0) {
                            UnloadTexture(current_tex);
                            current_tex.id = 0;
                        }
                      current_is_video.store(true);
                          current_w = cfg.screen_w;
                          current_h = cfg.screen_h;

                          // In-process g_mpv — initialized lazily on first video
                          if (!g_mpv.is_initialized()) {
                              g_mpv.surface_w    = cfg.screen_w;
                              g_mpv.surface_h    = cfg.screen_h;
                              g_mpv.video_volume = cfg.video_volume;
                             if (!g_mpv.init()) {
                                 g_logger.error("SWAP_TO_VIDEO: g_mpv.init() failed — skipping");
              current_is_video.store(false);
                                 advance(true);
                                 return;
                             }
                         }
                         if (!g_mpv.play((*items_ptr)[_swap_ci].path)) {
                            g_logger.error("SWAP_TO_VIDEO: g_mpv.play() failed — skipping");
                            current_is_video.store(false);
                            advance(true);
                            return;
                        }
                  } else {
                     current_is_video.store(false);
                 }

            transitioning = false;
                transition_timer = 0;
                transition_progress = 0.0;
                fading_in = true;
                fade_in_timer = 0.0f;
                item_timer = 0;
                 kb_timer = 0;
                 kb_zoom = 1.0f;
                 kb_pan_x = 0;
                 kb_pan_y = 0;
         // FIX v1.9.9: kick off preload for the item after the newly
                   // displayed one. advance() only preloads what was just swapped
                   // in — nothing preloads the successor until we do it here.
                    preload_next();
                    slide_debug("POST_SWAP: preload_kicked cur=%d", frame_next_index);
                    g_logger.info("SWAP_COMPLETE: cur=%d type=%s transitioning=false",
                        frame_next_index, current_is_video ? "video" : "image");
             }

        }

        if (cfg.ken_burns && !transitioning) {
            kb_timer += dt * cfg.ken_burns_speed;
            float phase = fmod(kb_timer, 2.0f);
          if (phase < 1.0f) {
                    kb_zoom = 1.0f + phase * cfg.ken_burns_zoom;
                    kb_pan_x = sin(kb_timer * 0.5f) * 0.03f;
                    kb_pan_y = cos(kb_timer * 0.7f) * 0.02f;
                } else {
                    float t = phase - 1.0f;
                    kb_zoom = (1.0f + cfg.ken_burns_zoom) - t * cfg.ken_burns_zoom;
                kb_pan_x = sin(kb_timer * 0.5f + 1.0f) * 0.03f * (1.0f - t);
                kb_pan_y = cos(kb_timer * 0.7f + 1.0f) * 0.02f * (1.0f - t);
            }
        }
    }

   void render() {
          int sw = GetScreenWidth();
          int sh = GetScreenHeight();
        // v6.0.6: Capture items_ptr and indices to prevent treadmill worker from replacing them mid-render (B276)
        // v6.0.12: Capture g_cfg under lock to prevent data races during render (B154)
        std::shared_ptr<std::vector<MediaItem>> items_ptr;
        int frame_ci, frame_ni;
        Config render_cfg;
        {
            std::lock_guard<std::mutex> lk(shuffle_mutex);
            items_ptr = items;
            frame_ci = current_index.load();
            frame_ni = next_index.load();
            std::lock_guard<std::mutex> lk2(g_config_mtx);
            render_cfg = g_cfg;
        }
        Color avg = current_bg_color;

      if (current_is_video) {
                    // ── NEW FIX v7.0.2: Clear background so transition fades don't accumulate ──
                    // Video decoders output RGB into FBO but leave alpha=0.
                    // Raylib alpha blending draws transparent video over stale frames → infinite black glow.
                    ClearBackground(BLACK);

                    // ── VIDEO RENDER: blit g_mpv.video_rt.texture to screen ──
                    // update_frame() decoded the latest mpv frame into video_rt BEFORE BeginDrawing.
                    // Raylib FBO source height is inverted (-h) for correct coordinate orientation.
                    if (g_mpv.is_initialized() && g_mpv.video_rt.texture.id != 0) {
                        // ── FIX v7.0.2: Disable blending so alpha=0 FBOs draw opaquely ──
                        rlDisableColorBlend();
                        Rectangle srcRec = {
                            0.0f, 0.0f,
                            (float)g_mpv.video_rt.texture.width,
                            -(float)g_mpv.video_rt.texture.height
                        };
                        Rectangle dstRec = {
                            0.0f, 0.0f,
                            (float)sw, (float)sh
                        };
                        DrawTexturePro(g_mpv.video_rt.texture, srcRec, dstRec, {0.0f, 0.0f}, 0.0f, WHITE);
                        rlEnableColorBlend();
                        // ─────────────────────────────────────────────────────────────────
                    }
             } else if (render_cfg.bias_lighting) {
                // ── Background: Animated YouTube-Style Ambient Bias Lighting ──

                // Base ambient background (darkened photo color)
                Color ambientBase = { 
                    (unsigned char)(avg.r * 0.15f), 
                    (unsigned char)(avg.g * 0.15f), 
                    (unsigned char)(avg.b * 0.15f), 255 
                };
                ClearBackground(ambientBase);

                Color transparent = { avg.r, avg.g, avg.b, 0 };
                float time_val = (float)GetTime() * render_cfg.bias_anim_speed;

                // Pre-calculate image dimensions for accurate edge-hugging glow
                float scale = 1.0f;
                float bpw = sw, bph = sh; // Background photo width/height
                if (current_w > 0 && current_h > 0) {
                    scale = std::min((float)sw / current_w, (float)sh / current_h);
                    if (render_cfg.matting && !current_is_video) {
                        int matting_total = render_cfg.matting_size * 2;
                        scale = std::min((float)(sw - matting_total) / current_w, (float)(sh - matting_total) / current_h);
                    }
                    bpw = current_w * scale;
                    bph = current_h * scale;
                }
                float bpx = (sw - bpw) / 2.0f;
                float bpy = (sh - bph) / 2.0f;
                
                // ── RADIATING / ABSORBING: Slow, Soft Blend Ambient Light ──
                if (render_cfg.bias_anim_speed > 0.001f && (render_cfg.bias_anim_style == "radiating" || render_cfg.bias_anim_style == "absorbing")) {
                    
                    float slow_time = time_val * 0.15f;

                    DrawCircleGradient({(float)(sw/2), (float)(sh/2)}, sw, {avg.r, avg.g, avg.b, 40}, transparent);

                    for (int i = 0; i < 2; i++) {
                        float phase = fmodf(slow_time + (i * 0.5f), 1.0f); 
                        if (render_cfg.bias_anim_style == "absorbing") phase = 1.0f - phase;
                        
                        float radius = sw * 0.1f + (sw * 1.3f * phase);
                        float alpha_curve = sinf(phase * PI); 
                        unsigned char orbAlpha = (unsigned char)(100.0f * alpha_curve);
                        
                        DrawCircleGradient({(float)(sw/2), (float)(sh/2)}, radius, {avg.r, avg.g, avg.b, orbAlpha}, transparent);
                    }

                } else if (render_cfg.bias_anim_style == "edge_glow" && render_cfg.bias_anim_speed > 0.001f) {
                    // ── EDGE GLOW: Seamless 360 corona hugging the photo bounds ──
                    float slow_time = time_val * 0.15f; 
                    
                    Color cGlow = avg;
                    if (render_cfg.bias_color_mode == "rainbow") {
                        float hueBase = fmodf((float)GetTime() * 40.0f * render_cfg.bias_anim_speed, 360.0f);
                        cGlow = ColorFromHSV(hueBase, 0.85f, 0.9f);
                    }

                    // Corona animation: breathes in and out slowly
                    float pulse = (sinf(slow_time) + 1.0f) * 0.5f; 
                    float min_expand = sw * 0.08f;
                    float max_expand = min_expand + (sw * 0.15f * pulse); // Dynamically expands outwards
                    int steps = 40; // High stack count for flawlessly smooth gradient
                    
                    // Draw stacked rounded rectangles to create a mathematical distance-field corona
                    for (int i = 0; i < steps; i++) {
                        float progress = (float)i / steps;
                        float expand = max_expand * progress;
                        
                        // Inverse quadratic falloff for soft, realistic ambient light scattering
                        float alpha_curve = 1.0f - progress;
                        alpha_curve = alpha_curve * alpha_curve; 
                        
                        // Peak alpha is very low (25) because 40 layers overlap
                       unsigned char a = (unsigned char)((render_cfg.bias_strength / 10.0f) * alpha_curve); 
                        if (a == 0) continue;

                        // Add 10px buffer to account for the thickness of the 3D frame border
                        Rectangle rec = { 
                            bpx - 10 - expand, 
                            bpy - 10 - expand, 
                            bpw + 20 + 2.0f*expand, 
                            bph + 20 + 2.0f*expand 
                        };
                        
                        // Calculate true distance-field corner rounding so it blends seamlessly 360-degrees
                        float corner_radius = 10.0f + expand;
                        float roundness = (corner_radius * 2.0f) / std::min(rec.width, rec.height);
                        roundness = std::min(1.0f, std::max(0.0f, roundness));
                        
                        DrawRectangleRounded(rec, roundness, 48, {cGlow.r, cGlow.g, cGlow.b, a});
                    }

                } else {
                    // ── PULSING (or Frozen if speed is 0) ──
                    float pulse = 0.5f;
                    if (render_cfg.bias_anim_speed > 0.001f) {
                        pulse = (sinf(time_val) + 1.0f) * 0.5f; 
                    }
                    
                    unsigned char glowAlpha = (unsigned char)(render_cfg.bias_strength * 0.8f + render_cfg.bias_strength * 0.4f * pulse);
                    
                    // Replaced crosshair rectangles with a single clean background orb
                    DrawCircleGradient({(float)sw/2, (float)sh/2}, sw, {avg.r, avg.g, avg.b, (unsigned char)(glowAlpha/2)}, transparent);
                    
                    float radiusAnim = sw / 1.5f + (sw * 0.05f * pulse);
                    unsigned char radialAlpha = (unsigned char)(80 + 30 * pulse);
                    DrawCircleGradient({(float)sw/2, (float)sh/2}, radiusAnim, {avg.r, avg.g, avg.b, radialAlpha}, transparent);
                }

          // Vignette to darken outer corners and draw focus to the center
                if (render_cfg.vignette_enabled) {
                    DrawRectangleGradientH(0,       0, sw/4,   sh, (Color){0,0,0,120}, (Color){0,0,0,0});
                    DrawRectangleGradientH(sw*3/4,  0, sw/4,   sh, (Color){0,0,0,0},   (Color){0,0,0,120});
                    DrawRectangleGradientV(0,       0, sw,     sh/4, (Color){0,0,0,100}, (Color){0,0,0,0});
                    DrawRectangleGradientV(0, sh*3/4, sw,     sh/4, (Color){0,0,0,0},   (Color){0,0,0,100});
                }
           } else {
               ClearBackground(BLACK);
           }

           if (items_ptr->empty()) {
             DrawText("No media items", sw/2 - 80, sh/2 - 20, 24, WHITE);
             return;
         }


         if (!current_is_video) {
              // --- COLLAGE MODE ---
           if (render_cfg.collage_enabled && current_tex.id != 0 && current_tex.width > 0 && current_tex.height > 0) {
              int matte = render_cfg.matting ? render_cfg.matting_size : 0;
              int cols = std::max(1, render_cfg.collage_cols);
              int rows = std::max(1, render_cfg.collage_rows);
             int cell_w = (sw - matte * 2) / cols;
             int cell_h = (sh - matte * 2) / rows;
            int gap = 4;

            for (int r = 0; r < rows; r++) {
                for (int c = 0; c < cols; c++) {
                    int cell_x = matte + c * cell_w;
                    int cell_y = matte + r * cell_h;
                    int cw = cell_w - gap;
                    int ch = cell_h - gap;

                     // Get item index for collage (cyclic)
                     int ri = frame_ci;
                     int total = (int)items_ptr->size();
                     int collage_idx = (ri + r * cols + c) % total;

                    if (collage_idx < 0) collage_idx += total;

                    // Use cached texture if available, otherwise current
                    Texture2D tex = current_tex;
                    int tw = current_w;
                    int th = current_h;

                    // If we have next texture loaded, use it for variety
                    if (c == 0 && r == 0 && loaded_tex.id != 0) {
                        tex = loaded_tex;
                        tw = next_w.load();
                        th = next_h.load();
                    }

                    if (tw > 0 && th > 0) {
                        float img_ratio = (float)tw / (float)th;
                        float cell_ratio = (float)cw / (float)ch;

                        float draw_w, draw_h;
                        if (img_ratio > cell_ratio) {
                            draw_w = (float)cw;
                            draw_h = draw_w / img_ratio;
                        } else {
                            draw_h = (float)ch;
                            draw_w = draw_h * img_ratio;
                        }

                        float draw_x = cell_x + (cw - draw_w) / 2.0f;
                        float draw_y = cell_y + (ch - draw_h) / 2.0f;

                        // FIX v16.6.0: Clamp to cell boundaries to prevent border bleed
                        float clamp_x = fmaxf(draw_x, (float)cell_x);
                        float clamp_y = fmaxf(draw_y, (float)cell_y);
                        float clamp_w = fminf(draw_w, (float)(cell_x + cw) - clamp_x);
                        float clamp_h = fminf(draw_h, (float)(cell_y + ch) - clamp_y);
                        if (clamp_w <= 0 || clamp_h <= 0) clamp_w = 0;

                        Rectangle src = {0, 0, (float)tw, (float)th};
                        Rectangle dst = {clamp_x, clamp_y, clamp_w, clamp_h};
                        if (clamp_w > 0 && clamp_h > 0)
                            DrawTexturePro(tex, src, dst, {0, 0}, 0.0f, WHITE);

                        // Cell border
                        DrawRectangleLines(cell_x, cell_y, cw, ch, (Color){100, 100, 100, 150});
                    }
                }
            }
} else // --- PHOTO RENDER ---
      if (current_tex.id != 0 && current_tex.width > 0 && current_tex.height > 0) {
            // ── Matte compensation: photo fits within inset area, full photo visible ──
            // FIX v16.7.0: apply matting_size, clamp Ken Burns to texture bounds,
            //   position 3D border outside photo rect
            int matte = (render_cfg.matting && render_cfg.matting_size > 0) ? render_cfg.matting_size : 0;
            float ax = (float)matte, ay = (float)matte;
            float aw = (float)(sw - matte * 2), ah = (float)(sh - matte * 2);

            float img_ratio = (float)current_tex.width / (float)current_tex.height;
            float area_ratio = aw / ah;
            float pw, ph;
            if (img_ratio > area_ratio) { pw = aw; ph = pw / img_ratio; }
            else                        { ph = ah; pw = ph * img_ratio; }
            float px = ax + (aw - pw) / 2.0f;
            float py = ay + (ah - ph) / 2.0f;

            // Ken Burns: zoom source rect — clamped to texture bounds, never bleeds outside dst
            float kb_src_w = (float)current_tex.width  / kb_zoom;
            float kb_src_h = (float)current_tex.height / kb_zoom;
            float kb_sx    = ((float)current_tex.width  - kb_src_w) * 0.5f
                              + kb_pan_x * (float)current_tex.width;
            float kb_sy    = ((float)current_tex.height - kb_src_h) * 0.5f
                              + kb_pan_y * (float)current_tex.height;
            kb_sx = std::max(0.0f, std::min(kb_sx, (float)current_tex.width  - kb_src_w));
            kb_sy = std::max(0.0f, std::min(kb_sy, (float)current_tex.height - kb_src_h));
            
            float rotation = 0.0f;
            float final_pw = pw, final_ph = ph;
            float final_px = px, final_py = py;
            Vector2 origin = {0, 0};

             if (render_cfg.auto_display_rotation && items_ptr) {
                 int idx = frame_ci;
                 if (idx >= 0 && idx < (int)items_ptr->size()) {
                     int rot = (*items_ptr)[idx].exif_rotation;

                    if (rot == 90) {
                        rotation = 90.0f;
                        std::swap(final_pw, final_ph);
                        final_px = ax + (aw - final_pw) / 2.0f;
                        final_py = ay + (ah - final_ph) / 2.0f;
                        origin = {final_pw / 2, final_ph / 2};
                    } else if (rot == 270) {
                        rotation = 270.0f;
                        std::swap(final_pw, final_ph);
                        final_px = ax + (aw - final_pw) / 2.0f;
                        final_py = ay + (ah - final_ph) / 2.0f;
                        origin = {final_pw / 2, final_ph / 2};
                    } else if (rot == 180) {
                        rotation = 180.0f;
                        origin = {final_pw / 2, final_ph / 2};
                    }
                }
            }
            
            Rectangle src = {kb_sx, kb_sy, kb_src_w, kb_src_h};
            Rectangle dst = {final_px + origin.x, final_py + origin.y, final_pw, final_ph};
            DrawTexturePro(current_tex,
                           src,
                           dst,
                           origin, rotation, WHITE);

            // ── 3D picture-frame border — Dynamic Colors based on Photo ──
            if (!current_is_video && render_cfg.border_enabled) {
                int bdr = render_cfg.border_width;
                float x1 = px,      y1 = py;
                float x2 = px + pw, y2 = py + ph;
                int ix1 = (int)x1, iy1 = (int)y1;
                int ix2 = (int)x2, iy2 = (int)y2;

                // Dynamically build lighting structure from average photo color
                Color hi  = { (unsigned char)std::min(255, avg.r + 65), (unsigned char)std::min(255, avg.g + 65), (unsigned char)std::min(255, avg.b + 65), 255 };
                Color lo  = { (unsigned char)(avg.r * 0.25f), (unsigned char)(avg.g * 0.25f), (unsigned char)(avg.b * 0.25f), 255 };

                // TL: two hi faces — subtle dark crease
                Color tl_seam = { (unsigned char)std::max(0, (int)avg.r - 35), (unsigned char)std::max(0, (int)avg.g - 35), (unsigned char)std::max(0, (int)avg.b - 35), 215 };
                // TR/BL: hi meets lo — bright glint
                Color tr_seam = { (unsigned char)std::min(255, avg.r + 85), (unsigned char)std::min(255, avg.g + 85), (unsigned char)std::min(255, avg.b + 85), 255 };
                Color bl_seam = tr_seam;
                // BR: two lo faces — near-black crease
                Color br_seam = { (unsigned char)(avg.r * 0.18f), (unsigned char)(avg.g * 0.18f), (unsigned char)(avg.b * 0.18f), 215 };

                // ── Side faces (full-width, full-height — overlaps corners) ──
                DrawRectangle(ix1,       iy1 - bdr, ix2 - ix1, bdr,     hi); // top
                DrawRectangle(ix1,       iy2,       ix2 - ix1, bdr,     lo); // bottom
                DrawRectangle(ix1 - bdr, iy1,       bdr, iy2 - iy1,     hi); // left
                DrawRectangle(ix2,       iy1,       bdr, iy2 - iy1,     lo); // right

               // ── TL corner (miter) ──
                DrawRectangle(ix1 - bdr, iy1 - bdr, bdr, bdr, hi);
                DrawLineEx((Vector2){(float)(ix1-1),      (float)(iy1-1)},
                           (Vector2){(float)(ix1-bdr+1),   (float)(iy1-bdr+1)}, 1.5f, tl_seam);

                // ── TR corner (solid hi base + lo triangle overlay for 3D miter effect) ──
                DrawRectangle(ix2, iy1 - bdr, bdr, bdr, hi);
                DrawTriangle({(float)ix2,       (float)iy1},
                             {(float)(ix2+bdr), (float)iy1},
                             {(float)(ix2+bdr), (float)(iy1-bdr)}, lo);
                DrawLineEx((Vector2){(float)(ix2+1),      (float)(iy1-1)},
                           (Vector2){(float)(ix2+bdr-1),   (float)(iy1-bdr+1)}, 1.5f, tr_seam);

                // ── BL corner (solid hi base + lo triangle overlay for 3D miter effect) ──
                DrawRectangle(ix1 - bdr, iy2, bdr, bdr, hi);
                DrawTriangle({(float)(ix1-bdr), (float)(iy2+bdr)},
                             {(float)ix1,       (float)(iy2+bdr)},
                             {(float)ix1,       (float)iy2}, lo);
                DrawLineEx((Vector2){(float)(ix1-1),      (float)(iy2+1)},
                           (Vector2){(float)(ix1-bdr+1),   (float)(iy2+bdr-1)}, 1.5f, bl_seam);

                // ── BR corner (miter) ──
                DrawRectangle(ix2, iy2, bdr, bdr, lo);
                DrawLineEx((Vector2){(float)(ix2+1),      (float)(iy2+1)},
                           (Vector2){(float)(ix2+bdr-1),   (float)(iy2+bdr-1)}, 1.5f, br_seam);

               // 1px outline at exact photo boundary for crisp separation
                 DrawRectangleLinesEx((Rectangle){(float)(ix1 - 1), (float)(iy1 - 1), (float)(ix2 - ix1 + 2), (float)(iy2 - iy1 + 2)}, 1, (Color){0,0,0,180});
             }

} // end photo-only block


} // end if (current_tex.id != 0) photo render

// ── Overlays: drawn for BOTH photos and videos ──
{
    int pad = 15;
    // Date overlay
    if (render_cfg.date_overlay_enabled) {
        char datebuf[64];
        time_t now = time(nullptr);
        struct tm tm_buf;
        struct tm* tm_info = localtime_r(&now, &tm_buf);
        if (tm_info && strftime(datebuf, sizeof(datebuf), render_cfg.date_text.c_str(), tm_info) != 0) {
            int dx = pad + (int)((sw - pad * 2) * render_cfg.date_x);
            int dy = pad + (int)((sh - pad * 2) * render_cfg.date_y);
            Color dcol = overlay_color_from_str(render_cfg.date_color);
            DrawText(datebuf, dx + 2, dy + 2, render_cfg.date_font_size, (Color){0,0,0,180});
            DrawText(datebuf, dx,     dy,     render_cfg.date_font_size, dcol);
        }
    }

    // Filename overlay
    int ri = current_index.load();
    if (render_cfg.filename_enabled && ri >= 0 && ri < (int)items_ptr->size()) {
        std::string fname = (*items_ptr)[ri].filename;
        double dur = 0.0;
        {
            std::lock_guard<std::mutex> lk(preload_mutex);
            dur = (*items_ptr)[ri].duration;
        }
        
        if (current_is_video) {
            double rem = g_mpv.video_time_remaining.load();
            if (rem > 0.0) {
                int r_mins = (int)rem / 60;
                int r_secs = (int)rem % 60;
                char rem_buf[32];
                snprintf(rem_buf, sizeof(rem_buf), " [%02d:%02d remaining]", r_mins, r_secs);
                fname += std::string(rem_buf);
            } else if (dur > 0) {
                int mins = (int)dur / 60;
                int secs = (int)dur % 60;
                char dur_buf[16];
                snprintf(dur_buf, sizeof(dur_buf), " (%d:%02d)", mins, secs);
                fname += std::string(dur_buf);
            }
        }
        int fx = pad + (int)((sw - pad * 2) * render_cfg.filename_x);
        int fy = pad + (int)((sh - pad * 2) * render_cfg.filename_y);
        DrawText(fname.c_str(), fx + 2, fy + 2, render_cfg.filename_font_size, (Color){0,0,0,180});
        DrawText(fname.c_str(), fx,     fy,     render_cfg.filename_font_size, WHITE);
    }

    // Count overlay
    if (render_cfg.count_enabled) {
        char cntbuf[128];
        std::snprintf(cntbuf, sizeof(cntbuf), "%d / %d", current_index.load() + 1, (int)items_ptr->size());
        int cx = pad + (int)((sw - pad * 2) * render_cfg.count_x);
        int cy = pad + (int)((sh - pad * 2) * render_cfg.count_y);
        int tw = MeasureText(cntbuf, render_cfg.count_font_size);
        cx = cx - tw / 2;
        DrawText(cntbuf, cx + 2, cy + 2, render_cfg.count_font_size, (Color){0,0,0,180});
        DrawText(cntbuf, cx,     cy,     render_cfg.count_font_size, (Color){200,200,200,220});
    }

    // Timer overlay
    if (render_cfg.timer_enabled) {
        char tbuf[32];
        if (current_is_video) {
            double rem = g_mpv.video_time_remaining.load();
            int r_mins = (int)rem / 60;
            int r_secs = (int)rem % 60;
            std::snprintf(tbuf, sizeof(tbuf), "%02d:%02d", r_mins, r_secs);
        } else {
            int rem = std::max(0, (int)(render_cfg.transition_delay - item_timer));
            std::snprintf(tbuf, sizeof(tbuf), "%ds", rem);
        }
        int tx = pad + (int)((sw - pad * 2) * render_cfg.timer_x);
        int ty = pad + (int)((sh - pad * 2) * render_cfg.timer_y);
        Color tcol = overlay_color_from_str(render_cfg.timer_color);
        DrawText(tbuf, tx + 2, ty + 2, render_cfg.timer_font_size, (Color){0,0,0,180});
        DrawText(tbuf, tx,     ty,     render_cfg.timer_font_size, tcol);
    }

    // Clock overlay
    if (render_cfg.clock_enabled) {
        char clkbuf[16];
        time_t now = time(nullptr);
        struct tm tm_buf_clk;
        struct tm* tmi = localtime_r(&now, &tm_buf_clk);
        if (tmi) {
            strftime(clkbuf, sizeof(clkbuf), render_cfg.clock_24h ? "%H:%M" : "%I:%M %p", tmi);
            int clkw = MeasureText(clkbuf, render_cfg.clock_font_size);
            int clkx = pad + (int)((sw - pad*2) * render_cfg.clock_x) - clkw/2;
            int clky = pad + (int)((sh - pad*2) * render_cfg.clock_y);
            Color clkcol = overlay_color_from_str(render_cfg.clock_color);
            DrawText(clkbuf, clkx+2, clky+2, render_cfg.clock_font_size, (Color){0,0,0,180});
            DrawText(clkbuf, clkx,   clky,   render_cfg.clock_font_size, clkcol);
        }
    }
}

// ── Transition overlays — cover all 4 cases (photo↔video) ──
float dur = (float)render_cfg.transition_duration;

// OUTGOING: fade current content to black as transition_progress → 1
if (transitioning) {
    float prog = ease_in_out(transition_progress);

    if (!current_is_video && render_cfg.transition_effect == "wipe" && shaders_loaded) {
        int loc0 = GetShaderLocation(wipe_shader, "texture0");
        int loc1 = GetShaderLocation(wipe_shader, "texture1");
        int locProg = GetShaderLocation(wipe_shader, "progress");
        if (loc0 != -1 && loc1 != -1 && locProg != -1) {
            SetShaderValueTexture(wipe_shader, loc0, current_tex);
            SetShaderValueTexture(wipe_shader, loc1, loaded_tex);
            SetShaderValue(wipe_shader, locProg, &transition_progress, SHADER_UNIFORM_FLOAT);
            BeginShaderMode(wipe_shader);
            Rectangle t_src = {0,0,(float)current_tex.width,(float)current_tex.height};
            DrawTexturePro(current_tex, t_src, {0,0,(float)sw,(float)sh}, {0,0}, 0.0f, WHITE);
            EndShaderMode();
        } else {
            unsigned char black_a = (unsigned char)(255.0f * prog);
            DrawRectangle(0, 0, sw, sh, (Color){0,0,0,black_a});
        }
    } else if (!current_is_video && render_cfg.transition_effect == "pixelate" && shaders_loaded) {
        int loc0 = GetShaderLocation(pixelate_shader, "texture0");
        int loc1 = GetShaderLocation(pixelate_shader, "texture1");
        int locProg = GetShaderLocation(pixelate_shader, "progress");
        if (loc0 != -1 && loc1 != -1 && locProg != -1) {
            SetShaderValueTexture(pixelate_shader, loc0, current_tex);
            SetShaderValueTexture(pixelate_shader, loc1, loaded_tex);
            SetShaderValue(pixelate_shader, locProg, &transition_progress, SHADER_UNIFORM_FLOAT);
            BeginShaderMode(pixelate_shader);
            Rectangle t_src = {0,0,(float)current_tex.width,(float)current_tex.height};
            DrawTexturePro(current_tex, t_src, {0,0,(float)sw,(float)sh}, {0,0}, 0.0f, WHITE);
            EndShaderMode();
        } else {
            unsigned char black_a = (unsigned char)(255.0f * prog);
            DrawRectangle(0, 0, sw, sh, (Color){0,0,0,black_a});
        }
    } else {
        // Crossfade, unsupported effect, or shaders not loaded: fade to black
        unsigned char black_a = (unsigned char)(255.0f * prog);
        DrawRectangle(0, 0, sw, sh, (Color){0, 0, 0, black_a});
    }
}

// INCOMING: fade in from black after every swap (covers video start + photo start)
if (fading_in && dur > 0.0f) {
    float t = std::min(fade_in_timer / dur, 1.0f);
    float prog = ease_in_out(t);
    unsigned char black_a = (unsigned char)(255.0f * (1.0f - prog));
    DrawRectangle(0, 0, sw, sh, (Color){0,0,0,black_a});
}

// ── CRT loading screen — shown only during initial preload, never for video ──
if (!current_is_video && current_tex.id == 0) {
          // ━━ CRT MONOCHROME LOADING SCREEN ━━
              scan_time += GetFrameTime();
              float scan_offset = fmodf(scan_time * 18.0f, 2.0f);

              // Dark background with subtle phosphor glow
              ClearBackground((Color){0, 8, 0, 255});

           // CRT terminal window dimensions
                int term_w = 520;
                int term_h = 320;
                int term_x = sw / 2 - term_w / 2;
                int term_y = sh / 2 - term_h / 2;
                int ch_w = (int)(console_font.baseSize * 0.8f);
                int ch_h = (int)(console_font.baseSize * 1.2f);
                int ox = term_x + ch_w;
                int oy = term_y + ch_h;
                int iw = term_w - ch_w * 2;
                int ih = term_h - ch_h * 2;

                Color green = {0, 255, 0, 255};
                Color dim_green = {0, 180, 0, 255};
                Color bg_fill = {0, 15, 0, 220};

        // Fullscreen phosphor glow (bright green tint)
              DrawRectangle(0, 0, sw, sh, (Color){0, 20, 0, 230});

              // Outer glow (shadow)
              DrawRectangle(term_x - 5, term_y - 5, term_w + 10, term_h + 10, (Color){0, 40, 0, 70});
              DrawRectangle(term_x - 3, term_y - 3, term_w + 6, term_h + 6, (Color){0, 60, 0, 110});
              DrawRectangle(term_x - 1, term_y - 1, term_w + 2, term_h + 2, (Color){0, 80, 0, 150});

                // Main terminal background
                DrawRectangle(term_x, term_y, term_w, term_h, bg_fill);

                // Solid CRT terminal border (2px green lines)
                {
                    int bx1 = ox;
                    int by1 = oy;
                    int bx2 = ox + iw;
                    int by2 = oy + ih;
                    int bw = bx2 - bx1;
                    int bh = by2 - by1;
                    // Top/bottom edges (2px high)
                    DrawRectangle(bx1, by1, bw + 2, 2, green);
                    DrawRectangle(bx1, by2, bw + 2, 2, green);
                    // Left/right edges (2px wide) - overlap corners
                    DrawRectangle(bx1, by1 + 2, 2, bh - 3, green);
                    DrawRectangle(bx1 + bw, by1 + 2, 2, bh - 3, green);
                }

// ── Title bar ──
               int title_y = term_y + ch_h + 8;
              const char* title = "piTrove v" VERSION "  [ PRELOAD CACHE ]";
              int title_w = MeasureTextEx(console_font, title, 14, 0.0f).x;
              int title_x = sw / 2 - title_w / 2;
              DrawTextEx(console_font, title, {(float)title_x, (float)title_y}, 14, 0.0f, green);

// Solid separator line
                {
                    int sep_x = ox;
                    int sep_y = title_y + 20;
                    int sep_w = iw;
                    DrawRectangle(sep_x, sep_y, sep_w + 1, 1, dim_green);
                }

              // ── Status text ──
              int fsz = 20;
              int text_y = title_y + 40;
              int text_x = sw / 2 - 100;

              // "Loading 5 images..." during initial phase, "Preloading images..." otherwise
              const char* status = transitioning ? "Preloading next photo..." : (preload_initial_phase.load() ? "Loading 5 images..." : "Preloading images...");
              DrawTextEx(console_font, status, {(float)(sw / 2 - 100), (float)text_y}, fsz, 0.0f, green);

              // Animated dots
              static int dot_cycle = 0;
              static float dot_timer = 0.0f;
              dot_timer += GetFrameTime();
              if (dot_timer > 0.5f) {
                  dot_timer = 0.0f;
                  dot_cycle = (dot_cycle + 1) % 4;
              }
              const char* dots[] = {"", ".", "..", "..."};
              int dw = MeasureTextEx(console_font, dots[dot_cycle], fsz, 0.0f).x;
              DrawTextEx(console_font, dots[dot_cycle], {(float)(sw / 2 + 100), (float)text_y}, fsz, 0.0f, green);

              // ── Progress bar ──
              int pprog = preload_progress.load();
              int pmax = preload_max.load();
              if (pmax > 0 && pprog > 0) {
                  int bar_y = text_y + fsz + 30;
                  int bar_x = sw / 2 - 150;
                  int bar_w = 300;
                  int bar_h = 24;
                  int filled = (bar_w * pprog) / pmax;
                  if (filled > bar_w) filled = bar_w;

                  // Bar background (dark green)
                  DrawRectangle(bar_x, bar_y, bar_w, bar_h, (Color){0, 30, 0, 255});
                  // Filled portion (bright green, block style)
                  DrawRectangle(bar_x, bar_y, filled, bar_h, (Color){0, 255, 0, 255});
                  // Inner grid pattern (block characters)
                  int block_size = 12;
                  for (int bx = bar_x; bx < bar_x + filled; bx += block_size) {
                      DrawRectangle(bx, bar_y, block_size - 1, bar_h, (Color){0, 120, 0, 80});
                  }
                  // Progress bar solid border (2px thick)
                    {
                        DrawRectangle(bar_x, bar_y, bar_w + 2, 2, green);
                        DrawRectangle(bar_x, bar_y + bar_h - 1, bar_w + 2, 2, green);
                        DrawRectangle(bar_x, bar_y + 2, 2, bar_h - 3, green);
                        DrawRectangle(bar_x + bar_w, bar_y + 2, 2, bar_h - 3, green);
                    }

                  // Progress text (centered, inside bar)
                  char pbuf[32];
                  int pct = (int)((float)pprog / (float)pmax * 100.0f); if (pct > 100) pct = 100;
                  std::snprintf(pbuf, sizeof(pbuf), "%3d%%  %d/%d", pct, pprog, pmax);
                  int ptext_w = MeasureTextEx(console_font, pbuf, 14, 0.0f).x;
                  int ptext_x = sw / 2 - ptext_w / 2;
                  DrawTextEx(console_font, pbuf, {(float)ptext_x, (float)(bar_y + 4)}, 14, 0.0f, (Color){0, 255, 0, 255});
              }

// ── Footer info ──
               int footer_y = term_y + term_h - ch_h * 2 - 30;
               char footer_buf[128];
               // v16.5.0: cache FPS to avoid expensive per-frame computation
               static int cached_fps = 0;
               static int fps_frame = 0;
               fps_frame++;
               if (fps_frame % 10 == 1) cached_fps = GetFPS();
               std::snprintf(footer_buf, sizeof(footer_buf), "SYS: MEMORY=%lldMB  CPU=ARMv8  FPS=%d", g_cfg.cache_mmap_size / (1024 * 1024), cached_fps);
              int footer_w = MeasureTextEx(console_font, footer_buf, 12, 0.0f).x;
              DrawTextEx(console_font, footer_buf, {(float)(sw / 2 - footer_w / 2), (float)footer_y}, 12, 0.0f, dim_green);

              // Blinking cursor
              static float blink_timer = 0.0f;
              blink_timer += GetFrameTime();
              bool cursor_visible = blink_timer < 0.5f;
              if (cursor_visible) {
                   int cursor_x = sw / 2 + 140 + dw;
                   int cursor_y = text_y + fsz - 4;
                   DrawRectangle(cursor_x, cursor_y, 8, fsz + 4, green);
               }

         // CRT scanlines (drawn LAST on top of entire screen)
               for (int sy = 0; sy < sh; sy += 2) {
                   DrawRectangle(0, sy, sw, 1, (Color){0, 255, 0, 30});
               }

              // CRT phosphor glow (inside terminal box, dims at edges)
               for (int gy = oy; gy < oy + ih; gy += 2) {
                   float cy = (float)(gy - oy) / ih;
                   float center = 1.0f - (2.0f * cy - 1.0f) * (2.0f * cy - 1.0f);
                   center = center * center;
                   if (center > 0.4f) {
                       DrawRectangle(ox + 2, gy, iw - 4, 1, (Color){0, (unsigned char)(std::min(255, (int)(50 * center))), 0, (unsigned char)(std::min(255, (int)(40 * center)))});
                   }
               }

              // CRT screen curvature vignette (bright center, dark edges)
              for (int y = 0; y < sh; y += 4) {
                  float edge = 1.0f - 0.3f * (1.0f - (2.0f * y / sh - 1.0f) * (2.0f * y / sh - 1.0f));
                  if (edge < 0.7f) {
                      DrawRectangle(0, y, sw, 4, (Color){0, 0, 0, (unsigned char)(std::min(255, (int)(255 * (1.0f - edge))))});
                  }
              }
          }

         // --- UNCONDITIONAL OVERLAYS ---

        // Weather
        if (!current_is_video && render_cfg.weather_enabled && g_weather_temp.load() > -999.0f) {
            char wbuf[64];
            std::snprintf(wbuf, sizeof(wbuf), "%.1f%cC", g_weather_temp.load(), (char)176);
            int wfs = 20;
            Vector2 ws = MeasureTextEx(hud_font, wbuf, wfs, 1.0f);
            DrawTextEx(hud_font, wbuf, {(float)(sw - ws.x - 20), (float)(sh - 50)},
                       wfs, 1.0f, (Color){255, 255, 100, 200});
        }

    // HUD bar (hidden during video playback)
        int hi = current_index.load();
         if (!current_is_video && hi >= 0 && hi < (int)items_ptr->size()) {
             if (hi != last_render_idx) {
                 char buf[2048];
                 std::snprintf(buf, sizeof(buf), "%d / %d  |  %s",
                               hi + 1, (int)items_ptr->size(),
                               (*items_ptr)[hi].filename.c_str());
                             cached_hud_size = MeasureTextEx(hud_font, buf, 16.0f, 1.0f);
                cached_hud_text = buf;
                last_render_idx = hi;
            }
            DrawRectangle(0, sh - 24, sw, 24, (Color){0, 0, 0, 180});
            DrawTextEx(hud_font, cached_hud_text.c_str(),
                       {(float)(sw/2 - (int)cached_hud_size.x/2), (float)(sh - 22)},
                       16, 1.0f, WHITE);
            const char* hint = "\xe2\x86\x90 \xe2\x86\x92 prev/next  |  SPACE shuffle  |  ESC quit";
            Vector2 hs = MeasureTextEx(hud_font, hint, 14.0f, 1.0f);
            DrawTextEx(hud_font, hint,
                       {(float)(sw/2 - (int)hs.x/2), 8.0f},
                       14, 1.0f, (Color){200, 200, 200, 180});
        }
   }

     void init() {
         hud_font = LoadFontEx("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", 20, nullptr, 0);
        if (hud_font.texture.id == 0) {
            hud_font = GetFontDefault();
            hud_font_loaded = false;
        } else {
            hud_font_loaded = true;
        }
        if (hud_font.texture.id != 0) SetTextureFilter(hud_font.texture, TEXTURE_FILTER_POINT);

      // Load shaders
          wipe_shader = LoadShaderFromMemory(0, wipeShaderCode);
          pixelate_shader = LoadShaderFromMemory(0, pixelateShaderCode);
          shaders_loaded = (wipe_shader.id > 0 && pixelate_shader.id > 0);

          // Console font for loading screen (P2/P3 style)
          console_font = LoadFontEx("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", 24, nullptr, 0);
          if (console_font.texture.id == 0) {
              console_font = GetFontDefault();
              console_font_loaded = false;
          } else {
              console_font_loaded = true;
          }
      }

  void cleanup() {
            // FIX v16.8.0: set stop_preload to force thread exit, then cancel and join
            stop_preload.store(true);
            preload_cancel.store(true);
            if (preload_thread.joinable()) preload_thread.join();
            if (first_img_thread.joinable()) first_img_thread.join();
// v3.2.0: cleanup subprocess mpv (safety net) — removed dead mpv_pid/mpv_running/mpv_monitor
        std::lock_guard<std::mutex> lk(first_img_mtx);
        if (first_img_tex.id) UnloadTexture(first_img_tex);
        first_img_tex.id = 0;
        if (current_tex.id) UnloadTexture(current_tex);
          if (loaded_tex.id) UnloadTexture(loaded_tex);
      if (wipe_shader.id != 0) UnloadShader(wipe_shader);
            if (pixelate_shader.id != 0) UnloadShader(pixelate_shader);
            if (hud_font_loaded) UnloadFont(hud_font);
            if (console_font_loaded) UnloadFont(console_font);
        }
};
// v4.1.4: Temporal Treadmill
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// Async-signal-safe SIGCHLD handler — reaps all zombie children
static void reap_children(int) {
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
}

static void spawn(const std::string& cmd) {
    pid_t pid = fork();
    if (pid == -1) {
        g_logger.debug("spawn fork() failed: %s", strerror(errno));
        return;
    }
    if (pid == 0) {
        // Child: close stdio, reset signal handlers to defaults before exec.
        // v16.5.0: SIGCHLD reset to SIG_DFL ensures child doesn't inherit
        // parent's reap_children handler — harmless since exec replaces the
        // entire process image anyway, but keeps the child's signal state clean.
        // FIX v16.8.0: prctl ensures child dies if parent crashes (prevents zombies)
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        signal(SIGCHLD, SIG_DFL);
        signal(SIGPIPE, SIG_DFL);
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
        _exit(127);
    }
    // Parent: return immediately (non-blocking)
    // v16.2.0: fork failures are silently ignored (rare, only on OOM)
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  BACKGROUND THREADS
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

static void weather_thread_func(const Config& c) {
    while (g_running.load()) {
    // v6.0.11: Read g_cfg under lock each iteration to see config changes (B202)
    bool we; float wl, wn;
    { std::lock_guard<std::mutex> lk(g_config_mtx); we = g_cfg.weather_enabled; wl = g_cfg.weather_lat; wn = g_cfg.weather_lon; }
    if (we && wl != -999.0f && wn != -999.0f) {
             // FIX v6.0.12: Validate latitude/longitude ranges to prevent shell injection via popen
             if (wl < -90.0f || wl > 90.0f || wn < -180.0f || wn > 180.0f) {
                 g_logger.warn("WEATHER: Invalid coordinates lat=%.4f lon=%.4f, skipping", wl, wn);
             } else {
                 // FIX v6.0.12: Use curl with --globoff to disable URL globbing, preventing shell injection
                 char cmd[600];
                 std::snprintf(cmd, sizeof(cmd),
                     "timeout 30 curl -s --globoff 'https://api.open-meteo.com/v1/forecast"
                     "?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code' </dev/null",
                     wl, wn);
                 FILE* fp = popen(cmd, "r");
                 if (fp) {
                     char buf[1024] = {0};
                     size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
                     pclose(fp);

                     if (n > 0 && g_running.load()) {
                         // Parse JSON manually (simple extraction)
                         char* temp_start = strstr(buf, "\"temperature_2m\":");
                         if (temp_start) {
                             float temp = 0.0f;
                             if (std::sscanf(temp_start, "\"temperature_2m\":%f", &temp) == 1) {
                                 g_weather_temp.store(temp);
                             }
                         }
                         char* code_start = strstr(buf, "\"weather_code\":");
                         if (code_start) {
                             int code = 0;
                             if (std::sscanf(code_start, "\"weather_code\":%d", &code) == 1) {
                                 g_weather_code.store(code);
                             }
                         }
                     }
                 }
             }
         }
        // Sleep for 10 minutes before next update
        for (int i = 0; i < 600 && g_running.load(); i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

static void http_thread_func(const Config& c, Slideshow& slide) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) return;

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(c.http_port);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        shutdown(server_fd, SHUT_RDWR);
        close(server_fd);
        return;
    }
    g_http_server_fd = server_fd;

    listen(server_fd, 5);

    // ── Base64 lookup table ──
    static const char b64table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto base64_encode = [](const unsigned char* data, size_t len) -> std::string {
        std::string out;
        out.reserve(((len + 2) / 3) * 4);
        size_t i = 0;
        while (i < len) {
            unsigned int val = 0;
            for (int j = 0; j < 3 && (i + j) < len; j++) {
                val = (val << 8) | data[i + j];
            }
            for (int j = 0; j < 4 && (i / 3) < (len + 2) / 3; j++) {
                int idx = (val >> (6 * (3 - j))) & 0x3F;
                if ((i / 3 * 3 + j) < (int)len) out += b64table[idx];
                else out += (val >> (6 * (3 - j))) & 0x3F ? '=' : '=';
            }
            i += 3;
        }
        return out;
    };

    const char* dashboard_html =
        "<!DOCTYPE html>\n"
        "<html lang=\"en\">\n"
        "<head>\n"
        "  <meta charset=\"UTF-8\">\n"
        "  <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
        "  <title>PiTrove Remote</title>\n"
        "  <style>\n"
        "    * { margin: 0; padding: 0; box-sizing: border-box; }\n"
        "    body {\n"
        "      font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Oxygen, sans-serif;\n"
        "      background: #0d0f13;\n"
        "      color: #e1e4e8;\n"
        "      min-height: 100vh;\n"
        "      display: flex;\n"
        "      flex-direction: column;\n"
        "      align-items: center;\n"
        "      justify-content: flex-start;\n"
        "      padding: 24px 16px;\n"
        "    }\n"
        "    .container {\n"
        "      width: 100%;\n"
        "      max-width: 520px;\n"
        "    }\n"
        "    .header {\n"
        "      text-align: center;\n"
        "      margin-bottom: 24px;\n"
        "      padding: 20px;\n"
        "      background: linear-gradient(135deg, #161b22, #1c2333);\n"
        "      border-radius: 16px;\n"
        "      border: 1px solid #30363d;\n"
        "    }\n"
        "    .header h1 {\n"
        "      font-size: 32px;\n"
        "      font-weight: 700;\n"
        "      background: linear-gradient(135deg, #58a6ff, #bc8cff);\n"
        "      -webkit-background-clip: text;\n"
        "      -webkit-text-fill-color: transparent;\n"
        "      background-clip: text;\n"
        "      margin-bottom: 4px;\n"
        "    }\n"
        "    .header p {\n"
        "      font-size: 14px;\n"
        "      color: #8b949e;\n"
        "    }\n"
        "    .card {\n"
        "      background: #161b22;\n"
        "      border-radius: 14px;\n"
        "      border: 1px solid #30363d;\n"
        "      padding: 20px;\n"
        "      margin-bottom: 16px;\n"
        "    }\n"
        "    .card-title {\n"
        "      font-size: 12px;\n"
        "      text-transform: uppercase;\n"
        "      letter-spacing: 1px;\n"
        "      color: #8b949e;\n"
        "      margin-bottom: 12px;\n"
        "    }\n"
        "    .preview-container {\n"
        "      width: 100%;\n"
        "      aspect-ratio: 16/9;\n"
        "      background: #0d1117;\n"
        "      border-radius: 10px;\n"
        "      display: flex;\n"
        "      align-items: center;\n"
        "      justify-content: center;\n"
        "      overflow: hidden;\n"
        "      border: 1px solid #30363d;\n"
        "      position: relative;\n"
        "    }\n"
        "    .preview-container img {\n"
        "      max-width: 100%;\n"
        "      max-height: 100%;\n"
        "      object-fit: contain;\n"
        "    }\n"
        "    .preview-video {\n"
        "      text-align: center;\n"
        "      color: #f0883e;\n"
        "    }\n"
        "    .preview-video .icon {\n"
        "      font-size: 40px;\n"
        "      margin-bottom: 8px;\n"
        "      animation: pulse 1.5s ease-in-out infinite;\n"
        "    }\n"
        "    .preview-video .text {\n"
        "      font-size: 15px;\n"
        "      font-weight: 500;\n"
        "    }\n"
        "    @keyframes pulse {\n"
        "      0%, 100% { opacity: 1; }\n"
        "      50% { opacity: 0.5; }\n"
        "    }\n"
        "    .count {\n"
        "      text-align: center;\n"
        "      padding: 10px;\n"
        "      font-size: 14px;\n"
        "      color: #8b949e;\n"
        "      margin-bottom: 8px;\n"
        "    }\n"
        "    .count span {\n"
        "      color: #58a6ff;\n"
        "      font-weight: 600;\n"
        "    }\n"
        "    .filename {\n"
        "      word-break: break-word;\n"
        "      font-size: 14px;\n"
        "      color: #c9d1d9;\n"
        "      padding: 12px;\n"
        "      background: #0d1117;\n"
        "      border-radius: 8px;\n"
        "      text-align: center;\n"
        "    }\n"
        "    .type-badge {\n"
        "      display: inline-block;\n"
        "      padding: 4px 10px;\n"
        "      border-radius: 20px;\n"
        "      font-size: 12px;\n"
        "      font-weight: 600;\n"
        "      text-transform: uppercase;\n"
        "      margin: 8px 0 0 0;\n"
        "    }\n"
        "    .type-photo {\n"
        "      background: #238636;\n"
        "      color: #ffffff;\n"
        "    }\n"
        "    .type-video {\n"
        "      background: #da3633;\n"
        "      color: #ffffff;\n"
        "    }\n"
        "    .nav-controls {\n"
        "      display: grid;\n"
        "      grid-template-columns: 1fr 1fr;\n"
        "      gap: 10px;\n"
        "      margin-bottom: 10px;\n"
        "    }\n"
        "    button {\n"
        "      padding: 14px 18px;\n"
        "      font-size: 15px;\n"
        "      font-weight: 500;\n"
        "      border: none;\n"
        "      border-radius: 10px;\n"
        "      cursor: pointer;\n"
        "      transition: all 0.15s ease;\n"
        "      display: flex;\n"
        "      align-items: center;\n"
        "      justify-content: center;\n"
        "      gap: 6px;\n"
        "      font-family: inherit;\n"
        "    }\n"
        "    button:active {\n"
        "      transform: scale(0.96);\n"
        "    }\n"
        "    button:disabled {\n"
        "      opacity: 0.5;\n"
        "      cursor: not-allowed;\n"
        "    }\n"
        "    .btn-prev {\n"
        "      background: #21262d;\n"
        "      color: #e1e4e8;\n"
        "      border: 1px solid #30363d;\n"
        "    }\n"
        "    .btn-prev:hover { background: #30363d; }\n"
        "    .btn-next {\n"
        "      background: #238636;\n"
        "      color: #ffffff;\n"
        "    }\n"
        "    .btn-next:hover { background: #2ea043; }\n"
        "    .btn-pause {\n"
        "      grid-column: span 2;\n"
        "      background: #da3633;\n"
        "      color: #ffffff;\n"
        "    }\n"
        "    .btn-pause:hover { background: #f85149; }\n"
        "    .btn-shuffle {\n"
        "      background: #1f6feb;\n"
        "      color: #ffffff;\n"
        "    }\n"
        "    .btn-shuffle:hover { background: #388bfd; }\n"
        "    .btn-shuffle.active {\n"
        "      background: #f0883e;\n"
        "    }\n"
        "    .btn-restart {\n"
        "      background: #21262d;\n"
        "      color: #e1e4e8;\n"
        "      border: 1px solid #30363d;\n"
        "    }\n"
        "    .btn-restart:hover { background: #30363d; }\n"
        "    .extra-controls {\n"
        "      display: grid;\n"
        "      grid-template-columns: 1fr 1fr;\n"
        "      gap: 10px;\n"
        "      margin-top: 10px;\n"
        "    }\n"
        "    .info {\n"
        "      padding: 14px;\n"
        "      background: #0d1117;\n"
        "      border-radius: 10px;\n"
        "      border: 1px solid #30363d;\n"
        "      font-size: 12px;\n"
        "      color: #8b949e;\n"
        "      text-align: center;\n"
        "      margin-top: 10px;\n"
        "      line-height: 1.6;\n"
        "    }\n"
        "    .info .key { color: #58a6ff; font-weight: 500; }\n"
        "    .loading {\n"
        "      color: #8b949e;\n"
        "      font-size: 14px;\n"
        "      text-align: center;\n"
        "      padding: 20px;\n"
        "    }\n"
        "    .spinner {\n"
        "      display: inline-block;\n"
        "      width: 16px;\n"
        "      height: 16px;\n"
        "      border: 2px solid #30363d;\n"
        "      border-top-color: #58a6ff;\n"
        "      border-radius: 50%;\n"
        "      animation: spin 0.8s linear infinite;\n"
        "      margin-right: 8px;\n"
        "    }\n"
        "    @keyframes spin { to { transform: rotate(360deg); } }\n"
        "    @media (max-width: 480px) {\n"
        "      .header h1 { font-size: 26px; }\n"
        "      .card { padding: 16px; }\n"
        "      button { padding: 12px 14px; font-size: 14px; }\n"
        "    }\n"
        "  </style>\n"
        "</head>\n"
        "<body>\n"
        "  <div class=\"container\">\n"
        "    <div class=\"header\">\n"
        "      <h1>PiTrove</h1>\n"
        "      <p>Digital Picture Frame Remote</p>\n"
        "    </div>\n"
        "    <div class=\"count\">\n"
        "      Item <span id=\"current\">—</span> of <span id=\"total\">—</span>\n"
        "    </div>\n"
        "    <div class=\"card\">\n"
        "      <div class=\"preview-container\" id=\"preview\">\n"
        "        <div class=\"loading\"><span class=\"spinner\"></span>Loading...</div>\n"
        "      </div>\n"
        "    </div>\n"
        "    <div class=\"card\">\n"
        "      <div class=\"card-title\">Current File</div>\n"
        "      <div class=\"filename\" id=\"filename\">Loading...</div>\n"
        "      <div style=\"text-align:center\" id=\"type-badge\"></div>\n"
        "    </div>\n"
        "    <div class=\"nav-controls\">\n"
        "      <button class=\"btn-prev\" onclick=\"nav('prev')\">← Previous</button>\n"
        "      <button class=\"btn-next\" onclick=\"nav('next')\">Next →</button>\n"
        "      <button class=\"btn-pause\" onclick=\"nav('pause')\">Pause / Resume</button>\n"
        "    </div>\n"
        "    <div class=\"extra-controls\">\n"
        "      <button class=\"btn-shuffle\" id=\"shuffle-btn\" onclick=\"toggleShuffle()\">Shuffle: OFF</button>\n"
        "      <button class=\"btn-restart\" onclick=\"restart()\">Restart Service</button>\n"
        "    </div>\n"
        "    <div class=\"info\">\n"
        "      Auto-refreshes every <span class=\"key\">10 seconds</span> • Preview updates on each item change\n"
        "    </div>\n"
        "  </div>\n"
        "  <script>\n"
        "    let shuffleOn = true;\n"
        "    let currentPreviewUrl = '';\n"
        "\n"
        "    function nav(action) {\n"
        "      fetch('/api/' + action)\n"
        "        .then(r => r.text())\n"
        "        .catch(err => console.error('Command failed:', err));\n"
        "    }\n"
        "\n"
        "    function toggleShuffle() {\n"
        "      fetch('/api/toggle_shuffle')\n"
        "        .then(() => { shuffleOn = !shuffleOn; updateShuffleBtn(); })\n"
        "        .catch(err => console.error('Shuffle failed:', err));\n"
        "    }\n"
        "\n"
        "    function updateShuffleBtn() {\n"
        "      const btn = document.getElementById('shuffle-btn');\n"
        "      if (btn) {\n"
        "        btn.textContent = 'Shuffle: ' + (shuffleOn ? 'ON' : 'OFF');\n"
        "        btn.classList.toggle('active', shuffleOn);\n"
        "      }\n"
        "    }\n"
        "\n"
        "    function restart() {\n"
        "      if (confirm('Restart piTrove service?')) {\n"
        "        fetch('/api/restart').then(() => {\n"
        "          document.getElementById('preview').innerHTML = '<div class=\"loading\"><span class=\"spinner\"></span>Restarting...</div>';\n"
        "        }).catch(err => console.error('Restart failed:', err));\n"
        "      }\n"
        "    }\n"
        "\n"
        "    function updateStatus() {\n"
        "      fetch('/api/status')\n"
        "        .then(r => r.json())\n"
        "        .then(data => {\n"
        "          const curr = document.getElementById('current');\n"
        "          const tot = document.getElementById('total');\n"
        "          const fn = document.getElementById('filename');\n"
        "          const badge = document.getElementById('type-badge');\n"
         "          const preview = document.getElementById('preview');\n"
         "          if (curr) curr.textContent = data.current || '—';\n"
         "          if (tot) tot.textContent = data.total || '—';\n"
         "          if (fn) fn.textContent = data.filename || '—';\n"
         "          if (typeof data.shuffle !== 'undefined') {\n"
         "            shuffleOn = data.shuffle;\n"
         "            updateShuffleBtn();\n"
         "          }\n"
        "          if (badge) {\n"
        "            if (data.is_video) {\n"
        "              badge.innerHTML = '<span class=\"type-badge type-video\">Video</span>';\n"
        "              preview.innerHTML = '<div class=\"preview-video\"><div class=\"icon\">▶</div><div class=\"text\">Playing a video...</div></div>';\n"
        "            } else {\n"
        "              badge.innerHTML = '<span class=\"type-badge type-photo\">Photo</span>';\n"
        "              // Only fetch preview if not already loaded\n"
        "              if (data.preview && currentPreviewUrl) {\n"
        "                // Already have preview\n"
        "              } else {\n"
        "                preview.innerHTML = '<div class=\"loading\"><span class=\"spinner\"></span>Loading...</div>';\n"
        "                fetch('/api/preview')\n"
        "                  .then(r => r.json())\n"
        "                  .then(d => {\n"
        "                    if (d.type === 'photo' && d.image) {\n"
        "                      currentPreviewUrl = d.image;\n"
        "                      preview.innerHTML = '<img src=\"' + d.image + '\" alt=\"Preview\">';\n"
        "                    } else {\n"
        "                      preview.innerHTML = '<div style=\"color:#8b949e;padding:20px\">No preview available</div>';\n"
        "                    }\n"
        "                  })\n"
        "                  .catch(() => {\n"
        "                    preview.innerHTML = '<div style=\"color:#8b949e;padding:20px\">Preview unavailable</div>';\n"
        "                  });\n"
        "              }\n"
        "            }\n"
        "          }\n"
        "        })\n"
        "        .catch(err => {\n"
        "          document.getElementById('filename').textContent = 'No response';\n"
        "          document.getElementById('current').textContent = '—';\n"
        "          document.getElementById('total').textContent = '—';\n"
        "          document.getElementById('preview').innerHTML = '<div style=\"color:#8b949e;padding:20px\">Service unavailable</div>';\n"
        "        });\n"
        "    }\n"
        "\n"
        "    updateStatus();\n"
        "    setInterval(updateStatus, 10000);\n"
        "  </script>\n"
        "</body>\n"
        "</html>\n";

    while (g_running.load()) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(server_fd, &fds);
        struct timeval tv = {1, 0};

        if (select(server_fd + 1, &fds, NULL, NULL, &tv) > 0) {
            int client_fd = accept(server_fd, NULL, NULL);
            if (client_fd >= 0) {
                // v6.0.6: Capture shared_ptr to prevent treadmill worker from replacing items during request
                auto items_ptr = slide.get_items();
                char buf[4096] = {0};
                read(client_fd, buf, sizeof(buf) - 1);

                char first_line[256] = {0};
                sscanf(buf, "%255[^\\r\\n]", first_line);

                // Dashboard page
                if (strncmp(first_line, "GET / ", 6) == 0 || strncmp(first_line, "GET /dashboard ", 15) == 0) {
                    char response[32768];
                    // v6.0.3: Use heap buffer for full dashboard HTML to avoid Content-Length truncation
                    std::string full_response = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
                        "Content-Length: " + std::to_string(strlen(dashboard_html)) + "\r\nConnection: close\r\n\r\n" + dashboard_html;
                    write(client_fd, full_response.c_str(), full_response.size());
                }
                // Navigation
                else if (strncmp(first_line, "GET /api/next ", 14) == 0) {
                    g_remote_command.store(1);
                    write(client_fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK", 39);
                } else if (strncmp(first_line, "GET /api/prev ", 14) == 0) {
                    g_remote_command.store(2);
                    write(client_fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK", 39);
                } else if (strncmp(first_line, "GET /api/pause ", 15) == 0) {
                    g_remote_command.store(3);
                    write(client_fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK", 39);
                } else if (strncmp(first_line, "GET /api/toggle_shuffle ", 24) == 0) {
                    {
                        std::lock_guard<std::mutex> lk(slide.shuffle_mutex);
                        slide.shuffle = !slide.shuffle;
                    }
                    write(client_fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK", 39);
                } else if (strncmp(first_line, "GET /api/restart ", 17) == 0) {
                    g_running.store(false);
                    write(client_fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK", 39);
                }
                // Status endpoint with full details
                else if (strncmp(first_line, "GET /api/status ", 16) == 0) {
                    int ci = slide.current_index.load();
                    std::string fname = "";
                    std::string itype = "photo";
                    bool is_video = slide.current_is_video.load();  // v6.0.10: atomic load (B7)
                    if (ci >= 0 && ci < (int)items_ptr->size()) {
                        fname = (*items_ptr)[ci].filename;
                        itype = (*items_ptr)[ci].type;
                        if (itype == "video") is_video = true;
                    }
                    // Escape quotes in filename for JSON
                    std::string json_fname;
                    for (char c : fname) {
                        if (c == '"') json_fname += "\\\"";
                        else if (c == '\\') json_fname += "\\\\";
                        else json_fname += c;
                    }
                    bool shuffle_val;
                    {
                        std::lock_guard<std::mutex> lk(slide.shuffle_mutex);
                        shuffle_val = slide.shuffle;
                    }
                    std::string json = "{\"current\":" + std::to_string(ci + 1) +
                                      ",\"total\":" + std::to_string(items_ptr->size()) +
                                      ",\"filename\":\"" + json_fname + "\"" +
                                      ",\"is_video\":" + std::string(is_video ? "true" : "false") +
                                      ",\"shuffle\":" + std::string(shuffle_val ? "true" : "false") +
                                      ",\"has_preview\":" + std::string(!is_video ? "true" : "false") +
                                      "}";
                    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: " + std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
                    write(client_fd, response.c_str(), response.size());
                }
                // Preview endpoint — returns base64 encoded image
                else if (strncmp(first_line, "GET /api/preview ", 17) == 0) {
                    int ci = slide.current_index.load();
                    std::string json = "{\"type\":\"video\",\"image\":\"\"}";
                    if (ci >= 0 && ci < (int)items_ptr->size() && (*items_ptr)[ci].type != "video") {
                        const std::string& path = (*items_ptr)[ci].path;
                    // Load image
                         std::string ext_lower = (*items_ptr)[ci].ext;
                         for (auto& ch : ext_lower) ch = (char)tolower(ch);
                      Image img{};  // v6.0.11: value-init to zero (B205)
                        try {
                            if (ext_lower == "heic" || ext_lower == "heif") {
                                img = LoadImageHEIC(path.c_str());
                            } else if (ext_lower == "webp") {
                                img = LoadImageWebP(path.c_str());
                            } else {
                                img = LoadImageRobust(path.c_str());
                            }
                            if (img.data && img.width > 0 && img.height > 0) {
                                // Scale down for preview (max 400px width)
                                int max_w = 400;
                                float ratio = (float)max_w / img.width;
                                if (ratio < 1.0f && (float)img.width > max_w) {
                                    int new_w = max_w;
                                    int new_h = (int)((float)img.height * ratio);
                                    Image scaled = ImageCopy(img);
                                    ImageResize(&scaled, new_w, new_h);
                                    // Export as PNG to memory
                                    int png_size = 0;
                                    unsigned char* png_data = ExportImageToMemory(scaled, "png", &png_size);
                                    if (png_data && png_size > 0) {
                                        std::string b64 = base64_encode(png_data, png_size);
                                        std::string data_uri = "data:image/png;base64," + b64;
                                        json = "{\"type\":\"photo\",\"image\":\"" + data_uri + "\"}";
                                        free(png_data);
                                    }
                                    UnloadImage(scaled);
                                } else {
                                    // Image already small enough, export as PNG
                                    int png_size = 0;
                                    unsigned char* png_data = ExportImageToMemory(img, "png", &png_size);
                                    if (png_data && png_size > 0) {
                                        std::string b64 = base64_encode(png_data, png_size);
                                        std::string data_uri = "data:image/png;base64," + b64;
                                        json = "{\"type\":\"photo\",\"image\":\"" + data_uri + "\"}";
                                        free(png_data);
                                    }
                             }
                            }
                            // v6.0.3: Always unload image after processing (success path)
                            UnloadImage(img);
                        } catch (...) {
                            // v6.0.3: Ensure cleanup on exception path
                            UnloadImage(img);
                        }
                    }
                    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: " + std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
                    write(client_fd, response.c_str(), response.size());
                }
                // Stats endpoint
                else if (strncmp(first_line, "GET /api/stats ", 15) == 0) {
                    int total = (int)items_ptr->size();
                    int photos = 0, videos = 0;
                    for (const auto& item : *items_ptr) {
                        if (item.type == "video") videos++;
                        else photos++;
                    }
                    bool shuffle_val;
                    {
                        std::lock_guard<std::mutex> lk(slide.shuffle_mutex);
                        shuffle_val = slide.shuffle;
                    }
                    std::string json = "{\"total\":" + std::to_string(total) +
                                      ",\"photos\":" + std::to_string(photos) +
                                      ",\"videos\":" + std::to_string(videos) +
                                      ",\"shuffle\":" + std::string(shuffle_val ? "true" : "false") +
                                      "}";
                    std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                        "Content-Length: " + std::to_string(json.size()) + "\r\nConnection: close\r\n\r\n" + json;
                    write(client_fd, response.c_str(), response.size());
                }
                else {
                    write(client_fd, "HTTP/1.1 404 Not Found\r\n\r\n", 26);
                }
                close(client_fd);
            }
        }
    }
    shutdown(server_fd, SHUT_RDWR);
    close(server_fd);
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  SIGNAL HANDLERS — v16.2.0: static functions (async-signal-safe, no UB)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
static void sigint_handler(int) { g_running.store(false, std::memory_order_relaxed); }
static void sigterm_handler(int) { g_running.store(false, std::memory_order_relaxed); }

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  MAIN
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
//  CONFIG WIZARD — v1.6.0
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
void config_wizard(const std::string& config_path) {
    auto save_cfg = [&]() -> bool {
        std::ofstream f(config_path);
        if(!f.is_open()) return false;
        f<<"# ==========================================\n";
        f<<"# piTrove Configuration File (v"<<VERSION<<")\n";
        f<<"# ==========================================\n\n";
        f<<"[paths]\n";
        f<<"media_dir = "<<g_cfg.media_dir<<"\n";
        f<<"cache_dir = "<<g_cfg.cache_dir<<"\n";
        f<<"log_dir = "<<g_cfg.log_dir<<"\n";
        f<<"splash_file = "<<g_cfg.splash_file<<"\n\n";
        f<<"[display]\n";
        f<<"resolution = "<<g_cfg.screen_w<<","<<g_cfg.screen_h<<"\n";
        f<<"fullscreen = "<<(g_cfg.fullscreen?"1":"0")<<"\n";
        f<<"rotation = "<<g_cfg.rotation<<"\n";
        f<<"slideshow_fps = "<<g_cfg.slideshow_fps<<"\n";
        f<<"splash_overlay_y = "<<g_cfg.splash_overlay_y<<"\n";
        f<<"auto_display_rotation = "<<(g_cfg.auto_display_rotation?"1":"0")<<"\n";
        f<<"border_enabled = "<<(g_cfg.border_enabled?"1":"0")<<"\n";
        f<<"border_width = "<<g_cfg.border_width<<"\n";
        f<<"vignette_enabled = "<<(g_cfg.vignette_enabled?"1":"0")<<"\n\n";
        f<<"[slideshow]\n";
        f<<"transition_delay = "<<g_cfg.transition_delay<<"\n";
        f<<"transition_duration = "<<g_cfg.transition_duration<<"\n";
        f<<"transition_effect = \""<<g_cfg.transition_effect<<"\"\n";
        f<<"ken_burns = "<<(g_cfg.ken_burns?"1":"0")<<"\n";
        f<<"ken_burns_speed = "<<g_cfg.ken_burns_speed<<"\n";
        f<<"ken_burns_zoom = "<<g_cfg.ken_burns_zoom<<"\n";
        f<<"shuffle = "<<(g_cfg.shuffle?"1":"0")<<"\n";
        f<<"bias_lighting = "<<(g_cfg.bias_lighting?"1":"0")<<"\n";
        f<<"bias_anim_speed = "<<g_cfg.bias_anim_speed<<"\n";
        f<<"bias_anim_style = \""<<g_cfg.bias_anim_style<<"\"\n";
        f<<"bias_color_mode = \""<<g_cfg.bias_color_mode<<"\"\n";
        f<<"bias_strength = "<<g_cfg.bias_strength<<"\n";
        f<<"matting = "<<(g_cfg.matting?"1":"0")<<"\n";
        f<<"matting_size = "<<g_cfg.matting_size<<"\n";
        f<<"cooldown_days = "<<g_cfg.cooldown_days<<"\n";
        f<<"clock_enabled = "<<(g_cfg.clock_enabled?"1":"0")<<"\n";
        f<<"clock_x = "<<g_cfg.clock_x<<"\n";
        f<<"clock_y = "<<g_cfg.clock_y<<"\n";
        f<<"clock_font_size = "<<g_cfg.clock_font_size<<"\n";
        f<<"clock_color = \""<<g_cfg.clock_color<<"\"\n";
        f<<"clock_24h = "<<(g_cfg.clock_24h?"1":"0")<<"\n\n";
        f<<"[scan]\n";
        f<<"recursive = "<<(g_cfg.recursive?"1":"0")<<"\n";
        f<<"depth = "<<g_cfg.scan_depth<<"\n";
        f<<"max_concurrent = "<<g_cfg.max_concurrent<<"\n";
        f<<"window_days = "<<g_cfg.scan_window_days<<"\n";
        f<<"ignore_folders = [";
        for(size_t j=0;j<g_cfg.ignore_folders.size();j++)
            f<<"\""<<g_cfg.ignore_folders[j]<<"\""<<(j<g_cfg.ignore_folders.size()-1?", ":"");
        f<<"]\n\n";
        f<<"[sqlite]\n";
        f<<"mmap_size = "<<g_cfg.cache_mmap_size<<"\n\n";
        f<<"[overlay]\n";
        f<<"timer_enabled = "<<(g_cfg.timer_enabled?"1":"0")<<"\n";
        f<<"timer_x = "<<g_cfg.timer_x<<"\n";
        f<<"timer_y = "<<g_cfg.timer_y<<"\n";
        f<<"timer_font_size = "<<g_cfg.timer_font_size<<"\n";
        f<<"timer_color = \""<<g_cfg.timer_color<<"\"\n";
        f<<"filename_enabled = "<<(g_cfg.filename_enabled?"1":"0")<<"\n";
        f<<"filename_x = "<<g_cfg.filename_x<<"\n";
        f<<"filename_y = "<<g_cfg.filename_y<<"\n";
        f<<"count_enabled = "<<(g_cfg.count_enabled?"1":"0")<<"\n";
        f<<"count_x = "<<g_cfg.count_x<<"\n";
        f<<"count_y = "<<g_cfg.count_y<<"\n";
        f<<"videos_per_photos = "<<g_cfg.videos_per_photos<<"\n";
        f<<"sleep_time = "<<(g_cfg.sleep_time.empty()?"\"\"":"\""+g_cfg.sleep_time+"\"")<<"\n";
        f<<"wake_time = "<<(g_cfg.wake_time.empty()?"\"\"":"\""+g_cfg.wake_time+"\"")<<"\n";
        f<<"filename_font_size = "<<g_cfg.filename_font_size<<"\n";
        f<<"count_font_size = "<<g_cfg.count_font_size<<"\n\n";
        f<<"[video]\n";
        f<<"volume = "<<g_cfg.video_volume<<"\n";
        f<<"probe_timeout = "<<g_cfg.video_probe_timeout<<"\n\n";
        f<<"[dashboard]\n";
        f<<"weather_enabled = "<<(g_cfg.weather_enabled?"1":"0")<<"\n";
        f<<"weather_lat = "<<g_cfg.weather_lat<<"\n";
        f<<"weather_lon = "<<g_cfg.weather_lon<<"\n\n";
        f<<"[remote]\n";
        f<<"http_enabled = "<<(g_cfg.http_enabled?"1":"0")<<"\n";
        f<<"http_port = "<<g_cfg.http_port<<"\n\n";
        f<<"[date_overlay]\n";
        f<<"enabled = "<<(g_cfg.date_overlay_enabled?"1":"0")<<"\n";
        f<<"text = "<<g_cfg.date_text<<"\n";
        f<<"x = "<<g_cfg.date_x<<"\n";
        f<<"y = "<<g_cfg.date_y<<"\n";
        f<<"font_size = "<<g_cfg.date_font_size<<"\n";
        f<<"color = "<<g_cfg.date_color<<"\n\n";
        f<<"[brightness]\n";
        f<<"auto = "<<(g_cfg.brightness_auto?"1":"0")<<"\n";
        f<<"auto_min = "<<g_cfg.brightness_auto_min<<"\n";
        f<<"auto_max = "<<g_cfg.brightness_auto_max<<"\n\n";
        f<<"[touch]\n";
        f<<"enabled = "<<(g_cfg.touch_enabled?"1":"0")<<"\n\n";
        f<<"[collage]\n";
        f<<"enabled = "<<(g_cfg.collage_enabled?"1":"0")<<"\n";
        f<<"cols = "<<g_cfg.collage_cols<<"\n";
        f<<"rows = "<<g_cfg.collage_rows<<"\n\n";
        f<<"[log]\n";
        f<<"level = "<<(g_cfg.verbose?"debug":"info")<<"\n";
        f.close();
        g_config_changed.store(true);
        return true;
    };

    // ── TERMINAL SIZING ──
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    int term_cols = w.ws_col;
    if (term_cols < 100) {
        printf("\033[8;40;155t"); // Request terminal resize to 155x40
        fflush(stdout);
        usleep(100000);
        ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
        term_cols = w.ws_col;
    }
    int tui_width = std::max(100, std::min(155, term_cols));

    system("stty -icanon -echo");
    printf("\033[?1049h\033[H\033[J");

    enum IT { STR, INT, FLT, TGL, ENM, LST };
    struct CI { const char* n; IT t; const char* desc; };

    // ── DEFINITIONS WITH DESCRIPTIONS ──
    static const CI CA[] = {
        {"Rotation", INT, "Screen rotation in degrees (0, 90, 180, 270)"},
        {"Ken Burns Zoom", FLT, "Zoom intensity for Ken Burns effect (0.0 to 1.0)"},
        {"Auto Display Rotation", TGL, "Rotate images based on EXIF orientation"},
        {"Brightness Auto", TGL, "Auto backlight dimming based on time of day"}
    };
    static const CI CB[] = {
        {"Media Directory", STR, "Root folder containing photos and videos"},
        {"Cache Directory", STR, "Folder for SQLite metadata cache"},
        {"Log Directory", STR, "Folder to store runtime logs"},
        {"Sleep Time", STR, "Time to turn off HDMI port (HH:MM, e.g. 23:00)"},
        {"Wake Time", STR, "Time to turn on HDMI port (HH:MM, e.g. 07:30)"},
        {"HTTP Remote", TGL, "Enable local web server to skip/pause"},
        {"Splash Overlay Y", FLT, "Vertical position of splash UI (0.0 to 1.0)"}
    };
    static const CI CC[] = {
        {"Timer Enabled", TGL, "Show remaining photo/video duration overlay"},
        {"Timer X Pos", FLT, "Horizontal position of timer (0.0 to 1.0)"},
        {"Timer Y Pos", FLT, "Vertical position of timer (0.0 to 1.0)"},
        {"Timer Size", INT, "Font size of timer text in pixels"},
        {"Timer Color", ENM, "Color of timer text"},
        {"Clock Enabled", TGL, "Show time overlay on screen"},
        {"Clock X Pos", FLT, "Horizontal position of clock (0.0 to 1.0)"},
        {"Clock Y Pos", FLT, "Vertical position of clock (0.0 to 1.0)"},
        {"Clock Size", INT, "Font size of clock text in pixels"},
        {"Clock Color", ENM, "Color of clock text"},
        {"Clock 24h", TGL, "Use 24-hour format for clock"},
        {"Count Enabled", TGL, "Show playlist progress (e.g., '14 / 2054')"}
    };
    static const CI CD[] = {
        {"Video Volume", INT, "Volume level for video playback (0=muted)"},
        {"Videos per Photos", INT, "Interleave ratio. E.g., '2' plays 2 vids per 10 pics"},
        {"Probe Timeout", INT, "Max seconds for ffprobe duration extraction (0=disabled)"}
    };
    static const CI CE[] = {
        {"Transition Delay", FLT, "Seconds to display photo before transitioning"},
        {"Transition Duration", FLT, "Seconds the transition animation takes"},
        {"Transition Effect", ENM, "Visual style when swapping (crossfade/wipe/pixelate)"},
        {"Ken Burns", TGL, "Smoothly pan and zoom across static photos"},
        {"Ken Burns Speed", FLT, "Speed multiplier for pan/zoom movement"},
        {"Bias Lighting", TGL, "Enable ambient background glow derived from photo"},
        {"Bias Anim Speed", FLT, "Speed of background glow animation (0.0 to stop)"},
        {"Bias Anim Style", ENM, "Visual style of glow (pulsing/edge_glow/aura)"},
        {"Bias Color Mode", ENM, "Color source for glow (auto/rainbow)"},
        {"Matting Enabled", TGL, "Draw 3D matte border around photos"},
        {"Matting Size", INT, "Thickness of the matte border in pixels"},
        {"Cooldown Days", INT, "Days to wait before showing a photo again (0=off)"},
        {"Shuffle", TGL, "Randomize photo/video order"}
    };
    static const CI CG[] = {
        {"Recursive Scan", TGL, "Recursively scan subdirectories"},
        {"Scan Depth", INT, "Max subdirectory depth to scan"},
        {"Temporal Window", INT, "Show media from ±X days of today, any year. 0=all"},
        {"Ignore Folders", LST, "Comma-separated folder names to skip"},
        {"Max Concurrent", INT, "Max threads during loading (match CPU cores)"}
    };
    static const CI CH[] = {
        {"Weather Enabled", TGL, "Fetch local weather via Open-Meteo API"},
        {"Latitude", FLT, "Location latitude for weather API"},
        {"Longitude", FLT, "Location longitude for weather API"}
    };
    static const CI CI2[] = {
        {"Log Level", ENM, "Console verbosity (debug, info, warn, error)"},
        {"Min Brightness", INT, "Floor for auto-brightness (0-100)"},
        {"SQLite mmap Size", INT, "Bytes to allocate for DB memory mapping"}
    };

    struct CAT { const char* n; const CI* i; int c; };
    static const CAT CATS[] = {
        {"Display", CA, 4},
        {"System", CB, 7},
        {"Overlays", CC, 12},
        {"Videos", CD, 3},
        {"Slideshow", CE, 13},
        {"Scanning", CG, 5},
        {"Weather", CH, 3},
        {"Advanced", CI2, 3}
    };

    // ── DATA ACCESSORS ──
    auto gv = [&](int c, int i) -> std::string {
        if (c == 0) switch(i) {
            case 0: return std::to_string(g_cfg.rotation);
            case 1: return std::to_string(g_cfg.ken_burns_zoom);
            case 2: return g_cfg.auto_display_rotation?"[ON]":"[OFF]";
            case 3: return g_cfg.brightness_auto?"[ON]":"[OFF]";
        }
        if (c == 1) switch(i) {
            case 0: return g_cfg.media_dir; case 1: return g_cfg.cache_dir; case 2: return g_cfg.log_dir;
            case 3: return g_cfg.sleep_time; case 4: return g_cfg.wake_time;
            case 5: return g_cfg.http_enabled?"[ON]":"[OFF]";
            case 6: return std::to_string(g_cfg.splash_overlay_y);
        }
        if (c == 2) switch(i) {
            case 0: return g_cfg.timer_enabled?"[ON]":"[OFF]";
            case 1: return std::to_string(g_cfg.timer_x);
            case 2: return std::to_string(g_cfg.timer_y);
            case 3: return std::to_string(g_cfg.timer_font_size);
            case 4: return g_cfg.timer_color;
            case 5: return g_cfg.clock_enabled?"[ON]":"[OFF]";
            case 6: return std::to_string(g_cfg.clock_x);
            case 7: return std::to_string(g_cfg.clock_y);
            case 8: return std::to_string(g_cfg.clock_font_size);
            case 9: return g_cfg.clock_color;
            case 10: return g_cfg.clock_24h?"[ON]":"[OFF]";
            case 11: return g_cfg.count_enabled?"[ON]":"[OFF]";
        }
    if (c == 3) switch(i) {
             case 0: return std::to_string(g_cfg.video_volume);
             case 1: return std::to_string(g_cfg.videos_per_photos);
             case 2: return std::to_string(g_cfg.video_probe_timeout);
         }
        if (c == 4) switch(i) {
            case 0: return std::to_string(g_cfg.transition_delay);
            case 1: return std::to_string(g_cfg.transition_duration);
            case 2: return g_cfg.transition_effect;
            case 3: return g_cfg.ken_burns?"[ON]":"[OFF]";
            case 4: return std::to_string(g_cfg.ken_burns_speed);
            case 5: return g_cfg.bias_lighting?"[ON]":"[OFF]";
            case 6: return std::to_string(g_cfg.bias_anim_speed);
            case 7: return g_cfg.bias_anim_style; case 8: return g_cfg.bias_color_mode;
            case 9: return g_cfg.matting?"[ON]":"[OFF]";
            case 10: return std::to_string(g_cfg.matting_size);
            case 11: return std::to_string(g_cfg.cooldown_days);
            case 12: return g_cfg.shuffle?"[ON]":"[OFF]";
        }
        if (c == 5) switch(i) {
            case 0: return g_cfg.recursive?"[ON]":"[OFF]";
            case 1: return std::to_string(g_cfg.scan_depth);
            case 2: return std::to_string(g_cfg.scan_window_days);
            case 3: { std::string s; for(size_t x=0;x<g_cfg.ignore_folders.size();x++) s+=g_cfg.ignore_folders[x]+(x<g_cfg.ignore_folders.size()-1?",":""); return s; }
            case 4: return std::to_string(g_cfg.max_concurrent);
        }
        if (c == 6) switch(i) {
            case 0: return g_cfg.weather_enabled?"[ON]":"[OFF]";
            case 1: return std::to_string(g_cfg.weather_lat);
            case 2: return std::to_string(g_cfg.weather_lon);
        }
        if (c == 7) switch(i) {
            case 0: return g_cfg.verbose?"debug":"info";
            case 1: return std::to_string(g_cfg.brightness_auto_min);
            case 2: return std::to_string(g_cfg.cache_mmap_size);
        }
        return "";
    };

    auto sv = [&](int c, int i, const std::string& v) {
        if(v.empty()) return;
        std::lock_guard<std::mutex> lk(g_config_mtx);  // v6.0.10: protect g_cfg writes (B4)
        try {
            if(c==0) switch(i){
                case 0:{ try { g_cfg.rotation=std::stoi(v); } catch(...) {} break; }
                case 1:{ try { g_cfg.ken_burns_zoom=std::stof(v); } catch(...) {} break; }
                case 2:g_cfg.auto_display_rotation=(v=="1"||v=="ON"||v=="true"||v=="[ON]");break;
                case 3:g_cfg.brightness_auto=(v=="1"||v=="ON"||v=="true"||v=="[ON]");break;
            }
            else if(c==1) switch(i){
                case 0:g_cfg.media_dir=v;break; case 1:g_cfg.cache_dir=v;break; case 2:g_cfg.log_dir=v;break;
                case 3:g_cfg.sleep_time=v;break; case 4:g_cfg.wake_time=v;break;
                case 5:g_cfg.http_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]");break;
                case 6:{ try { g_cfg.splash_overlay_y=std::stof(v); } catch(...) {} break; }
            }
            else if(c==2) switch(i){
                case 0:g_cfg.timer_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]");break;
               case 1:{ try { g_cfg.timer_x=std::stof(v); } catch(...) {} break; } case 2:{ try { g_cfg.timer_y=std::stof(v); } catch(...) {} break; }
                case 3:{ try { g_cfg.timer_font_size=std::stoi(v); } catch(...) {} break; } case 4:g_cfg.timer_color=v;break;
                case 5:g_cfg.clock_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]");break;
                case 6:{ try { g_cfg.clock_x=std::stof(v); } catch(...) {} break; } case 7:{ try { g_cfg.clock_y=std::stof(v); } catch(...) {} break; }
                case 8:{ try { g_cfg.clock_font_size=std::stoi(v); } catch(...) {} break; } case 9:g_cfg.clock_color=v;break;
                case 10:g_cfg.clock_24h=(v=="1"||v=="ON"||v=="true"||v=="[ON]");break;
                case 11:g_cfg.count_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]");break;
            }
   else if(c==3) switch(i){
                  case 0:{ try { g_cfg.video_volume=std::stoi(v); } catch(...) {} break; }
                  case 1:{ try { g_cfg.videos_per_photos=std::stoi(v); } catch(...) {} break; }
                  case 2:{ try { g_cfg.video_probe_timeout=std::stoi(v); } catch(...) {} break; }
              }
             else if(c==4) switch(i){
                case 0:{ try { g_cfg.transition_delay=std::stof(v); } catch(...) {} break; } case 1:{ try { g_cfg.transition_duration=std::stof(v); } catch(...) {} break; }
                case 2:g_cfg.transition_effect=v;break;
                case 3:g_cfg.ken_burns=(v=="1"||v=="ON"||v=="true"||v=="[ON]");break;
                case 4:{ try { g_cfg.ken_burns_speed=std::stof(v); } catch(...) {} break; }
                case 5:g_cfg.bias_lighting=(v=="1"||v=="ON"||v=="true"||v=="[ON]");break;
                case 6:{ try { g_cfg.bias_anim_speed=std::stof(v); } catch(...) {} break; }
                case 7:g_cfg.bias_anim_style=v;break; case 8:g_cfg.bias_color_mode=v;break;
                case 9:g_cfg.matting=(v=="1"||v=="ON"||v=="true"||v=="[ON]");break;
                case 10:{ try { g_cfg.matting_size=std::stoi(v); } catch(...) {} break; }
                case 11:{ try { g_cfg.cooldown_days=std::stoi(v); } catch(...) {} break; }
                case 12:g_cfg.shuffle=(v=="1"||v=="ON"||v=="true"||v=="[ON]");break;
            }
            else if(c==5) switch(i){
                case 0:g_cfg.recursive=(v=="1"||v=="ON"||v=="true"||v=="[ON]");break;
                case 1:{ try { g_cfg.scan_depth=std::stoi(v); } catch(...) {} break; }
                case 2:{ try { g_cfg.scan_window_days=std::stoi(v); } catch(...) {} break; }
                case 3:{
                    g_cfg.ignore_folders.clear();
                    std::string clean = v;
                    clean.erase(std::remove(clean.begin(), clean.end(), '['), clean.end());
                    clean.erase(std::remove(clean.begin(), clean.end(), ']'), clean.end());
                    clean.erase(std::remove(clean.begin(), clean.end(), '"'), clean.end());
                    clean.erase(std::remove(clean.begin(), clean.end(), '\''), clean.end());
                    
                    size_t pos = 0, f;
                    while((f = clean.find(',', pos)) != std::string::npos) {
                        std::string token = clean.substr(pos, f - pos);
                        token.erase(0, token.find_first_not_of(" \t"));
                        token.erase(token.find_last_not_of(" \t") + 1);
                        if (!token.empty()) g_cfg.ignore_folders.push_back(token);
                        pos = f + 1;
                    }
                    std::string last = clean.substr(pos);
                    last.erase(0, last.find_first_not_of(" \t"));
                    last.erase(last.find_last_not_of(" \t") + 1);
                    if (!last.empty()) g_cfg.ignore_folders.push_back(last);
                }break;
                case 4:{ try { g_cfg.max_concurrent=std::stoi(v); } catch(...) {} break; }
            }
            else if(c==6) switch(i){
                case 0:g_cfg.weather_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]");break;
                case 1:{ try { float vl=std::stof(v); if(vl>=-90.0f&&vl<=90.0f) g_cfg.weather_lat=vl; } catch(...) {} break; }
                case 2:{ try { float vn=std::stof(v); if(vn>=-180.0f&&vn<=180.0f) g_cfg.weather_lon=vn; } catch(...) {} break; }
            }
            else if(c==7) switch(i){
                case 0:{ if(v=="debug") g_cfg.verbose=true; else g_cfg.verbose=false; }break;
                case 1:{ try { g_cfg.brightness_auto_min=std::stoi(v); } catch(...) {} break; }
                case 2:{ try { g_cfg.cache_mmap_size=std::stoll(v); } catch(...) {} break; }
            }
        } catch(...) {}
    };

    auto enums = [&](int c, int i) -> std::vector<std::string> {
        if(c==4&&i==2) return {"crossfade","wipe","pixelate"};
        if(c==4&&i==7) return {"pulsing","radiating","absorbing","edge_glow","aura"};
        if(c==4&&i==8) return {"auto","rainbow"};
        if(c==2&&i==4) return {"yellow","white","cyan","red"};
        if(c==2&&i==9) return {"yellow","white","cyan","red"};
        if(c==2&&i==10) return {"yellow","white","cyan","gray"};
        if(c==7&&i==0) return {"debug","info","warn","error"};
        return {};
    };

    int sel = 0, sel_sub = 0;
    bool edit_mode = false;
    std::string ed_buf;
    bool run = true;

    // ── MAIN TUI LOOP ──
    while(run) {
        printf("\033[H\033[J"); // Clear Screen

        // Layout Geometry
        int cat_w = 20;
        int name_w = 22;
        int val_w = 26;
        int desc_w = tui_width - cat_w - name_w - val_w - 6;

        // Header
        printf("\033[1;36m  piTrove Configuration Engine v%s\033[0m\n", VERSION);
        printf("  \033[90m"); for(int i=0; i<tui_width-4; i++) printf("━"); printf("\033[0m\n\n");

     // Top Category Bar
        printf("  ");
        for(int i=0; i<8; i++) {
            if(i==sel) printf("\033[7;33m %s \033[0m  ", CATS[i].n);
            else printf("\033[1;37m%s\033[0m  ", CATS[i].n);
        }
        printf("\n\n");

        // Column Headers
        printf("  \033[1;36m%-*s %-*s %-*s\033[0m\n", name_w, "Setting", val_w, "Value", desc_w, "Description");
        printf("  \033[90m"); for(int i=0; i<tui_width-4; i++) printf("─"); printf("\033[0m\n");

        // Rows
        for(int i=0; i<CATS[sel].c; i++) {
            const auto& item = CATS[sel].i[i];
            std::string val = gv(sel, i);

            if (item.t == TGL) {
                val = (val == "1" || val == "[ON]") ? "[  ON  ]" : "[ OFF  ]";
            }

            // Description truncation if terminal gets impossibly small
            std::string desc = item.desc;
            if ((int)desc.length() > desc_w) desc = desc.substr(0, desc_w - 3) + "...";

            if(edit_mode && i==sel_sub) {
                printf("  \033[1;32m%-*s \033[7;37m%-*s\033[0m \033[90m%-*s\033[0m\n", name_w, item.n, val_w, ed_buf.c_str(), desc_w, desc.c_str());
            } else if(i==sel_sub) {
                printf("  \033[1;32m%-*s \033[1;37m%-*s\033[0m \033[90m%-*s\033[0m\n", name_w, item.n, val_w, val.c_str(), desc_w, desc.c_str());
            } else {
                printf("  %-*s \033[37m%-*s\033[0m \033[90m%-*s\033[0m\n", name_w, item.n, val_w, val.c_str(), desc_w, desc.c_str());
            }
        }

        // Fill remaining height to prevent bouncing
        for (int i=CATS[sel].c; i<15; i++) printf("\n");

        printf("\n  \033[90m"); for(int i=0; i<tui_width-4; i++) printf("─"); printf("\033[0m\n");

        // Footer / Keybinds
        if(!edit_mode) {
            printf("  \033[1;37m[\xE2\x86\x91\xE2\x86\x93]\033[0m Select    \033[1;37m[\xE2\x86\x90\xE2\x86\x92]\033[0m Category    \033[1;37m[SPACE/ENTER]\033[0m Toggle/Edit    \033[1;32m[S]\033[0m Save    \033[1;31m[Q]\033[0m Quit\n");
        } else {
            printf("  \033[1;32m[ENTER]\033[0m Confirm   \033[1;31m[ESC]\033[0m Cancel      \033[1;37m[\xE2\x86\x91\xE2\x86\x93]\033[0m Cycle Options\n");
        }

        // ── Restart Notice (dynamic) ──
        if (!edit_mode && g_config_changed.load()) {
            printf("\n  \033[1;33m[NOTICE]\033[0m Previous changes detected. Use \033[1;32m[S]\033[0m to save, then \033[1;36mpiTrove --restart\033[0m to apply.\n");
        }

        // INPUT LOOP
        char c;
        if(read(STDIN_FILENO, &c, 1) == 1) {
            if(!edit_mode) {
                if(c == 'q' || c == 'Q') { if(!save_cfg()) printf("\033[0;31m[ERROR]\033[0m Failed to save config file.\n"); run = false; }
                else if(c == 's' || c == 'S') { if(!save_cfg()) { printf("\033[0;31m[ERROR]\033[0m Failed to save config file.\n"); } else { printf("\033[1;32m[OK]\033[0m Configuration saved.\n"); } run = false; }
                else if(c == '\033') {
                    char seq[2];
                    if(read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                        if(seq[1] == 'A') { if(sel_sub>0) sel_sub--; else if(sel>0){ sel--; sel_sub=CATS[sel].c-1; } } // UP
              else if(seq[1] == 'B') { if(sel_sub<CATS[sel].c-1) sel_sub++; else if(sel<7){ sel++; sel_sub=0; } } // DOWN
                        else if(seq[1] == 'D') { if(sel>0) { sel--; sel_sub=0; } } // LEFT Category
                        else if(seq[1] == 'C') { if(sel<7) { sel++; sel_sub=0; } } // RIGHT Category
                    }
                }
                else if(c == '\n' || c == '\r' || c == ' ') {
                    if (CATS[sel].i[sel_sub].t == TGL) { // Instantly toggle
                        std::string v = gv(sel, sel_sub);
                        sv(sel, sel_sub, (v=="1"||v=="[ON]"||v=="[  ON  ]") ? "0" : "1");
                    } else if (CATS[sel].i[sel_sub].t == ENM && c == ' ') { // Space quick-cycles enums
                        auto opts = enums(sel, sel_sub);
                        std::string curr = gv(sel, sel_sub);
                        auto it = std::find(opts.begin(), opts.end(), curr);
                        int idx = (it != opts.end()) ? std::distance(opts.begin(), it) : 0;
                        sv(sel, sel_sub, opts[(idx + 1) % opts.size()]);
                    } else if (c != ' ') { // Enter key goes into Edit Mode for text/numbers
                        edit_mode = true;
                        ed_buf = gv(sel, sel_sub);
                    }
                }
            } else { // In Edit Mode
                if(c == '\n' || c == '\r') {
                    sv(sel, sel_sub, ed_buf);
                    edit_mode = false;
                }
                else if(c == '\033') { // Escape or Arrow keys inside edit mode
                    char seq[2];
                    if(read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1) {
                        if(seq[1] == 'A' || seq[1] == 'B') {
                            const auto& item = CATS[sel].i[sel_sub];
                            if(item.t == ENM) {
                                auto opts = enums(sel, sel_sub);
                                if(!opts.empty()) {
                                    auto it = std::find(opts.begin(), opts.end(), ed_buf);
                                    int idx = (it != opts.end()) ? std::distance(opts.begin(), it) : 0;
                                    if (seq[1] == 'B') idx = (idx + 1) % opts.size(); // DOWN
                                    if (seq[1] == 'A') idx = (idx - 1 + opts.size()) % opts.size(); // UP
                                    ed_buf = opts[idx];
                                }
                            }
                        }
                    } else {
                        edit_mode = false; // ESC cancels
                    }
                }
                else if(c == 127 || c == 8) { // Backspace
                    if(!ed_buf.empty()) ed_buf.pop_back();
                }
                else if(c >= 32 && c <= 126) {
                    // Restrict bounds for float/int edits
                    const auto& item = CATS[sel].i[sel_sub];
                    if (item.t == FLT && !isdigit(c) && c != '.' && c != '-') continue;
                    if (item.t == INT && !isdigit(c) && c != '-') continue;
                    ed_buf += c;
                }
            }
        }
    }

    system("stty icanon echo");
    printf("\033[?1049l"); // Restore original terminal buffer
}

// The Midnight Temporal Treadmill Background Daemon
void treadmill_worker(const Config& cfg, Slideshow& slideshow_ref) {
    // Force extreme low priority so this thread NEVER starves the Raylib loop or OS Network stack
    nice(19);

    while (g_running.load()) {
        // Calculate time until next midnight
        auto now = std::chrono::system_clock::now();
        time_t tnow = std::chrono::system_clock::to_time_t(now);
        tm date_buf;
        tm *date = localtime_r(&tnow, &date_buf);
        if (!date) { std::this_thread::sleep_for(std::chrono::minutes(1)); continue; }
        tm date_copy = *date;
        date_copy.tm_hour = 0; date_copy.tm_min = 0; date_copy.tm_sec = 0;
        date_copy.tm_mday += 1; // Set to next midnight

        auto midnight = std::chrono::system_clock::from_time_t(std::mktime(&date_copy));

       // Subdivided 1-second steps guarantee quick response times when g_running goes false
        while (std::chrono::system_clock::now() < midnight && g_running.load()) {
            for (int i = 0; i < 30 && g_running.load(); i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        if (!g_running.load()) break;

        g_logger.info("TREADMILL: Midnight sync initiated. Beginning throttled network scan...");

        // Throttled directory scan using existing scan_directory
        std::vector<MediaItem> new_playlist;
        std::atomic<int64_t> scan_count{0};

        // Scan with 10-second timeout per directory entry to prevent CIFS hangs
        scan_directory(cfg.media_dir, 10, new_playlist, scan_count);

        g_logger.info("TREADMILL: Scanned %lld items, %zu new candidates", (long long)scan_count.load(), new_playlist.size());

        // Build new playlist with cooldown filter
        std::vector<MediaItem> active_photos;
        std::vector<MediaItem> active_videos;
        time_t now_ts = time(nullptr);

        for (auto& mi : new_playlist) {
            long long days_since = (now_ts - mi.last_shown) / 86400LL;
            if (mi.last_shown == 0 || days_since >= cfg.cooldown_days) {
                if (mi.type == "video") active_videos.push_back(mi);
                else active_photos.push_back(mi);
            }
        }

        if (active_photos.empty() && active_videos.empty() && !new_playlist.empty()) {
            for (auto& mi : new_playlist) {
                if (mi.type == "video") active_videos.push_back(mi);
                else active_photos.push_back(mi);
            }
        }

        // Randomize photo order
        if (!active_photos.empty()) {
            std::random_device rd;
            std::mt19937 g(rd());
            std::shuffle(active_photos.begin(), active_photos.end(), g);
        }

        // Interleave videos
        std::vector<MediaItem> final_playlist;
        if (cfg.videos_per_photos <= 0) {
            final_playlist = std::move(active_photos);
        } else {
            size_t p_idx = 0, v_idx = 0;
            while (p_idx < active_photos.size() || v_idx < active_videos.size()) {
                for (int i = 0; i < 10 && p_idx < active_photos.size(); i++) {
                    final_playlist.push_back(active_photos[p_idx++]);
                }
                for (int i = 0; i < cfg.videos_per_photos && v_idx < active_videos.size(); i++) {
                    final_playlist.push_back(active_videos[v_idx++]);
                }
            }
        }

         // Hot Swap
         if (!final_playlist.empty()) {
             // Use local RNG to shuffle outside the lock to prevent main-loop stutter
             std::random_device rd;
             std::mt19937 local_rng(rd());
             std::shuffle(final_playlist.begin(), final_playlist.end(), local_rng);
         }
         {
             std::lock_guard<std::mutex> lock(slideshow_ref.shuffle_mutex);
             // B263, B268: Reset indices and preloads before swapping items to prevent OOB/state inconsistency
             slideshow_ref.current_index = 0;
             slideshow_ref.next_index = -1;
             if (slideshow_ref.loaded_tex.id != 0) {
                 UnloadTexture(slideshow_ref.loaded_tex);
                 slideshow_ref.loaded_tex = {};
             }
             slideshow_ref.items = std::make_shared<std::vector<MediaItem>>(std::move(final_playlist));
             g_logger.info("TREADMILL: Hot-swap successful. New active playlist size: %zu", slideshow_ref.items->size());
         }

    }
}


int main(int argc, char** argv) {
    // Wire global handlers for hardware faults and terminations
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGFPE,  crash_handler);
    signal(SIGILL,  crash_handler);
    std::set_terminate(terminate_handler);

    std::string home_dir = getenv("HOME") ? getenv("HOME") : "/home/pi";
    std::string config_path = home_dir + "/piTrove/src/config/config.toml";
    bool run_config = false;
    bool run_restart = false;
    
    // Cache configuration context for path references during crash events
    g_crash_cache_dir = home_dir + "/.cache/piTrove";
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 < argc) {
                // --config /path/to/config.toml — use provided path, don't launch TUI
                config_path = argv[i + 1];
            } else {
                // --config alone — launch TUI
                run_config = true;
            }
        }
        if (strcmp(argv[i], "--restart") == 0) run_restart = true;
    }

    // ── DEFINE CACHE DIR & PID PATH ──
    std::string cache_dir = home_dir + "/.cache/piTrove";
    std::string pid_path = cache_dir + "/piTrove.pid";
    
    // FIX 2: Silence Raylib's massive INFO and WARNING log spam
    SetTraceLogLevel(LOG_ERROR);

    // ── CPU AFFINITY: Reserve Core 0 for Pi OS / Network Stack ──
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(1, &cpuset); // Bind to Core 1
    CPU_SET(2, &cpuset); // Bind to Core 2
    CPU_SET(3, &cpuset); // Bind to Core 3
    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) == 0) {
        g_logger.info("SYSTEM: CPU Affinity locked to Cores 1-3. Core 0 reserved for OS.");
    } else {
        g_logger.warn("SYSTEM: Failed to set CPU affinity.");
    }

    // ── RESTART LOGIC ──    
    // Ensure cache directory exists before we try to write/read a PID file
    // Ensure cache directory exists with proper permissions
    if (std::filesystem::exists(cache_dir)) {
        chmod(cache_dir.c_str(), 0777);
    } else {
        std::filesystem::create_directories(cache_dir);
        chmod(cache_dir.c_str(), 0777);
    }

    // FIX 1: Redirect Mesa shader cache to our own folder to prevent permission denied spam
    setenv("MESA_SHADER_CACHE_DIR", cache_dir.c_str(), 1);

    // ── RESTART LOGIC ──
    if (run_restart) {
        FILE* pf = fopen(pid_path.c_str(), "r");
        if (pf) {
            pid_t old_pid = 0;
            // Read the PID of the currently running instance
            if (fscanf(pf, "%d", &old_pid) == 1 && old_pid > 0 && old_pid != getpid()) {
                printf("\033[1;33m[INFO]\033[0m Restarting piTrove (Killing background PID %d)...\n", old_pid);
                kill(old_pid, SIGTERM);
                usleep(800000); // Give it a moment to safely clean up VRAM and SQLite
            }
            fclose(pf);
        }
        printf("\033[1;32m[OK]\033[0m Restarting piTrove service...\n");
        system("systemctl restart piTrove.service 2>/dev/null || true");
        return 0; // Exit immediately! Do not launch the app in the SSH terminal!
    }

   // ── PID FILE LOCKING ──
     int pid_fd = open(pid_path.c_str(), O_CREAT | O_RDWR, 0666);
     bool is_already_running = false;
     // v1.9.5: use lambda to ensure pid_fd is closed on all early returns (Y5)
     auto close_pid_fd = [pid_fd]() { if (pid_fd >= 0) close(pid_fd); };
     
     if (pid_fd >= 0) {
         // Try to get an exclusive lock on the PID file
         if (flock(pid_fd, LOCK_EX | LOCK_NB) != 0) {
             is_already_running = true;
             
             // App is already running!
             if (!run_config) {
                 printf("\033[0;31m[FAIL]\033[0m piTrove is already running in the background!\n");
                 printf("       Use \033[1;36mpiTrove --config\033[0m to edit settings safely.\n");
                 printf("       Use \033[1;36mpiTrove --restart\033[0m to reboot the application.\n");
                 close_pid_fd();
                 return 1;
             }
             // If run_config == true, we DO NOT exit. We allow the TUI to launch!
         } else {
             // We got the lock! It is safe to write our PID to the file.
             ftruncate(pid_fd, 0);
             dprintf(pid_fd, "%d\n", getpid());
         }
     }

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigterm_handler);
    signal(SIGSEGV, SIG_DFL);
    // v16.3.0: reap zombie children from spawn() and mpv fork
    signal(SIGCHLD, reap_children);

  printf("piTrove v%s (ARM64) — Digital Picture Frame\n", VERSION);

      Config cfg = g_cfg;
      cfg = load_config(config_path.c_str());
      slide_debug("CFG: delay=%f duration=%f", cfg.transition_delay, cfg.transition_duration);
      {
          std::lock_guard<std::mutex> lock(g_config_mtx);
          g_cfg = cfg;
          // v1.8.4: second safety net — force 1920x1080 regardless of config contents
          g_cfg.screen_w = 1920;
          g_cfg.screen_h = 1080;
          // v1.9.5: Clamp config values to prevent crashes (Z1)
          g_cfg.slideshow_fps = std::max(1, g_cfg.slideshow_fps);
          g_cfg.transition_delay = std::max(1.0, g_cfg.transition_delay);
          g_cfg.transition_duration = std::max(0.1, g_cfg.transition_duration);
          if (g_cfg.ken_burns_zoom < 0.01) g_cfg.ken_burns_zoom = 0.01;
          if (g_cfg.ken_burns_zoom > 1.0) g_cfg.ken_burns_zoom = 1.0;
          g_cfg.border_width = std::max(0, g_cfg.border_width);
          g_cfg.collage_cols = std::max(1, g_cfg.collage_cols);
          g_cfg.collage_rows = std::max(1, g_cfg.collage_rows);
          // Auto-swap brightness min/max if inverted (Z7)
          if (g_cfg.brightness_auto && g_cfg.brightness_auto_min > g_cfg.brightness_auto_max) {
              int tmp = g_cfg.brightness_auto_min;
              g_cfg.brightness_auto_min = g_cfg.brightness_auto_max;
              g_cfg.brightness_auto_max = tmp;
              g_logger.warn("brightness_auto_min/max swapped (min=%d > max=%d)", tmp, g_cfg.brightness_auto_max);
          }
      }


g_logger.init(cfg.log_dir, cfg.verbose ? LogLevel::DEBUG : LogLevel::INFO);
    g_logger.info("piTrove v%s starting (verbose=%s, log_level=%d)", VERSION,
        cfg.verbose ? "yes" : "no", cfg.verbose ? 0 : 1);
    g_logger.info("Media dir: %s", g_cfg.media_dir.c_str());
    g_logger.info("Cache dir: %s", cfg.cache_dir.c_str());
    g_logger.info("Config: slideshow_delay=%.1f transition_dur=%f effect=%s",
        g_cfg.transition_delay, g_cfg.transition_duration, g_cfg.transition_effect.c_str());
    g_logger.info("Config: videos_per_photos=%d shuffle=%d bias=%d borders=%d",
        g_cfg.videos_per_photos, g_cfg.shuffle ? 1 : 0,
        g_cfg.bias_lighting ? 1 : 0, g_cfg.border_enabled ? 1 : 0);

     // v1.9.5: Always warn if media_dir doesn't exist, even without TTY (Y6)
     if (!std::filesystem::exists(g_cfg.media_dir)) {
         fprintf(stderr, "WARN: media_dir '%s' does not exist — slideshow will be empty\n", g_cfg.media_dir.c_str());
     }

     if ((run_config || !std::filesystem::exists(g_cfg.media_dir)) && isatty(STDIN_FILENO)) {
         config_wizard(config_path);
         cfg = g_cfg;
     }


   // ── CONFIG WIZARD ──
     if (run_config) {
         if (g_config_changed.load()) {
             printf("\n  \033[1;32m[OK]\033[0m Configuration updated successfully.\n");
             printf("  \033[1;33m[NOTICE]\033[0m Use \033[1;36mpiTrove --restart\033[0m to apply your new settings.\n\n");
         }
         close_pid_fd();
         return 0; // Exit the app so it doesn't accidentally try to launch the slideshow
     }

    g_logger.info("Initializing display... res=%dx%d fullscreen=%d rotation=%d",
        g_cfg.screen_w, g_cfg.screen_h, g_cfg.fullscreen ? 1 : 0, g_cfg.rotation);
    g_logger.info("Display features: bias=%d borders=%d vignette=%d matting=%d kenburns=%d",
        g_cfg.bias_lighting ? 1 : 0, g_cfg.border_enabled ? 1 : 0, g_cfg.vignette_enabled ? 1 : 0,
        g_cfg.matting ? 1 : 0, g_cfg.ken_burns ? 1 : 0);
    g_logger.info("Overlay features: timer=%d filename=%d count=%d date=%d clock=%d",
        g_cfg.timer_enabled ? 1 : 0, g_cfg.filename_enabled ? 1 : 0, g_cfg.count_enabled ? 1 : 0,
        g_cfg.date_overlay_enabled ? 1 : 0, g_cfg.clock_enabled ? 1 : 0);

    // FIX 6: Aggressive OS anti-blanking. Keep screen ON permanently.
    spawn("setterm -blank 0 -powerdown 0 -powersave off > /dev/tty0 2>&1");
    spawn("xset s off -dpms 2>/dev/null");
    spawn("xset s noblank 2>/dev/null");

    // FIX: Reinforce HDMI Monitor target for DRM/KMS and X11
    setenv("DISPLAY", ":0", 0);
    setenv("RAYLIB_DRM_DISPLAY", "0", 1);

    // FIX v16.6.0: Strict font existence check before InitWindow
    {
        const char* font_paths[] = {
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
            "fonts/DejaVuSansMono-Bold.ttf",
            nullptr
        };
        bool font_found = false;
        for (int i = 0; font_paths[i]; i++) {
            if (std::filesystem::exists(font_paths[i])) { font_found = true; break; }
        }
       if (!font_found) {
             g_logger.error("CRITICAL: No font file found! Ensure DejaVuSansMono-Bold.ttf exists in /usr/share/fonts/truetype/dejavu/ or fonts/");
             close_pid_fd();
             return 1;
         }
    }

    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const char* display = std::getenv("DISPLAY");
    if (!wayland && !display) {
        g_logger.warn("No WAYLAND_DISPLAY or DISPLAY set. "
                      "Expected WAYLAND_DISPLAY=wayland-1 (Weston). "
                      "Attempting InitWindow anyway...");
    }
    try {
         InitWindow(cfg.screen_w, cfg.screen_h, APP_NAME " v" VERSION);
     } catch (...) {
         g_logger.error("Failed to initialize display (GLFW).");
         close_pid_fd();
         return 1;
     }
    SetTargetFPS(cfg.slideshow_fps);


    SplashScreen splash;

    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
    //  SINGLE INSTANCE — POSIX flock with stale PID recovery
    // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

    {
        std::string pid_path = cfg.cache_dir + "/piTrove.pid";
        int fd = open(pid_path.c_str(), O_RDWR | O_CREAT, 0644);
        if (fd >= 0) {
            if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                // Locked — write our PID
                ftruncate(fd, 0);
                std::string pid_str = std::to_string(getpid()) + "\n";
                write(fd, pid_str.c_str(), pid_str.size());
                // Do NOT close(fd) — closing releases the lock
                // OS cleans up automatically on process exit
            } else {
                // Another instance holds the lock — read PID and check liveness
                char buf[64] = {0};
                ssize_t len = read(fd, buf, sizeof(buf) - 1);
                std::string existing_pid = "UNKNOWN";
                if (len > 0) {
                    buf[len] = '\0';
                    existing_pid = buf;
                    try {
                        pid_t old_pid = std::stoi(existing_pid);
                        if (old_pid == getpid()) {
                            // Our own stale lock — reclaim it
                            flock(fd, LOCK_UN);
                            if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                                g_logger.info("Reclaimed stale lock (own PID %s)", existing_pid.c_str());
                                ftruncate(fd, 0);
                                std::string pid_str = std::to_string(getpid()) + "\n";
                                write(fd, pid_str.c_str(), pid_str.size());
                            }
                        } else if (kill(old_pid, 0) == 0) {
                            // Different process alive — truly running
                            fprintf(stderr, "[CRITICAL] piTrove already running (PID %s). Exiting.\n", existing_pid.c_str());
                            close(fd);
                            exit(1);
                        } else {
                            // Dead process — stale lock, try to reclaim
                            flock(fd, LOCK_UN);
                            if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                                g_logger.info("Reclaimed stale lock (dead PID %s)", existing_pid.c_str());
                                ftruncate(fd, 0);
                                std::string pid_str = std::to_string(getpid()) + "\n";
                                write(fd, pid_str.c_str(), pid_str.size());
                            }
                        }
                    } catch (...) {
                        // Unparseable PID — try to reclaim
                        flock(fd, LOCK_UN);
                        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                            ftruncate(fd, 0);
                            std::string pid_str = std::to_string(getpid()) + "\n";
                            write(fd, pid_str.c_str(), pid_str.size());
                        }
                    }
                } else {
                    // Empty file — try to relock
                    flock(fd, LOCK_UN);
                    if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
                        ftruncate(fd, 0);
                        std::string pid_str = std::to_string(getpid()) + "\n";
                        write(fd, pid_str.c_str(), pid_str.size());
                    } else {
                        fprintf(stderr, "[CRITICAL] Cannot acquire lock. Another instance may be running.\n");
                        close(fd);
                        exit(1);
                    }
                }
            }
        }
    }

    // Wait for DRM/KMS GPU context to stabilize, then load splash
    usleep(500000);
    if (!splash.logo_loaded) splash.load(cfg.splash_file);

    // ── Fast-path: skip scan+cache if DB already exists ──────────────────────
    std::string db_path = cfg.cache_dir + "/cache.db";
    struct stat db_stat{};
    bool db_exists = (stat(db_path.c_str(), &db_stat) == 0 && db_stat.st_size > 0);

   std::vector<MediaItem> scanned_items;
    // FIX v16.2.0: fast_cache must outlive the if-block (goto jumps past its scope)
    // Using raw new[] so it lives until program exit (simplest correct approach)
    // FIX v4.1.5: Auto-detect and remove corrupted databases before trying to use them
    if (db_exists) {
        bool db_ok = verify_database(db_path);
        if (!db_ok) {
            g_logger.error("CORRUPT DB: cache.db is corrupted — removing and will rebuild on next boot");
            std::filesystem::remove(db_path);
            db_exists = false;
        }
    }
    CacheManager* fast_cache = db_exists ? new CacheManager() : nullptr;

    if (db_exists && fast_cache && fast_cache->open(cfg.cache_dir)) {
        g_cache = fast_cache;
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(fast_cache->db,
            "SELECT path, type, w, h, duration, exif, last_shown "
            "FROM cache ORDER BY path;",
            -1, &stmt, nullptr);
        if (rc == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                MediaItem mi;
                mi.path          = (const char*)sqlite3_column_text(stmt, 0);
                mi.type          = (const char*)sqlite3_column_text(stmt, 1);
                mi.width         = sqlite3_column_int(stmt, 2);
                mi.height        = sqlite3_column_int(stmt, 3);
                mi.duration      = sqlite3_column_double(stmt, 4);
                mi.exif_rotation = sqlite3_column_int(stmt, 5);
                mi.last_shown    = sqlite3_column_int64(stmt, 6);
                auto slash = mi.path.rfind('/');
                mi.filename = (slash != std::string::npos) ? mi.path.substr(slash + 1) : mi.path;
                // Derive ext from filename
                auto dot = mi.filename.find_last_of('.');
                mi.ext = (dot != std::string::npos && dot < mi.filename.size() - 1) ? mi.filename.substr(dot + 1) : "";
                mi.cached = true;
                scanned_items.push_back(mi);
            }
            sqlite3_finalize(stmt);
        } else {
            g_logger.warn("Failed to read cache DB — falling back to full scan");
            db_exists = false;
        }
        if (db_exists) {
            g_logger.info("Loaded %d items from cache DB", (int)scanned_items.size());
            int photos = std::count_if(scanned_items.begin(), scanned_items.end(), [](const MediaItem& i){ return i.type == "image"; });
            int videos = std::count_if(scanned_items.begin(), scanned_items.end(), [](const MediaItem& i){ return i.type == "video"; });
            g_logger.info("Items: photos=%d videos=%d", photos, videos);
            if (photos == 0 && videos == 0) {
                g_logger.error("Cache DB loaded 0 valid items — will re-scan");
            } else {
                g_database_complete.store(true);
                goto slideshow_start;
            }
        } else {
            // Fast-path open failed — fall through to full scan
            delete fast_cache;
            fast_cache = nullptr;
        }
    }
    // If we reach here (either fast-path failed or db doesn't exist), fast_cache is already freed

    {
    // PHASE 1: SCAN
    g_logger.info("Phase 1: Scanning media...");
    std::atomic<int64_t> scan_count{0};

    auto scan_start = std::chrono::steady_clock::now();

    // Start threads
    auto subdirs = read_dir_timeout(g_cfg.media_dir, 15000);
    if (subdirs.empty()) {
        g_logger.error("read_dir returned empty or timed out for '%s'", g_cfg.media_dir.c_str());
    }
    std::mutex items_mtx;
    std::atomic<int> threads_remaining{0};

    auto work = [&](int start, int end) {
            std::thread worker_inner([&, start, end]() {
                g_logger.info("Worker thread starting: start=%d end=%d subdirs=%zu",
                    start, end, subdirs.size());
                try {
                    std::vector<MediaItem> local_items;
                    for (int i = start; i < end; i++) {
                        if (subdirs[i].empty() || subdirs[i][0] == '.') continue;
                        std::string dir = g_cfg.media_dir + "/" + subdirs[i];
                        scan_directory(dir, cfg.scan_depth - 1,
                                       local_items, scan_count);
                    }
                    std::lock_guard<std::mutex> lk(items_mtx);
                    scanned_items.insert(scanned_items.end(),
                                         std::make_move_iterator(local_items.begin()),
                                         std::make_move_iterator(local_items.end()));
                    g_logger.info("Worker scan complete: start=%d end=%d", start, end);
                } catch (const std::exception& e) {
                    g_logger.error("Scan worker crashed: %s", e.what());
                } catch (...) {
                    g_logger.error("Scan worker crashed (unknown)");
                }
            });
            worker_inner.join();
            g_logger.info("Worker thread done: start=%d end=%d", start, end);
            threads_remaining.fetch_sub(1, std::memory_order_release);
    };
   std::vector<std::thread> threads;
    int total_threads = cfg.max_concurrent + 1; // Workers + 1 root thread
    threads_remaining.store(total_threads, std::memory_order_release);

    // Root thread — wrapped in try-catch so fetch_sub(1) always executes
    // Also wrapped with timeout to prevent CIFS hangs
    threads.emplace_back([&]() {
        try {
            std::vector<MediaItem> root_items;
            g_logger.info("Root scan thread starting (depth=0, window=%d)", cfg.scan_window_days);

            auto root_future = std::async(std::launch::async, [&]() {
                try {
                    std::vector<MediaItem> local_root;
                    std::error_code ec;
                    for (const auto& entry : std::filesystem::directory_iterator(g_cfg.media_dir, ec)) {
                        if (!entry.is_regular_file(ec)) continue;
                        std::string path = entry.path().string();
                        std::string ext = entry.path().extension().string();
                        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                        static const std::vector<std::string> exts = {".jpg",".jpeg",".png",".webp",".heic",".heif",".gif",".bmp",".tiff",".mp4",".mov",".mkv",".avi",".webm"};
                        if (std::find(exts.begin(), exts.end(), ext) == exts.end()) continue;
                        std::string fname = entry.path().filename().string();
                        if (fname.empty() || fname[0] == '.') continue;
                        struct stat st;
                        if (stat(path.c_str(), &st) != 0) continue;
                        MediaItem mi;
                        mi.path = path;
                        mi.filename = entry.path().stem().string();
                        mi.ext = ext;
                        mi.file_size = st.st_size;
                        mi.modified_time = st.st_mtime;
                        mi.type = is_image(path) ? "image" : "video";
                        local_root.push_back(std::move(mi));
                    }
                    return local_root;
                } catch (...) {
                    return std::vector<MediaItem>{};
                }
            });

            if (root_future.wait_for(std::chrono::seconds(30)) == std::future_status::ready) {
                root_items = root_future.get();
            } else {
                g_logger.warn("Root scan timed out after 30s — skipping root files to prevent CIFS hang");
            }

            if (!root_items.empty()) {
                std::lock_guard<std::mutex> lk(items_mtx);
                scanned_items.insert(scanned_items.end(),
                                      std::make_move_iterator(root_items.begin()),
                                      std::make_move_iterator(root_items.end()));
            }
        } catch (const std::exception& e) {
            g_logger.error("Root scan wrapper crashed: %s", e.what());
        } catch (...) {
            g_logger.error("Root scan wrapper crashed (unknown reason)");
        }
        threads_remaining.fetch_sub(1, std::memory_order_release);
    });


    // Bug 8 fix: use hardware_concurrency capped at 3 cores (cores 1-3 available).
    // This maximises throughput while honouring the OS-reserved core 0 policy.
    int hw_cores  = std::max(1, (int)std::thread::hardware_concurrency());
    int max_scan_threads = std::max(1, std::min(hw_cores - 1, (int)subdirs.size()));

    // Recompute total_threads for the display
    total_threads = max_scan_threads + 1;
    threads_remaining.store(total_threads, std::memory_order_release);

    int chunk2 = std::max(1, (int)subdirs.size() / max_scan_threads);
    for (int t = 0; t < max_scan_threads; t++) {
        int start = t * chunk2;
        int end = (t == max_scan_threads - 1) ? (int)subdirs.size() : start + chunk2;
        threads.emplace_back(work, start, end);
    }
    // Root thread already counted above
    g_logger.info("Threads created, entering scan loop. threads_remaining=%d", threads_remaining.load());

    // Unified render loop while threads are running
    int dot_counter = 0;
    while (threads_remaining.load(std::memory_order_acquire) > 0) {
        if (WindowShouldClose()) { g_running.store(false); break; }
        dot_counter++;
        splash.draw_unified_screen(scan_count.load(), true, 0, 0, false, dot_counter);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    // --- TEMPORAL WINDOW FAILSAFE ---
    // If the window_days filter is too restrictive and returns 0 files,
    // automatically fallback to a full library scan (window_days = 0).
   if (scanned_items.empty() && cfg.scan_window_days != 0) {
        g_logger.warn("Temporal window found 0 files. Rescanning all media...");
        scan_count.store(0);
        // Join old threads before destroying them (prevent std::terminate)
        for (auto& t : threads) { if (t.joinable()) t.join(); }
        threads.clear();
        // FIX: Synchronize thread bounds cleanly before initialization mapping
        total_threads = cfg.max_concurrent + 1;
        threads_remaining.store(total_threads, std::memory_order_release);

        threads.emplace_back([&]() {
            try {
                std::vector<MediaItem> root_items;
                scan_directory(g_cfg.media_dir, 0, root_items, scan_count);
                {
                    std::lock_guard<std::mutex> lk(items_mtx);
                    scanned_items.insert(scanned_items.end(),
                                         std::make_move_iterator(root_items.begin()),
                                         std::make_move_iterator(root_items.end()));
                }
            } catch (...) {
                g_logger.error("Failsafe root scan thread crashed");
            }
            threads_remaining.fetch_sub(1, std::memory_order_release);
        });

        int chunk = (int)subdirs.size() / std::max(cfg.max_concurrent, 1);
        for (int t = 0; t < cfg.max_concurrent; t++) {
            int start = t * chunk;
            int end = (t == cfg.max_concurrent - 1) ? (int)subdirs.size() : (t + 1) * chunk;
            threads.emplace_back([&, start, end]() {
                try {
                    std::vector<MediaItem> local_items;
                    for (int i = start; i < end; i++) {
                        if (subdirs[i].empty() || subdirs[i][0] == '.') continue;
                        std::string dir = g_cfg.media_dir + "/" + subdirs[i];
                        scan_directory(dir, cfg.scan_depth - 1, local_items, scan_count);
                    }
                    std::lock_guard<std::mutex> lk(items_mtx);
                    scanned_items.insert(scanned_items.end(),
                                          std::make_move_iterator(local_items.begin()),
                                          std::make_move_iterator(local_items.end()));
                } catch (const std::exception& e) {
                    g_logger.error("Failsafe worker thread crashed: %s", e.what());
                } catch (...) {
                    g_logger.error("Failsafe worker thread crashed");
                }
                threads_remaining.fetch_sub(1, std::memory_order_release);
            });
        }

       while (threads_remaining.load(std::memory_order_acquire) > 0) {
            if (WindowShouldClose()) { g_running.store(false); break; }
            dot_counter++;
            splash.draw_unified_screen(scan_count.load(), true, 0, 0, false, dot_counter);
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        for (auto& t : threads) { if (t.joinable()) t.join(); }
    }
    // --------------------------------

    auto scan_end = std::chrono::steady_clock::now();
    auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(scan_end - scan_start).count();
    g_logger.info("Scan complete: %d items in %ld ms",
                  (int)scanned_items.size(), scan_ms);
    g_logger.info("Scan breakdown: photos=%d videos=%d ignored=0",
        std::count_if(scanned_items.begin(), scanned_items.end(), [](const MediaItem& i){ return i.type == "image"; }),
        std::count_if(scanned_items.begin(), scanned_items.end(), [](const MediaItem& i){ return i.type == "video"; }));

    // Final render after scan complete
    if (splash.logo_loaded) {
        BeginDrawing();
        splash.render(1, 0, 0, (int)scanned_items.size(), "Scanning...");
        EndDrawing();
    }

    // PHASE 2: CACHE
    g_logger.info("Phase 2: Caching metadata...");
    BeginDrawing();
    splash.render(2, 0, (int64_t)scanned_items.size(), 0, "Caching...");
    EndDrawing();

    // FIX v16.7.0: dynamic allocation — stack-local CacheManager was destroyed at block close (line 4445),
    // but g_cache pointer was used throughout slideshow (UAF crash). Heap allocation keeps it alive.
    if (!g_cache) {
        CacheManager* cache_instance = new CacheManager();
        if (!cache_instance->open(cfg.cache_dir)) {
            delete cache_instance;
            return 1;
        }
        g_cache = cache_instance;
    }
    CacheManager* cache_instance = g_cache;

    auto cache_start = std::chrono::steady_clock::now();
    int cached = 0, processed = 0;
    int total = (int)scanned_items.size();

    // FIX v5.3.0: Clear the stale log buffer to prevent old cache data flicker on screen
    {
        std::lock_guard<std::mutex> lock(splash.log_mutex);
        splash.log_buffer.clear();
    }
    // Force one clean frame to start the progress bar exactly at 0%
    splash.draw_unified_screen(total, false, 0, total, true, dot_counter);

    // Bulk transaction — wraps entire Phase 3 in single BEGIN/COMMIT for max speed
    cache_instance->begin_transaction();

    // SAFE PATH EXTRACTOR: Avoids std::filesystem UTF-8 aborts on corrupted filenames
    auto get_display_path = [](const std::string& path) -> std::string {
        if (path.empty()) return "./";
        int slashes = 0;
        for (int i = (int)path.length() - 1; i >= 0; i--) {
            if (path[i] == '/') {
                if (++slashes == 3) return "." + path.substr(i);
            }
        }
        return (path.front() != '/') ? ("./" + path) : ("." + path);
    };

    auto last_render = std::chrono::steady_clock::now();

    for (int i = 0; i < total; i++) {
        auto& mi = scanned_items[i];

        if (cache_instance->load_cached(mi)) {
            mi.cached = true;
            processed++;
            cached++;

            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<float>(now - last_render).count() >= 0.1f) {
                dot_counter++;
                splash.current_cache_file = get_display_path(mi.path);
                splash.draw_unified_screen(total, false, i, total, true, dot_counter);
                last_render = now;
            }
            continue;
        }

        if (mi.type == "image") {
            // Phase 2: Defer EXIF rotation to display time — reading EXIF per-file over CIFS hangs
            // auto_display_rotation=1 in preload_next/load_item handles rotation at display time
            mi.exif_rotation = 1;
            mi.width = 1920; mi.height = 1080; // Placeholder dimensions
        } else if (mi.type == "video") {
            // Phase 2: Skip video probing — ffprobe 8s timeout × 905 videos = hours on CIFS
            // Duration/probe deferred to display time (preload_next handles lazy probe)
            mi.width = g_cfg.screen_w;
            mi.height = g_cfg.screen_h;
            mi.duration = 0.0;
        }

        // ALWAYS insert into SQLite. 0 means "not a bad file"
        cache_instance->upsert(mi, 0);

        processed++;

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<float>(now - last_render).count() >= 0.1f) {
            dot_counter++;
            splash.current_cache_file = get_display_path(mi.path);
            splash.draw_unified_screen((int)scanned_items.size(), false, processed, total, true, dot_counter);
            last_render = now;
        }
    }
    cache_instance->commit_transaction();
    g_database_complete.store(true); // Database transaction fully flushed and valid

    auto cache_end = std::chrono::steady_clock::now();
    auto cache_ms = std::chrono::duration_cast<std::chrono::milliseconds>(cache_end - cache_start).count();
    g_logger.info("Caching complete: %d/%d items in %ld ms (already_cached=%d, new=%d)",
                  cached, total, cache_ms,
                  std::count_if(scanned_items.begin(), scanned_items.end(), [](const MediaItem& i){ return i.cached; }),
                  total - std::count_if(scanned_items.begin(), scanned_items.end(), [](const MediaItem& i){ return i.cached; }));

    // Smooth fade transition to black
    float fade_alpha = 0.0f;
    while (fade_alpha <= 1.0f && !WindowShouldClose()) {
        fade_alpha += GetFrameTime() * 1.5f;
        BeginDrawing();
        splash.draw_unified_screen((int)scanned_items.size(), false, processed, total, true, dot_counter);
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                      (Color){0, 0, 0, (unsigned char)(255 * fade_alpha)});
        EndDrawing();
    }
    } // end Phase 1+2 block

    slideshow_start:
    // PHASE 3 (formerly Phase 3): SLIDESHOW
    g_logger.info("Starting slideshow with %d items (photos=%d, videos=%d, shuffle=%d)",
        (int)scanned_items.size(),
        std::count_if(scanned_items.begin(), scanned_items.end(), [](const MediaItem& i){ return i.type == "image"; }),
        std::count_if(scanned_items.begin(), scanned_items.end(), [](const MediaItem& i){ return i.type == "video"; }),
        g_cfg.shuffle ? 1 : 0);

    Slideshow slide;
    slide.init();
    slide.shuffle = g_cfg.shuffle;

    // Launch treadmill daemon
    std::thread treadmill_thread(treadmill_worker, std::ref(cfg), std::ref(slide));

    // Launch background threads — pass config copy to avoid data race on g_cfg
    std::thread weather_thread, http_thread;
    if (g_cfg.weather_enabled) weather_thread = std::thread(weather_thread_func, g_cfg);
    if (g_cfg.http_enabled) http_thread = std::thread(http_thread_func, g_cfg, std::ref(slide));

    // Cooldown filter & Playlist Construction
    std::vector<MediaItem> active_photos;
    std::vector<MediaItem> active_videos;
    time_t now_ts = time(nullptr);

    for (auto& mi : scanned_items) {
        long long days_since = (now_ts - mi.last_shown) / 86400LL;
        if (mi.last_shown == 0 || days_since >= g_cfg.cooldown_days) {
            if (mi.type == "video") active_videos.push_back(mi);
            else active_photos.push_back(mi);
        }
    }

    if (active_photos.empty() && active_videos.empty() && !scanned_items.empty()) {
        g_logger.info("All files on cooldown — showing full set");
        for (auto& mi : scanned_items) {
            if (mi.type == "video") active_videos.push_back(mi);
            else active_photos.push_back(mi);
        }
    }

    // Randomize photo order on every launch
    if (!active_photos.empty()) {
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(active_photos.begin(), active_photos.end(), g);
    }

    std::vector<MediaItem> active_items;
    if (g_cfg.videos_per_photos <= 0) {
        // Videos completely disabled
        active_items = std::move(active_photos);
    } else {
        // Interleave videos into the photo stream
        size_t p_idx = 0, v_idx = 0;
        while (p_idx < active_photos.size() || v_idx < active_videos.size()) {
            for (int i = 0; i < 10 && p_idx < active_photos.size(); i++) {
                active_items.push_back(active_photos[p_idx++]);
            }
            for (int i = 0; i < g_cfg.videos_per_photos && v_idx < active_videos.size(); i++) {
                active_items.push_back(active_videos[v_idx++]);
            }
        }
    }

auto items_ptr = std::make_shared<std::vector<MediaItem>>(std::move(active_items));
   slide.items = items_ptr;

    // ── Ensure first slide is always an image, never a video ──
    int start_idx = 0;
    if (!items_ptr->empty()) {
        for (size_t i = 0; i < items_ptr->size(); i++) {
            if ((*items_ptr)[i].type == "image") {
                start_idx = (int)i;
                break;
            }
        }

        slide.current_index.store(start_idx);
        slide.next_index.store((start_idx + 1) % (int)items_ptr->size());
        
        // Clean, centralized reentrant invocation replaces 150 lines of duplicate buggy thread logic
        slide.preload_next(); 

        // ── Load first image — uses preloaded texture if ready, else loads from disk ──
        slide.current_tex.id = 0;

        bool first_loaded = false;
        // Try starting from start_idx, skip corrupted files with limited fallback
        for (int fallback = 0; fallback < 10 && !first_loaded; fallback++) {
            int try_idx = (start_idx + fallback) % (int)items_ptr->size();
            if ((*items_ptr)[try_idx].type != "image") continue;
            // Check corrupted cache — skip if already failed
            {
                std::lock_guard<std::mutex> lk(slide.corrupted_cache_mtx);
                auto it = slide.corrupted_cache.find((*items_ptr)[try_idx].path);
                if (it != slide.corrupted_cache.end() && it->second.first >= 1) continue;
            }
            slide.load_item((*items_ptr)[try_idx], items_ptr);
            if (slide.current_tex.id != 0) {
                first_loaded = true;
                slide_debug("INIT_FIRST_LOADED: path=%s tex id=%d w=%d h=%d", (*items_ptr)[try_idx].path.substr(0,60).c_str(), slide.current_tex.id, slide.current_tex.width, slide.current_tex.height);
                if (fallback > 0) {
                    slide.current_index.store(try_idx);
                    slide.next_index.store((try_idx + 1) % (int)items_ptr->size());
                }
            }
        }
        if (!first_loaded && (*items_ptr)[start_idx].type == "image") {
            slide.load_item((*items_ptr)[start_idx], items_ptr);
        }
    }

    splash.cleanup();

      // Show first frame cleanly — all init is done, GL state is uncontaminated
           // v3.0.0: render always (mpv frames render through transparent clear)
           // When transitioning (video→photo or photo→video), always render the transition effect.
           if (!slide.current_is_video || slide.transitioning) {
               BeginDrawing();
               slide.render();
               EndDrawing();
           }

    // MAIN LOOP
    auto last_time = std::chrono::steady_clock::now();

    while (g_running.load() && !WindowShouldClose()) {
        // v6.0.6: Capture shared_ptr to prevent treadmill worker from replacing items mid-loop
        std::shared_ptr<std::vector<MediaItem>> items_ptr;
        {
            std::lock_guard<std::mutex> lk(slide.shuffle_mutex);
            items_ptr = slide.items;
            slide.frame_current_index = slide.current_index.load();
            slide.frame_next_index = slide.next_index.load();
        }
        auto now = std::chrono::steady_clock::now();
         float dt = std::chrono::duration<float>(now - last_time).count();
         last_time = now;
         if (IsKeyPressed(KEY_ESCAPE)) {
              g_running.store(false);
              printf("\033[1;33m[INFO]\033[0m Exiting piTrove after ESC...\n");
              // Background & prevents deadlock: systemd may send SIGTERM to this
              // process via restart, and system() blocks until systemctl returns.
              system("systemctl restart piTrove.service 2>/dev/null &");
          }
    if (IsKeyPressed(KEY_SPACE)) {
             std::lock_guard<std::mutex> lk(slide.shuffle_mutex);
             slide.shuffle = !slide.shuffle;
         }
          if (IsKeyPressed(KEY_RIGHT) && items_ptr->size() > 1) slide.advance(true);
          if (IsKeyPressed(KEY_LEFT) && items_ptr->size() > 1) slide.advance(false);
          if (IsKeyPressed(KEY_R) && items_ptr->size() > 1) {
             std::lock_guard<std::mutex> lk(slide.shuffle_mutex);
             slide.shuffle = !slide.shuffle;
         }
          // v1.9.5: Consume g_remote_command from HTTP thread (Y3)
           // v6.0.4: If navigation happens during video playback, kill mpv immediately
           //         instead of relying on mpv monitor to detect g_remote_command (race condition)
           int cmd = g_remote_command.exchange(0);
           if ((cmd == 1 || cmd == 2) && items_ptr->size() > 1) {
if (slide.current_is_video) {
                    // mpv_video_play() fallback removed — mpv_render_context handles lifecycle
                }
               slide.advance(cmd == 1);
           }

         // Touch support: tap left=back, right=forward
           if (g_cfg.touch_enabled && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
              Vector2 tap_pos = GetTouchPosition(0);
              int sw = GetScreenWidth();
              if (tap_pos.x < sw / 2 && items_ptr->size() > 1) {
                  slide.advance(false);
              } else if (tap_pos.x >= sw / 2 && items_ptr->size() > 1) {
                  slide.advance(true);
              }
          }

      // Brightness control via xrandr/wlr-randr
           if (g_cfg.brightness_auto && !g_cfg.sleep_time.empty() && !g_cfg.wake_time.empty()) {
                time_t now_ts = time(nullptr);
                 struct tm tm_buf;
                 struct tm* tm_info = localtime_r(&now_ts, &tm_buf);
                 char current_time[8] = {0};
                 if (tm_info) std::snprintf(current_time, sizeof(current_time), "%02d:%02d", tm_info->tm_hour, tm_info->tm_min);

               // v1.9.6: Track last_brightness_min to fire brightness only on transition (H5)
               static int last_brightness_min = -1;
               static bool brightness_set = false;
               if (last_brightness_min != tm_info->tm_min) {
                   last_brightness_min = tm_info->tm_min;
                   if (!brightness_set && strcmp(current_time, g_cfg.sleep_time.c_str()) == 0) {
                       spawn("xrandr --output HDMI-A-1 --brightness " + std::to_string((float)g_cfg.brightness_auto_min / 100.0f));
                       brightness_set = true;
                   } else if (brightness_set && strcmp(current_time, g_cfg.wake_time.c_str()) == 0) {
                       spawn("xrandr --output HDMI-A-1 --brightness " + std::to_string((float)g_cfg.brightness_auto_max / 100.0f));
                       brightness_set = false;
                   }
               }
           }

         slide.update(dt);

        // Sleep/Wake power management (Guaranteed minute firing)
          if (!g_cfg.sleep_time.empty() && !g_cfg.wake_time.empty()) {
              time_t now = time(nullptr);
               struct tm tm_buf;
               struct tm* tm_info = localtime_r(&now, &tm_buf);
               char current_time[8] = {0};
               if (tm_info) std::snprintf(current_time, sizeof(current_time), "%02d:%02d", tm_info->tm_hour, tm_info->tm_min);

              static int last_checked_min = -1;
             static bool display_off = false;

            if (last_checked_min != tm_info->tm_min) {
                  last_checked_min = tm_info->tm_min;
                  if (!display_off && strcmp(current_time, g_cfg.sleep_time.c_str()) == 0) {
                      spawn("wlr-randr --output HDMI-A-1 --off || xset dpms force off 2>/dev/null");
                      display_off = true;
                  } else if (display_off && strcmp(current_time, g_cfg.wake_time.c_str()) == 0) {
                      spawn("wlr-randr --output HDMI-A-1 --on || xset dpms force on 2>/dev/null");
                      display_off = false;
                  }
              }
         }

             // Decode one mpv video frame into g_mpv.video_rt BEFORE BeginDrawing.
               // CRITICAL: Call unconditionally. Relying on g_mpv_frame_available causes
               // missed edge-triggers if mpv signals OSD updates instead of FRAME updates.
               if (slide.current_is_video && g_mpv.is_initialized() && g_mpv.is_playing()) {
                  g_mpv.update_frame();
               }

              // Always render — video frame decoded above, render() blits video_rt.
              // Overlays (borders, date, clock, weather) draw on top inside render().
              BeginDrawing();
              slide.render();
              EndDrawing();
           // v2.9.0: Main loop diagnostic logging
              if (!slide.current_is_video && !slide.transitioning) {
                  static int frame_count = 0;
                  if (++frame_count % 30 == 0) {
                      g_logger.debug("MAIN_LOOP: cur=%d is_video=%d transitioning=%d preload_ready=%d",
                           slide.current_index.load(),
                           slide.current_is_video ? 1 : 0,
                           slide.transitioning ? 1 : 0,
                           slide.preload_ready.load() ? 1 : 0);
                  }
              }

    }
    // Force a black frame on the DRM display before exiting.
    // On DRM/KMS, the framebuffer persists after process exit, so we must
    // explicitly write a black frame and swap it before closing raylib.
    BeginDrawing();
    ClearBackground(BLACK);
    EndDrawing();

    slide.cleanup();
    // Ensure preload_thread is fully stopped before closing VRAM context
    if (slide.preload_thread.joinable()) slide.preload_thread.join();
    CloseWindow();

    // v3.0.4: Gracefully unblock socket operations before joining (L1/L2)
    if (g_http_server_fd > 0) shutdown(g_http_server_fd, SHUT_RDWR);

    // Join background threads before they go out of scope (prevent std::terminate)
    if (treadmill_thread.joinable()) treadmill_thread.join();
    if (weather_thread.joinable()) weather_thread.join();
    if (http_thread.joinable()) http_thread.join();

    // v3.1.1: mpv cleanup done in slide.cleanup() via in-process g_mpv + subprocess safety net

    // Clean up PID file on exit
    std::string pidfile = cfg.cache_dir + "/piTrove.pid";
    std::remove(pidfile.c_str());

    // FIX v16.7.0: free heap-allocated cache (was stack-allocated in v1.6.6, causing UAF)
    if (g_cache) { delete g_cache; g_cache = nullptr; }

  // v1.9.5: Close slide_debug FILE* on exit (W3)
     slide_debug_close();

     g_logger.info("piTrove v%s exiting cleanly", VERSION);
     return 0;
}

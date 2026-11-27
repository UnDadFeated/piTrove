#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "image_loader.h"
#include "blur.h"
#include <condition_variable>
#include <thread>
#include <filesystem>
#include <utility>
#include <optional>
#include <webp/decode.h>
#include <libheif/heif.h>
#include <lcms2.h>
#include <tiffio.h>
#include <zlib.h>
#include <span>
#include <mutex>
#include <bit>
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

#include "config.h"
#include "renderer.h"
#include "util.h"
#include <SDL3_image/SDL_image.h>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <cstdio>
#include <memory>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// EXIF tags and library
#include <libexif/exif-data.h>
#include <libexif/exif-entry.h>
#include <libexif/exif-content.h>
#include <libexif/exif-tag.h>
#include <libexif/exif-utils.h>

// ============================================================
// Public API
// ============================================================

static bool is_webp(std::span<const uint8_t> bytes) {
    return bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
           std::memcmp(bytes.data() + 8, "WEBP", 4) == 0;
}

// SDL_image 3.2.x has shared codec initialization state for PNG and JPEG.
std::mutex g_sdl_image_mutex;

namespace {

constexpr size_t max_icc_size = 4 * 1024 * 1024;

bool usable_dimensions(int w, int h) {
    constexpr size_t max_pixels = 64ULL * 1024 * 1024;
    return w > 0 && h > 0 && w <= 16384 && h <= 16384 &&
           static_cast<size_t>(w) <= max_pixels / static_cast<size_t>(h);
}

// High-megapixel smartphone JPEGs (108MP-200MP) can be DCT-downscaled directly by
// libjpeg-turbo into a small RGBA buffer; keep a strict upper bound against bombs.
bool jpeg_scalable_dimensions(int w, int h) {
    constexpr size_t max_source_pixels = 256ULL * 1024 * 1024;
    return w > 0 && h > 0 && w <= 32768 && h <= 32768 &&
           static_cast<size_t>(w) <= max_source_pixels / static_cast<size_t>(h);
}

enum class CommonFormat { Unknown, Jpeg, Png, Gif, Bmp };

CommonFormat common_format(std::span<const uint8_t> data) {
    if (data.size() >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff)
        return CommonFormat::Jpeg;
    constexpr uint8_t png_signature[]{0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    if (data.size() >= sizeof(png_signature) &&
        std::memcmp(data.data(), png_signature, sizeof(png_signature)) == 0) return CommonFormat::Png;
    if (data.size() >= 6 && (std::memcmp(data.data(), "GIF87a", 6) == 0 ||
                             std::memcmp(data.data(), "GIF89a", 6) == 0)) return CommonFormat::Gif;
    if (data.size() >= 2 && data[0] == 'B' && data[1] == 'M') return CommonFormat::Bmp;
    return CommonFormat::Unknown;
}

uint16_t read_le16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

uint32_t read_le32(const uint8_t* bytes) {
    return read_le16(bytes) | (static_cast<uint32_t>(read_le16(bytes + 2)) << 16);
}

uint32_t read_be32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) | (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

struct JpegDecoder {
    jpeg_decompress_struct state{};
    JpegErrorMgr error{};
    uint8_t* pixels = nullptr;

    JpegDecoder() {
        state.err = jpeg_std_error(&error.pub);
        error.pub.error_exit = jpeg_error_exit;
        error.pub.output_message = [](j_common_ptr) {};
    }

    ~JpegDecoder() {
        jpeg_destroy_decompress(&state);
        std::free(pixels);
    }
};

bool jpeg_dimensions(std::span<const uint8_t> data, int& w, int& h, bool* buffered = nullptr) {
    // The decoder mutates this storage even when it exits via longjmp.
    const auto decoder = std::make_unique<JpegDecoder>();
    auto& jpeg = decoder->state;
    if (setjmp(decoder->error.jump)) return false;
    jpeg_create_decompress(&jpeg);
    jpeg_mem_src(&jpeg, data.data(), data.size());
    if (jpeg_read_header(&jpeg, TRUE) != JPEG_HEADER_OK) return false;
    w = static_cast<int>(jpeg.image_width);
    h = static_cast<int>(jpeg.image_height);
    if (buffered) *buffered = jpeg_has_multiple_scans(&jpeg);
    return jpeg_scalable_dimensions(w, h);
}

unsigned int jpeg_scale(int src_w, int src_h, int max_dim) {
    int target_dim = max_dim;
    if (target_dim <= 0 && !usable_dimensions(src_w, src_h)) {
        target_dim = 4096;
    }
    unsigned int scale_num = 8;
    if (target_dim > 0 && (src_w > target_dim || src_h > target_dim)) {
        scale_num = 1;
        for (unsigned int num = 1; num <= 8; ++num) {
            const int sw = static_cast<int>((static_cast<uint64_t>(src_w) * num + 7) / 8);
            const int sh = static_cast<int>((static_cast<uint64_t>(src_h) * num + 7) / 8);
            scale_num = num;
            if (sw >= target_dim || sh >= target_dim) break;
        }
    }
    while (scale_num > 1 &&
           !usable_dimensions((src_w * scale_num + 7) / 8, (src_h * scale_num + 7) / 8)) {
        --scale_num;
    }
    return scale_num;
}

bool decode_jpeg_libjpeg(std::span<const uint8_t> data, RawImage& result, int max_dim) {
    const auto decoder = std::make_unique<JpegDecoder>();
    auto& jpeg = decoder->state;
    if (setjmp(decoder->error.jump)) return false;
    jpeg_create_decompress(&jpeg);
    jpeg_mem_src(&jpeg, data.data(), data.size());
    if (jpeg_read_header(&jpeg, TRUE) != JPEG_HEADER_OK) return false;
    const int src_w = static_cast<int>(jpeg.image_width);
    const int src_h = static_cast<int>(jpeg.image_height);
    if (!jpeg_scalable_dimensions(src_w, src_h)) return false;

    jpeg.scale_num = jpeg_scale(src_w, src_h, max_dim);
    jpeg.scale_denom = 8;
    const bool is_cmyk = (jpeg.jpeg_color_space == JCS_CMYK || jpeg.jpeg_color_space == JCS_YCCK);
    jpeg.out_color_space = is_cmyk ? JCS_CMYK : JCS_EXT_RGBA;
    jpeg_calc_output_dimensions(&jpeg);

    const int out_w = static_cast<int>(jpeg.output_width);
    const int out_h = static_cast<int>(jpeg.output_height);
    if (!usable_dimensions(out_w, out_h) || jpeg.output_components != 4) return false;

    const size_t stride = static_cast<size_t>(out_w) * 4;
    decoder->pixels = static_cast<uint8_t*>(std::malloc(stride * static_cast<size_t>(out_h)));
    if (!decoder->pixels) return false;

    jpeg_start_decompress(&jpeg);
    while (jpeg.output_scanline < jpeg.output_height) {
        JSAMPROW row = decoder->pixels + static_cast<size_t>(jpeg.output_scanline) * stride;
        jpeg_read_scanlines(&jpeg, &row, 1);
        if (is_cmyk) {
            for (int x = 0; x < out_w; ++x) {
                const uint8_t c = row[x * 4 + 0];
                const uint8_t m = row[x * 4 + 1];
                const uint8_t y = row[x * 4 + 2];
                const uint8_t k = row[x * 4 + 3];
                row[x * 4 + 0] = static_cast<uint8_t>((static_cast<unsigned>(c) * k) / 255U);
                row[x * 4 + 1] = static_cast<uint8_t>((static_cast<unsigned>(m) * k) / 255U);
                row[x * 4 + 2] = static_cast<uint8_t>((static_cast<unsigned>(y) * k) / 255U);
                row[x * 4 + 3] = 255;
            }
        }
    }
    jpeg_finish_decompress(&jpeg);

    result.width = out_w;
    result.height = out_h;
    result.pixels = std::exchange(decoder->pixels, nullptr);
    return true;
}

bool common_dimensions(CommonFormat format, std::span<const uint8_t> data, int& w, int& h) {
    switch (format) {
        case CommonFormat::Jpeg:
            return jpeg_dimensions(data, w, h);
        case CommonFormat::Png:
            if (data.size() < 33 || read_be32(data.data() + 8) != 13 ||
                std::memcmp(data.data() + 12, "IHDR", 4) != 0) return false;
            if (read_be32(data.data() + 16) > INT_MAX || read_be32(data.data() + 20) > INT_MAX)
                return false;
            w = static_cast<int>(read_be32(data.data() + 16));
            h = static_cast<int>(read_be32(data.data() + 20));
            break;
        case CommonFormat::Gif:
            if (data.size() < 13) return false;
            w = read_le16(data.data() + 6);
            h = read_le16(data.data() + 8);
            break;
        case CommonFormat::Bmp: {
            if (data.size() < 26) return false;
            const uint32_t header = read_le32(data.data() + 14);
            if (header == 12) {
                w = read_le16(data.data() + 18);
                h = read_le16(data.data() + 20);
            } else {
                if (header < 16 || header > data.size() - 14) return false;
                w = std::bit_cast<int32_t>(read_le32(data.data() + 18));
                const auto signed_height = std::bit_cast<int32_t>(read_le32(data.data() + 22));
                if (signed_height == INT_MIN) return false;
                h = std::abs(signed_height);
            }
            break;
        }
        case CommonFormat::Unknown:
            return false;
    }
    return usable_dimensions(w, h);
}

bool skip_gif_blocks(std::span<const uint8_t> data, size_t& pos) {
    while (pos < data.size()) {
        const auto length = data[pos++];
        if (length == 0) return true;
        if (length > data.size() - pos) return false;
        pos += length;
    }
    return false;
}

bool first_gif_frame(std::span<const uint8_t>& data, SDL_Rect& frame, int w, int h) {
    const auto color_table_size = [](uint8_t flags) -> size_t {
        return (flags & 0x80) ? 3U << ((flags & 7) + 1) : 0;
    };
    size_t pos = 13 + color_table_size(data[10]);
    while (pos < data.size()) {
        const auto marker = data[pos++];
        if (marker == '!') {
            if (pos >= data.size()) return false;
            ++pos; // Extension type.
            if (!skip_gif_blocks(data, pos)) return false;
            continue;
        }
        if (marker != ',' || data.size() - pos < 9) return false;
        frame = {read_le16(data.data() + pos), read_le16(data.data() + pos + 2),
                 read_le16(data.data() + pos + 4), read_le16(data.data() + pos + 6)};
        if (!usable_dimensions(frame.w, frame.h) || frame.x + frame.w > w || frame.y + frame.h > h)
            return false;
        pos += 9 + color_table_size(data[pos + 8]);
        if (pos >= data.size()) return false;
        ++pos; // LZW minimum code size.
        if (!skip_gif_blocks(data, pos)) return false;
        // SDL_image should only see the first frame we checked.
        data = data.first(pos);
        return true;
    }
    return false;
}

bool decode_common(CommonFormat format, std::span<const uint8_t> data, RawImage& result) {
    SDL_Rect frame{};
    if (format == CommonFormat::Gif && !first_gif_frame(data, frame, result.width, result.height))
        return false;
    std::unique_ptr<SDL_IOStream, decltype(&SDL_CloseIO)>
        input(SDL_IOFromConstMem(data.data(), data.size()), SDL_CloseIO);
    if (!input) return false;
    using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
    Surface decoded(nullptr, SDL_DestroySurface);
    {
        std::lock_guard lock(g_sdl_image_mutex);
        switch (format) {
            case CommonFormat::Jpeg: decoded.reset(IMG_LoadJPG_IO(input.get())); break;
            case CommonFormat::Png: decoded.reset(IMG_LoadPNG_IO(input.get())); break;
            case CommonFormat::Gif: decoded.reset(IMG_LoadGIF_IO(input.get())); break;
            case CommonFormat::Bmp: decoded.reset(IMG_LoadBMP_IO(input.get())); break;
            case CommonFormat::Unknown: return false;
        }
    }
    if (!decoded || !usable_dimensions(decoded->w, decoded->h)) return false;
    Surface rgba = decoded->format == SDL_PIXELFORMAT_RGBA32 && !SDL_SurfaceHasColorKey(decoded.get())
        ? std::move(decoded)
        : Surface(SDL_ConvertSurface(decoded.get(), SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
    decoded.reset();
    if (!rgba) return false;
    if (format == CommonFormat::Gif) {
        if (rgba->w != frame.w || rgba->h != frame.h) return false;
        if (frame.w != result.width || frame.h != result.height) {
            Surface canvas(SDL_CreateSurface(result.width, result.height, SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
            if (!canvas) return false;
            uint32_t background = 0;
            const auto index = data[11];
            const auto palette_entries = 2U << (data[10] & 7);
            if ((data[10] & 0x80) && index > 0 && index < palette_entries) {
                const auto* rgb = data.data() + 13 + 3 * index;
                background = SDL_MapSurfaceRGBA(canvas.get(), rgb[0], rgb[1], rgb[2], 255);
            }
            if (!SDL_FillSurfaceRect(canvas.get(), nullptr, background) ||
                !SDL_SetSurfaceBlendMode(rgba.get(), SDL_BLENDMODE_NONE) ||
                !SDL_BlitSurface(rgba.get(), nullptr, canvas.get(), &frame)) return false;
            rgba = std::move(canvas);
        }
    }
    if (rgba->w != result.width || rgba->h != result.height || !SDL_LockSurface(rgba.get())) return false;
    const size_t stride = static_cast<size_t>(result.width) * 4;
    if (rgba->pixels && rgba->pitch >= 0 && static_cast<size_t>(rgba->pitch) >= stride) {
        result.pixels = static_cast<uint8_t*>(std::malloc(stride * result.height));
        if (result.pixels) {
            const auto* src = static_cast<const uint8_t*>(rgba->pixels);
            for (int y = 0; y < result.height; ++y)
                std::memcpy(result.pixels + y * stride, src + static_cast<size_t>(y) * rgba->pitch, stride);
        }
    }
    SDL_UnlockSurface(rgba.get());
    return result.pixels != nullptr;
}

bool is_tiff(std::span<const uint8_t> data) {
    return data.size() >= 4 && (
        (data[0] == 'I' && data[1] == 'I' && (data[2] == 42 || data[2] == 43) && data[3] == 0) ||
        (data[0] == 'M' && data[1] == 'M' && data[2] == 0 && (data[3] == 42 || data[3] == 43)));
}

bool is_heif(std::span<const uint8_t> data) {
    return data.size() >= 12 && std::memcmp(data.data() + 4, "ftyp", 4) == 0 &&
           heif_check_filetype(data.data(), static_cast<int>(std::min<size_t>(data.size(), 4096))) ==
               heif_filetype_yes_supported;
}

struct TiffReader {
    std::span<const uint8_t> data;
    size_t offset = 0;
};

auto open_tiff(TiffReader& input) {
    auto* options = TIFFOpenOptionsAlloc();
    if (options) TIFFOpenOptionsSetMaxSingleMemAlloc(options, 256 * 1024 * 1024);
    auto* tiff = TIFFClientOpenExt("image", "rm", &input,
        [](thandle_t handle, void* out, tmsize_t count) -> tmsize_t {
            auto& in = *static_cast<TiffReader*>(handle);
            if (count < 0 || in.offset > in.data.size()) return 0;
            const size_t size = std::min(static_cast<size_t>(count), in.data.size() - in.offset);
            if (size) std::memcpy(out, in.data.data() + in.offset, size);
            in.offset += size;
            return size;
        },
        [](thandle_t, void*, tmsize_t) -> tmsize_t { return 0; },
        [](thandle_t handle, toff_t offset, int origin) -> toff_t {
            auto& in = *static_cast<TiffReader*>(handle);
            const size_t base = origin == SEEK_SET ? 0 : origin == SEEK_CUR ? in.offset : in.data.size();
            const int64_t delta = static_cast<int64_t>(offset);
            if (origin != SEEK_SET && origin != SEEK_CUR && origin != SEEK_END) return -1;
            if (delta >= 0) {
                if (static_cast<uint64_t>(delta) > in.data.size() - base) return -1;
                in.offset = base + delta;
            } else {
                const uint64_t magnitude = 0 - static_cast<uint64_t>(delta);
                if (origin == SEEK_SET || magnitude > base) return -1;
                in.offset = base - magnitude;
            }
            return in.offset;
        },
        [](thandle_t) { return 0; },
        [](thandle_t handle) -> toff_t { return static_cast<TiffReader*>(handle)->data.size(); },
        nullptr, nullptr, options);
    if (options) TIFFOpenOptionsFree(options);
    return std::unique_ptr<TIFF, decltype(&TIFFClose)>(tiff, TIFFClose);
}

bool tiff_dimensions(TIFF* tiff, int& w, int& h) {
    uint32_t width = 0, height = 0;
    if (!tiff || !TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) ||
        !TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) ||
        width > INT_MAX || height > INT_MAX) return false;
    w = static_cast<int>(width);
    h = static_cast<int>(height);
    return usable_dimensions(w, h);
}

auto open_heif(std::span<const uint8_t> data) {
    std::unique_ptr<heif_context, decltype(&heif_context_free)> ctx(heif_context_alloc(), heif_context_free);
    heif_image_handle* handle = nullptr;
    if (ctx) {
        heif_context_set_maximum_image_size_limit(ctx.get(), 16384);
        heif_context_set_max_decoding_threads(ctx.get(), 2);
        if (heif_context_read_from_memory_without_copy(ctx.get(), data.data(), data.size(), nullptr).code == heif_error_Ok)
            heif_context_get_primary_image_handle(ctx.get(), &handle);
    }
    return std::pair(std::move(ctx), std::unique_ptr<heif_image_handle, decltype(&heif_image_handle_release)>(
        handle, heif_image_handle_release));
}

bool heif_dimensions(const heif_image_handle* handle, int& w, int& h) {
    if (!handle || !usable_dimensions(heif_image_handle_get_ispe_width(handle),
                                      heif_image_handle_get_ispe_height(handle))) return false;
    w = heif_image_handle_get_width(handle);
    h = heif_image_handle_get_height(handle);
    return usable_dimensions(w, h);
}

void convert_icc(RawImage& result, cmsHPROFILE source) {
    if (!source || cmsGetColorSpace(source) != cmsSigRgbData) return;
    auto* srgb = cmsCreate_sRGBProfile();
    if (!srgb) return;
    auto* transform = cmsCreateTransform(source, TYPE_RGBA_8, srgb, TYPE_RGBA_8,
                                         INTENT_RELATIVE_COLORIMETRIC, cmsFLAGS_COPY_ALPHA);
    if (transform) {
        cmsDoTransform(transform, result.pixels, result.pixels,
                       static_cast<cmsUInt32Number>(static_cast<size_t>(result.width) * result.height));
        cmsDeleteTransform(transform);
    }
    cmsCloseProfile(srgb);
}

void convert_icc(RawImage& result, std::span<const uint8_t> profile) {
    if (profile.empty() || profile.size() > max_icc_size) return;
    auto* source = cmsOpenProfileFromMem(profile.data(), static_cast<cmsUInt32Number>(profile.size()));
    if (!source) return;
    convert_icc(result, source);
    cmsCloseProfile(source);
}

uint32_t chunk_size(const uint8_t* data, bool little_endian) {
    uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value = (value << 8) | data[little_endian ? 3 - i : i];
    return value;
}

void convert_embedded_profile(RawImage& result, std::span<const uint8_t> data) {
    const bool webp = is_webp(data);
    if (webp || (data.size() >= 8 && std::memcmp(data.data(), "\x89PNG\r\n\x1a\n", 8) == 0)) {
        size_t offset = webp ? 12 : 8;
        while (data.size() - offset >= (webp ? 8u : 12u)) {
            const auto* header = data.data() + offset;
            const size_t len = chunk_size(header + (webp ? 4 : 0), webp);
            const size_t overhead = webp ? 8 + (len & 1) : 12;
            if (overhead > data.size() - offset || len > data.size() - offset - overhead) break;
            const auto* type = header + (webp ? 0 : 4);
            const auto payload = data.subspan(offset + 8, len);
            if (webp && std::memcmp(type, "ICCP", 4) == 0) {
                convert_icc(result, payload);
                break;
            }
            if (!webp && std::memcmp(type, "iCCP", 4) == 0) {
                const auto split = std::find(payload.begin(), payload.end(), 0);
                const size_t start = static_cast<size_t>(split - payload.begin()) + 2;
                if (start < payload.size() && payload[start - 1] == 0) {
                    std::vector<uint8_t> profile(max_icc_size);
                    uLongf size = profile.size();
                    if (uncompress(profile.data(), &size, payload.data() + start, payload.size() - start) == Z_OK)
                        convert_icc(result, {profile.data(), size});
                }
                break;
            }
            if (!webp && std::memcmp(type, "IDAT", 4) == 0) break;
            offset += overhead + len;
        }
        return;
    }
    if (data.size() < 4 || data[0] != 0xff || data[1] != 0xd8) return;
    std::array<std::span<const uint8_t>, 256> chunks{};
    size_t count = 0, size = 0, found = 0;
    for (size_t pos = 2; data.size() - pos >= 4 && data[pos] == 0xff;) {
        if (data[pos + 1] == 0xff) { ++pos; continue; }
        if (data[pos + 1] == 0xda || data[pos + 1] == 0xd9) break;
        const size_t len = (static_cast<size_t>(data[pos + 2]) << 8) | data[pos + 3];
        if (len < 2 || len > data.size() - pos - 2) return;
        if (data[pos + 1] == 0xe2 && len > 16 &&
            std::memcmp(data.data() + pos + 4, "ICC_PROFILE\0", 12) == 0) {
            const size_t index = data[pos + 16];
            const size_t total = data[pos + 17];
            if (index == 0 || index > total || (count && count != total) || !chunks[index].empty()) return;
            count = total;
            chunks[index] = data.subspan(pos + 18, len - 16);
            size += len - 16;
            if (size > max_icc_size) return;
            ++found;
        }
        pos += len + 2;
    }
    if (count == 0 || found != count) return;
    std::vector<uint8_t> profile;
    profile.reserve(size);
    for (size_t i = 1; i <= count; ++i) profile.insert(profile.end(), chunks[i].begin(), chunks[i].end());
    convert_icc(result, profile);
}

void convert_heif_profile(RawImage& result, const heif_image_handle* handle) {
    const size_t size = heif_image_handle_get_raw_color_profile_size(handle);
    if (size > 0 && size <= max_icc_size) {
        std::vector<uint8_t> profile(size);
        if (heif_image_handle_get_raw_color_profile(handle, profile.data()).code == heif_error_Ok)
            convert_icc(result, profile);
        return;
    }
    heif_color_profile_nclx* description = nullptr;
    if (heif_image_handle_get_nclx_color_profile(handle, &description).code != heif_error_Ok) return;
    const std::unique_ptr<heif_color_profile_nclx, decltype(&heif_nclx_color_profile_free)>
        owner(description, heif_nclx_color_profile_free);
    if (description->color_primaries != 12 || description->transfer_characteristics != 13) return;
    const cmsCIExyY d65{0.3127, 0.3290, 1};
    const cmsCIExyYTRIPLE p3{{0.680, 0.320, 1}, {0.265, 0.690, 1}, {0.150, 0.060, 1}};
    const cmsFloat64Number parameters[]{2.4, 1.0 / 1.055, 0.055 / 1.055, 1.0 / 12.92, 0.04045};
    auto* curve = cmsBuildParametricToneCurve(nullptr, 4, parameters);
    if (!curve) return;
    cmsToneCurve* curves[]{curve, curve, curve};
    auto* source = cmsCreateRGBProfile(&d65, &p3, curves);
    if (source) {
        convert_icc(result, source);
        cmsCloseProfile(source);
    }
    cmsFreeToneCurve(curve);
}

} // namespace

bool ImageLoader::image_dimensions(std::span<const uint8_t> buffer, int& width, int& height) {
    if (buffer.empty() || buffer.size() > INT_MAX) return false;
    if (is_tiff(buffer)) {
        TiffReader reader{buffer};
        const auto tiff = open_tiff(reader);
        return tiff_dimensions(tiff.get(), width, height);
    }
    if (is_heif(buffer)) {
        const auto [ctx, handle] = open_heif(buffer);
        return heif_dimensions(handle.get(), width, height);
    }
    if (is_webp(buffer))
        return WebPGetInfo(buffer.data(), buffer.size(), &width, &height) && usable_dimensions(width, height);
    return common_dimensions(common_format(buffer), buffer, width, height);
}

namespace {

class DecodeReservation {
public:
    DecodeReservation(size_t bytes, std::stop_token stop) {
        std::unique_lock lock(mutex);
        // One large decode may take the entire budget and run by itself.
        if (in_use == 0) {
            limit = std::clamp<uint64_t>(get_available_ram_bytes() / 3,
                                        64ULL * 1024 * 1024, 512ULL * 1024 * 1024);
        }
        if (cv.wait(lock, stop, [&] { return std::min(bytes, limit) <= limit - in_use; }) &&
            !stop.stop_requested()) {
            reserved = std::min(bytes, limit);
            in_use += reserved;
        }
    }

    ~DecodeReservation() {
        if (!reserved) return;
        {
            std::lock_guard lock(mutex);
            in_use -= reserved;
        }
        cv.notify_all();
    }

    DecodeReservation(const DecodeReservation&) = delete;
    DecodeReservation& operator=(const DecodeReservation&) = delete;
    explicit operator bool() const { return reserved != 0; }

private:
    size_t reserved = 0;
    static inline std::mutex mutex;
    static inline std::condition_variable_any cv;
    static inline size_t limit = 0;
    static inline size_t in_use = 0;
};

std::optional<size_t> estimate_decode_peak(std::span<const uint8_t> buffer, int max_dim) {
    int width = 0, height = 0;
    size_t jpeg_coefficients = 0;
    if (common_format(buffer) == CommonFormat::Jpeg) {
        bool buffered = false;
        if (!jpeg_dimensions(buffer, width, height, &buffered)) return std::nullopt;
        if (buffered) jpeg_coefficients = static_cast<size_t>(width) * height * 8;
        const auto scale = jpeg_scale(width, height, max_dim);
        width = (width * scale + 7) / 8;
        height = (height * scale + 7) / 8;
    } else if (!ImageLoader::image_dimensions(buffer, width, height)) {
        return std::nullopt;
    }
    if (!usable_dimensions(width, height)) return std::nullopt;
    // Allow for the decoder's output, RGBA conversion, and display-size scaling/rotation.
    return static_cast<size_t>(width) * height * 16 + jpeg_coefficients + buffer.size();
}

RawImage decode_rgba_unreserved(std::span<const uint8_t> buffer, int max_dim) {
    RawImage result;
    if (buffer.empty() || buffer.size() > INT_MAX) return {};
    if (is_tiff(buffer)) {
        TiffReader reader{buffer};
        const auto tiff = open_tiff(reader);
        if (!tiff_dimensions(tiff.get(), result.width, result.height)) return {};
        const size_t pixels = static_cast<size_t>(result.width) * result.height;
        result.pixels = static_cast<uint8_t*>(std::malloc(pixels * 4));
        if (!result.pixels || !TIFFReadRGBAImageOriented(tiff.get(), result.width, result.height,
                reinterpret_cast<uint32_t*>(result.pixels), ORIENTATION_TOPLEFT, 1)) return {};
        if constexpr (std::endian::native == std::endian::big) {
            for (size_t i = 0; i < pixels; ++i) {
                auto* rgba = result.pixels + i * 4;
                std::reverse(rgba, rgba + 4);
            }
        }
        uint32_t size = 0;
        uint8_t* profile = nullptr;
        if (TIFFGetField(tiff.get(), TIFFTAG_ICCPROFILE, &size, &profile) && profile)
            convert_icc(result, {profile, size});
    } else if (is_heif(buffer)) {
        const auto [ctx, handle] = open_heif(buffer);
        if (!heif_dimensions(handle.get(), result.width, result.height)) return {};
        std::unique_ptr<heif_decoding_options, decltype(&heif_decoding_options_free)>
            options(heif_decoding_options_alloc(), heif_decoding_options_free);
        if (!options) return {};
        options->convert_hdr_to_8bit = 1;
        heif_image* image = nullptr;
        const auto status = heif_decode_image(handle.get(), &image, heif_colorspace_RGB,
                                              heif_chroma_interleaved_RGBA, options.get());
        const std::unique_ptr<heif_image, decltype(&heif_image_release)> owner(image, heif_image_release);
        if (status.code != heif_error_Ok || !image) return {};
        result.width = heif_image_get_width(image, heif_channel_interleaved);
        result.height = heif_image_get_height(image, heif_channel_interleaved);
        if (!usable_dimensions(result.width, result.height)) return {};
        int pitch = 0;
        const auto* plane = heif_image_get_plane_readonly(image, heif_channel_interleaved, &pitch);
        const size_t stride = static_cast<size_t>(result.width) * 4;
        if (!plane || pitch < 0 || static_cast<size_t>(pitch) < stride) return {};
        result.pixels = static_cast<uint8_t*>(std::malloc(stride * result.height));
        if (!result.pixels) return {};
        for (int y = 0; y < result.height; ++y)
            std::memcpy(result.pixels + y * stride, plane + static_cast<size_t>(y) * pitch, stride);
        convert_heif_profile(result, handle.get());
    } else if (is_webp(buffer)) {
        if (!WebPGetInfo(buffer.data(), buffer.size(), &result.width, &result.height) ||
            !usable_dimensions(result.width, result.height)) return {};
        const size_t stride = static_cast<size_t>(result.width) * 4;
        const size_t size = stride * result.height;
        result.pixels = static_cast<uint8_t*>(std::malloc(size));
        if (!result.pixels || !WebPDecodeRGBAInto(buffer.data(), buffer.size(), result.pixels, size,
                                                 static_cast<int>(stride))) return {};
        convert_embedded_profile(result, buffer);
    } else {
        const auto format = common_format(buffer);
        if (format == CommonFormat::Jpeg) {
            if (!decode_jpeg_libjpeg(buffer, result, max_dim)) return {};
        } else {
            if (!common_dimensions(format, buffer, result.width, result.height) ||
                !decode_common(format, buffer, result)) return {};
        }
        convert_embedded_profile(result, buffer);
    }
    result.channels = 4;
    result.format = ImageFormat::RGBA32;
    result.valid = result.pixels != nullptr;
    return result;
}

} // namespace

RawImage ImageLoader::decode_rgba(std::span<const uint8_t> buffer, int max_dim) {
    const auto peak = estimate_decode_peak(buffer, max_dim);
    if (!peak) return {};
    const DecodeReservation reservation(*peak, {});
    return decode_rgba_unreserved(buffer, max_dim);
}

namespace {

std::vector<uint8_t> read_image_file(const std::string& path, std::stop_token stop) {
    if (stop.stop_requested()) { errno = ECANCELED; return {}; }
    std::vector<uint8_t> buffer;
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    [[unlikely]]
    if (fd < 0) {
        return buffer;
    }
    struct stat st{};
    const int stat_result = ::fstat(fd, &st);
    if (stat_result != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        const int saved_err = stat_result != 0 ? errno : EINVAL;
        ::close(fd);
        errno = saved_err;
        return buffer;
    }
    constexpr size_t max_img_size = 128ULL * 1024 * 1024;
    const size_t total = static_cast<size_t>(st.st_size);
    if (total > max_img_size) {
        ::close(fd);
        errno = EFBIG;
        return buffer;
    }
    if (stop.stop_requested()) {
        ::close(fd);
        errno = ECANCELED;
        return {};
    }
    try {
        buffer.resize(total);
    } catch (const std::bad_alloc&) {
        ::close(fd);
        errno = ENOMEM;
        return {};
    }
    size_t offset = 0;
    constexpr size_t chunk_size = 1024 * 1024;
    while (offset < total && !stop.stop_requested()) {
        const ssize_t n = ::read(fd, buffer.data() + offset, std::min(total - offset, chunk_size));
        if (n < 0) {
            if (errno == EINTR) continue;
            const int saved_err = errno;
            ::close(fd);
            buffer.clear();
            errno = saved_err;
            return buffer;
        }
        if (n == 0) {
            ::close(fd);
            buffer.clear();
            errno = 0;
            return buffer;
        }
        offset += static_cast<size_t>(n);
    }
    ::close(fd);
    if (stop.stop_requested()) { errno = ECANCELED; return {}; }
    return buffer;
}

struct ImageRead {
    std::mutex mutex;
    std::condition_variable_any ready;
    std::stop_source cancel;
    std::vector<uint8_t> buffer;
    int error = 0;
    bool finished = false;
};

} // namespace

std::vector<uint8_t> ImageLoader::read_file_to_buffer(const std::string& path) {
    return read_file_to_buffer(path, {});
}

std::vector<uint8_t> ImageLoader::read_file_to_buffer(const std::string& path, std::stop_token stop) {
    if (stop.stop_requested()) { errno = ECANCELED; return {}; }
    // A NAS call can remain in the kernel after cancellation. Keep each abandoned
    // worker's slot until that call returns so retries cannot exhaust the process.
    static std::atomic<unsigned> active{0};
    constexpr unsigned max_workers = 8;
    auto count = active.load();
    while (count < max_workers && !active.compare_exchange_weak(count, count + 1)) {}
    if (count >= max_workers) { errno = EAGAIN; return {}; }

    std::shared_ptr<ImageRead> task;
    try {
        task = std::make_shared<ImageRead>();
        std::thread([task, path] {
            errno = 0;
            auto buffer = read_image_file(path, task->cancel.get_token());
            const int error = buffer.empty() ? errno : 0;
            {
                std::lock_guard lock(task->mutex);
                task->buffer = std::move(buffer);
                task->error = error;
                task->finished = true;
            }
            active.fetch_sub(1);
            task->ready.notify_one();
        }).detach();
    } catch (const std::system_error&) {
        active.fetch_sub(1);
        errno = EAGAIN;
        return {};
    } catch (const std::bad_alloc&) {
        active.fetch_sub(1);
        errno = ENOMEM;
        return {};
    }

    std::unique_lock lock(task->mutex);
    const bool finished = task->ready.wait_for(lock, stop, std::chrono::seconds(5),
                                               [&] { return task->finished; });
    if (!finished || stop.stop_requested()) {
        task->cancel.request_stop();
        errno = stop.stop_requested() ? ECANCELED : ETIMEDOUT;
        return {};
    }
    errno = task->error;
    return std::move(task->buffer);
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



static void compute_average_color(const RawImage& src, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (!src.valid || !src.pixels || src.width <= 0 || src.height <= 0) {
        r = 220; g = 210; b = 195;
        return;
    }
    const int MAX_STEPS = 64;
    int step_x = std::max(1, src.width / MAX_STEPS);
    int step_y = std::max(1, src.height / MAX_STEPS);
    long long sum_r = 0, sum_g = 0, sum_b = 0, samples = 0;
    for (int y = 0; y < src.height; y += step_y) {
        for (int x = 0; x < src.width; x += step_x) {
            long long offset = (long long)y * 4 * src.width + (long long)x * 4;
            if (offset >= 0 && offset + 3 < (long long)src.width * src.height * 4) {
                const uint8_t* px = src.pixels + offset;
                sum_r += px[0]; sum_g += px[1]; sum_b += px[2];
                ++samples;
            }
        }
    }
    if (samples <= 0) {
        r = 220; g = 210; b = 195;
    } else {
        r = (uint8_t)(sum_r / samples);
        g = (uint8_t)(sum_g / samples);
        b = (uint8_t)(sum_b / samples);
    }
}

static void compute_edge_data(const RawImage& raw,
    uint8_t edge_r[4], uint8_t edge_g[4], uint8_t edge_b[4],
    std::vector<uint8_t>& edge_top_rgb, std::vector<uint8_t>& edge_bot_rgb,
    std::vector<uint8_t>& edge_lft_rgb, std::vector<uint8_t>& edge_rgt_rgb)
{
    if (!raw.valid || raw.width <= 0 || raw.height <= 0) return;
    int w = raw.width, h = raw.height;
    const uint8_t* px = raw.pixels;
    int stride = w * 4;
    const int STEPS = 32;

    // Edge average colors (sampled, matching get_edge_average_color)
    auto sample_edge = [&](int x0, int y0, int x1, int y1) -> GpuColor {
        int sx = std::max(1, (x1 - x0) / STEPS);
        int sy = std::max(1, (y1 - y0) / STEPS);
        long long r = 0, g = 0, b = 0, n = 0;
        for (int y = y0; y < y1; y += sy)
            for (int x = x0; x < x1; x += sx) {
                long long offset = (long long)y * stride + (long long)x * 4;
                if (offset >= 0) {
                    const uint8_t* p = px + offset;
                    r += p[0]; g += p[1]; b += p[2]; n++;
                }
            }
        if (n <= 0) return GpuColor{0,0,0,255};
        return GpuColor{(uint8_t)(r/n), (uint8_t)(g/n), (uint8_t)(b/n), 255};
    };
    int depth = 8;
    GpuColor ec = sample_edge(0, 0, w, std::min(depth, h));
    edge_r[0] = ec.r; edge_g[0] = ec.g; edge_b[0] = ec.b;
    ec = sample_edge(0, std::max(0, h - depth), w, h);
    edge_r[1] = ec.r; edge_g[1] = ec.g; edge_b[1] = ec.b;
    ec = sample_edge(0, 0, std::min(depth, w), h);
    edge_r[2] = ec.r; edge_g[2] = ec.g; edge_b[2] = ec.b;
    ec = sample_edge(std::max(0, w - depth), 0, w, h);
    edge_r[3] = ec.r; edge_g[3] = ec.g; edge_b[3] = ec.b;

    // Per-pixel edge strips (3px deep average per position)
    int ns = h < 3 ? h : 3;
    edge_top_rgb.resize(w * 3);
    for (int x = 0; x < w; x++) {
        int ar = 0, ag = 0, ab = 0;
        for (int d = 0; d < ns; d++) {
            const uint8_t* p = px + x * 4 + d * stride;
            ar += p[0]; ag += p[1]; ab += p[2];
        }
        edge_top_rgb[x * 3 + 0] = (uint8_t)(ns > 0 ? (ar / ns) : 0);
        edge_top_rgb[x * 3 + 1] = (uint8_t)(ns > 0 ? (ag / ns) : 0);
        edge_top_rgb[x * 3 + 2] = (uint8_t)(ns > 0 ? (ab / ns) : 0);
    }

    edge_bot_rgb.resize(w * 3);
    for (int x = 0; x < w; x++) {
        int ar = 0, ag = 0, ab = 0;
        for (int d = 0; d < ns; ++d) {
            const int ry = h - 1 - d;
            const uint8_t* p = px + x * 4 + ry * stride;
            ar += p[0]; ag += p[1]; ab += p[2];
        }
        edge_bot_rgb[x * 3 + 0] = static_cast<uint8_t>(ar / ns);
        edge_bot_rgb[x * 3 + 1] = static_cast<uint8_t>(ag / ns);
        edge_bot_rgb[x * 3 + 2] = static_cast<uint8_t>(ab / ns);
    }

    ns = w < 3 ? w : 3;
    edge_lft_rgb.resize(h * 3);
    for (int y = 0; y < h; y++) {
        int ar = 0, ag = 0, ab = 0;
        for (int ww = 0; ww < ns; ww++) {
            const uint8_t* p = px + y * stride + ww * 4;
            ar += p[0]; ag += p[1]; ab += p[2];
        }
        edge_lft_rgb[y * 3 + 0] = (uint8_t)(ns > 0 ? (ar / ns) : 0);
        edge_lft_rgb[y * 3 + 1] = (uint8_t)(ns > 0 ? (ag / ns) : 0);
        edge_lft_rgb[y * 3 + 2] = (uint8_t)(ns > 0 ? (ab / ns) : 0);
    }

    edge_rgt_rgb.resize(h * 3);
    for (int y = 0; y < h; y++) {
        int ar = 0, ag = 0, ab = 0;
        int sample_count = 0;
        for (int ww = 0; ww < ns; ww++) {
            int wc = w - 1 - ww;
            if (wc >= 0 && wc < w) {
                const uint8_t* p = px + y * stride + wc * 4;
                ar += p[0]; ag += p[1]; ab += p[2];
                sample_count++;
            }
        }
        edge_rgt_rgb[y * 3 + 0] = (uint8_t)(sample_count > 0 ? (ar / sample_count) : 0);
        edge_rgt_rgb[y * 3 + 1] = (uint8_t)(sample_count > 0 ? (ag / sample_count) : 0);
        edge_rgt_rgb[y * 3 + 2] = (uint8_t)(sample_count > 0 ? (ab / sample_count) : 0);
    }
}

PreparedImage ImageLoader::prepare(std::span<const uint8_t> buffer, std::stop_token stop) {
    int blur_radius;
    int max_dim;
    bool blurred;
    {
        std::shared_lock lock(g_config_mtx);
        blur_radius = g_renderer.scale_px(g_cfg.blur_radius);
        max_dim = std::max(1, g_cfg.max_texture_dim);
        blurred = g_cfg.blurred_background && g_cfg.bg_style == "photo";
    }
    if (stop.stop_requested()) return {};
    const auto peak = estimate_decode_peak(buffer, max_dim);
    if (!peak) return {};
    const DecodeReservation reservation(*peak, stop);
    if (!reservation) return {};

    auto raw = decode_rgba_unreserved(buffer, max_dim);
    if (!raw.valid || stop.stop_requested()) return {};
    const int w = raw.width, h = raw.height;

    const auto metadata = read_metadata_from_memory(buffer);

    // Work with display-sized, already-oriented pixels before sampling edges.
    if (std::max(w, h) > max_dim || (metadata.rotation >= 2 && metadata.rotation <= 8)) {
        using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
        Surface src(SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, raw.pixels, w * 4), SDL_DestroySurface);
        if (!src) { return {}; }
        if (std::max(w, h) > max_dim) {
            const float scale = static_cast<float>(max_dim) / std::max(w, h);
            auto* scaled = SDL_ScaleSurface(src.get(), std::max(1, static_cast<int>(w * scale)),
                                            std::max(1, static_cast<int>(h * scale)), SDL_SCALEMODE_LINEAR);
            if (!scaled) { return {}; }
            src.reset(scaled);
        }
        if (metadata.rotation >= 2 && metadata.rotation <= 8) {
            auto* rotated = ImageLoader::apply_exif_rotation(src.get(), metadata.rotation);
            if (!rotated) { return {}; }
            src.reset(rotated);
        }
        const size_t stride = static_cast<size_t>(src->w) * 4;
        auto* adjusted = static_cast<uint8_t*>(malloc(stride * src->h));
        if (!adjusted) { return {}; }
        auto* source = static_cast<const uint8_t*>(src->pixels);
        for (int row = 0; row < src->h; ++row) {
            memcpy(adjusted + row * stride, source + static_cast<size_t>(row) * src->pitch, stride);
        }
        free(raw.pixels);
        raw.pixels = adjusted;
        raw.width = src->w;
        raw.height = src->h;
    }

    PreparedImage prepared;
    auto& data = prepared.data;
    data.width = raw.width;
    data.height = raw.height;
    data.exif_rotation = metadata.rotation;
    data.is_camera = metadata.is_camera;
    data.creation_time = metadata.creation_time;
    data.latitude = metadata.latitude;
    data.longitude = metadata.longitude;
    data.has_gps = metadata.has_gps;
    if (blurred) data.blur_raw = box_blur(raw, blur_radius);
    compute_matte_color(raw, data.matte_r, data.matte_g, data.matte_b);
    compute_average_color(raw, data.avg_r, data.avg_g, data.avg_b);
    compute_edge_data(raw, data.edge_r, data.edge_g, data.edge_b,
        data.edge_top_rgb, data.edge_bot_rgb, data.edge_lft_rgb, data.edge_rgt_rgb);
    prepared.raw = std::move(raw);
    return prepared;
}

std::shared_ptr<ImageData> ImageLoader::make_surface(PreparedImage image, const std::string& path) {
    auto data = std::make_shared<ImageData>(std::move(image.data));
    data->filename = std::filesystem::path(path).filename().string();
    if (!image.raw.valid) return data;
    const size_t stride = static_cast<size_t>(data->width) * 4;
    const DecodeReservation reservation(stride * data->height, {});
    data->surface = SDL_CreateSurface(data->width, data->height, SDL_PIXELFORMAT_RGBA32);
    if (!data->surface) return data;
    auto* dst = static_cast<uint8_t*>(data->surface->pixels);
    for (int y = 0; y < data->height; ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * data->surface->pitch,
                    image.raw.pixels + static_cast<size_t>(y) * stride, stride);
    }
    data->valid = true;
    return data;
}

std::shared_ptr<ImageData> ImageLoader::load(const std::string& path) {
    errno = 0;
    std::vector<uint8_t> buffer = read_file_to_buffer(path);
    if (buffer.empty()) {
        int err = errno;
        auto result = make_surface({}, path);
        if (err != 0 && err != ENOENT && err != ELOOP && err != EINVAL && err != EFBIG) {
            result->transient_error = true;
            g_logger.warn("ImageLoader: Detected network/IO error (errno={}: {}) for path: {}", err, strerror(err), path.c_str());
            trigger_error(101); // E101: NAS_MOUNT_FAILED
        } else {
            trigger_error(201); // E201: IMAGE_LOAD_ERROR
        }
        return result;
    }

    auto result = make_surface(prepare(buffer), path);
    if (!result->valid) trigger_error(201);
    return result;
}

void ImageLoader::load_texture(ImageData* data, SDL_Renderer* renderer) {
    g_logger.info("[TRACE] ImageLoader::load_texture data={} surface={} renderer={}", (void*)data, data ? (void*)data->surface : nullptr, (void*)renderer);
    if (!data || !data->surface || !renderer || data->texture || data->width <= 0 || data->height <= 0) return;

    int max_dim = 1920;
    { std::shared_lock lk(g_config_mtx); max_dim = g_cfg.max_texture_dim; }
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
            const size_t bstride = static_cast<size_t>(data->blur_raw.width) * 4;
            if (bsurf->pixels && bsurf->pitch >= 0 && static_cast<size_t>(bsurf->pitch) >= bstride && SDL_LockSurface(bsurf)) {
                auto* dst_px = static_cast<uint8_t*>(bsurf->pixels);
                for (int y = 0; y < data->blur_raw.height; ++y) {
                    memcpy(dst_px + static_cast<size_t>(y) * bsurf->pitch,
                           data->blur_raw.pixels + static_cast<size_t>(y) * bstride, bstride);
                }
                SDL_UnlockSurface(bsurf);
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
                const int bpp = SDL_BYTESPERPIXEL(surface->format);
                const size_t row_bytes = std::min<size_t>(
                    static_cast<size_t>(surface->w) * static_cast<size_t>(bpp),
                    std::min<size_t>(static_cast<size_t>(surface->pitch), static_cast<size_t>(dst->pitch)));
                uint8_t* src_px = (uint8_t*)surface->pixels;
                uint8_t* dst_px = (uint8_t*)dst->pixels;
                for (int y = 0; y < surface->h; y++) {
                    memcpy(dst_px + static_cast<size_t>(y) * dst->pitch,
                           src_px + static_cast<size_t>(surface->h - 1 - y) * surface->pitch, row_bytes);
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

    // 4. GPS Geotagging coordinates (latitude and longitude)
    if (ed->ifd[EXIF_IFD_GPS]) {
        ExifEntry* lat_entry = exif_content_get_entry(ed->ifd[EXIF_IFD_GPS], static_cast<ExifTag>(EXIF_TAG_GPS_LATITUDE));
        ExifEntry* lat_ref = exif_content_get_entry(ed->ifd[EXIF_IFD_GPS], static_cast<ExifTag>(EXIF_TAG_GPS_LATITUDE_REF));
        ExifEntry* lon_entry = exif_content_get_entry(ed->ifd[EXIF_IFD_GPS], static_cast<ExifTag>(EXIF_TAG_GPS_LONGITUDE));
        ExifEntry* lon_ref = exif_content_get_entry(ed->ifd[EXIF_IFD_GPS], static_cast<ExifTag>(EXIF_TAG_GPS_LONGITUDE_REF));

        if (lat_entry && lon_entry && lat_entry->data && lon_entry->data &&
            lat_entry->format == EXIF_FORMAT_RATIONAL && lon_entry->format == EXIF_FORMAT_RATIONAL &&
            lat_entry->components == 3 && lon_entry->components == 3) {
            
            ExifByteOrder bo = exif_data_get_byte_order(ed);
            unsigned int rat_size = exif_format_get_size(EXIF_FORMAT_RATIONAL);

            auto parse_coord = [&](ExifEntry* entry) -> double {
                ExifRational r0 = exif_get_rational(entry->data, bo);
                ExifRational r1 = exif_get_rational(entry->data + rat_size, bo);
                ExifRational r2 = exif_get_rational(entry->data + rat_size * 2, bo);

                double d = (r0.denominator != 0) ? ((double)r0.numerator / r0.denominator) : 0.0;
                double m = (r1.denominator != 0) ? ((double)r1.numerator / r1.denominator) : 0.0;
                double s = (r2.denominator != 0) ? ((double)r2.numerator / r2.denominator) : 0.0;

                return d + (m / 60.0) + (s / 3600.0);
            };

            double lat = parse_coord(lat_entry);
            double lon = parse_coord(lon_entry);

            if (lat_ref && lat_ref->data && lat_ref->size > 0) {
                char ref_c = static_cast<char>(lat_ref->data[0]);
                if (ref_c == 'S' || ref_c == 's') lat = -lat;
            }

            if (lon_ref && lon_ref->data && lon_ref->size > 0) {
                char ref_c = static_cast<char>(lon_ref->data[0]);
                if (ref_c == 'W' || ref_c == 'w') lon = -lon;
            }

            if (lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0 && (lat != 0.0 || lon != 0.0)) {
                meta.latitude = lat;
                meta.longitude = lon;
                meta.has_gps = true;
            }
        }
    }

    exif_data_unref(ed);
    return meta;
}

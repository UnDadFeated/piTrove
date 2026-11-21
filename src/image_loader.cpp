#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "image_loader.h"
#include <webp/decode.h>
#include <libheif/heif.h>
#include <lcms2.h>
#include <tiffio.h>
#include <zlib.h>
#include <span>
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

static bool is_webp(std::span<const uint8_t> bytes) {
    return bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0 &&
           std::memcmp(bytes.data() + 8, "WEBP", 4) == 0;
}

namespace {

constexpr size_t max_icc_size = 4 * 1024 * 1024;

bool usable_dimensions(int w, int h) {
    constexpr size_t max_pixels = 64ULL * 1024 * 1024;
    return w > 0 && h > 0 && w <= 16384 && h <= 16384 &&
           static_cast<size_t>(w) <= max_pixels / static_cast<size_t>(h);
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
    int channels = 0;
    const bool known = is_webp(buffer) ? WebPGetInfo(buffer.data(), buffer.size(), &width, &height) :
        stbi_info_from_memory(buffer.data(), static_cast<int>(buffer.size()), &width, &height, &channels);
    return known && usable_dimensions(width, height);
}

RawImage ImageLoader::decode_rgba(std::span<const uint8_t> buffer) {
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
    } else {
        if (!image_dimensions(buffer, result.width, result.height)) return {};
        if (is_webp(buffer)) {
            const size_t stride = static_cast<size_t>(result.width) * 4;
            const size_t size = stride * result.height;
            result.pixels = static_cast<uint8_t*>(std::malloc(size));
            if (!result.pixels || !WebPDecodeRGBAInto(buffer.data(), buffer.size(), result.pixels, size,
                                                     static_cast<int>(stride))) return {};
        } else {
            int channels = 0;
            result.pixels = stbi_load_from_memory(buffer.data(), static_cast<int>(buffer.size()),
                                                  &result.width, &result.height, &channels, 4);
        }
        if (result.pixels) convert_embedded_profile(result, buffer);
    }
    result.channels = 4;
    result.format = ImageFormat::RGBA32;
    result.valid = result.pixels != nullptr;
    return result;
}

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
    constexpr size_t max_img_size = 128ULL * 1024 * 1024;
    if (static_cast<size_t>(size) > max_img_size) {
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

    RawImage raw = decode_rgba(buffer);
    if (!raw.valid) {
        trigger_error(201);
        return result;
    }

    auto* surf = SDL_CreateSurface(raw.width, raw.height, SDL_PIXELFORMAT_RGBA32);
    if (!surf) {
        trigger_error(201);
        return result;
    }
    const size_t stride = static_cast<size_t>(raw.width) * 4;
    auto* dst = static_cast<uint8_t*>(surf->pixels);
    for (int y = 0; y < raw.height; ++y) {
        memcpy(dst + static_cast<size_t>(y) * surf->pitch, raw.pixels + y * stride, stride);
    }
    raw = RawImage{};

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
    result->latitude = meta.latitude;
    result->longitude = meta.longitude;
    result->has_gps = meta.has_gps;
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

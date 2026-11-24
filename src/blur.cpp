#include "blur.h"
#include <algorithm>
#include <cmath>
#include <span>

// 3-pass separable box blur operating on raw RGBA pixels
// Pass 1: horizontal, Pass 2: vertical, Pass 3: horizontal
static void box_blur_h(const uint8_t* src, uint8_t* dst, int width, int height, int radius) {
    const size_t stride = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height; ++y) {
        const uint8_t* srow = src + static_cast<size_t>(y) * stride;
        uint8_t* drow = dst + static_cast<size_t>(y) * stride;
        int sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0, count = 0;
        const int init_end = std::min(width - 1, radius);
        for (int sx = 0; sx <= init_end; ++sx) {
            const uint8_t* p = srow + sx * 4;
            sum_r += p[0]; sum_g += p[1]; sum_b += p[2]; sum_a += p[3];
            ++count;
        }
        for (int x = 0; x < width; ++x) {
            uint8_t* d = drow + x * 4;
            d[0] = static_cast<uint8_t>(sum_r / count);
            d[1] = static_cast<uint8_t>(sum_g / count);
            d[2] = static_cast<uint8_t>(sum_b / count);
            d[3] = static_cast<uint8_t>(sum_a / count);
            const int add_x = x + radius + 1;
            if (add_x < width) {
                const uint8_t* p = srow + add_x * 4;
                sum_r += p[0]; sum_g += p[1]; sum_b += p[2]; sum_a += p[3];
                ++count;
            }
            const int rem_x = x - radius;
            if (rem_x >= 0) {
                const uint8_t* p = srow + rem_x * 4;
                sum_r -= p[0]; sum_g -= p[1]; sum_b -= p[2]; sum_a -= p[3];
                --count;
            }
        }
    }
}

static void box_blur_v(const uint8_t* src, uint8_t* dst, int width, int height, int radius) {
    const size_t stride = static_cast<size_t>(width) * 4;
    for (int x = 0; x < width; ++x) {
        const size_t col_off = static_cast<size_t>(x) * 4;
        int sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0, count = 0;
        const int init_end = std::min(height - 1, radius);
        for (int sy = 0; sy <= init_end; ++sy) {
            const uint8_t* p = src + static_cast<size_t>(sy) * stride + col_off;
            sum_r += p[0]; sum_g += p[1]; sum_b += p[2]; sum_a += p[3];
            ++count;
        }
        for (int y = 0; y < height; ++y) {
            uint8_t* d = dst + static_cast<size_t>(y) * stride + col_off;
            d[0] = static_cast<uint8_t>(sum_r / count);
            d[1] = static_cast<uint8_t>(sum_g / count);
            d[2] = static_cast<uint8_t>(sum_b / count);
            d[3] = static_cast<uint8_t>(sum_a / count);
            const int add_y = y + radius + 1;
            if (add_y < height) {
                const uint8_t* p = src + static_cast<size_t>(add_y) * stride + col_off;
                sum_r += p[0]; sum_g += p[1]; sum_b += p[2]; sum_a += p[3];
                ++count;
            }
            const int rem_y = y - radius;
            if (rem_y >= 0) {
                const uint8_t* p = src + static_cast<size_t>(rem_y) * stride + col_off;
                sum_r -= p[0]; sum_g -= p[1]; sum_b -= p[2]; sum_a -= p[3];
                --count;
            }
        }
    }
}

static void separable_box_blur(std::span<uint8_t> pixels, int width, int height, int radius) {
    if (width <= 0 || height <= 0 || radius <= 0) return;
    size_t buf_size = static_cast<size_t>(width) * height * 4;
    uint8_t* tmp = static_cast<uint8_t*>(malloc(buf_size));
    if (!tmp) return;

    box_blur_h(pixels.data(), tmp, width, height, radius);
    box_blur_v(tmp, pixels.data(), width, height, radius);
    box_blur_h(pixels.data(), tmp, width, height, radius);
    memcpy(pixels.data(), tmp, buf_size);

    free(tmp);
}

// Downsample image to max_dim using nearest-neighbor, writes to new allocation
static uint8_t* downsample_image(std::span<const uint8_t> src_pixels, int src_w, int src_h,
                                  int& dst_w, int& dst_h, int max_dim) {
    if (src_w <= max_dim && src_h <= max_dim) {
        dst_w = src_w;
        dst_h = src_h;
        return nullptr;  // no downsample needed
    }

    float scale = (float)max_dim / (float)std::max(src_w, src_h);
    dst_w = std::max(1, (int)(src_w * scale));
    dst_h = std::max(1, (int)(src_h * scale));

    size_t buf_size = (size_t)dst_w * dst_h * 4;
    uint8_t* dst = (uint8_t*)malloc(buf_size);
    if (!dst) { dst_w = src_w; dst_h = src_h; return nullptr; }

    for (int y = 0; y < dst_h; y++) {
        for (int x = 0; x < dst_w; x++) {
            int sy = (int)((float)y / scale);
            int sx = (int)((float)x / scale);
            if (sx < 0) sx = 0;
            if (sx >= src_w) sx = src_w - 1;
            if (sy < 0) sy = 0;
            if (sy >= src_h) sy = src_h - 1;
            const uint8_t* src_px = src_pixels.data() + sy * src_w * 4 + sx * 4;
            uint8_t* dst_px = dst + y * dst_w * 4 + x * 4;
            for (int c = 0; c < 4; c++) dst_px[c] = src_px[c];
        }
    }
    return dst;
}

RawImage box_blur(const RawImage& src, int radius) {
    RawImage out;
    out.valid = false;

    if (!src.valid || src.width <= 0 || src.height <= 0) return out;
    // Dynamically scale downsample dimension with source image resolution (360px - 960px)
    int max_dim = std::clamp(std::max(src.width, src.height) / 4, 360, 960);
    // Scale radius down proportionally to match downsampled resolution
    if (src.width > max_dim || src.height > max_dim) {
        float scale = (float)max_dim / (float)std::max(src.width, src.height);
        radius = std::max(1, (int)std::round(radius * scale));
    }
    radius = std::max(1, std::min(radius, 24));

    int dw = src.width, dh = src.height;
    uint8_t* work = downsample_image(std::span<const uint8_t>(src.pixels, (size_t)src.width * src.height * 4), src.width, src.height, dw, dh, max_dim);
    if (!work) {
        // No downsample needed, but we MUST copy the pixels so we don't modify/free the original sharp pixels!
        size_t buf_size = (size_t)src.width * src.height * 4;
        work = (uint8_t*)malloc(buf_size);
        if (!work) return out;
        memcpy(work, src.pixels, buf_size);
        dw = src.width;
        dh = src.height;
    }

    out.width = dw;
    out.height = dh;
    out.channels = 4;
    out.format = src.format;
    out.valid = true;
    out.pixels = work;

    // Run blur in-place on the working buffer
    separable_box_blur(std::span<uint8_t>(out.pixels, (size_t)dw * dh * 4), dw, dh, radius);

    return out;
}

void compute_matte_color(const RawImage& src, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (!src.valid || src.width <= 0 || src.height <= 0) {
        r = 0; g = 0; b = 0;
        return;
    }

    int x0 = src.width / 4, y0 = src.height / 4;
    int x1 = src.width * 3 / 4, y1 = src.height * 3 / 4;

    long sum_r = 0, sum_g = 0, sum_b = 0, count = 0;
    int step = 16;
    for (int y = y0; y < y1; y += step) {
        for (int x = x0; x < x1; x += step) {
            const uint8_t* px = src.pixels + y * 4 * src.width + x * 4;
            sum_r += px[0]; sum_g += px[1]; sum_b += px[2];
            count++;
        }
    }

    if (count == 0) {
        r = 0; g = 0; b = 0;
        return;
    }
    r = (uint8_t)(sum_r / count);
    g = (uint8_t)(sum_g / count);
    b = (uint8_t)(sum_b / count);
}

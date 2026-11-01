#include "nv12_converter.h"
#include <cstring>
#include <cstdlib>
extern "C" {
#include <libavutil/imgutils.h>
#include <libavcodec/defs.h>
}

NV12Converter::~NV12Converter() { sws_freeContext(scaler); }

bool NV12Converter::convert(const AVFrame* source, VideoFrame& output, int target_w, int target_h) {
    if (!source || av_image_check_size(source->width, source->height, 0, nullptr) < 0) return false;
    bool downscale = target_w > 0 && target_h > 0 &&
                     (source->width > target_w || source->height > target_h);
    const int width = downscale ? target_w : source->width;
    const int height = downscale ? target_h : source->height;
    if (av_image_check_size(width, height, 0, nullptr) < 0) return false;
    int strides[4] = {};
    if (av_image_fill_linesizes(strides, AV_PIX_FMT_NV12, width) < 0) return false;
    int total = av_image_get_buffer_size(AV_PIX_FMT_NV12, width, height, 1);
    if (total < 0) return false;
    const int chroma_height = (height + 1) / 2;
    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.format = source->format;
    frame.is_nv12 = true;
    frame.linesize_y = strides[0];
    frame.linesize_uv = strides[1];
    frame.data_size = total;
    // Padding accommodates SIMD readers/writers used by FFmpeg.
    frame.data = new uint8_t[size_t(strides[0]) * height + AV_INPUT_BUFFER_PADDING_SIZE]();
    frame.data_uv = new uint8_t[size_t(strides[1]) * chroma_height + AV_INPUT_BUFFER_PADDING_SIZE]();
    auto copy_plane = [](uint8_t* dst, int stride, const uint8_t* src, int source_stride, int rows) {
        if (!src || std::abs(source_stride) < stride) return false;
        for (int y = 0; y < rows; ++y)
            std::memcpy(dst + size_t(y) * stride, src + ptrdiff_t(y) * source_stride, stride);
        return true;
    };
    if (!downscale && source->format == AV_PIX_FMT_NV12) {
        if (!copy_plane(frame.data, strides[0], source->data[0], source->linesize[0], height) ||
            !copy_plane(frame.data_uv, strides[1], source->data[1], source->linesize[1], chroma_height)) return false;
    } else if (!downscale && (source->format == AV_PIX_FMT_YUV420P || source->format == AV_PIX_FMT_YUVJ420P)) {
        const int chroma_width = (width + 1) / 2;
        if (!copy_plane(frame.data, strides[0], source->data[0], source->linesize[0], height) ||
            !source->data[1] || !source->data[2] ||
            std::abs(source->linesize[1]) < chroma_width || std::abs(source->linesize[2]) < chroma_width) return false;
        for (int y = 0; y < chroma_height; ++y) {
            const uint8_t* u = source->data[1] + ptrdiff_t(y) * source->linesize[1];
            const uint8_t* v = source->data[2] + ptrdiff_t(y) * source->linesize[2];
            uint8_t* dst = frame.data_uv + size_t(y) * strides[1];
            for (int x = 0; x < chroma_width; ++x) {
                dst[x * 2] = u[x];
                dst[x * 2 + 1] = v[x];
            }
        }
    } else {
        scaler = sws_getCachedContext(scaler, source->width, source->height,
                                     static_cast<AVPixelFormat>(source->format), width, height,
                                     AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) return false;
        // libswscale's plane arrays have four entries, even for two-plane NV12.
        uint8_t* planes[4] = {frame.data, frame.data_uv, nullptr, nullptr};
        if (sws_scale(scaler, source->data, source->linesize, 0, source->height, planes, strides) != height)
            return false;
    }
    output = std::move(frame);
    return true;
}

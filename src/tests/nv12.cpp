#include "nv12_converter.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
extern "C" {
#include <libavutil/imgutils.h>
}
static void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void test_format(AVPixelFormat format, int width, int height) {
    AVFrame* source = av_frame_alloc();
    check(source, "frame allocation failed");
    source->format = format;
    source->width = width;
    source->height = height;
    check(av_frame_get_buffer(source, 32) == 0, "source buffer allocation failed");
    const int chroma_height = (height + 1) / 2;
    std::memset(source->data[0], 90, source->linesize[0] * height);
    if (format == AV_PIX_FMT_NV12) {
        for (int y = 0; y < chroma_height; ++y)
            for (int x = 0; x < (width + 1) / 2; ++x) {
                source->data[1][y * source->linesize[1] + x * 2] = 100;
                source->data[1][y * source->linesize[1] + x * 2 + 1] = 150;
            }
    } else if (format == AV_PIX_FMT_YUV420P) {
        std::memset(source->data[1], 100, source->linesize[1] * chroma_height);
        std::memset(source->data[2], 150, source->linesize[2] * chroma_height);
    }
    NV12Converter converter;
    VideoFrame output;
    check(converter.convert(source, output, 1920, 1080), "native conversion failed");
    check(output.width == width && output.height == height, "smaller video was upscaled");
    check(output.linesize_uv >= 2 * ((width + 1) / 2), "odd-width chroma stride too short");
    if (format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_YUV420P) {
        // Read the last chroma pair: formerly missing for odd heights and widths.
        const int last = (chroma_height - 1) * output.linesize_uv + ((width + 1) / 2 - 1) * 2;
        check(output.data_uv[last] == 100 && output.data_uv[last + 1] == 150, "last chroma pair lost");
        source->data[0] += (height - 1) * source->linesize[0];
        source->linesize[0] *= -1;
        check(converter.convert(source, output, 1920, 1080), "negative-stride conversion failed");
    }
    check(converter.convert(source, output, 31, 17), "odd-sized scaling failed");
    check(output.width == 31 && output.height == 17, "scaled output dimensions wrong");
    check(converter.convert(source, output, 27, 15), "cached scaler failed after target resize");
    av_frame_free(&source);
}
int main() {
    try {
        for (auto format : {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, AV_PIX_FMT_RGB24}) {
            test_format(format, 65, 37);
            test_format(format, 64, 38);
        }
        NV12Converter converter;
        VideoFrame output;
        check(!converter.convert(nullptr, output, 10, 10), "null frame accepted");
        std::cout << "NV12 odd dimensions, strides, pixel formats, and scaling passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

#pragma once
#include "video_decoder.h"
extern "C" {
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

// Decode-thread-local converter. Output owns tightly packed NV12 planes, including
// the final chroma row/column for odd dimensions. Never upscales smaller sources.
class NV12Converter {
    SwsContext* scaler = nullptr;
public:
    NV12Converter() = default;
    ~NV12Converter();
    NV12Converter(const NV12Converter&) = delete;
    NV12Converter& operator=(const NV12Converter&) = delete;
    bool convert(const AVFrame* source, VideoFrame& output, int target_w, int target_h);
};

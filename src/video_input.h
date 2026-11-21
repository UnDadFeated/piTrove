#ifndef PITROVE_VIDEO_INPUT_H
#define PITROVE_VIDEO_INPUT_H

extern "C" {
#include <libavformat/avformat.h>
}

inline int open_local_video_input(AVFormatContext** context, const char* path) {
    AVDictionary* options = nullptr;
    int result = av_dict_set(&options, "protocol_whitelist", "file", 0);
    // MOV includes MP4 and Matroska includes WebM. NUT is used by playback tests.
    if (result >= 0) result = av_dict_set(&options, "format_whitelist", "mov,matroska,avi,nut", 0);
    if (result >= 0) result = avformat_open_input(context, path, nullptr, &options);
    av_dict_free(&options);
    return result;
}

#endif // PITROVE_VIDEO_INPUT_H

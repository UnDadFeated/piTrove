#ifndef PITROVE_MEDIA_ITEM_H
#define PITROVE_MEDIA_ITEM_H

#include <string>
#include <cstdint>

struct MediaItem {
    std::string path;
    std::string filename;
    std::string ext;
    std::string type{"image"}; // "image" or "video"
    int        width{0};
    int        height{0};
    double      duration{0.0};
    double      framerate{0.0};
    int         exif_rotation{0};
    int64_t     file_size{0};
    int64_t     modified_time{0};
    bool        cached{false};
    int64_t     last_shown{0};
    mutable int is_camera{-1}; // -1 = unknown, 0 = no camera EXIF (screenshot), 1 = has camera EXIF
    mutable int64_t creation_time{0};
    mutable double latitude{0.0};
    mutable double longitude{0.0};
    mutable bool has_gps{false};
};

#endif // PITROVE_MEDIA_ITEM_H

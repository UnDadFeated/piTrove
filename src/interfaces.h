#pragma once

#include <string>
#include <vector>
#include "expected.h"
#include "media_item.h"

namespace pitrove {

struct Error {
    std::string code;
    std::string message;
};

struct ScanOptions {
    std::string root;
    bool include_video = true;
};

class IMediaScanner {
public:
    virtual ~IMediaScanner() = default;
    virtual Expected<std::vector<MediaItem>, Error> scan(const ScanOptions& options) = 0;
};

class IMetadataCache {
public:
    virtual ~IMetadataCache() = default;
    virtual Expected<bool, Error> is_corrupt(const std::string& path) = 0;
};

class IRemoteControl {
public:
    virtual ~IRemoteControl() = default;
    virtual Expected<bool, Error> publish_state(const std::string& topic,
                                                const std::string& payload) = 0;
};

} // namespace pitrove

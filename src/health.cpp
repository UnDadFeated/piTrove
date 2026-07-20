#include "health.h"
#include <chrono>
#include <filesystem>
#include <fstream>

namespace pitrove { namespace health {

static const std::filesystem::path heartbeat_path =
    "/app/cache/run/heartbeat";

void heartbeat_tick() {
    std::error_code ec;
    std::filesystem::create_directories(heartbeat_path.parent_path(), ec);

    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::filesystem::path tmp = heartbeat_path;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::trunc);
        out << now;
    }

    std::filesystem::rename(tmp, heartbeat_path, ec);
}

}} // namespace pitrove::health

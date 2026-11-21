#include "health.h"
#include <ctime>
#include <filesystem>
#include <fstream>

namespace pitrove { namespace health {

static const std::filesystem::path heartbeat_path =
    "/app/cache/run/heartbeat";

void heartbeat_tick() {
    std::error_code ec;
    std::filesystem::create_directories(heartbeat_path.parent_path(), ec);

    // /proc/uptime uses the same boot clock and is independent of NTP.
    timespec now{};
    if (clock_gettime(CLOCK_BOOTTIME, &now) != 0) return;

    std::filesystem::path tmp = heartbeat_path;
    tmp += ".tmp";

    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!(out << now.tv_sec) || !(out.flush())) return;
    }

    std::filesystem::rename(tmp, heartbeat_path, ec);
}

}} // namespace pitrove::health

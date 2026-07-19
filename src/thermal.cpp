#include "thermal.h"
#include <fstream>
#include <thread>
#include <chrono>

namespace pitrove { namespace thermal {

static std::atomic<Quality> current_quality{Quality::Normal};

int read_cpu_temp_c() {
    std::ifstream in("/sys/class/thermal/thermal_zone0/temp");
    int millideg = 0;
    if (in >> millideg) {
        return millideg / 1000;
    }
    return 0;
}

Quality get_quality() {
    return current_quality.load();
}

void monitor_thread(std::atomic<bool>& running) {
    while (running.load()) {
        const int temp = read_cpu_temp_c();

        if (temp >= 85) {
            current_quality.store(Quality::Minimal);
        } else if (temp >= 75) {
            current_quality.store(Quality::Reduced);
        } else {
            current_quality.store(Quality::Normal);
        }

        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

}} // namespace pitrove::thermal

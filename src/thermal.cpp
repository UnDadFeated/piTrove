#include "thermal.h"
#include "config.h"
#include "util.h"
#include <atomic>
#include <condition_variable>
#include <fstream>
#include <thread>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <shared_mutex>

extern Config g_cfg;
extern std::shared_mutex g_config_mtx;

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

void set_fan_speed_percent(int percent) {
    static std::mutex mutex;
    static bool unavailable = false;
    std::lock_guard lock(mutex);
    if (unavailable) return;
    percent = std::clamp(percent, 0, 100);

    // Locate hwmon device for pwm fan
    std::string pwm_path;
    std::string enable_path;
    std::error_code path_error;

    for (int i = 0; i < 8; ++i) {
        std::string p = "/sys/class/hwmon/hwmon" + std::to_string(i) + "/pwm1";
        std::string e = "/sys/class/hwmon/hwmon" + std::to_string(i) + "/pwm1_enable";
        std::ifstream name("/sys/class/hwmon/hwmon" + std::to_string(i) + "/name");
        std::string driver;
        if ((name >> driver) && (driver == "pwmfan" || driver == "pwm-fan") && std::filesystem::exists(p, path_error)) {
            pwm_path = p;
            enable_path = e;
            break;
        }
    }

    if (pwm_path.empty()) {
        return;
    }

    auto write_pwm = [&](const std::string& path, int value) {
        std::ofstream out(path);
        if (out && (out << value << '\n' << std::flush)) return true;
        unavailable = true;
        g_logger.warn("Thermal fan control unavailable: could not write {}. Leaving fan control to the host.", path);
        return false;
    };

    if (std::filesystem::exists(enable_path, path_error) && !write_pwm(enable_path, percent == 0 ? 2 : 1)) return;
    if (percent > 0) {
        write_pwm(pwm_path, static_cast<int>(std::lround(percent * 255.0 / 100.0)));
    }
}

void monitor_thread(std::stop_token stop) {
    std::mutex mutex;
    std::condition_variable_any cv;
    std::unique_lock lock(mutex);
    while (!stop.stop_requested()) {
        const int temp = read_cpu_temp_c();

        if (temp >= 85) {
            current_quality.store(Quality::Minimal);
        } else if (temp >= 75) {
            current_quality.store(Quality::Reduced);
        } else {
            current_quality.store(Quality::Normal);
        }

        // Emergency thermal safety override: if temp >= 75C, force 100% fan speed
        if (temp >= 75) {
            set_fan_speed_percent(100);
        } else {
            int desired_speed = 30;
            {
                std::shared_lock lk(g_config_mtx);
                desired_speed = g_cfg.fan_speed;
            }
            set_fan_speed_percent(desired_speed);
        }

        cv.wait_for(lock, stop, std::chrono::seconds(5), [] { return false; });
    }
}

}} // namespace pitrove::thermal

#include "thermal.h"
#include "config.h"
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
    percent = std::clamp(percent, 0, 100);

    // Locate hwmon device for pwm fan
    std::string pwm_path = "";
    std::string enable_path = "";

    for (int i = 0; i < 8; ++i) {
        std::string p = "/sys/class/hwmon/hwmon" + std::to_string(i) + "/pwm1";
        std::string e = "/sys/class/hwmon/hwmon" + std::to_string(i) + "/pwm1_enable";
        if (std::filesystem::exists(p)) {
            pwm_path = p;
            enable_path = e;
            break;
        }
    }

    if (pwm_path.empty()) {
        // Fallback: check cooling_device0
        std::string cd = "/sys/class/thermal/cooling_device0/cur_state";
        if (std::filesystem::exists(cd)) {
            std::ofstream out(cd);
            int state = (percent == 0) ? 0 : std::clamp((int)round((float)percent / 25.0f), 1, 4);
            out << state << "\n";
        }
        return;
    }

    if (percent == 0) {
        // Kernel automatic thermal control
        if (std::filesystem::exists(enable_path)) {
            std::ofstream out(enable_path);
            out << "2\n";
        }
    } else {
        // Manual PWM speed control
        if (std::filesystem::exists(enable_path)) {
            std::ofstream out(enable_path);
            out << "1\n";
        }
        int pwm_val = (int)round((float)percent * 2.55f);
        pwm_val = std::clamp(pwm_val, 0, 255);
        std::ofstream out(pwm_path);
        out << pwm_val << "\n";
    }
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

        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

}} // namespace pitrove::thermal

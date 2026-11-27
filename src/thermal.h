#ifndef PITROVE_THERMAL_H
#define PITROVE_THERMAL_H

#include <stop_token>

namespace pitrove { namespace thermal {

enum class Quality {
    Normal,
    Reduced,
    Minimal
};

Quality get_quality();
void monitor_thread(std::stop_token stop);
int read_cpu_temp_c();
void set_fan_speed_percent(int percent);

}} // namespace pitrove::thermal

#endif // PITROVE_THERMAL_H

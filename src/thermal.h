#ifndef PITROVE_THERMAL_H
#define PITROVE_THERMAL_H

#include <atomic>

namespace pitrove { namespace thermal {

enum class Quality {
    Normal,
    Reduced,
    Minimal
};

Quality get_quality();
void monitor_thread(std::atomic<bool>& running);
int read_cpu_temp_c();

}} // namespace pitrove::thermal

#endif // PITROVE_THERMAL_H

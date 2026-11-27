#include "util.h"
#include <fcntl.h>
#include <memory>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace {

std::mutex display_mutex;
std::atomic<bool> hardware_blanked{false};
int display_fd = -1;
std::string display_connector;

bool set_drm_power(bool on) {
    if (display_fd < 0) return false;
    const std::unique_ptr<drmModeRes, decltype(&drmModeFreeResources)>
        resources(drmModeGetResources(display_fd), drmModeFreeResources);
    if (!resources) return false;
    for (int index = 0; index < resources->count_connectors; ++index) {
        const std::unique_ptr<drmModeConnector, decltype(&drmModeFreeConnector)>
            connector(drmModeGetConnectorCurrent(display_fd, resources->connectors[index]), drmModeFreeConnector);
        if (!connector || connector->connection != DRM_MODE_CONNECTED) continue;
        const char* type = drmModeGetConnectorTypeName(connector->connector_type);
        if (!type || (!display_connector.empty() && display_connector != "auto" &&
                      display_connector != std::format("{}-{}", type, connector->connector_type_id))) continue;
        for (int property = 0; property < connector->count_props; ++property) {
            const std::unique_ptr<drmModePropertyRes, decltype(&drmModeFreeProperty)>
                info(drmModeGetProperty(display_fd, connector->props[property]), drmModeFreeProperty);
            if (info && std::string_view(info->name) == "DPMS") {
                return drmModeConnectorSetProperty(display_fd, connector->connector_id, info->prop_id,
                    on ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF) == 0;
            }
        }
    }
    return false;
}

bool apply_power(bool on) {
    const bool request_changed = g_screen_blanked.load() != !on;
    if (!request_changed && (!on || !hardware_blanked.load())) return false;
    bool hardware_changed = set_drm_power(on);
    if (!hardware_changed) {
        const int fd = open("/sys/class/graphics/fb0/blank", O_WRONLY | O_CLOEXEC);
        if (fd >= 0) {
            hardware_changed = write(fd, on ? "0" : "1", 1) == 1;
            close(fd);
        } else {
            g_logger.debug("Display power control unavailable; using a black frame when asleep.");
        }
    }
    if (hardware_changed) hardware_blanked.store(!on);
    g_screen_blanked.store(!on);
    return request_changed || hardware_changed;
}

} // namespace

void set_display_drm_fd(int fd, std::string connector) {
    std::lock_guard lock(display_mutex);
    display_fd = fd;
    display_connector = std::move(connector);
}

bool set_display_power(bool on) {
    std::lock_guard lock(display_mutex);
    return apply_power(on);
}

bool toggle_display_power() {
    std::lock_guard lock(display_mutex);
    return apply_power(g_screen_blanked.load());
}

bool display_hardware_blanked() { return hardware_blanked.load(); }

#include "safe_mode.h"

#include <csignal>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace pitrove::safe_mode {

static volatile std::sig_atomic_t crash_fd = -1;

void init() {
    const int fd = open("/app/cache/crash-state", O_CREAT | O_RDWR | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return;
    struct stat info{};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        close(fd);
        return;
    }
    if (info.st_size > 4096 || (info.st_size && time(nullptr) - info.st_mtime > 300))
        (void)ftruncate(fd, 0);
    crash_fd = fd;
}

void record_crash() noexcept {
    const int fd = crash_fd;
    if (fd < 0) return;
    const time_t now = time(nullptr);
    (void)write(fd, &now, sizeof(now));
}

bool should_enter_safe_mode() {
    const int fd = crash_fd;
    if (fd < 0) return false;
    const time_t now = time(nullptr);
    unsigned crashes = 0;
    time_t recorded = 0;
    for (off_t offset = 0; offset < 4096; offset += sizeof(recorded)) {
        if (pread(fd, &recorded, sizeof(recorded), offset) != sizeof(recorded)) break;
        if (recorded <= now && now - recorded <= 300 && ++crashes >= 3) return true;
    }
    return false;
}

void clear() {
    const int fd = crash_fd;
    if (fd >= 0) (void)ftruncate(fd, 0);
}

} // namespace pitrove::safe_mode

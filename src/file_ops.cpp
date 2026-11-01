#include "file_ops.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <cerrno>
#include <cstdio>
#include <vector>

bool move_without_replace(const std::filesystem::path& source,
                          const std::filesystem::path& destination, std::error_code& error) {
    if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) == 0) {
        error.clear();
        return true;
    }
    int saved = errno;
    // A hard-link + unlink fallback preserves no-replace semantics for regular files.
    if (saved == ENOSYS || saved == EINVAL || saved == EOPNOTSUPP) {
        if (link(source.c_str(), destination.c_str()) == 0) {
            if (unlink(source.c_str()) == 0) { error.clear(); return true; }
            saved = errno; // Keep both names if removing the source fails; never lose data.
        } else saved = errno;
    }
    error = std::error_code(saved, std::generic_category());
    return false;
}

bool write_file_atomic(const std::string& path, const std::string& contents) {
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int fd = mkstemp(name.data()); // O_EXCL, mode 0600; never follows a stale .tmp symlink.
    if (fd < 0) return false;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct stat existing{};
    bool ok = stat(path.c_str(), &existing) != 0 ||
              fchown(fd, existing.st_uid, existing.st_gid) == 0;
    // Keep the existing owner's access when the container writes a host bind mount.

    size_t offset = 0;
    while (ok && offset < contents.size()) {
        ssize_t count = write(fd, contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        offset += static_cast<size_t>(count);
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(name.data(), path.c_str()) == 0) return true;
    unlink(name.data());
    return false;
}

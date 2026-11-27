#include "file_ops.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <cerrno>
#include <cstdio>
#include <vector>
#include <array>

namespace {
bool write_all(int fd, const char* data, size_t size) {
    while (size) {
        const auto count = write(fd, data, size);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        data += count;
        size -= count;
    }
    return true;
}

void sync_parent(const std::filesystem::path& path) {
    auto parent = path.parent_path();
    int fd = open(parent.empty() ? "." : parent.c_str(), O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { fsync(fd); close(fd); }
}

// Publish a fully written temporary file atomically where supported.
int publish(const std::filesystem::path& source, const std::filesystem::path& destination) {
    if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) == 0)
        return 0;
    int error = errno;
    if (error != ENOSYS && error != EINVAL && error != EOPNOTSUPP) return error;
    if (link(source.c_str(), destination.c_str()) == 0)
        return unlink(source.c_str()) == 0 ? 0 : errno;
    error = errno;
    if (error != ENOSYS && error != EINVAL && error != EOPNOTSUPP && error != EPERM) return error;
    // Filesystems such as CIFS nounix support neither RENAME_NOREPLACE nor hard links.
    // Check that destination does not already exist (without following symlinks) before renaming.
    struct stat existing{};
    if (lstat(destination.c_str(), &existing) == 0) return EEXIST;
    if (errno != ENOENT) return errno;
    return rename(source.c_str(), destination.c_str()) == 0 ? 0 : errno;
}

int copy_across_filesystems(const std::filesystem::path& source, const std::filesystem::path& destination) {
    int input = open(source.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (input < 0) return errno;
    struct stat before{};
    if (fstat(input, &before) != 0 || !S_ISREG(before.st_mode)) { close(input); return EINVAL; }
    auto temporary = (destination.parent_path() / ".pitrove-move.XXXXXX").string();
    int output = mkostemp(temporary.data(), O_CLOEXEC);
    if (output < 0) { const int error = errno; close(input); return error; }
    int error = 0;
    std::array<char, 128 * 1024> buffer;
    for (;;) {
        const auto count = read(input, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { error = errno; break; }
        if (!count) break;
        if (!write_all(output, buffer.data(), count)) { error = errno ? errno : EIO; break; }
    }
    struct stat after{};
    if (!error && (lstat(source.c_str(), &after) != 0 || before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino || before.st_size != after.st_size ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec))
        error = ESTALE;
    close(input);
    const timespec times[]{before.st_atim, before.st_mtim};
    if (!error && (fchmod(output, before.st_mode & 0777) != 0 || futimens(output, times) != 0 || fsync(output) != 0))
        error = errno;
    if (close(output) != 0 && !error) error = errno;
    if (!error) error = publish(temporary, destination);
    unlink(temporary.c_str());
    if (error) return error;
    sync_parent(destination);
    if (lstat(source.c_str(), &after) != 0 || after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
        return ESTALE;
    return unlink(source.c_str()) == 0 ? 0 : errno;
}
} // namespace

bool move_without_replace(const std::filesystem::path& source,
                          const std::filesystem::path& destination, std::error_code& error) {
    int result = publish(source, destination);
    if (result == EXDEV) result = copy_across_filesystems(source, destination);
    if (!result) {
        sync_parent(destination);
        if (source.parent_path() != destination.parent_path()) sync_parent(source);
    }
    error = std::error_code(result, std::generic_category());
    return result == 0;
}

bool write_file_atomic(const std::string& path, const std::string& contents) {
    std::string pattern = path + ".tmp.XXXXXX";
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int fd = mkstemp(name.data()); // O_EXCL, mode 0600; never follows a stale .tmp symlink.
    if (fd < 0) return false;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    struct stat existing{};
    bool ok = true;
    // Keep the existing owner's access when running as root on a host bind mount.
    if (geteuid() == 0 && stat(path.c_str(), &existing) == 0) {
        ok = fchown(fd, existing.st_uid, existing.st_gid) == 0;
    }

    if (ok) ok = fchmod(fd, 0600) == 0 && write_all(fd, contents.data(), contents.size());
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    if (ok && rename(name.data(), path.c_str()) == 0) {
        sync_parent(path);
        return true;
    }
    unlink(name.data());
    return false;
}

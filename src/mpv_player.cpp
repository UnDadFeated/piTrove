#include "mpv_player.h"
#include "util.h"
#include "config.h"
#include <dirent.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <cstring>
#include <xf86drm.h>
#include <EGL/egl.h>

MpvPlayer g_mpv_player;

MpvPlayer::MpvPlayer() {}

MpvPlayer::~MpvPlayer() {
    stop();
}

int MpvPlayer::find_drm_fd() {
    DIR *dir = opendir("/proc/self/fd");
    if (!dir) return -1;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        char link[256], target[256];
        std::snprintf(link, sizeof(link), "/proc/self/fd/%s", entry->d_name);
        ssize_t len = readlink(link, target, sizeof(target) - 1);
        if (len > 0) {
            target[len] = '\0';
            if (std::strstr(target, "/dev/dri/card")) {
                int fd = std::atoi(entry->d_name);
                closedir(dir);
                g_logger.info("VIDEO_DRM: Found SDL2 DRM fd=%d → %s", fd, target);
                return fd;
            }
        }
    }
    closedir(dir);
    return -1;
}

bool MpvPlayer::play(const std::string& path, int volume) {
    std::lock_guard<std::mutex> lk(mtx);

    // Stop any existing playback
    if (video_pid > 0) {
        kill(video_pid, SIGTERM);
        int status;
        waitpid(video_pid, &status, 0);
        video_pid = -1;
    }

    // Resolve DRM fd (cached after first search)
    if (drm_fd < 0) {
        drm_fd = find_drm_fd();
    }

    // Drop DRM master context so mpv can render
    if (drm_fd >= 0) {
        int rc = drmDropMaster(drm_fd);
        g_logger.info("VIDEO_DRM: drmDropMaster(fd=%d) = %d", drm_fd, rc);
    } else {
        g_logger.error("VIDEO_DRM: No DRM fd discovered. Process mpv may fail to acquire screen.");
    }

    int matte_px = 0;
    {
        std::lock_guard<std::mutex> lock(g_config_mtx);
        matte_px = g_cfg.matting_size;
    }

    char margin_x_arg[64], margin_y_arg[64];
    std::snprintf(margin_x_arg, sizeof(margin_x_arg), "--osd-margin-x=%d", matte_px + 8);
    std::snprintf(margin_y_arg, sizeof(margin_y_arg), "--osd-margin-y=%d", matte_px + 8);
    char cmd[256];

    pid_t pid = fork();
    if (pid == 0) {
        // Child Process: close inherited parent descriptors and execute mpv
        for (int i = 3; i < 1024; ++i) close(i);

        // Redirect stdout/stderr to log file to inspect mpv status
        int dbg = open("/home/pi/mpv_debug.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dbg >= 0) {
            dup2(dbg, STDOUT_FILENO);
            dup2(dbg, STDERR_FILENO);
            close(dbg);
        }

        if (volume > 0) {
            std::snprintf(cmd, sizeof(cmd), "--volume=%d", volume);
            execlp("mpv", "mpv",
                "--vo=drm",
                "--drm-connector=HDMI-A-1",
                "--hwdec=auto",
                "--keepaspect=force",
                "--no-osc",
                "--no-osd-bar",
                "--osd-level=3",
                "--osd-status-msg=${filename} - ${time-remaining}",
                "--osd-align-x=left",
                "--osd-align-y=bottom",
                margin_x_arg,
                margin_y_arg,
                "--osd-font-size=10",
                "--no-sub",
                cmd,
                path.c_str(),
                nullptr);
        } else {
            execlp("mpv", "mpv",
                "--vo=drm",
                "--drm-connector=HDMI-A-1",
                "--hwdec=auto",
                "--keepaspect=force",
                "--no-osc",
                "--no-osd-bar",
                "--osd-level=3",
                "--osd-status-msg=${filename} - ${time-remaining}",
                "--osd-align-x=left",
                "--osd-align-y=bottom",
                margin_x_arg,
                margin_y_arg,
                "--osd-font-size=10",
                "--no-sub",
                "--no-audio",
                path.c_str(),
                nullptr);
        }
        _exit(1); // child exit if exec fails
    } else if (pid > 0) {
        video_pid = pid;
        active.store(true);
        g_logger.info("VIDEO_PLAY: Spawned child process mpv (pid=%d) for path=%s", pid, path.c_str());
        return true;
    }

    // Fork failed: reclaim DRM master right away
    g_logger.error("VIDEO_PLAY: Forking child process failed: %s", std::strerror(errno));
    if (drm_fd >= 0) {
        drmSetMaster(drm_fd);
    }
    return false;
}

void MpvPlayer::stop() {
    std::lock_guard<std::mutex> lk(mtx);
    if (video_pid > 0) {
        kill(video_pid, SIGTERM);
        usleep(200000); // 200ms grace period
        int status;
        pid_t result = waitpid(video_pid, &status, WNOHANG);
        if (result == 0) {
            kill(video_pid, SIGKILL);
            waitpid(video_pid, &status, 0);
        }
        g_logger.info("VIDEO_STOP: Successfully killed child mpv (pid=%d)", video_pid);
        video_pid = -1;
    }
    active.store(false);

    // Reclaim DRM master context for SDL
    if (drm_fd >= 0) {
        int rc = drmSetMaster(drm_fd);
        g_logger.info("VIDEO_DRM: drmSetMaster(fd=%d) = %d", drm_fd, rc);
        eglGetError(); // clear stale EGL context flags
    }
}

bool MpvPlayer::is_active() {
    return active.load();
}

bool MpvPlayer::check_status() {
    if (video_pid <= 0) return false;

    int status;
    pid_t result = waitpid(video_pid, &status, WNOHANG);
    if (result > 0) {
        // Child process finished
        g_logger.info("VIDEO_EOF: mpv (pid=%d) finished playback (status=%d)", video_pid,
                      WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        video_pid = -1;
        active.store(false);

        // Reclaim DRM master context for SDL
        if (drm_fd >= 0) {
            int rc = drmSetMaster(drm_fd);
            g_logger.info("VIDEO_DRM: drmSetMaster(fd=%d) = %d", drm_fd, rc);
            eglGetError(); // clear stale EGL context flags
        }
        return false;
    }
    return true; // Still playing
}

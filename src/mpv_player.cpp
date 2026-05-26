#include "mpv_player.h"
#include "util.h"
#include "config.h"
#include <dirent.h>
#include <algorithm>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <cstring>
#include <xf86drm.h>
#include <EGL/egl.h>
#include <thread>
#include <chrono>

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
                g_logger.info("VIDEO_DRM: Found SDL3 DRM fd=%d → %s", fd, target);
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

    // Initialize pause tracking
    last_paused = false;

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

    bool cc_enabled = true;
    std::string subtitles_dir = "";
    std::string connector_arg = "--drm-connector=HDMI-A-1";
    std::string audio_arg = "";
    {
        std::lock_guard<std::mutex> lock(g_config_mtx);
        cc_enabled = g_cfg.closed_captions_enabled;
        subtitles_dir = g_cfg.video_subtitles_dir;
        if (!g_cfg.drm_connector.empty() && g_cfg.drm_connector != "auto") {
            connector_arg = "--drm-connector=" + g_cfg.drm_connector;
        }
        if (!g_cfg.video_audio_device.empty() && g_cfg.video_audio_device != "auto") {
            audio_arg = "--audio-device=" + g_cfg.video_audio_device;
        }
    }

    // Look for matching .srt in subtitles folder
    std::string sub_file = "";
    if (!subtitles_dir.empty() && cc_enabled) {
        std::string basename = path.substr(path.find_last_of('/') + 1);
        size_t dot = basename.find_last_of('.');
        if (dot != std::string::npos) basename = basename.substr(0, dot);
        std::string srt_path = subtitles_dir + "/" + basename + ".srt";
        struct stat st;
        if (stat(srt_path.c_str(), &st) == 0) {
            sub_file = "--sub-file=" + srt_path;
            g_logger.info("VIDEO_SUB: Found subtitle match: %s", srt_path.c_str());
        }
    }

    int matte_px = 96;
    {
        std::lock_guard<std::mutex> lock(g_config_mtx);
        matte_px = g_cfg.matting_size;
    }
    std::string margin_x_arg = "--osd-margin-x=" + std::to_string(matte_px + 8);
    std::string margin_y_arg = "--osd-margin-y=" + std::to_string(matte_px + 8);
    std::string sub_margin_y_arg = "--sub-margin-y=" + std::to_string(matte_px + 8);

    // Dynamically calculate thread pool size based on CPU cores (max_cores - 1)
    unsigned int max_cores = std::thread::hardware_concurrency();
    if (max_cores == 0) max_cores = 4; // safe default fallback
    unsigned int threads_to_use = (max_cores > 1) ? (max_cores - 1) : 1;
    char threads_arg[64];
    std::snprintf(threads_arg, sizeof(threads_arg), "--vd-lavc-threads=%u", threads_to_use);

    g_logger.info("VIDEO_PLAY: Launching mpv with dynamic core limit: %s, connector: %s, audio_device: %s",
                  threads_arg, connector_arg.c_str(), audio_arg.empty() ? "default" : audio_arg.c_str());

    pid_t pid = fork();
    if (pid == 0) {
        // Child Process: close inherited parent descriptors and execute mpv
        for (int i = 3; i < 1024; ++i) close(i);

        // Redirect stdout/stderr to log file to inspect mpv status
        int dbg = open("/app/logs/mpv_debug.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (dbg >= 0) {
            dup2(dbg, STDOUT_FILENO);
            dup2(dbg, STDERR_FILENO);
            close(dbg);
        }

        std::vector<std::string> args = {
            "mpv",
            "--vo=drm",
            connector_arg,
            "--hwdec=auto",
            "--keepaspect=yes",
            "--no-osc",
            "--no-osd-bar",
            "--osd-level=3",
            "--osd-status-msg=${filename} - ${time-remaining}",
            "--osd-align-x=left",
            "--osd-align-y=bottom",
            margin_x_arg,
            margin_y_arg,
            "--osd-font-size=10",
            threads_arg
        };

        if (volume > 0) {
            args.push_back("--volume=" + std::to_string(volume));
            if (!audio_arg.empty()) {
                args.push_back(audio_arg);
            }
        } else {
            args.push_back("--no-audio");
        }

        if (cc_enabled) {
            args.push_back("--sub-create-cc-track=yes");
            args.push_back("--sub-auto=all");
            args.push_back("--sub-visibility=yes");
            args.push_back("--sid=auto");
            args.push_back("--sub-align-x=center");
            args.push_back("--sub-align-y=bottom");
            args.push_back(sub_margin_y_arg);
            if (!sub_file.empty()) {
                args.push_back(sub_file);
            }
        } else {
            args.push_back("--no-sub");
        }

        args.push_back(path);

        // Convert to char* argv array
        std::vector<char*> argv_exec;
        argv_exec.reserve(args.size() + 1);
        for (const auto& a : args) {
            argv_exec.push_back(const_cast<char*>(a.c_str()));
        }
        argv_exec.push_back(nullptr);

        execvp("mpv", argv_exec.data());
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

void MpvPlayer::reclaim_drm_master() {
    if (drm_fd >= 0) {
        int rc = drmSetMaster(drm_fd);
        g_logger.info("VIDEO_DRM: drmSetMaster(fd=%d) = %d", drm_fd, rc);
        eglGetError(); // clear stale EGL context flags
    }
}

void MpvPlayer::stop() {
    std::lock_guard<std::mutex> lk(mtx);
    if (video_pid > 0) {
        kill(video_pid, SIGTERM);
        std::this_thread::sleep_for(std::chrono::microseconds(200000)); // 200ms grace period
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

    reclaim_drm_master();
}

bool MpvPlayer::is_active() {
    return active.load();
}

bool MpvPlayer::check_status(bool reclaim_drm_on_eof) {
    std::lock_guard<std::mutex> lk(mtx);
    if (video_pid <= 0) return false;

    // Handle dynamic process pausing/resuming
    bool current_paused = g_slideshow_paused.load();
    if (current_paused != last_paused) {
        if (current_paused) {
            kill(video_pid, SIGSTOP);
            g_logger.info("VIDEO_PAUSE: Sent SIGSTOP to child mpv (pid=%d)", video_pid);
        } else {
            kill(video_pid, SIGCONT);
            g_logger.info("VIDEO_RESUME: Sent SIGCONT to child mpv (pid=%d)", video_pid);
        }
        last_paused = current_paused;
    }

    int status;
    pid_t result = waitpid(video_pid, &status, WNOHANG);
    if (result > 0) {
        video_pid = -1;
        active.store(false);
        g_logger.info("VIDEO_EOF: mpv (pid=%d) finished playback (status=%d)", result,
                      WIFEXITED(status) ? WEXITSTATUS(status) : -1);

        if (reclaim_drm_on_eof) {
            reclaim_drm_master();
        }
        return false;
    }
    return true;
}

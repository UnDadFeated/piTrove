#ifndef PITROVE_MPV_PLAYER_H
#define PITROVE_MPV_PLAYER_H

#include <string>
#include <atomic>
#include <mutex>
#include <sys/types.h>

class MpvPlayer {
private:
    pid_t video_pid{-1};
    int drm_fd{-1};
    std::atomic<bool> active{false};
    std::mutex mtx;
    bool last_paused{false};

public:
    MpvPlayer();
    ~MpvPlayer();

    // Start video playback via mpv child subprocess (drops DRM master)
    bool play(const std::string& path, int volume);
    
    // Stop video playback immediately (SIGTERM/SIGKILL + reclaim DRM master)
    void stop();
    
    // Query if mpv is actively running
    bool is_active();
    
    // Non-blocking poll for child exit (reclaims DRM master on exit)
    bool check_status(bool reclaim_drm_on_eof = true);
    
    // Explicitly reclaim DRM master context back to SDL3
    void reclaim_drm_master();

private:
    int find_drm_fd();
};

inline MpvPlayer g_mpv_player;

#endif // PITROVE_MPV_PLAYER_H

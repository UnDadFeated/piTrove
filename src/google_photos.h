#ifndef PITROVE_GOOGLE_PHOTOS_H
#define PITROVE_GOOGLE_PHOTOS_H

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

class GooglePhotosManager {
    std::mutex lifecycle_mtx;
    std::jthread sync_thread;
    std::atomic<bool> running{false};
    std::atomic<unsigned> revision{0};
    std::mutex status_mtx;
    std::string state = "ready";
    std::string picker_url;
    unsigned imported = 0;
    unsigned failed = 0;

    void import_selection(std::stop_token stop, const std::string& id, const std::string& token,
                          const std::string& directory, int poll_seconds, int lifetime_seconds);

public:
    ~GooglePhotosManager();
    void start();
    void stop();
    bool is_running() const { return running.load(); }
    unsigned media_revision() const { return revision.load(); }
    std::string create_picker();
    std::string picker_status();
};

inline GooglePhotosManager g_google_photos;

#endif

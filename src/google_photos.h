#include <condition_variable>
#ifndef PITROVE_GOOGLE_PHOTOS_H
#define PITROVE_GOOGLE_PHOTOS_H

#include <string>
#include <thread>
#include <mutex>
#include <atomic>

class GooglePhotosManager {
private:
    std::thread sync_thread;
  std::condition_variable stop_cv;
  std::mutex stop_mtx;
    std::atomic<bool> running{false};
    std::mutex sync_mtx;

    std::string get_access_token();
    void download_media(const std::string& access_token);
    std::string execute_curl(const std::string& cmd);
    std::string parse_json_value(const std::string& json, const std::string& key);

public:
    GooglePhotosManager() = default;
    ~GooglePhotosManager();

    void start();
    void stop();
    void sync_now();
};

inline GooglePhotosManager g_google_photos;

#endif // PITROVE_GOOGLE_PHOTOS_H

#include "google_photos.h"
#include "config.h"
#include "util.h"
#include <iostream>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <memory>

GooglePhotosManager g_google_photos;

GooglePhotosManager::~GooglePhotosManager() {
    stop();
}

void GooglePhotosManager::start() {
    bool enabled = false;
    {
        std::lock_guard<std::mutex> lock(g_config_mtx);
        enabled = g_cfg.google_photos_enabled;
    }
    if (!enabled) return;

    if (running.load()) return;
    running.store(true);

    sync_thread = std::thread([this]() {
        g_logger.info("GooglePhotos: Background sync thread started.");
        
        while (running.load()) {
            bool current_enabled = false;
            int interval_mins = 60;
            {
                std::lock_guard<std::mutex> lock(g_config_mtx);
                current_enabled = g_cfg.google_photos_enabled;
                interval_mins = g_cfg.google_photos_sync_interval;
            }

            if (current_enabled) {
                try {
                    sync_now();
                } catch (const std::exception& e) {
                    g_logger.error("GooglePhotos: Sync failed with exception: %s", e.what());
                } catch (...) {
                    g_logger.error("GooglePhotos: Sync failed with unknown exception.");
                }
            }

            // Sleep for interval minutes, checking running status every second for rapid exits
            int total_sleep_sec = interval_mins * 60;
            for (int i = 0; i < total_sleep_sec && running.load(); i++) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        g_logger.info("GooglePhotos: Background sync thread exiting.");
    });
}

void GooglePhotosManager::stop() {
    if (!running.load()) return;
    running.store(false);
    if (sync_thread.joinable()) {
        sync_thread.join();
    }
}

void GooglePhotosManager::sync_now() {
    std::lock_guard<std::mutex> lock(sync_mtx);
    
    std::string client_id, client_secret, refresh_token, cache_dir;
    {
        std::lock_guard<std::mutex> lk(g_config_mtx);
        client_id = g_cfg.google_photos_client_id;
        client_secret = g_cfg.google_photos_client_secret;
        refresh_token = g_cfg.google_photos_refresh_token;
        cache_dir = g_cfg.google_photos_cache_dir;
    }

    if (client_id.empty() || client_secret.empty() || refresh_token.empty()) {
        g_logger.warn("GooglePhotos: Setup incomplete. Missing Client ID, Client Secret, or Refresh Token.");
        return;
    }

    g_logger.info("GooglePhotos: Initiating cloud media synchronization to cache folder '%s'...", cache_dir.c_str());

    std::filesystem::create_directories(cache_dir);

    std::string access_token = get_access_token();
    if (access_token.empty()) {
        g_logger.error("GooglePhotos: Failed to obtain OAuth 2.0 access token from Google.");
        g_active_error_code.store(301); // E301
        return;
    }

    download_media(access_token);
}

std::string GooglePhotosManager::get_access_token() {
    std::string client_id, client_secret, refresh_token;
    {
        std::lock_guard<std::mutex> lk(g_config_mtx);
        client_id = g_cfg.google_photos_client_id;
        client_secret = g_cfg.google_photos_client_secret;
        refresh_token = g_cfg.google_photos_refresh_token;
    }

    // Build the curl command securely to fetch access token
    std::string cmd = "curl -s -X POST https://oauth2.googleapis.com/token "
                      "-d client_id=\"" + client_id + "\" "
                      "-d client_secret=\"" + client_secret + "\" "
                      "-d refresh_token=\"" + refresh_token + "\" "
                      "-d grant_type=refresh_token";

    std::string json = execute_curl(cmd);
    std::string access_token = parse_json_value(json, "access_token");
    if (access_token.empty()) {
        std::string err = parse_json_value(json, "error");
        std::string err_desc = parse_json_value(json, "error_description");
        if (err == "invalid_client") {
            g_active_error_code.store(303); // E303: GOOGLE_PHOTOS_CLIENT_INVALID
        } else if (err == "invalid_grant" || err_desc.find("expired") != std::string::npos || err_desc.find("revoked") != std::string::npos) {
            g_active_error_code.store(304); // E304: GOOGLE_PHOTOS_REFRESH_TOKEN_EXPIRED
        } else {
            g_active_error_code.store(301); // E301: GOOGLE_PHOTOS_SYNC_FAILED
        }
    }
    return access_token;
}

void GooglePhotosManager::download_media(const std::string& access_token) {
    std::string album_id, cache_dir;
    {
        std::lock_guard<std::mutex> lk(g_config_mtx);
        album_id = g_cfg.google_photos_album_id;
        cache_dir = g_cfg.google_photos_cache_dir;
    }

    std::string cmd;
    if (!album_id.empty()) {
        // Query specific album
        cmd = "curl -s -X POST https://photoslibrary.googleapis.com/v1/mediaItems:search "
              "-H \"Authorization: Bearer " + access_token + "\" "
              "-H \"Content-type: application/json\" "
              "-d '{\"albumId\": \"" + album_id + "\", \"pageSize\": 100}'";
    } else {
        // Query all media items
        cmd = "curl -s -X GET \"https://photoslibrary.googleapis.com/v1/mediaItems?pageSize=100\" "
              "-H \"Authorization: Bearer " + access_token + "\"";
    }

    std::string json = execute_curl(cmd);
    
    // Check for API errors in the json response
    if (json.find("\"error\"") != std::string::npos) {
        std::string err_msg = parse_json_value(json, "message");
        if (json.find("RESOURCE_EXHAUSTED") != std::string::npos || json.find("429") != std::string::npos) {
            g_active_error_code.store(302); // E302: GOOGLE_PHOTOS_RATE_LIMITED
            g_logger.error("GooglePhotos: API Rate Limited (RESOURCE_EXHAUSTED).");
            return;
        } else if (json.find("ALBUM_NOT_FOUND") != std::string::npos || err_msg.find("album") != std::string::npos) {
            g_active_error_code.store(305); // E305: GOOGLE_PHOTOS_ALBUM_NOT_FOUND
            g_logger.error("GooglePhotos: Album not found or inaccessible.");
            return;
        } else {
            g_active_error_code.store(301); // E301: GOOGLE_PHOTOS_SYNC_FAILED
            g_logger.error("GooglePhotos: API error: %s", err_msg.c_str());
            return;
        }
    }
    
    // Tiny, super-robust JSON list parser
    size_t pos = 0;
    int items_downloaded = 0;
    int items_skipped = 0;

    while (true) {
        // Find next mediaItem block
        size_t id_pos = json.find("\"id\"", pos);
        if (id_pos == std::string::npos) break;

        std::string id = parse_json_value(json.substr(id_pos), "id");
        if (id.empty()) {
            pos = id_pos + 4;
            continue;
        }

        size_t url_pos = json.find("\"baseUrl\"", id_pos);
        if (url_pos == std::string::npos) break;

        std::string baseUrl = parse_json_value(json.substr(url_pos), "baseUrl");
        
        size_t mime_pos = json.find("\"mimeType\"", id_pos);
        std::string mime = "image/jpeg";
        if (mime_pos != std::string::npos) {
            mime = parse_json_value(json.substr(mime_pos), "mimeType");
        }

        size_t fn_pos = json.find("\"filename\"", id_pos);
        std::string original_filename = "photo.jpg";
        if (fn_pos != std::string::npos) {
            original_filename = parse_json_value(json.substr(fn_pos), "filename");
        }

        // Determine local extension
        std::string ext = "jpg";
        bool is_video = (mime.find("video") != std::string::npos);
        if (is_video) {
            ext = "mp4";
        } else {
            size_t dot = original_filename.rfind('.');
            if (dot != std::string::npos) {
                ext = original_filename.substr(dot + 1);
            }
        }

        std::string filename = "gphoto_" + id + "." + ext;
        std::string local_path = cache_dir + "/" + filename;

        if (!std::filesystem::exists(local_path)) {
            g_logger.info("GooglePhotos: Downloading new %s: %s", (is_video ? "video" : "photo"), original_filename.c_str());
            std::string download_url = baseUrl + (is_video ? "=dv" : "=d");
            
            // Build the curl download command
            std::string dl_cmd = "curl -s -o \"" + local_path + "\" \"" + download_url + "\"";
            std::string dl_res = execute_curl(dl_cmd);

            // Double check that file is non-empty and valid
            if (std::filesystem::exists(local_path) && std::filesystem::file_size(local_path) > 0) {
                items_downloaded++;
            } else {
                g_logger.error("GooglePhotos: Failed to download item: %s", original_filename.c_str());
                std::filesystem::remove(local_path);
            }
        } else {
            items_skipped++;
        }

        // Advance cursor past this item block
        pos = url_pos + 10;
    }

    g_logger.info("GooglePhotos: Sync complete. Downloaded=%d, Skipped=%d", items_downloaded, items_skipped);
    
    // Clear Google Photos E301-E305 errors if sync finishes successfully
    int current_err = g_active_error_code.load();
    if (current_err >= 301 && current_err <= 305) {
        g_active_error_code.store(0);
    }
}

std::string GooglePhotosManager::execute_curl(const std::string& cmd) {
    std::shared_ptr<FILE> pipe(popen((cmd + " 2>/dev/null").c_str(), "r"), pclose);
    if (!pipe) {
        g_logger.error("GooglePhotos: Failed to popen curl command.");
        return "";
    }

    char buffer[4096];
    std::string result = "";
    while (!feof(pipe.get())) {
        if (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
            result += buffer;
        }
    }
    return result;
}

std::string GooglePhotosManager::parse_json_value(const std::string& json, const std::string& key) {
    size_t key_pos = json.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return "";

    size_t colon_pos = json.find(":", key_pos);
    if (colon_pos == std::string::npos) return "";

    size_t quote_start = json.find("\"", colon_pos);
    if (quote_start == std::string::npos) return "";

    size_t quote_end = json.find("\"", quote_start + 1);
    if (quote_end == std::string::npos) return "";

    return json.substr(quote_start + 1, quote_end - quote_start - 1);
}

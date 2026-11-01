#include <curl/curl.h>
#include <thread>
#include "google_photos.h"
#include "http_transfer.h"
#include <fcntl.h>
#include <unistd.h>
#include "config.h"
#include "util.h"
#include <sys/socket.h>
#include <netdb.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <utility>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>


GooglePhotosManager::~GooglePhotosManager() { stop(); }

void GooglePhotosManager::start() {
  bool enabled = false;
  {
    std::lock_guard lock(g_config_mtx);
    enabled = g_cfg.google_photos_enabled;
  }
  if (!enabled)
    return;

  if (running.load())
    return;
  running.store(true);

  std::jthread t;
  if (spawn_thread_safe(t, "google_photos_sync", [this]() {
    g_logger.info("GooglePhotos: Background sync thread started.");

    while (running.load()) {
      bool current_enabled = false;
      int interval_mins = 60;
      {
        std::lock_guard lock(g_config_mtx);
        current_enabled = g_cfg.google_photos_enabled;
        interval_mins = g_cfg.google_photos_sync_interval;
      }

      if (current_enabled) {
        try {
          sync_now();
        } catch (const std::exception &e) {
          g_logger.error("GooglePhotos: Sync failed with exception: {}",
                         e.what());
        } catch (...) {
          g_logger.error("GooglePhotos: Sync failed with unknown exception.");
        }
      }

      // Efficient condition variable sleep with instant wake-up on stop()
      std::unique_lock<std::mutex> lk(stop_mtx);
      stop_cv.wait_for(lk, std::chrono::minutes(interval_mins), [this] { return !running.load(); });
    }

    g_logger.info("GooglePhotos: Background sync thread exiting.");
  })) {
    sync_thread = std::move(t);
  } else {
    running.store(false);
  }
}

void GooglePhotosManager::stop() {
  if (!running.load())
    return;
  running.store(false);
  stop_cv.notify_all();
  if (sync_thread.joinable()) {
    sync_thread.join();
  }
}

void GooglePhotosManager::sync_now() {
  std::lock_guard<std::mutex> lock(sync_mtx);

  std::string client_id, client_secret, refresh_token, cache_dir;
  {
    std::lock_guard lk(g_config_mtx);
    client_id = g_cfg.google_photos_client_id;
    client_secret = g_cfg.google_photos_client_secret;
    refresh_token = g_cfg.google_photos_refresh_token;
    cache_dir = g_cfg.google_photos_cache_dir;
  }

  if (client_id.empty() || client_secret.empty() || refresh_token.empty()) {
    trigger_error(318); // E318: GOOGLE_PHOTOS_CONFIG_MISSING
    return;
  }

  // Check for critical disk space in cache directory (E403)
  std::error_code space_ec;
  std::filesystem::create_directories(cache_dir, space_ec);
  auto space_info = std::filesystem::space(cache_dir, space_ec);
  if (!space_ec && space_info.available < 50 * 1024 * 1024) {
    trigger_error(403); // E403: DISK_SPACE_CRITICAL
    g_logger.error("GooglePhotos: Storage space critically low (<50MB) on cache drive '{}'. Skipping sync.", cache_dir.c_str());
    return;
  } else {
    if (is_error_active(403)) {
      clear_error(403);
    }
  }

  g_logger.info("GooglePhotos: Initiating cloud media synchronization to cache "
                "folder '%s'...",
                cache_dir.c_str());

  std::string access_token = get_access_token();
  if (access_token.empty()) {
    trigger_error(301); // E301: GOOGLE_PHOTOS_SYNC_FAILED
    return;
  }

  download_media(access_token);
}


static std::string url_encode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size() * 3);
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

// Escape a value for a JSON string body. The remote nextPageToken was interpolated
// into the search request body unescaped.
static std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          static const char* hex = "0123456789abcdef";
          out += "\\u00";
          out += hex[(c >> 4) & 0x0F];
          out += hex[c & 0x0F];
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

// Real HTTP POST helper.
//
// execute_curl() takes a *shell-shaped* string but only recovers a URL and an
// `Authorization: Bearer` header from it, then issues a plain GET. Every `-X POST` and
// `-d` pair was therefore silently discarded: the refresh-token grant went out as a
// bodyless GET (which the token endpoint rejects, so Google Photos could never
// authenticate), and the mediaItems:search body was dropped. These helpers set
// CURLOPT_POST/POSTFIELDS explicitly, so a flag added later cannot be silently ignored.
static std::string http_post(const std::string& url, const std::string& body,
                             const std::string& content_type, const std::string& bearer) {
  CURL* curl = curl_easy_init();
  if (!curl) return "";
  std::string response;
  http_transfer::TextBuffer buffer{&response, 4 * 1024 * 1024};
  curl_slist* headers = curl_slist_append(nullptr, ("Content-Type: " + content_type).c_str());
  if (!bearer.empty()) {
    headers = curl_slist_append(headers, ("Authorization: Bearer " + bearer).c_str());
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, http_transfer::append_text);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  CURLcode res = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
  curl_easy_cleanup(curl);
  curl_slist_free_all(headers);
  if (res != CURLE_OK || status < 200 || status >= 300) {
    g_logger.error("GooglePhotos: POST {} failed: {}", url, curl_easy_strerror(res));
    return res == CURLE_OK ? response : ""; // Preserve structured API errors for diagnostics.
  }
  return response;
}

static std::string post_form(const std::string& url,
                             const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string body;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i) body += "&";
    body += url_encode(fields[i].first) + "=" + url_encode(fields[i].second);
  }
  return http_post(url, body, "application/x-www-form-urlencoded", "");
}

std::string GooglePhotosManager::get_access_token() {
  std::string client_id, client_secret, refresh_token;
  {
    std::lock_guard lk(g_config_mtx);
    client_id = g_cfg.google_photos_client_id;
    client_secret = g_cfg.google_photos_client_secret;
    refresh_token = g_cfg.google_photos_refresh_token;
  }

  // Real form POST. The previous shell-shaped string lost its -X POST and -d fields when
  // execute_curl() reduced it to a URL, so this grant was sent as a bodyless GET.
  std::string json = post_form("https://oauth2.googleapis.com/token",
                               {{"client_id", client_id},
                                {"client_secret", client_secret},
                                {"refresh_token", refresh_token},
                                {"grant_type", "refresh_token"}});
  std::string access_token = parse_json_value(json, "access_token");
  if (access_token.empty()) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("oauth2.googleapis.com", nullptr, &hints, &res) != 0) {
      trigger_error(105); // E105: DNS_RESOLUTION_FAILURE
    } else {
      freeaddrinfo(res);
      if (is_error_active(105)) {
        clear_error(105);
      }
    }

    std::string err = parse_json_value(json, "error");
    std::string err_desc = parse_json_value(json, "error_description");
    if (err == "invalid_client") {
      trigger_error(303); // E303: GOOGLE_PHOTOS_CLIENT_INVALID
    } else if (err == "invalid_grant" ||
               err_desc.find("expired") != std::string::npos ||
               err_desc.find("revoked") != std::string::npos) {
      trigger_error(304); // E304: GOOGLE_PHOTOS_REFRESH_TOKEN_EXPIRED
    } else {
      if (!is_error_active(105)) {
        trigger_error(301); // E301: GOOGLE_PHOTOS_SYNC_FAILED
      }
    }
  }
  return access_token;
}

void GooglePhotosManager::download_media(const std::string &access_token) {
  std::string album_id, cache_dir;
  {
    std::lock_guard lk(g_config_mtx);
    album_id = g_cfg.google_photos_album_id;
    cache_dir = g_cfg.google_photos_cache_dir;
  }

  int items_downloaded = 0;
  int items_skipped = 0;
  std::string page_token = "";

  do {
    if (!running.load()) {
      break;
    }

    std::string json;
    if (!album_id.empty()) {
      // Query specific album
      std::string post_data = "{\"albumId\": \"" + sanitize_alphanumeric(album_id) + "\", \"pageSize\": 100";
      if (!page_token.empty()) {
        post_data += ", \"pageToken\": \"" + json_escape(page_token) + "\"";
      }
      post_data += "}";
      // Real JSON POST; the previous shell string lost its body via execute_curl().
      json = http_post("https://photoslibrary.googleapis.com/v1/mediaItems:search",
                       post_data, "application/json", access_token);
    } else {
      // Query all media items
      std::string url = "https://photoslibrary.googleapis.com/v1/mediaItems?pageSize=100";
      if (!page_token.empty()) {
        url += "&pageToken=" + url_encode(page_token);
      }
      // Pass the URL and bearer directly; do not parse a shell command back into HTTP.
      if (!http_transfer::get(url, json, access_token) && json.find("\"error\"") == std::string::npos) {
        trigger_error(301);
        return;
      }
    }

    // Check for API errors in the json response
    if (json.find("\"error\"") != std::string::npos) {
      std::string err_msg = parse_json_value(json, "message");
      if (json.find("RESOURCE_EXHAUSTED") != std::string::npos ||
          json.find("429") != std::string::npos) {
        trigger_error(302); // E302: GOOGLE_PHOTOS_RATE_LIMITED
        return;
      } else if (json.find("ALBUM_NOT_FOUND") != std::string::npos ||
                 err_msg.find("album") != std::string::npos) {
        trigger_error(305); // E305: GOOGLE_PHOTOS_ALBUM_NOT_FOUND
        return;
      } else {
        trigger_error(301); // E301: GOOGLE_PHOTOS_SYNC_FAILED
        return;
      }
    }

    if (json.find("\"id\"") == std::string::npos) {
      break;
    }

    // Tiny, super-robust JSON list parser
    size_t pos = 0;

    while (true) {
      if (!running.load()) {
        break;
      }
      // Find next mediaItem block
      size_t id_pos = json.find("\"id\"", pos);
      if (id_pos == std::string::npos)
        break;

      std::string id = parse_json_value(json.substr(id_pos), "id");
      if (id.empty()) {
        pos = id_pos + 4;
        continue;
      }

      size_t url_pos = json.find("\"baseUrl\"", id_pos);
      if (url_pos == std::string::npos)
        break;

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

      std::string filename = "gphoto_" + sanitize_alphanumeric(id) + "." + sanitize_alphanumeric(ext);
      std::string local_path = cache_dir + "/" + filename;

      if (!std::filesystem::exists(local_path)) {
        std::string download_url = baseUrl + (is_video ? "=dv" : "=d");

        bool domain_valid = http_transfer::is_google_media_url(download_url);
        if (!domain_valid) {
          g_logger.error("GooglePhotos: Security warning: download URL is not a Google Photos domain. Skipping download.");
          pos = id_pos + 4; // advance: 'continue' would otherwise skip the cursor update
          continue;
        }

        g_logger.info("GooglePhotos: Downloading new {}: {}",
                      (is_video ? "video" : "photo"), original_filename.c_str());

        std::error_code space_error;
        auto space = std::filesystem::space(cache_dir, space_error);
        constexpr size_t reserve = 50 * 1024 * 1024;
        size_t limit = !space_error && space.available > reserve ? size_t(space.available - reserve) : 0;
        if (limit && http_transfer::download(download_url, local_path, limit, &running)) {
          ++items_downloaded;
        } else {
          g_logger.error("GooglePhotos: Failed to download item: {}", original_filename);
        }

      } else {
        items_skipped++;
      }

      // Advance cursor past this item block
      pos = url_pos + 10;
    }

    page_token = parse_json_value(json, "nextPageToken");
  } while (!page_token.empty());

  g_logger.info("GooglePhotos: Sync complete. Downloaded={}, Skipped={}",
                items_downloaded, items_skipped);

  // Clear Google Photos E301-E318 errors if sync finishes successfully
  int current_err = g_active_error_code.load();
  if (current_err >= 301 && current_err <= 318) {
    trigger_error(0);
  }
}

std::string GooglePhotosManager::parse_json_value(const std::string& json, const std::string& key) {
    std::string key_pattern = std::string("\"") + key + "\"";
    size_t key_pos = json.find(key_pattern);
    if (key_pos == std::string::npos) return "";

    size_t colon_pos = json.find(':', key_pos);
    if (colon_pos == std::string::npos) return "";

    size_t quote_start = json.find('"', colon_pos);
    if (quote_start == std::string::npos) return "";

    size_t quote_end = std::string::npos;
    for (size_t i = quote_start + 1; i < json.size(); ++i) {
        if (json[i] == '"') {
            size_t bs_count = 0;
            size_t j = i;
            while (j > quote_start + 1 && json[j - 1] == '\\') {
                bs_count++;
                j--;
            }
            if (bs_count % 2 == 0) {
                quote_end = i;
                break;
            }
        }
    }
    if (quote_end == std::string::npos) return "";
    return json.substr(quote_start + 1, quote_end - quote_start - 1);
}

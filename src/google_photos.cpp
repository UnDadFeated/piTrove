#include "google_photos.h"
#include "config.h"
#include "http_protocol.h"
#include "http_transfer.h"
#include "util.h"
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <filesystem>
#include <memory>

namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;
constexpr std::string_view endpoint = "https://photospicker.googleapis.com/v1";

std::string text_field(const Json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto value = object.find(key);
    return value != object.end() && value->is_string() ? value->get<std::string>() : std::string{};
}

Json parse_json(const std::string& body) {
    return Json::parse(body, nullptr, false);
}

Json picker_request(const std::string& path, const std::string& token, const char* method = nullptr,
                    const std::atomic<bool>* running = nullptr) {
    if (running && !running->load()) return {};
    const std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) return {};
    const auto url = std::string(endpoint) + path;
    auto* raw_headers = curl_slist_append(nullptr, ("Authorization: Bearer " + token).c_str());
    if (!raw_headers) return {};
    if (method && std::string_view(method) == "POST") {
        if (auto* extended = curl_slist_append(raw_headers, "Content-Type: application/json")) raw_headers = extended;
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, "{}");
    }
    const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);
    http_transfer::configure_public_https(curl.get());
    http_transfer::set_cancel_flag(curl.get(), running);
    std::string response;
    http_transfer::TextBuffer buffer{&response, 1024 * 1024};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, http_transfer::append_text);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 4L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 8L);
    if (method) curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, method);
    long code = 0;
    if (curl_easy_perform(curl.get()) != CURLE_OK ||
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &code) != CURLE_OK ||
        code < 200 || code >= 300) return {};
    return response.empty() ? Json::object() : parse_json(response);
}

std::string access_token(const Config& config, const std::atomic<bool>* running = nullptr) {
    if (config.google_photos_client_id.empty() || config.google_photos_client_secret.empty() ||
        config.google_photos_refresh_token.empty()) return {};
    const auto encode = http_protocol::url_encode;
    const auto body = "grant_type=refresh_token&client_id=" + encode(config.google_photos_client_id) +
        "&client_secret=" + encode(config.google_photos_client_secret) +
        "&refresh_token=" + encode(config.google_photos_refresh_token);
    std::string response;
    if (!http_transfer::post_form("https://oauth2.googleapis.com/token", body, response, running)) return {};
    auto token = text_field(parse_json(response), "access_token");
    if (token.size() > 8192 || token.find_first_of("\r\n") != token.npos) return {};
    return token;
}

const char* media_extension(std::string_view type) {
    if (type == "image/jpeg") return ".jpg";
    if (type == "image/png") return ".png";
    if (type == "image/webp") return ".webp";
    if (type == "image/gif") return ".gif";
    if (type == "image/heic" || type == "image/heif") return ".heic";
    if (type == "image/tiff") return ".tiff";
    if (type.starts_with("video/")) return ".mp4";
    return nullptr;
}

int duration_seconds(const Json& object, const char* key, int fallback) {
    auto raw = text_field(object, key);
    if (raw.empty() || raw.back() != 's') return fallback;
    raw.pop_back();
    const double parsed = safe_stod(raw, fallback);
    return static_cast<int>(std::clamp(parsed, 1.0, 3600.0));
}

} // namespace

GooglePhotosManager::~GooglePhotosManager() { stop(); }

void GooglePhotosManager::start() {
    std::lock_guard lifecycle(lifecycle_mtx);
    stop_locked();
    std::shared_lock lock(g_config_mtx);
    running.store(g_cfg.google_photos_enabled);
}

void GooglePhotosManager::stop() {
    std::lock_guard lifecycle(lifecycle_mtx);
    stop_locked();
}

void GooglePhotosManager::stop_locked() {
    lifecycle_generation.fetch_add(1);
    running.store(false);
    sync_thread.request_stop();
    if (sync_thread.joinable()) sync_thread.join();
    std::lock_guard lock(status_mtx);
    state = "ready";
    picker_url.clear();
}

GooglePhotosManager::LinkResult GooglePhotosManager::link_account(
    uint64_t expected_generation, const std::string& client_id,
    const std::string& client_secret, const std::string& refresh_token) {
    std::lock_guard lifecycle(lifecycle_mtx);
    if (expected_generation != generation()) return LinkResult::stale;
    {
        std::unique_lock lock(g_config_mtx);
        Config previous = g_cfg;
        g_cfg.google_photos_client_id = client_id;
        g_cfg.google_photos_client_secret = client_secret;
        g_cfg.google_photos_refresh_token = refresh_token;
        g_cfg.google_photos_enabled = true;
        const auto save_path = g_cfg.loaded_path.empty() ? "/app/config/config.toml" : g_cfg.loaded_path;
        if (!g_cfg.save(save_path)) {
            g_cfg = std::move(previous);
            return LinkResult::save_failed;
        }
    }
    stop_locked();
    running.store(true);
    return LinkResult::saved;
}

std::string GooglePhotosManager::picker_status() {
    std::lock_guard lock(status_mtx);
    return Json{{"state", state}, {"picker_url", picker_url}, {"imported", imported}, {"failed", failed}}.dump();
}

std::string GooglePhotosManager::create_picker() {
    Config config;
    uint64_t expected_generation;
    {
        std::lock_guard lifecycle(lifecycle_mtx);
        expected_generation = generation();
        {
            std::lock_guard lock(status_mtx);
            if (state == "picking" || state == "importing" || state == "creating")
                return Json{{"state", state}, {"picker_url", picker_url}}.dump();
            if (!running.load()) return R"({"state":"unlinked"})";
            state = "creating";
            picker_url.clear();
        }
        if (sync_thread.joinable()) sync_thread.join();
        std::shared_lock lock(g_config_mtx);
        config = g_cfg;
    }
    auto reset_creating = [&] {
        std::lock_guard lifecycle(lifecycle_mtx);
        if (generation() != expected_generation) return;
        std::lock_guard lock(status_mtx);
        if (state == "creating") state = "ready";
    };
    const auto token = access_token(config, &running);
    if (generation() != expected_generation) return picker_status();
    if (token.empty()) {
        reset_creating();
        return R"({"state":"unlinked"})";
    }
    const auto session = picker_request("/sessions", token, "POST", &running);
    const auto id = text_field(session, "id");
    const auto url = text_field(session, "pickerUri");
    if (id.empty() || id.size() > 4096 || !url.starts_with("https://photos.google.com/")) {
        reset_creating();
        if (!id.empty() && id.size() <= 4096)
            picker_request("/sessions/" + http_protocol::url_encode(id), token, "DELETE");
        return R"({"state":"authorization_failed"})";
    }
    const auto settings = session.contains("pollingConfig") ? session["pollingConfig"] : Json{};
    const int interval = std::max(2, duration_seconds(settings, "pollInterval", 5));
    const int lifetime = duration_seconds(settings, "timeoutIn", 600);
    const auto directory = config.google_photos_cache_dir.empty()
        ? "/app/cache/google_photos" : config.google_photos_cache_dir;

    bool delete_session = false;
    {
        std::lock_guard lifecycle(lifecycle_mtx);
        if (generation() != expected_generation || !running.load()) {
            delete_session = true;
        } else {
            {
                std::lock_guard lock(status_mtx);
                imported = failed = 0;
                state = "picking";
                picker_url = url + "/autoclose";
            }
            if (!spawn_thread_safe(sync_thread, "google_photos_picker",
                                   [this, id, token, directory, interval, lifetime](std::stop_token stop) {
                    try {
                        import_selection(stop, id, token, directory, interval, lifetime);
                    } catch (...) {
                        std::lock_guard lock(status_mtx);
                        state = "failed";
                        picker_url.clear();
                    }
                })) {
                delete_session = true;
                std::lock_guard lock(status_mtx);
                state = "failed";
                picker_url.clear();
            }
        }
    }
    if (delete_session) {
        picker_request("/sessions/" + http_protocol::url_encode(id), token, "DELETE");
    }
    return picker_status();
}

void GooglePhotosManager::import_selection(std::stop_token stop, const std::string& id,
                                           const std::string& initial_token, const std::string& directory,
                                           int interval, int lifetime) {
    const auto session_path = "/sessions/" + http_protocol::url_encode(id);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(lifetime);
    std::string token = initial_token;
    auto token_issued = std::chrono::steady_clock::now();
    auto refresh_token_if_needed = [&](bool force) {
        if (!force && std::chrono::steady_clock::now() - token_issued < std::chrono::minutes(45)) return;
        Config config;
        { std::shared_lock lock(g_config_mtx); config = g_cfg; }
        auto refreshed = access_token(config, &running);
        if (!refreshed.empty()) {
            token = std::move(refreshed);
            token_issued = std::chrono::steady_clock::now();
        }
    };
    std::mutex wait_mutex;
    std::condition_variable_any wait;
    bool selected = false;
    while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
        std::unique_lock lock(wait_mutex);
        wait.wait_for(lock, stop, std::chrono::seconds(interval), [] { return false; });
        if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) break;
        refresh_token_if_needed(false);
        const auto status = picker_request(session_path, token, nullptr, &running);
        if (status.is_object() && status.contains("mediaItemsSet") && status["mediaItemsSet"] == true) {
            selected = true;
            break;
        }
    }
    unsigned count = 0, errors = 0;
    if (selected && !stop.stop_requested()) {
        {
            std::lock_guard lock(status_mtx);
            state = "importing";
            picker_url.clear();
        }
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec || sodium_init() < 0) ++errors;
        else {
            std::string page_token;
            for (int page = 0; page < 50 && !stop.stop_requested(); ++page) {
                refresh_token_if_needed(false);
                auto document = picker_request("/mediaItems?pageSize=100&sessionId=" +
                    http_protocol::url_encode(id) + "&pageToken=" + http_protocol::url_encode(page_token),
                    token, nullptr, &running);
                if (!document.is_object() && !stop.stop_requested()) {
                    refresh_token_if_needed(true);
                    document = picker_request("/mediaItems?pageSize=100&sessionId=" +
                        http_protocol::url_encode(id) + "&pageToken=" + http_protocol::url_encode(page_token),
                        token, nullptr, &running);
                }
                if (!document.is_object()) { ++errors; break; }
                const auto items = document.find("mediaItems");
                if (items != document.end() && items->is_array()) {
                    for (const auto& item : *items) {
                        if (stop.stop_requested()) break;
                        refresh_token_if_needed(false);
                        const auto file = item.find("mediaFile");
                        if (file == item.end()) { ++errors; continue; }
                        const auto identity = text_field(item, "id");
                        const auto base_url = text_field(*file, "baseUrl");
                        const auto type = text_field(*file, "mimeType");
                        const auto* extension = media_extension(type);
                        if (!extension || identity.empty() || !http_transfer::is_google_media_url(base_url)) {
                            ++errors;
                            continue;
                        }
                        const auto url = base_url + (type.starts_with("video/") ? "=dv" : "=d");
                        unsigned char digest[16];
                        crypto_generichash(digest, sizeof(digest), reinterpret_cast<const unsigned char*>(identity.data()),
                                           identity.size(), nullptr, 0);
                        char hex[sizeof(digest) * 2 + 1];
                        sodium_bin2hex(hex, sizeof(hex), digest, sizeof(digest));
                        const auto path = (std::filesystem::path(directory) / (std::string("gphoto_") + hex + extension)).string();
                        if (std::filesystem::is_regular_file(path, ec)) continue;
                        const auto storage = std::filesystem::space(directory, ec);
                        constexpr uintmax_t reserve = 64ULL * 1024 * 1024;
                        const size_t budget = !ec && storage.available > reserve
                            ? std::min<uintmax_t>(storage.available - reserve, 512ULL * 1024 * 1024) : 0;
                        if (budget && http_transfer::download(url, path, budget, &running, token)) ++count;
                        else ++errors;
                    }
                }
                const auto next_token = text_field(document, "nextPageToken");
                if (next_token.empty()) break;
                if (next_token == page_token || page == 49) { ++errors; break; }
                page_token = next_token;
            }
        }
    }
    if (!stop.stop_requested()) picker_request(session_path, token, "DELETE", &running);
    if (count) revision.fetch_add(1);
    {
        std::lock_guard lock(status_mtx);
        imported = count;
        failed = errors;
        picker_url.clear();
        state = selected ? (errors ? "partial" : "complete") : "expired";
    }
}

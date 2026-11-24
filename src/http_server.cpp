#include "http_server.h"
#include "http_protocol.h"
#include "http_transfer.h"
#include "chores_http.h"
#include "chores_page.h"
#include "dashboard_page.h"
#include "config_page.h"
#include "config_editor.h"
#include "oauth_state.h"
#include <sodium.h>
#include "auth.h"
#include "media_item.h"
#include "util.h"
#include "config.h"
#include "mqtt.h"
#include "google_photos.h"
#include "news_ticker.h"
#include "calendar.h"
#include "stock_streamer.h"
#include "renderer.h"

#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <chrono>
#include <cctype>
#include <format>
#include <thread>
#include <functional>
#include <fstream>
#include <cstring>
#include <unordered_map>
#include <cmath>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <poll.h>
#include <cstdlib>
#include <csignal>

extern std::vector<MediaItem> g_eligible;
extern int current_idx;
extern std::mutex g_playlist_mtx;

static OAuthState g_oauth_state;
static std::jthread g_server_thread;
static std::atomic<bool> g_server_running{false};
static std::atomic<int> g_listen_fd{-1};
static std::atomic<int> g_active_connections{0};

struct TrackedThreadInfo {
    std::jthread thread;
    std::shared_ptr<std::atomic<bool>> finished;
};
static std::vector<TrackedThreadInfo> g_http_client_threads;
static std::mutex g_http_threads_mtx;

static std::vector<int> g_active_client_fds;
static std::mutex g_active_fds_mtx;
static std::mutex g_capture_request_mtx;
static std::mutex g_infopanels_lifecycle_mtx;

static std::atomic<int64_t> g_last_next_cmd{0};
static std::atomic<int64_t> g_last_prev_cmd{0};
static constexpr int64_t CMD_COOLDOWN_MS = 500;

void register_client_fd(int fd) {
    std::lock_guard<std::mutex> lk(g_active_fds_mtx);
    g_active_client_fds.push_back(fd);
}

static void unregister_client_fd(int fd) {
    std::lock_guard<std::mutex> lk(g_active_fds_mtx);
    auto it = std::find(g_active_client_fds.begin(), g_active_client_fds.end(), fd);
    if (it != g_active_client_fds.end()) {
        g_active_client_fds.erase(it);
    }
}

static bool spawn_tracked_thread(std::function<void()> func) {
    std::lock_guard lock(g_http_threads_mtx);
    if (!g_server_running.load()) return false;
    for (auto it = g_http_client_threads.begin(); it != g_http_client_threads.end();) {
        if (it->finished->load()) {
            if (it->thread.joinable()) it->thread.join();
            it = g_http_client_threads.erase(it);
        } else ++it;
    }
    // Include background requests in the budget; HTTP connections already have a
    // separate cap. Reserve storage before starting a thread that may spawn work.
    if (g_http_client_threads.size() >= 64) return false;
    try {
        g_http_client_threads.reserve(g_http_client_threads.size() + 1);
        auto finished = std::make_shared<std::atomic<bool>>(false);
        std::jthread thread;
        if (!spawn_thread_safe(thread, "http_client", [func = std::move(func), finished] {
            try { func(); } catch (...) {}
            finished->store(true);
        })) return false;
        g_http_client_threads.push_back({std::move(thread), finished});
        return true;
    } catch (...) { return false; }
}

static std::string parse_json_value(const std::string& json, const std::string& key) {
    if (auto key_pos = json.find("\"" + key + "\""); key_pos == std::string::npos) return "";
    else if (auto colon_pos = json.find(":", key_pos); colon_pos == std::string::npos) return "";
    else if (auto quote_start = json.find("\"", colon_pos); quote_start == std::string::npos) return "";
    else {
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
}

static bool has_query_param(const std::string& request, const std::string& key) {
    return http_protocol::query_param(request, key).has_value();
}
static std::string get_query_param(const std::string& request, const std::string& key) {
    return http_protocol::query_param(request, key).value_or("");
}
// Escape a value for interpolation into an HTML text/attribute context. The OAuth
// pages splice request-derived strings (?error=, the Host header) into markup, which
// was a reflected-XSS sink.
static std::string html_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;         break;
        }
    }
    return out;
}

static std::string get_setup_html() {
    return webui::start("Google Photos", "settings") + R"PAGE(
<div class="page-heading"><div><h1>Google Photos</h1><p>Choose photos and videos to add to the frame.</p></div></div>
<div class="stack">
  <section class="card"><div class="card-heading"><h2>Import media</h2></div>
    <div class="row wrap"><button id="pick" class="primary">Start an import</button>
    <a id="picker" class="button" target="_blank" rel="noopener noreferrer" hidden>Open Google Photos</a></div>
    <p id="picker-status" class="status" role="status"></p>
  </section>
  <section class="card"><div class="card-heading"><h2>Link Google Photos</h2></div>
    <p class="hint">Enable the Google Photos Picker API for your Google Cloud project. Use a web OAuth client and add the loopback redirect URI shown below.</p>
    <p id="oauth-uri" class="hint"></p>
    <form id="link-photos">
      <div class="field"><label for="client-id">OAuth client ID</label><input id="client-id" required autocomplete="off" maxlength="512"></div>
      <div class="field"><label for="client-secret">OAuth client secret</label><input type="password" id="client-secret" required autocomplete="new-password" maxlength="512"></div>
      <button type="submit" class="primary">Link account</button>
    </form><p id="oauth-status" class="status" role="status"></p>
  </section>
</div>
<script>
document.addEventListener('DOMContentLoaded',()=>{
 const { $,request,onUnlock }=FrameUI;
 const loopback=location.hostname==='localhost'||location.hostname==='127.0.0.1';
 $('oauth-uri').textContent=loopback?location.origin+'/google_photos_callback':'Open this page through a localhost SSH tunnel to link an account.';
 $('link-photos').hidden=!loopback;
 let timer;
 const update=data=>{
   clearTimeout(timer);
   const labels={ready:'Start an import after linking your Google account.',picking:'Open Google Photos, choose your media, and select Done.',importing:'Importing your selection…',complete:'Imported '+data.imported+' new items.',partial:'Imported '+data.imported+' new items. '+data.failed+' could not be saved.',expired:'Selection expired. Start a new import.',failed:'Import failed. Start a new import.',unlinked:'Link your Google Photos account first.',authorization_failed:'Google did not create a Picker session. Check the enabled API and relink your account.'};
   $('picker-status').textContent=labels[data.state]||'';
   const validLink=data.state==='picking'&&data.picker_url?.startsWith('https://photos.google.com/');
   const open=$('picker');open.hidden=!validLink;if(validLink)open.href=data.picker_url;else open.removeAttribute('href');
   $('pick').disabled=data.state==='picking'||data.state==='importing';
   if($('pick').disabled)timer=setTimeout(refresh,5000);
 };
 const refresh=async()=>{try{update(await request('/api/google_photos/picker'));}catch(e){$('picker-status').textContent=e.message;}};
 onUnlock(refresh);
 $('pick').addEventListener('click',async()=>{
   $('pick').disabled=true;$('picker-status').textContent='Starting a Google Photos selection…';
   try{update(await request('/api/google_photos/picker',{fields:{},timeout:40000}));}catch(e){$('picker-status').textContent=e.message;$('pick').disabled=false;}
 });
 $('link-photos').addEventListener('submit',async e=>{
   e.preventDefault();const secret=$('client-secret');const button=e.currentTarget.querySelector('button');
   button.disabled=true;
   try{const result=await request('/google_photos_setup',{fields:{client_id:$('client-id').value,client_secret:secret.value}});secret.value='';
     if(result.authorization_url?.startsWith('https://accounts.google.com/'))location.assign(result.authorization_url);
   }catch(error){$('oauth-status').textContent=error.message;}finally{button.disabled=false;}
 });
 refresh();
});
</script>
)PAGE" + webui::end(VERSION);
}

static std::string get_success_html() {
    return webui::start("Google Photos linked", "settings") +
        R"PAGE(<section class="card stack"><h1>Google Photos linked</h1><p>Open the importer to choose the photos and videos you want on the frame.</p>
        <div><a class="button primary" href="/google_photos_setup">Choose media</a></div></section>)PAGE" +
        webui::end(VERSION);
}

static std::string get_error_html(const std::string& message) {
    return webui::start("Integration error", "settings") +
        "<section class=\"card stack\"><h1>Integration failed</h1><p>" + html_escape(message) +
        "</p><div><a class=\"button primary\" href=\"/google_photos_setup\">Try again</a></div></section>" +
        webui::end(VERSION);
}

static std::string escape_json(const std::string& s) {
    std::string result;
    result.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b':  result += "\\b"; break;
            case '\f':  result += "\\f"; break;
            case '\n':  result += "\\n"; break;
            case '\r':  result += "\\r"; break;
            case '\t':  result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) <= 0x1f) {
                    result += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                } else {
                    result += c;
                }
        }
    }
    return result;
}

static std::string get_api_status(bool include_private_details) {
    int idx = 0;
    int total = 0;
    std::string filename;
    std::string type;
    bool shuffle = false;
    bool paused = false;

    {
        std::lock_guard<std::mutex> lk(g_playlist_mtx);
        total = std::ssize(g_eligible);
        if (total > 0 && current_idx >= 0 && current_idx < total) {
            idx = current_idx;
            if (include_private_details) filename = g_eligible[current_idx].filename;
            type = g_eligible[current_idx].type;
        }
    }
    
    bool mqtt_enabled = false;
    std::string mqtt_broker = "";
    int mqtt_port = 1883;
    bool screen_blanked = g_screen_blanked.load();
    double transition_delay = 15.0;

    {
        std::shared_lock lk(g_config_mtx);
        shuffle = g_cfg.shuffle;
        mqtt_enabled = g_cfg.mqtt_enabled;
        if (include_private_details) mqtt_broker = g_cfg.mqtt_broker;
        mqtt_port = g_cfg.mqtt_port;
        transition_delay = g_cfg.transition_delay;
    }

    paused = g_slideshow_paused.load();

    // Query SoC Temp (Linux sys thermal zone 0)
    double temp_c = 0.0;
    std::ifstream temp_file("/sys/class/thermal/thermal_zone0/temp");
    if (temp_file.is_open()) {
        int raw_temp;
        if (temp_file >> raw_temp) {
            temp_c = raw_temp / 1000.0;
        }
        temp_file.close();
    }

    // Query SQLite cache size
    double db_mb = 0.0;
    try {
        std::string cache_dir;
        { std::shared_lock lk(g_config_mtx); cache_dir = g_cfg.cache_dir; }
        std::string db_path = cache_dir + "/cache.db";
        if (std::filesystem::exists(db_path)) {
            db_mb = std::filesystem::file_size(db_path) / (1024.0 * 1024.0);
        }
    } catch (...) {}

    std::string temp_str = temp_c > 0.0 ? std::format("{:.1f}\u00b0C", temp_c) : std::string("N/A");
    std::string db_str = db_mb > 0.0 ? std::format("{:.2f} MB", db_mb) : std::string("0.00 MB");

    // Query uptime
    std::string uptime_str = "N/A";
    {
        std::ifstream uptime_file("/proc/uptime");
        if (uptime_file.is_open()) {
            double uptime_sec;
            if (uptime_file >> uptime_sec) {
                int days = static_cast<int>(uptime_sec) / 86400;
                int hours = (static_cast<int>(uptime_sec) % 86400) / 3600;
                int mins = (static_cast<int>(uptime_sec) % 3600) / 60;
                if (days > 0) {
                    uptime_str = std::format("{}d {}h", days, hours);
                } else {
                    uptime_str = std::format("{}h {}m", hours, mins);
                }
            }
            uptime_file.close();
        }
    }

    // Query memory usage
    std::string mem_str = "N/A";
    {
        std::ifstream meminfo("/proc/meminfo");
        if (meminfo.is_open()) {
            std::string line;
            uint64_t mem_total = 0, mem_available = 0;
            while (std::getline(meminfo, line)) {
                const auto colon = line.find(':');
                const auto name = std::string_view(line).substr(0, colon);
                if (name == "MemTotal" || name == "MemAvailable") {
                    uint64_t kilobytes = 0;
                    std::istringstream value(line.substr(colon + 1));
                    if (value >> kilobytes) {
                        (name == "MemTotal" ? mem_total : mem_available) = kilobytes / 1024;
                    }
                }
            }
            meminfo.close();
            if (mem_total > 0) {
                const auto used_mb = mem_total - std::min(mem_total, mem_available);
                const auto pct = static_cast<int>(100.0 * used_mb / mem_total);
                mem_str = std::format("{}MB/{}MB ({}%)", used_mb, mem_total, pct);
            }
        }
    }

    // Report the filesystem that backs the app. No subprocess is needed per poll.
    std::string disk_str = "N/A";
    struct statvfs disk{};
    if (statvfs("/", &disk) == 0 && disk.f_blocks >= disk.f_bfree) {
        const auto used = static_cast<long double>(disk.f_blocks - disk.f_bfree);
        const auto accessible = used + disk.f_bavail;
        if (accessible > 0)
            disk_str = std::format("{}%", static_cast<int>(std::ceil(100 * used / accessible)));
    }

    // Determine MQTT status string
    std::string mqtt_status = mqtt_enabled ? "enabled" : "disabled";

    return std::format(R"JSON({{
  "index": {},
  "total": {},
  "filename": "{}",
  "is_video": {},
  "shuffle": {},
  "paused": {},
  "uptime": "{}",
  "temp": "{}",
  "mem": "{}",
  "disk": "{}",
  "db_size": "{}",
  "mqtt_status": "{}",
  "mqtt_broker": "{}",
  "mqtt_port": {},
  "screen_blanked": {},
  "item_timer": {},
  "transition_delay": {}
}})JSON", idx, total, escape_json(filename),
        (type == "video" ? "true" : "false"),
        (shuffle ? "true" : "false"),
        (paused ? "true" : "false"),
        uptime_str, temp_str, mem_str, disk_str, db_str,
        mqtt_status, escape_json(mqtt_broker), mqtt_port,
        (screen_blanked ? "true" : "false"),
        g_item_timer.load(), transition_delay);
}

static std::string get_api_settings() {
    std::shared_lock lk(g_config_mtx);
    return std::format(R"JSON({{
  "transition_delay": {},
  "transition_duration": {},
  "transition_effect": "{}",
  "ken_burns": {},
  "ken_burns_speed": {},
  "shuffle": {},
  "play_just_photos": {},
  "play_just_videos": {},
  "twin_portrait_enabled": {},
  "video_volume": {},
  "timer_enabled": {},
  "filename_enabled": {},
  "count_enabled": {},
  "date_overlay_enabled": {},
  "clock_enabled": {},
  "blurred_background": {},
  "color_matched_matte": {},
  "matting": {},
  "matting_size": {},
  "mqtt_enabled": {},
  "mqtt_broker": "{}",
  "mqtt_port": {},
  "mqtt_topic_prefix": "{}",
  "google_photos_enabled": {},
  "google_photos_album_id": "{}",
  "google_photos_sync_interval": {},
  "api_key_set": {},
  "touch_enabled": {},
  "geotag_enabled": {},
  "geotag_x": {},
  "geotag_y": {},
  "geotag_offset_x": {},
  "geotag_offset_y": {},
  "geotag_font_size": {},
  "geotag_color": "{}",
  "log_level": "{}",
  "log_keep_count": {},
  "timezone": "{}",
  "infopanels_enabled": {},
  "left_panel_enabled": {},
  "left_panel_max_chores": {},
  "left_panel_show_completed": {},
  "left_panel_clock_enabled": {},
  "news_enabled": {},
  "news_source": "{}",
  "news_local_query": "{}",
  "news_blacklist": "{}",
  "news_refresh_minutes": {},
  "news_scroll_speed": {},
  "news_font_size": {},
  "gcalendar_enabled": {},
  "gcalendar_source_type": "{}",
  "gcalendar_ical_url_set": {},
  "gcalendar_name": "{}",
  "gcalendar_refresh_minutes": {},
  "gcalendar_max_events": {}
}})JSON",
        g_cfg.transition_delay,
        g_cfg.transition_duration,
        escape_json(g_cfg.transition_effect),
        g_cfg.ken_burns,
        g_cfg.ken_burns_speed,
        g_cfg.shuffle,
        g_cfg.play_just_photos,
        g_cfg.play_just_videos,
        g_cfg.twin_portrait_enabled,
        g_cfg.video_volume,
        g_cfg.timer_enabled,
        g_cfg.filename_enabled,
        g_cfg.count_enabled,
        g_cfg.date_overlay_enabled,
        g_cfg.clock_enabled,
        g_cfg.blurred_background,
        g_cfg.color_matched_matte,
        g_cfg.matting,
        g_cfg.matting_size,
        g_cfg.mqtt_enabled,
        escape_json(g_cfg.mqtt_broker),
        g_cfg.mqtt_port,
        escape_json(g_cfg.mqtt_topic_prefix),
        g_cfg.google_photos_enabled,
        escape_json(g_cfg.google_photos_album_id),
        g_cfg.google_photos_sync_interval,
        !g_cfg.http_api_key.empty(),
        g_cfg.touch_enabled,
        g_cfg.geotag_enabled,
        g_cfg.geotag_x,
        g_cfg.geotag_y,
        g_cfg.geotag_offset_x,
        g_cfg.geotag_offset_y,
        g_cfg.geotag_font_size,
        escape_json(g_cfg.geotag_color),
        escape_json(g_cfg.log_level),
        g_cfg.log_keep_count,
        escape_json(g_cfg.timezone),
        g_cfg.infopanels_enabled,
        g_cfg.left_panel_enabled,
        g_cfg.left_panel_max_chores,
        g_cfg.left_panel_show_completed,
        g_cfg.left_panel_clock_enabled,
        g_cfg.news_enabled,
        escape_json(g_cfg.news_source),
        escape_json(g_cfg.news_local_query),
        escape_json([&](){ std::string r; for (size_t k=0; k<g_cfg.news_blacklist.size(); ++k) { if(k>0) r += ", "; r += g_cfg.news_blacklist[k]; } return r; }()),
        g_cfg.news_refresh_minutes,
        g_cfg.news_scroll_speed,
        g_cfg.news_font_size,
        g_cfg.gcalendar_enabled,
        escape_json(g_cfg.gcalendar_source_type),
        !g_cfg.gcalendar_ical_url.empty(),
        escape_json(g_cfg.gcalendar_name),
        g_cfg.gcalendar_refresh_minutes,
        g_cfg.gcalendar_max_events);
}

static thread_local int response_timeout_ms = 2000;

static const std::string VIDEO_FALLBACK_SVG = R"SVG(
<svg xmlns="http://www.w3.org/2000/svg" width="400" height="300" viewBox="0 0 400 300">
  <defs>
    <linearGradient id="bg" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" stop-color="#1e1b4b"/>
      <stop offset="100%" stop-color="#311042"/>
    </linearGradient>
  </defs>
  <rect width="100%" height="100%" fill="url(#bg)"/>
  <circle cx="200" cy="130" r="45" fill="none" stroke="#a21caf" stroke-width="4" stroke-dasharray="10 5" opacity="0.8"/>
  <path d="M190 110 L220 130 L190 150 Z" fill="#f43f5e"/>
  <text x="200" y="210" font-family="'Outfit', 'Inter', sans-serif" font-size="18" fill="#f8fafc" font-weight="600" text-anchor="middle">Video Playback Active</text>
  <text x="200" y="235" font-family="'Outfit', 'Inter', sans-serif" font-size="12" fill="#94a3b8" text-anchor="middle">Rendering directly on HDMI display</text>
</svg>
)SVG";

static void send_response(int fd, const std::string& status_line, const std::string& mime, const std::string& body) {
    std::string headers = std::format(
        "{}\r\nContent-Type: {}\r\nContent-Length: {}\r\n"
        "Cache-Control: no-store, must-revalidate\r\n"
        "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: frame-ancestors 'none'; base-uri 'none'; object-src 'none'\r\n"
        "Connection: close\r\n\r\n",
        status_line, mime == "text/html" ? "text/html; charset=utf-8" : mime, body.size());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(response_timeout_ms);
    if (http_protocol::send_until(fd, headers, deadline)) http_protocol::send_until(fd, body, deadline);
}

static bool valid_screenshot_name(std::string_view name) {
    return name.ends_with(".png") && name.size() <= 255 &&
           name.find("..") == name.npos && name.find_first_of("/\\") == name.npos &&
           name.find('\0') == name.npos;
}

static bool stream_regular_file(int fd, const std::string& path, std::string_view mime, const char* not_found_msg) {
    const int file_fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (file_fd < 0) {
        send_response(fd, "HTTP/1.1 404 Not Found", "text/plain", not_found_msg);
        return false;
    }
    struct FdGuard {
        int f;
        ~FdGuard() { if (f >= 0) ::close(f); }
    } guard{file_fd};

    struct stat st{};
    if (::fstat(file_fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        send_response(fd, "HTTP/1.1 404 Not Found", "text/plain", not_found_msg);
        return false;
    }
    auto remaining = static_cast<uintmax_t>(st.st_size);
    const auto headers = std::format(
        "HTTP/1.1 200 OK\r\nContent-Type: {}\r\nContent-Length: {}\r\n"
        "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n\r\n",
        mime, remaining);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(response_timeout_ms);
    if (!http_protocol::send_until(fd, headers, deadline)) return false;

    char buffer[8192];
    while (remaining && std::chrono::steady_clock::now() < deadline) {
        const size_t want = static_cast<size_t>(std::min<uintmax_t>(sizeof(buffer), remaining));
        const ssize_t bytes = ::read(file_fd, buffer, want);
        if (bytes < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (bytes == 0) break;
        if (!http_protocol::send_until(fd, std::string_view(buffer, static_cast<size_t>(bytes)), deadline)) break;
        remaining -= static_cast<uintmax_t>(bytes);
    }
    return remaining == 0;
}

static void handle_screenshot(int fd) {
    std::unique_lock capture(g_capture_request_mtx, std::try_to_lock);
    if (!capture.owns_lock()) {
        send_response(fd, "HTTP/1.1 409 Conflict", "text/plain", "Screenshot capture already in progress");
        return;
    }
    std::string dir = "/app/screenshots";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y%m%d_%H%M%S", &tm_buf);

    std::string shot_filename = std::string("screenshot_") + time_str + "_" + std::to_string(SDL_GetTicks()) + ".png";
    std::string shot_path = dir + "/" + shot_filename;

    {
        std::lock_guard lk(g_screenshot_mtx);
        g_screenshot_ready.store(false, std::memory_order_release);
        g_screenshot_ok.store(false, std::memory_order_release);
        g_screenshot_out_path = shot_path;
    }
    g_screenshot_requested.store(true, std::memory_order_release);

    std::unique_lock lk(g_screenshot_mtx);
    bool ready = g_screenshot_cv.wait_for(lk, std::chrono::milliseconds(5000), []{
        return g_screenshot_ready.load(std::memory_order_acquire);
    });

    if (!ready) {
        g_screenshot_requested.store(false, std::memory_order_release);
        lk.unlock();
        send_response(fd, "HTTP/1.1 504 Gateway Timeout", "text/plain", "Screenshot capture timed out");
        return;
    }
    const bool ok = g_screenshot_ok.load(std::memory_order_acquire);
    lk.unlock(); // File I/O and socket writes must not hold the renderer's state mutex.

    if (!ok) {
        send_response(fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Failed to capture screenshot");
        return;
    }

    // Auto-prune older screenshots keeping the latest 50
    try {
        std::vector<std::filesystem::directory_entry> entries;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (std::filesystem::is_regular_file(entry.symlink_status()) &&
                valid_screenshot_name(entry.path().filename().string())) {
                entries.push_back(entry);
            }
        }
        if (entries.size() > 50) {
            std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
                return a.last_write_time() < b.last_write_time();
            });
            for (size_t i = 0; i < entries.size() - 50; ++i) {
                std::filesystem::remove(entries[i].path(), ec);
            }
        }
    } catch (...) {}

    stream_regular_file(fd, shot_path, "image/png", "Failed to open screenshot file");
}

static void handle_screenshots_list(int fd) {
    std::string dir = "/app/screenshots";
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> entries;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (std::filesystem::is_regular_file(entry.symlink_status()) &&
                valid_screenshot_name(entry.path().filename().string())) {
                entries.push_back(entry);
            }
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.last_write_time() > b.last_write_time(); // newest first
        });
    } catch (...) {}

    std::ostringstream ss;
    ss << "[";
    bool first = true;
    for (const auto& entry : entries) {
        const auto size = entry.file_size(ec);
        if (ec) continue; // A capture may have aged out after we listed the directory.
        if (!first) ss << ",";
        first = false;
        const auto name = entry.path().filename().string();
        ss << "{\"filename\":\"" << escape_json(name) << "\",\"size_bytes\":" << size
           << ",\"url\":\"/api/screenshots/" << http_protocol::url_encode(name) << "\"}";
    }
    ss << "]";
    send_response(fd, "HTTP/1.1 200 OK", "application/json", ss.str());
}

static void handle_screenshot_file(int fd, const std::string& filename) {
    if (!valid_screenshot_name(filename)) {
        send_response(fd, "HTTP/1.1 400 Bad Request", "text/plain", "Invalid filename");
        return;
    }
    const std::string path = "/app/screenshots/" + filename;
    stream_regular_file(fd, path, "image/png", "Screenshot not found");
}

static void handle_preview(int fd) {
    std::string path = "";
    std::string type = "image";

    {
        std::lock_guard<std::mutex> lk(g_playlist_mtx);
        if (!g_eligible.empty() && current_idx >= 0 && current_idx < std::ssize(g_eligible)) {
            path = g_eligible[current_idx].path;
            type = g_eligible[current_idx].type;
        }
    }

    // Video Fallback Card
    if (type == "video" || path.empty()) {
        send_response(fd, "HTTP/1.1 200 OK", "image/svg+xml", VIDEO_FALLBACK_SVG);
        return;
    }

    // Determine MIME type
    std::string mime = "image/jpeg";
    std::string lower_path = path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::string ext = std::filesystem::path(lower_path).extension().string();
    if (ext == ".png") mime = "image/png";
    else if (ext == ".webp") mime = "image/webp";
    else if (ext == ".gif") mime = "image/gif";

    stream_regular_file(fd, path, mime, "File not found on disk");
}

static void handle_client(int client_fd, uint32_t peer_ip) {
    struct ClientFdGuard {
        int fd;
        ClientFdGuard(int f) : fd(f) { try { register_client_fd(fd); } catch (...) { close(fd); throw; } }
        ~ClientFdGuard() { unregister_client_fd(fd); close(fd); }
    };
    ClientFdGuard guard(client_fd);

    int socket_timeout_secs = 5;
    { std::shared_lock lk(g_config_mtx); socket_timeout_secs = g_cfg.http_socket_timeout; }
    response_timeout_ms = std::clamp<int>(socket_timeout_secs, 2, 60) * 1000;

    std::string request;
    http_protocol::Request parsed_request;
    auto received = http_protocol::receive(client_fd, request, parsed_request, response_timeout_ms);
    if (received != http_protocol::Result::complete) {
        const char* status = received == http_protocol::Result::too_large ? "HTTP/1.1 413 Payload Too Large" :
                             received == http_protocol::Result::timeout ? "HTTP/1.1 408 Request Timeout" :
                             "HTTP/1.1 400 Bad Request";
        send_response(client_fd, status, "text/plain", "Incomplete or invalid HTTP request");
        return;
    }
    const auto host = parsed_request.headers.find("host");
    if (host == parsed_request.headers.end() || !http_protocol::controller_host_allowed(host->second)) {
        send_response(client_fd, "HTTP/1.1 400 Bad Request", "text/plain",
                      "Open piTrove using its LAN IP address or .local hostname");
        return;
    }
    auto matches_route = [&](std::string_view method, std::string_view path) {
        return parsed_request.method == method && parsed_request.path == path;
    };

    if (!request.empty()) {
        // Each source has its own bounded failed-attempt window.
        using Clock = std::chrono::steady_clock;
        struct AuthAttempts {
            Clock::time_point expires;
            unsigned failures;
        };
        static std::mutex auth_mutex;
        static std::unordered_map<uint32_t, AuthAttempts> attempts;
        constexpr unsigned max_failures = 5;
        constexpr auto lockout = std::chrono::minutes(1);
        const auto configured_key = [] {
            std::shared_lock lock(g_config_mtx);
            return g_cfg.http_api_key;
        };
        const auto valid_key = [&](std::string_view api_key) {
            const auto header = parsed_request.headers.find("authorization");
            if (header == parsed_request.headers.end()) return false;
            std::string_view supplied = header->second;
            if (supplied.size() >= 7 && strncasecmp(supplied.data(), "Bearer ", 7) == 0)
                supplied.remove_prefix(7);
            return supplied.size() == api_key.size() &&
                   sodium_memcmp(supplied.data(), api_key.data(), api_key.size()) == 0;
        };

        auto is_authorized = [&](bool* status_details = nullptr) {
            const auto api_key = configured_key();
            const bool has_credentials = parsed_request.headers.contains("authorization");
            if (status_details && !has_credentials) {
                *status_details = api_key.empty();
                return true;
            }
            if (status_details) *status_details = true;
            if (!http_protocol::controller_origin_allowed(parsed_request)) {
                send_response(client_fd, "HTTP/1.1 403 Forbidden", "text/plain",
                              "Open this action from your piTrove controller");
                return false;
            }
            // Opt-in: with no key configured the control API stays open. Reported at
            // startup rather than enforced, so an absent key is a deliberate choice.
            if (api_key.empty()) return true;

            if (!has_credentials) {
                send_response(client_fd, "HTTP/1.1 401 Unauthorized", "text/plain", "API key required");
                return false;
            }

            bool limited = false;
            {
                std::lock_guard lock(auth_mutex);
                const auto now = Clock::now();
                std::erase_if(attempts, [&](const auto& item) { return item.second.expires <= now; });
                const auto previous = attempts.find(peer_ip);
                if (previous != attempts.end() && previous->second.failures >= max_failures) {
                    limited = true;
                } else if (valid_key(api_key)) {
                    if (previous != attempts.end()) attempts.erase(previous);
                    return true;
                } else if (previous != attempts.end()) {
                    if (++previous->second.failures == max_failures) previous->second.expires = now + lockout;
                } else if (attempts.size() < 1024) {
                    attempts.emplace(peer_ip, AuthAttempts{now + lockout, 1});
                } else {
                    limited = true;
                }
            }
            send_response(client_fd, limited ? "HTTP/1.1 429 Too Many Requests" : "HTTP/1.1 401 Unauthorized",
                          "text/plain", limited ? "Too many failed attempts" : "Invalid API key");
            return false;
        };

        // Query parameters can contain OAuth codes or credentials. Log only the route.
        g_logger.info("HTTP: Request: {} {}", parsed_request.method, parsed_request.path);

        // Very simple router
        if (matches_route("GET", "/") || matches_route("GET", "/dashboard")) {
            send_response(client_fd, "HTTP/1.1 200 OK", "text/html", get_dashboard_html(VERSION));
        } 
        else if (matches_route("GET", "/chores")) {
            send_response(client_fd, "HTTP/1.1 200 OK", "text/html; charset=utf-8", get_chores_page(VERSION));
        }
        else if (matches_route("GET", "/config")) {
            send_response(client_fd, "HTTP/1.1 200 OK", "text/html; charset=utf-8", get_config_page(VERSION));
        }
        else if (parsed_request.path == "/api/config") {
            if (!is_authorized()) return;
            config_editor::Result result;
            if (parsed_request.method == "GET") {
                std::shared_lock lock(g_config_mtx);
                result = config_editor::handle(g_cfg, parsed_request.method, request, "");
            } else {
                std::unique_lock lock(g_config_mtx);
                const auto path = g_cfg.loaded_path.empty() ? "/app/config/config.toml" : g_cfg.loaded_path;
                result = config_editor::handle(g_cfg, parsed_request.method, request, path);
            }
            // Each subsystem reads config under its own lock; never hold the editor lock here.
            if (result.status == 200 && !result.changed.empty()) {
                const auto touched = [&](std::string_view prefix) {
                    return std::any_of(result.changed.begin(), result.changed.end(),
                                       [&](const std::string& id) { return id.starts_with(prefix); });
                };
                const bool master_changed = std::find(result.changed.begin(), result.changed.end(),
                                                       "infopanels.enabled") != result.changed.end();
                const bool update_news = master_changed || touched("news.");
                const bool update_calendar = master_changed || touched("gcalendar.") || touched("mqtt.timezone");
                const bool update_stocks = master_changed || touched("stockstreamer.");
                const bool update_photos = touched("google_photos.");
                if (update_news || update_calendar || update_stocks || update_photos || touched("log.level")) {
                    // Update against the latest saved config after entering the lifecycle lock.
                    // A concurrent save can finish while the previous request joins a worker.
                    std::lock_guard lifecycle(g_infopanels_lifecycle_mtx);
                    bool news, calendar, stocks, photos;
                    std::string level;
                    {
                        std::shared_lock lock(g_config_mtx);
                        news = g_cfg.infopanels_enabled && g_cfg.news_enabled;
                        calendar = g_cfg.infopanels_enabled && g_cfg.gcalendar_enabled;
                        stocks = g_cfg.infopanels_enabled && g_cfg.stockstreamer_enabled;
                        photos = g_cfg.google_photos_enabled;
                        level = g_cfg.log_level;
                    }
                    const auto reconcile = [](auto& integration, bool enabled) {
                        if (!enabled) integration.stop();
                        else if (integration.is_running()) integration.sync();
                        else integration.start();
                    };
                    if (update_news) reconcile(g_news_ticker, news);
                    if (update_calendar) reconcile(g_calendar, calendar);
                    if (update_stocks) reconcile(g_stock_streamer, stocks);
                    if (update_photos) {
                        g_google_photos.stop();
                        if (photos) g_google_photos.start();
                    }
                    if (touched("log.level")) {
                        g_logger.level.store(level == "debug" || level == "trace" ? LogLevel::DEBUG :
                            level == "warn" || level == "warning" ? LogLevel::WARN :
                            level == "error" ? LogLevel::ERROR : LogLevel::INFO);
                    }
                }
            }
            send_response(client_fd, "HTTP/1.1 " + std::to_string(result.status) + (result.status==200?" OK":" Error"), "application/json; charset=utf-8", result.body);
        }
        else if (parsed_request.path == "/api/chores" || parsed_request.path.starts_with("/api/chores/")) {
            if (!is_authorized()) return;
            std::string timezone;
            { std::shared_lock lock(g_config_mtx); timezone = g_cfg.timezone; }
            std::string today = format_epoch_tz(time(nullptr), timezone, "%Y-%m-%d");
            if (!chores::valid_date(today)) today = format_epoch_tz(time(nullptr), "UTC", "%Y-%m-%d");
            const auto response = chores::handle_http(chores::g_store, parsed_request.method,
                                                     parsed_request.path, request, today, timezone);
            send_response(client_fd, "HTTP/1.1 " + std::to_string(response.status) +
                          (response.status == 200 ? " OK" : " Error"), "application/json; charset=utf-8", response.body);
        }
        else if (matches_route("GET", "/google_photos_setup")) {
            send_response(client_fd, "HTTP/1.1 200 OK", "text/html; charset=utf-8", get_setup_html());
        }
        else if (matches_route("POST", "/google_photos_setup")) {
            if (!is_authorized()) return;
            const auto hostname = std::string_view(host->second).substr(0, host->second.find(':'));
            if ((hostname != "localhost" && hostname != "127.0.0.1") ||
                parsed_request.target != parsed_request.path) {
                send_response(client_fd, "HTTP/1.1 400 Bad Request", "text/plain",
                              "Use the localhost controller to link Google Photos.");
                return;
            }
            const auto client_id = get_query_param(request, "client_id");
            const auto client_secret = get_query_param(request, "client_secret");
            if (client_id.empty() || client_id.size() > 512 || client_secret.empty() || client_secret.size() > 512) {
                send_response(client_fd, "HTTP/1.1 400 Bad Request", "text/plain", "Enter both OAuth credentials.");
                return;
            }
            const std::string redirect_uri = "http://" + host->second + "/google_photos_callback";
            const auto state = g_oauth_state.begin(client_id, client_secret, redirect_uri);
            if (state.empty()) {
                send_response(client_fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Failed to initialize authorization");
                return;
            }
            const auto url = "https://accounts.google.com/o/oauth2/v2/auth?client_id=" + http_protocol::url_encode(client_id) +
                "&redirect_uri=" + http_protocol::url_encode(redirect_uri) +
                "&response_type=code&scope=https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fphotospicker.mediaitems.readonly&access_type=offline&prompt=consent&state=" + state;
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"authorization_url\":\"" + escape_json(url) + "\"}");
        }
        else if (matches_route("GET", "/api/google_photos/picker") || matches_route("POST", "/api/google_photos/picker")) {
            if (!is_authorized()) return;
            const auto response = parsed_request.method == "POST" ? g_google_photos.create_picker() : g_google_photos.picker_status();
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", response);
        }
        else if (matches_route("GET", "/google_photos_callback")) {
            auto flow = g_oauth_state.consume(get_query_param(request, "state"));
            if (!flow) {
                send_response(client_fd, "HTTP/1.1 400 Bad Request", "text/plain", "Invalid or expired authorization state; restart setup");
                return;
            }
            std::string code = get_query_param(request, "code");
            std::string error = get_query_param(request, "error");
            
            if (!error.empty()) {
                send_response(client_fd, "HTTP/1.1 200 OK", "text/html", get_error_html("Access denied or authentication cancelled: " + error));
            } else if (code.empty()) {
                send_response(client_fd, "HTTP/1.1 200 OK", "text/html", get_error_html("OAuth code parameter is missing."));
            } else {
                // Keep credentials out of shell arguments; encode every form value.
                std::string body = "client_id=" + http_protocol::url_encode(flow->client_id) +
                    "&client_secret=" + http_protocol::url_encode(flow->client_secret) +
                    "&code=" + http_protocol::url_encode(code) +
                    "&redirect_uri=" + http_protocol::url_encode(flow->redirect_uri) +
                    "&grant_type=authorization_code";
                std::string json;
                const bool exchanged = http_transfer::post_form("https://oauth2.googleapis.com/token", body, json);
                std::string refresh_token = exchanged ? parse_json_value(json, "refresh_token") : "";
                
                if (refresh_token.empty()) {
                    // Try to parse error details
                    std::string err_desc = parse_json_value(json, "error_description");
                    if (err_desc.empty()) err_desc = parse_json_value(json, "error");
                    if (err_desc.empty()) err_desc = "Could not obtain refresh token. Note that Google only sends the refresh_token on the FIRST authorization. If you are re-authorizing, go to your Google Account and remove piTrove's permissions first, then retry.";
                    
                    send_response(client_fd, "HTTP/1.1 200 OK", "text/html", get_error_html(err_desc));
                } else {
                    // Preserve the previous live settings if persistence fails.
                    {
                        std::lock_guard lock(g_config_mtx);
                        Config previous = g_cfg;
                        g_cfg.google_photos_client_id = flow->client_id;
                        g_cfg.google_photos_client_secret = flow->client_secret;
                        g_cfg.google_photos_refresh_token = refresh_token;
                        g_cfg.google_photos_enabled = true;
                        std::string save_path = g_cfg.loaded_path.empty() ? "/app/config/config.toml" : g_cfg.loaded_path;
                        if (!g_cfg.save(save_path)) {
                            g_cfg = std::move(previous);
                            send_response(client_fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Failed to save configuration");
                            return;
                        }
                    }

                    // Clear error and restart background sync thread safely
                    for (int code = 301; code <= 318; ++code) clear_error(code);
                    
                    g_google_photos.stop();
                    g_google_photos.start();
                    
                    send_response(client_fd, "HTTP/1.1 200 OK", "text/html", get_success_html());
                }
            }
        } 
        else if (matches_route("GET", "/api/settings/update")) {
            if (!is_authorized()) return;
            bool changed = false;
            bool validation_failed = false;
            std::string err_msg = "";
            // NewsTicker::fetch_sync() takes a shared_lock on g_config_mtx. Calling it
            // from inside the exclusive lock below self-deadlocks the whole process
            // (std::shared_mutex is not recursive), stalling every subsystem that
            // needs a shared lock. Defer it until the lock has been released.
            bool refetch_news = false;
            bool reconfigure_panels = false;
            
            {
                std::lock_guard lock(g_config_mtx);
                Config previous = g_cfg;

                for (const auto* key : {"left_panel_enabled", "left_panel_show_completed", "left_panel_clock_enabled"}) {
                    if (has_query_param(request, key)) {
                        const auto value = get_query_param(request, key);
                        if (value != "0" && value != "1" && value != "true" && value != "false") {
                            validation_failed = true; err_msg = "Choose a valid left panel setting";
                        }
                    }
                }
                if (has_query_param(request, "left_panel_max_chores")) {
                    const int value = safe_stoi(get_query_param(request, "left_panel_max_chores"), -1);
                    if (value < 1 || value > 20) { validation_failed = true; err_msg = "Visible chores must be between 1 and 20"; }
                }
                // 1. Validation Checks
                if (has_query_param(request, "transition_delay")) {
                    double val = safe_stod(get_query_param(request, "transition_delay"), -999.0);
                    if (!std::isfinite(val) || val < 1.0) {
                        validation_failed = true;
                        err_msg = "transition_delay must be >= 1.0";
                    }
                }
                if (!validation_failed && has_query_param(request, "transition_duration")) {
                    double val = safe_stod(get_query_param(request, "transition_duration"), -999.0);
                    if (!std::isfinite(val) || val < 0.1 || val > 10.0) {
                        validation_failed = true;
                        err_msg = "transition_duration must be between 0.1 and 10.0";
                    }
                }
                if (!validation_failed && has_query_param(request, "ken_burns_speed")) {
                    double val = safe_stod(get_query_param(request, "ken_burns_speed"), -999.0);
                    if (!std::isfinite(val) || val < 0.001 || val > 5.0) {
                        validation_failed = true;
                        err_msg = "ken_burns_speed must be between 0.001 and 5.0";
                    }
                }
                if (!validation_failed && has_query_param(request, "video_volume")) {
                    int val = safe_stoi(get_query_param(request, "video_volume"), -999);
                    if (val < 0 || val > 150) {
                        validation_failed = true;
                        err_msg = "video_volume must be between 0 and 150";
                    }
                }
                if (!validation_failed && has_query_param(request, "mqtt_port")) {
                    int val = safe_stoi(get_query_param(request, "mqtt_port"), -999);
                    if (val < 1 || val > 65535) {
                        validation_failed = true;
                        err_msg = "mqtt_port must be between 1 and 65535";
                    }
                }
                if (!validation_failed && has_query_param(request, "google_photos_sync_interval")) {
                    int val = safe_stoi(get_query_param(request, "google_photos_sync_interval"), -999);
                    if (val < 1) {
                        validation_failed = true;
                        err_msg = "google_photos_sync_interval must be >= 1";
                    }
                }
                if (!validation_failed && has_query_param(request, "gcalendar_ical_url")) {
                    validation_failed = true;
                    err_msg = "Use POST /api/config to update secret URLs";
                }
                if (!validation_failed && has_query_param(request, "timezone")) {
                    const std::string tz_val = get_query_param(request, "timezone");
                    if (!config_editor::is_valid_timezone(tz_val)) {
                        validation_failed = true;
                        err_msg = "Choose an installed display timezone, such as America/Los_Angeles";
                    }
                }
                if (!validation_failed && has_query_param(request, "log_level")) {
                    const std::string lvl = get_query_param(request, "log_level");
                    if (lvl != "debug" && lvl != "trace" && lvl != "info" &&
                        lvl != "warn" && lvl != "warning" && lvl != "error") {
                        validation_failed = true;
                        err_msg = "Invalid log_level";
                    }
                }
                auto valid_text_param = [](const std::string& v, size_t max_len) {
                    return v.size() <= max_len &&
                           std::none_of(v.begin(), v.end(), [](unsigned char ch) { return ch < 32 || ch == 127; });
                };
                for (const auto* key : {"transition_effect", "news_source", "news_local_query",
                                        "gcalendar_name", "mqtt_broker", "mqtt_topic_prefix",
                                        "google_photos_album_id"}) {
                    if (!validation_failed && has_query_param(request, key)) {
                        if (!valid_text_param(get_query_param(request, key), 512)) {
                            validation_failed = true;
                            err_msg = std::string("Invalid characters or length for ") + key;
                        }
                    }
                }
                if (!validation_failed && has_query_param(request, "news_blacklist")) {
                    if (!valid_text_param(get_query_param(request, "news_blacklist"), 4096)) {
                        validation_failed = true;
                        err_msg = "Invalid characters or length for news_blacklist";
                    }
                }
                
                // 2. Apply updates if validation succeeded
                if (!validation_failed) {
                    if (has_query_param(request, "transition_delay")) {
                        double val = safe_stod(get_query_param(request, "transition_delay"), g_cfg.transition_delay);
                        if (g_cfg.transition_delay != val) { g_cfg.transition_delay = val; changed = true; }
                    }
                    if (has_query_param(request, "transition_duration")) {
                        double val = safe_stod(get_query_param(request, "transition_duration"), g_cfg.transition_duration);
                        if (g_cfg.transition_duration != val) { g_cfg.transition_duration = val; changed = true; }
                    }
                    if (has_query_param(request, "transition_effect")) {
                        std::string val = get_query_param(request, "transition_effect");
                        if (!val.empty() && g_cfg.transition_effect != val) { g_cfg.transition_effect = val; changed = true; }
                    }
                    if (has_query_param(request, "ken_burns")) {
                        std::string val = get_query_param(request, "ken_burns");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.ken_burns != desired) { g_cfg.ken_burns = desired; changed = true; }
                    }
                    if (has_query_param(request, "ken_burns_speed")) {
                        double val = safe_stod(get_query_param(request, "ken_burns_speed"), g_cfg.ken_burns_speed);
                        if (g_cfg.ken_burns_speed != val) { g_cfg.ken_burns_speed = val; changed = true; }
                    }
                    if (has_query_param(request, "shuffle")) {
                        std::string val = get_query_param(request, "shuffle");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.shuffle != desired) { g_cfg.shuffle = desired; changed = true; }
                    }
                    if (has_query_param(request, "play_just_photos")) {
                        std::string val = get_query_param(request, "play_just_photos");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.play_just_photos != desired) { g_cfg.play_just_photos = desired; changed = true; }
                    }
                    if (has_query_param(request, "play_just_videos")) {
                        std::string val = get_query_param(request, "play_just_videos");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.play_just_videos != desired) { g_cfg.play_just_videos = desired; changed = true; }
                    }
                    if (has_query_param(request, "twin_portrait_enabled")) {
                        std::string val = get_query_param(request, "twin_portrait_enabled");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.twin_portrait_enabled != desired) { g_cfg.twin_portrait_enabled = desired; changed = true; }
                    }
                    if (has_query_param(request, "video_volume")) {
                        int val = safe_stoi(get_query_param(request, "video_volume"), g_cfg.video_volume);
                        if (g_cfg.video_volume != val) { g_cfg.video_volume = val; changed = true; }
                    }
                    if (has_query_param(request, "timer_enabled")) {
                        std::string val = get_query_param(request, "timer_enabled");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.timer_enabled != desired) { g_cfg.timer_enabled = desired; changed = true; }
                    }
                    if (has_query_param(request, "filename_enabled")) {
                        std::string val = get_query_param(request, "filename_enabled");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.filename_enabled != desired) { g_cfg.filename_enabled = desired; changed = true; }
                    }
                    if (has_query_param(request, "count_enabled")) {
                        std::string val = get_query_param(request, "count_enabled");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.count_enabled != desired) { g_cfg.count_enabled = desired; changed = true; }
                    }
                    if (has_query_param(request, "date_overlay_enabled")) {
                        std::string val = get_query_param(request, "date_overlay_enabled");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.date_overlay_enabled != desired) { g_cfg.date_overlay_enabled = desired; changed = true; }
                    }
                    if (has_query_param(request, "clock_enabled")) {
                        std::string val = get_query_param(request, "clock_enabled");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.clock_enabled != desired) { g_cfg.clock_enabled = desired; changed = true; }
                    }
                    if (has_query_param(request, "blurred_background")) {
                        std::string val = get_query_param(request, "blurred_background");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.blurred_background != desired) { g_cfg.blurred_background = desired; changed = true; }
                    }
                    if (has_query_param(request, "color_matched_matte")) {
                        std::string val = get_query_param(request, "color_matched_matte");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.color_matched_matte != desired) { g_cfg.color_matched_matte = desired; changed = true; }
                    }
                    if (has_query_param(request, "matting")) {
                        std::string val = get_query_param(request, "matting");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.matting != desired) { g_cfg.matting = desired; changed = true; }
                    }
                    if (has_query_param(request, "matting_size")) {
                        std::string val = get_query_param(request, "matting_size");
                        try { int v = std::clamp(std::stoi(val), 0, 500); if (g_cfg.matting_size != v) { g_cfg.matting_size = v; changed = true; } } catch(...) {}
                    }
                    if (has_query_param(request, "geotag_enabled")) {
                        std::string val = get_query_param(request, "geotag_enabled");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.geotag_enabled != desired) { g_cfg.geotag_enabled = desired; changed = true; }
                    }
                    if (has_query_param(request, "timezone")) {
                        std::string val = get_query_param(request, "timezone");
                        if (!val.empty() && g_cfg.timezone != val) { g_cfg.timezone = val; changed = true; }
                    }
                    if (has_query_param(request, "left_panel_enabled")) {
                        const auto value = get_query_param(request, "left_panel_enabled");
                        const bool desired = value == "1" || value == "true";
                        if (g_cfg.left_panel_enabled != desired) { g_cfg.left_panel_enabled = desired; changed = true; }
                    }
                    if (has_query_param(request, "left_panel_show_completed")) {
                        const auto value = get_query_param(request, "left_panel_show_completed");
                        const bool desired = value == "1" || value == "true";
                        if (g_cfg.left_panel_show_completed != desired) { g_cfg.left_panel_show_completed = desired; changed = true; }
                    }
                    if (has_query_param(request, "left_panel_clock_enabled")) {
                        const auto value = get_query_param(request, "left_panel_clock_enabled");
                        const bool desired = value == "1" || value == "true";
                        if (g_cfg.left_panel_clock_enabled != desired) { g_cfg.left_panel_clock_enabled = desired; changed = true; }
                    }
                    if (has_query_param(request, "left_panel_max_chores")) {
                        const int desired = safe_stoi(get_query_param(request, "left_panel_max_chores"), 10);
                        if (g_cfg.left_panel_max_chores != desired) { g_cfg.left_panel_max_chores = desired; changed = true; }
                    }
                    if (has_query_param(request, "infopanels_enabled")) {
                        std::string val = get_query_param(request, "infopanels_enabled");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.infopanels_enabled != desired) {
                            g_cfg.infopanels_enabled = desired;
                            changed = true;
                            reconfigure_panels = true;
                        }
                    }
                    if (has_query_param(request, "news_enabled")) {
                        std::string val = get_query_param(request, "news_enabled");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.news_enabled != desired) {
                            g_cfg.news_enabled = desired;
                            changed = true;
                            reconfigure_panels = true;
                        }
                    }
                    if (has_query_param(request, "news_source")) {
                        std::string val = get_query_param(request, "news_source");
                        if (!val.empty() && g_cfg.news_source != val) { g_cfg.news_source = val; changed = true; }
                    }
                    if (has_query_param(request, "news_local_query")) {
                        std::string val = trim(get_query_param(request, "news_local_query"));
                        if (!val.empty() && g_cfg.news_local_query != val) {
                            g_cfg.news_local_query = val;
                            changed = true;
                            g_logger.info("HTTP: Updated news local query/zipcode to '{}'", val);
                            refetch_news = true; // deferred: fetch_sync() re-locks g_config_mtx
                        }
                    }
                    if (has_query_param(request, "news_blacklist")) {
                        std::string val = get_query_param(request, "news_blacklist");
                        std::vector<std::string> bl;
                        std::stringstream ss(val);
                        std::string dom;
                        while (std::getline(ss, dom, ',')) {
                            dom = trim(dom);
                            if (!dom.empty()) bl.push_back(dom);
                        }
                        g_cfg.news_blacklist = bl;
                        changed = true;
                        g_logger.info("HTTP: Updated news blacklist ({} entries)", bl.size());
                        refetch_news = true; // deferred: fetch_sync() re-locks g_config_mtx
                    }
                    if (has_query_param(request, "gcalendar_enabled")) {
                        std::string val = get_query_param(request, "gcalendar_enabled");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.gcalendar_enabled != desired) {
                            g_cfg.gcalendar_enabled = desired;
                            changed = true;
                            reconfigure_panels = true;
                        }
                    }

                    if (has_query_param(request, "gcalendar_refresh_minutes")) {
                        std::string val = get_query_param(request, "gcalendar_refresh_minutes");
                        try {
                            int v = std::clamp(std::stoi(val), 5, 120);
                            if (g_cfg.gcalendar_refresh_minutes != v) {
                                g_cfg.gcalendar_refresh_minutes = v;
                                changed = true;
                            }
                        } catch(...) {}
                    }
                    if (has_query_param(request, "gcalendar_name")) {
                        std::string val = get_query_param(request, "gcalendar_name");
                        if (!val.empty() && g_cfg.gcalendar_name != val) { g_cfg.gcalendar_name = val; changed = true; }
                    }
                    if (has_query_param(request, "geotag_offset_x")) {
                        std::string val = get_query_param(request, "geotag_offset_x");
                        try { int v = std::stoi(val); if (g_cfg.geotag_offset_x != v) { g_cfg.geotag_offset_x = v; changed = true; } } catch(...) {}
                    }
                    if (has_query_param(request, "geotag_offset_y")) {
                        std::string val = get_query_param(request, "geotag_offset_y");
                        try { int v = std::stoi(val); if (g_cfg.geotag_offset_y != v) { g_cfg.geotag_offset_y = v; changed = true; } } catch(...) {}
                    }
                    if (has_query_param(request, "log_level")) {
                        std::string val = get_query_param(request, "log_level");
                        if (!val.empty() && g_cfg.log_level != val) {
                            g_cfg.log_level = val;
                            g_cfg.verbose = (val == "debug" || val == "trace");
                            if (val == "debug" || val == "trace") g_logger.level = LogLevel::DEBUG;
                            else if (val == "warn" || val == "warning") g_logger.level = LogLevel::WARN;
                            else if (val == "error") g_logger.level = LogLevel::ERROR;
                            else g_logger.level = LogLevel::INFO;
                            changed = true;
                        }
                    }
                    if (has_query_param(request, "mqtt_enabled")) {
                        std::string val = get_query_param(request, "mqtt_enabled");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.mqtt_enabled != desired) { g_cfg.mqtt_enabled = desired; changed = true; }
                    }
                    if (has_query_param(request, "mqtt_broker")) {
                        std::string val = get_query_param(request, "mqtt_broker");
                        if (!val.empty() && g_cfg.mqtt_broker != val) { g_cfg.mqtt_broker = val; changed = true; }
                    }
                    if (has_query_param(request, "mqtt_port")) {
                        int val = safe_stoi(get_query_param(request, "mqtt_port"), g_cfg.mqtt_port);
                        if (g_cfg.mqtt_port != val) { g_cfg.mqtt_port = val; changed = true; }
                    }
                    if (has_query_param(request, "mqtt_topic_prefix")) {
                        std::string val = get_query_param(request, "mqtt_topic_prefix");
                        if (!val.empty() && g_cfg.mqtt_topic_prefix != val) { g_cfg.mqtt_topic_prefix = val; changed = true; }
                    }
                    if (has_query_param(request, "google_photos_enabled")) {
                        std::string val = get_query_param(request, "google_photos_enabled");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.google_photos_enabled != desired) { g_cfg.google_photos_enabled = desired; changed = true; reconfigure_panels = true; }
                    }
                    if (has_query_param(request, "google_photos_album_id")) {
                        std::string val = get_query_param(request, "google_photos_album_id");
                        if (g_cfg.google_photos_album_id != val) { g_cfg.google_photos_album_id = val; changed = true; }
                    }
                    if (has_query_param(request, "google_photos_sync_interval")) {
                        int val = safe_stoi(get_query_param(request, "google_photos_sync_interval"), g_cfg.google_photos_sync_interval);
                        if (g_cfg.google_photos_sync_interval != val) { g_cfg.google_photos_sync_interval = val; changed = true; }
                    }
                    if (has_query_param(request, "touch_enabled")) {
                        std::string val = get_query_param(request, "touch_enabled");
                        bool desired = (val == "true" || val == "1");
                        if (g_cfg.touch_enabled != desired) { g_cfg.touch_enabled = desired; changed = true; }
                    }
                }
                if (!validation_failed && g_cfg.play_just_photos && g_cfg.play_just_videos) {
                    g_cfg = std::move(previous);
                    validation_failed = true;
                    err_msg = "Choose photos only or videos only, not both";
                }
                if (!validation_failed && changed) {
                    std::string save_path = g_cfg.loaded_path.empty() ? "/app/config/config.toml" : g_cfg.loaded_path;
                    if (!g_cfg.save(save_path)) {
                        g_cfg = std::move(previous);
                        send_response(client_fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Failed to save configuration");
                        return;
                    }
                }
            }
            // Lifecycle calls can join workers that read config. Serialize callers and
            // snapshot the latest desired state after releasing the config mutex.
            if (!validation_failed && reconfigure_panels) {
                std::lock_guard lifecycle(g_infopanels_lifecycle_mtx);
                bool news, calendar, stocks, photos;
                {
                    std::shared_lock lock(g_config_mtx);
                    news = g_cfg.infopanels_enabled && g_cfg.news_enabled;
                    calendar = g_cfg.infopanels_enabled && g_cfg.gcalendar_enabled;
                    stocks = g_cfg.infopanels_enabled && g_cfg.stockstreamer_enabled;
                    photos = g_cfg.google_photos_enabled;
                }
                if (news != g_news_ticker.is_running()) { if (news) g_news_ticker.start(); else g_news_ticker.stop(); }
                if (calendar != g_calendar.is_running()) { if (calendar) g_calendar.start(); else g_calendar.stop(); }
                if (stocks != g_stock_streamer.is_running()) { if (stocks) g_stock_streamer.start(); else g_stock_streamer.stop(); }
                if (photos != g_google_photos.is_running()) { if (photos) g_google_photos.start(); else g_google_photos.stop(); }
            }
            // g_config_mtx is released above; fetch_sync() may now take its shared_lock.
            if (!validation_failed && refetch_news) {
                g_news_ticker.fetch_sync();
            }
            
            if (validation_failed) {
                trigger_error(807); // E807: HTTP_SETTINGS_CLAMP_VIOLATION
                g_logger.error("HTTP settings clamp violation: {}", err_msg);
                std::string json_err = std::format("{{\n  \"status\": \"error\",\n  \"message\": \"HTTP settings clamp violation: {}\"\n}}", escape_json(err_msg));
                send_response(client_fd, "HTTP/1.1 400 Bad Request", "application/json", json_err);
            } else {
                if (is_error_active(807)) {
                    clear_error(807);
                }
                send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
            }
        }
        else if (matches_route("GET", "/api/settings")) {
            if (is_authorized()) {
                send_response(client_fd, "HTTP/1.1 200 OK", "application/json", get_api_settings());
            }
        }
        else if (matches_route("GET", "/api/logs")) {
            if (is_authorized()) {
            std::string log_content = "";
            std::string path = g_logger.get_log_file_path();
            bool read_success = false;
            
            if (!path.empty() && std::filesystem::exists(path)) {
                std::ifstream log_file(path);
                if (log_file.is_open()) {
                    log_file.seekg(0, std::ios::end);
                    const std::streamoff size = log_file.tellg();
                    if (size >= 0) {
                        const auto length = std::min<std::streamoff>(size, 30720);
                        log_file.seekg(size - length);
                        log_content.resize(static_cast<size_t>(length));
                        log_file.read(log_content.data(), length);
                        log_content.resize(static_cast<size_t>(log_file.gcount()));
                        if (size > length) {
                            const auto first_line = log_content.find('\n');
                            if (first_line != log_content.npos) log_content.erase(0, first_line + 1);
                        }
                        read_success = !log_file.bad();
                    }
                }
            }
            
            if (!read_success) {
                trigger_error(126); // E126: HTTP_LOG_STREAM_IO_ERROR
                log_content = "No logs available or log file not initialized yet.";
            } else {
                if (is_error_active(126)) {
                    clear_error(126);
                }
            }
            
            std::string json_oss = std::format("{{\n  \"logs\": \"{}\"\n}}", escape_json(log_content));
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", json_oss);
                    }
        }
        else if (matches_route("GET", "/api/news")) {
            if (!is_authorized()) return;
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", g_news_ticker.get_status_json());
        }
        else if (matches_route("GET", "/api/calendar/sync") || matches_route("POST", "/api/calendar/sync")) {
            if (is_authorized()) {
                if (spawn_tracked_thread([]() {
                    g_calendar.sync();
                })) {
                    send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\",\"message\":\"Calendar sync triggered\"}");
                } else {
                    send_response(client_fd, "HTTP/1.1 503 Service Unavailable", "application/json", "{\"status\":\"error\",\"message\":\"Failed to start calendar sync\"}");
                }
            }
        }
        else if (matches_route("GET", "/api/calendar")) {
            if (!is_authorized()) return;
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", g_calendar.get_status_json());
        }
        else if (matches_route("GET", "/api/status")) {
            bool include_private_details = false;
            if (!is_authorized(&include_private_details)) return;
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", get_api_status(include_private_details));
        } 
        else if (matches_route("GET", "/api/next")) {
            if (!is_authorized()) return;
            static std::atomic<int64_t> last_next_ms{0};
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - last_next_ms.load() < 500) {
                send_response(client_fd, "HTTP/1.1 429 Too Many Requests", "text/plain", "Rate limited");
                return;
            }
            last_next_ms.store(now_ms);
            g_remote_command.store(1);
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
        } 
        else if (parsed_request.method == "GET" && parsed_request.path.starts_with("/api/screenshots/")) {
            if (!is_authorized()) return;
            std::string sub = parsed_request.path.substr(std::string_view("/api/screenshots/").size());
            handle_screenshot_file(client_fd, http_protocol::decode_form(sub));
        }
        else if (matches_route("GET", "/api/screenshots")) {
            if (!is_authorized()) return;
            handle_screenshots_list(client_fd);
        }
        else if (matches_route("GET", "/api/screenshot")) {
            // Was the only state-changing route with neither is_authorized nor a cooldown:
            // each call blocks its worker up to 5s and forces a full framebuffer readback
            // plus PNG encode on the render thread.
            if (!is_authorized()) return;
            static std::atomic<int64_t> last_shot_ms{0};
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - last_shot_ms.load() < 2000) {
                send_response(client_fd, "HTTP/1.1 429 Too Many Requests", "text/plain", "Rate limited");
                return;
            }
            last_shot_ms.store(now_ms);
            handle_screenshot(client_fd);
        }
        else if (matches_route("GET", "/api/preview")) {
            if (!is_authorized()) return;
            handle_preview(client_fd);
        } 
        else if (matches_route("GET", "/api/prev")) {
            if (!is_authorized()) return;
            static std::atomic<int64_t> last_prev_ms{0};
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - last_prev_ms.load() < 500) {
                send_response(client_fd, "HTTP/1.1 429 Too Many Requests", "text/plain", "Rate limited");
                return;
            }
            last_prev_ms.store(now_ms);
            g_remote_command.store(2);
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
        } 
        else if (matches_route("GET", "/api/pause")) {
            if (!is_authorized()) return;
            static std::atomic<int64_t> last_pause_ms{0};
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - last_pause_ms.load() < 500) {
                send_response(client_fd, "HTTP/1.1 429 Too Many Requests", "text/plain", "Rate limited");
                return;
            }
            last_pause_ms.store(now_ms);
            g_remote_command.store(3);
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
        } 
        else if (matches_route("GET", "/api/toggle_shuffle")) {
            if (!is_authorized()) return;
            static std::atomic<int64_t> last_shuffle_ms{0};
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - last_shuffle_ms.load() < 500) {
                send_response(client_fd, "HTTP/1.1 429 Too Many Requests", "text/plain", "Rate limited");
                return;
            }
            last_shuffle_ms.store(now_ms);
            {
                std::lock_guard lock(g_config_mtx);
                const bool previous = g_cfg.shuffle;
                g_cfg.shuffle = !previous;
                const auto path = g_cfg.loaded_path.empty() ? "/app/config/config.toml" : g_cfg.loaded_path;
                if (!g_cfg.save(path)) {
                    g_cfg.shuffle = previous;
                    send_response(client_fd, "HTTP/1.1 500 Internal Server Error", "application/json", "{\"error\":\"Shuffle could not be saved\"}");
                    return;
                }
            }
            g_config_changed.store(true);
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
        } 
        else if (matches_route("GET", "/api/stocks")) {
            if (!is_authorized()) return;
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", g_stock_streamer.get_status_json());
        }
        else if (matches_route("GET", "/api/news/refresh") || matches_route("POST", "/api/news/refresh")) {
            if (!is_authorized()) return;
            g_news_ticker.sync();
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", g_news_ticker.get_status_json());
        }
        else if (matches_route("GET", "/api/restart")) {
            if (!is_authorized()) return;
            static std::atomic<int64_t> last_restart_ms{0};
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - last_restart_ms.load() < 500) {
                send_response(client_fd, "HTTP/1.1 429 Too Many Requests", "text/plain", "Rate limited");
                return;
            }
            last_restart_ms.store(now_ms);
            g_running.store(false);
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
        } 
        else if (matches_route("GET", "/api/toggle_screen")) {
            if (!is_authorized()) return;
            static std::atomic<int64_t> last_toggle_screen_ms{0};
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - last_toggle_screen_ms.load() < 500) {
                send_response(client_fd, "HTTP/1.1 429 Too Many Requests", "text/plain", "Rate limited");
                return;
            }
            last_toggle_screen_ms.store(now_ms);
            toggle_display_power();
            std::string prefix, topic;
            { std::shared_lock lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; topic = g_cfg.mqtt_motionsensor_topic; }
            mqtt_publish(prefix + "/status/screen", g_screen_blanked.load() ? "OFF" : "ON", true);
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
        }
        else if (matches_route("GET", "/api/trigger_motion")) {
            if (!is_authorized()) return;
            static std::atomic<int64_t> last_motion_ms{0};
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - last_motion_ms.load() < 500) {
                send_response(client_fd, "HTTP/1.1 429 Too Many Requests", "text/plain", "Rate limited");
                return;
            }
            last_motion_ms.store(now_ms);
            g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
            std::string prefix, sensor_topic;
            { std::shared_lock lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; sensor_topic = g_cfg.mqtt_motionsensor_topic; }
            if (set_display_power(true)) {
                mqtt_publish(prefix + "/status/screen", "ON", true);
            }
            if (!sensor_topic.empty()) {
                mqtt_publish(sensor_topic, "ON", false);
                std::string topic_copy = sensor_topic;
                spawn_tracked_thread([topic_copy]() {
                    for (int i = 0; i < 20 && g_server_running.load(); i++) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    if (g_server_running.load()) {
                        mqtt_publish(topic_copy, "OFF", false);
                    }
                });
            }
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
        }
        else if (matches_route("GET", "/api/play_video")) {
            if (!is_authorized()) return;
            static std::atomic<int64_t> last_video_ms{0};
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - last_video_ms.load() < 500) {
                send_response(client_fd, "HTTP/1.1 429 Too Many Requests", "text/plain", "Rate limited");
                return;
            }
            last_video_ms.store(now_ms);
            g_remote_command.store(5);
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"forcing_next_video\"}");
        }
        else {
            send_response(client_fd, "HTTP/1.1 404 Not Found", "text/plain", "Not Found");
        }
    }
}

static void server_loop(int port) {
    struct sockaddr_in server_addr;
    int current_port = port;
    int max_attempts;
    { std::shared_lock lk(g_config_mtx); max_attempts = g_cfg.http_bind_attempts; }
    bool bound = false;

    for (int attempt = 0; attempt < max_attempts && current_port <= 65535; attempt++) {
        int socket_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (socket_fd < 0) {
            g_logger.error("HTTP: Failed to open stream socket.");
            return;
        }

        // Set SO_REUSEADDR to avoid address in use crashes on quick restart
        int optval = 1;
        setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        std::memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(current_port);

        if (bind(socket_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) >= 0) {
            g_listen_fd.store(socket_fd);
            bound = true;
            break;
        }

        g_logger.warn("HTTP: Port {} was in use, trying next port...", current_port);
        close(socket_fd);
        current_port++;
    }

    if (!bound) {
        trigger_error(104); // E104: PORT_BIND_CONFLICT
        return;
    }

    if (current_port != port) {
        g_logger.warn("HTTP: Port {} was in use. Dynamic fallback bound to port {}", port, current_port);
        {
            std::lock_guard lock(g_config_mtx);
            g_cfg.http_port = current_port;
        }
        g_config_changed.store(true);
    }

    int active_fd = g_listen_fd.load();
    if (active_fd < 0 || listen(active_fd, 64) < 0) {
        g_logger.error("HTTP: Listen failed on socket.");
        int fd_to_close = g_listen_fd.exchange(-1);
        if (fd_to_close >= 0) {
            close(fd_to_close);
        }
        return;
    }

    g_logger.info("HTTP: Background Web Remote server active on port {}", current_port);
    // Report the effective auth state at startup. Deliberately advisory: setting
    // [remote] api_key is opt-in and no key means the control API stays open, so this
    // makes the trade-off visible in the log instead of enforcing it.
    {
        std::string key;
        {
            std::shared_lock<std::shared_mutex> lk(g_config_mtx);
            key = g_cfg.http_api_key;
        }
        if (key.empty()) {
            g_logger.warn("HTTP: control API is UNAUTHENTICATED (no [remote] api_key set). Set one in config.toml if this host is on a shared network.");
        } else {
            g_logger.info("HTTP: control API requires a bearer API key ([remote] api_key is set).");
        }
    }

    while (g_server_running.load()) {
        int fd = g_listen_fd.load();
        if (fd < 0) break;

        pollfd listener{fd, POLLIN, 0};
        int select_rc = poll(&listener, 1, 1000);
        if (select_rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (select_rc == 0) continue; // select timed out, loop around to check running status

        if (!g_server_running.load() || (listener.revents & (POLLHUP | POLLERR | POLLNVAL))) break;
        if (!(listener.revents & POLLIN)) continue;
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int current_fd = g_listen_fd.load();
        if (current_fd < 0) break;
        int client_fd = accept4(current_fd, (struct sockaddr*)&client_addr, &client_len, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (!g_server_running.load()) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED || errno == EMFILE) {
                if (errno == EMFILE) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                continue;
            }
            g_logger.error("HTTP server accept failed: {}", strerror(errno));
            continue;
        }

        // Set a socket read/write timeout to prevent slow/hanging clients from starving the connection pool
        struct timeval timeout;
        {
            std::shared_lock lk(g_config_mtx);
            timeout.tv_sec = std::max(2, g_cfg.http_socket_timeout);
        }
        timeout.tv_usec = 0;
        if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
            g_logger.warn("HTTP: Failed to set SO_RCVTIMEO on client socket.");
        }
        if (setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
            g_logger.warn("HTTP: Failed to set SO_SNDTIMEO on client socket.");
        }

        int prev = g_active_connections.fetch_add(1);
        if (prev >= 32) {
            g_active_connections.fetch_sub(1);
            constexpr std::string_view busy =
                "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send(client_fd, busy.data(), busy.size(), MSG_DONTWAIT | MSG_NOSIGNAL);
            close(client_fd);
            continue;
        }
        if (!spawn_tracked_thread([client_fd, peer_ip = client_addr.sin_addr.s_addr]() {
            struct ConnectionCountGuard { ~ConnectionCountGuard() { g_active_connections.fetch_sub(1); } } count_guard;
            handle_client(client_fd, peer_ip);
        })) {
            g_active_connections.fetch_sub(1);
            close(client_fd);
        }
    }

    int fd_to_close = g_listen_fd.exchange(-1);
    if (fd_to_close >= 0) {
        close(fd_to_close);
    }
    g_logger.info("HTTP: Background remote controller server stopped.");
}

void start_http_server(int port) {
    if (g_server_running.load()) return;
    g_server_running.store(true);
    if (!spawn_thread_safe(g_server_thread, "http_server", server_loop, port)) {
        g_server_running.store(false);
        return;
    }
}

void stop_http_server() {
    if (!g_server_running.load()) return;
    g_server_running.store(false);
    
    // shutdown socket to interrupt select/accept
    int fd = g_listen_fd.load();
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }

    // shutdown all active client connections to unblock reads/writes
    {
        std::lock_guard<std::mutex> lk(g_active_fds_mtx);
        for (int cfd : g_active_client_fds) {
            shutdown(cfd, SHUT_RDWR);
        }
    }
    
    if (g_server_thread.joinable()) {
        g_server_thread.join();
    }

    // A client can spawn a background request while it is finishing. Never hold
    // the registry mutex while joining it; shutdown rejects further admission.
    std::vector<TrackedThreadInfo> pending;
    {
        std::lock_guard lock(g_http_threads_mtx);
        pending.swap(g_http_client_threads);
    }
    for (auto& info : pending) if (info.thread.joinable()) info.thread.join();
}

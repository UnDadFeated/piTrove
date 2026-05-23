#include "http_server.h"
#include "media_item.h"
#include "util.h"
#include "config.h"
#include "mqtt.h"
#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <fstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <algorithm>
#include <filesystem>
#include <iomanip>

extern std::vector<MediaItem> g_eligible;
extern int current_idx;
extern std::mutex g_playlist_mtx;

static std::thread g_server_thread;
static std::atomic<bool> g_server_running{false};
static int g_listen_fd = -1;

// Premium Glassmorphic Dashboard HTML
static const std::string DASHBOARD_HTML = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>piTrove Remote Controller</title>
    <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600;800&family=JetBrains+Mono&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg-grad: linear-gradient(135deg, #09090b 0%, #180828 50%, #03001e 100%);
            --accent: #d946ef;
            --accent-glow: rgba(217, 70, 239, 0.4);
            --neon-blue: #06b6d4;
            --neon-blue-glow: rgba(6, 182, 212, 0.4);
            --card-bg: rgba(255, 255, 255, 0.03);
            --card-border: rgba(255, 255, 255, 0.08);
            --glass-blur: blur(20px);
            --text-main: #f8fafc;
            --text-muted: #94a3b8;
        }

        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
            user-select: none;
        }

        body {
            font-family: 'Outfit', sans-serif;
            background: var(--bg-grad);
            background-attachment: fixed;
            color: var(--text-main);
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            align-items: center;
            overflow-x: hidden;
            padding: 2rem 1rem;
        }

        .container {
            width: 100%;
            max-width: 600px;
            display: flex;
            flex-direction: column;
            gap: 1.5rem;
        }

        /* Glassmorphic Header */
        header {
            background: var(--card-bg);
            border: 1px solid var(--card-border);
            backdrop-filter: var(--glass-blur);
            -webkit-backdrop-filter: var(--glass-blur);
            border-radius: 24px;
            padding: 1.5rem;
            text-align: center;
            box-shadow: 0 8px 32px 0 rgba(0, 0, 0, 0.37);
            position: relative;
            overflow: hidden;
        }

        header::before {
            content: '';
            position: absolute;
            top: 0; left: 0; right: 0; height: 3px;
            background: linear-gradient(90deg, var(--accent), var(--neon-blue));
        }

        h1 {
            font-weight: 800;
            font-size: 1.8rem;
            letter-spacing: 1.5px;
            background: linear-gradient(90deg, #fdfbf7, var(--text-main));
            -webkit-background-clip: text;
            -webkit-text-fill-color: transparent;
            text-shadow: 0 4px 12px rgba(0,0,0,0.1);
        }

        .subtitle {
            font-size: 0.85rem;
            color: var(--text-muted);
            text-transform: uppercase;
            letter-spacing: 2px;
            margin-top: 0.25rem;
        }

        /* Live Preview Card */
        .preview-card {
            background: var(--card-bg);
            border: 1px solid var(--card-border);
            backdrop-filter: var(--glass-blur);
            -webkit-backdrop-filter: var(--glass-blur);
            border-radius: 28px;
            padding: 1rem;
            display: flex;
            flex-direction: column;
            align-items: center;
            gap: 1rem;
            box-shadow: 0 16px 48px 0 rgba(0, 0, 0, 0.4);
            position: relative;
        }

        .preview-container {
            width: 100%;
            aspect-ratio: 4 / 3;
            border-radius: 20px;
            background: #000;
            overflow: hidden;
            position: relative;
            border: 1px solid rgba(255, 255, 255, 0.05);
            display: flex;
            align-items: center;
            justify-content: center;
        }

        .preview-image {
            width: 100%;
            height: 100%;
            object-fit: contain;
            transition: opacity 0.3s ease;
        }

        .preview-loading {
            position: absolute;
            font-size: 0.9rem;
            color: var(--text-muted);
            pointer-events: none;
            opacity: 0;
            transition: opacity 0.2s ease;
        }

        .media-info {
            width: 100%;
            display: flex;
            flex-direction: column;
            gap: 0.25rem;
            padding: 0 0.5rem;
        }

        .media-title {
            font-weight: 600;
            font-size: 1.1rem;
            white-space: nowrap;
            overflow: hidden;
            text-overflow: ellipsis;
            color: var(--text-main);
        }

        .media-meta {
            display: flex;
            justify-content: space-between;
            align-items: center;
            font-size: 0.85rem;
            color: var(--text-muted);
        }

        .badge {
            padding: 0.25rem 0.6rem;
            border-radius: 8px;
            font-size: 0.75rem;
            font-weight: 600;
            text-transform: uppercase;
            letter-spacing: 0.5px;
            border: 1px solid rgba(255,255,255,0.1);
        }

        .badge-photo {
            background: rgba(6, 182, 212, 0.15);
            color: var(--neon-blue);
            border-color: rgba(6, 182, 212, 0.3);
        }

        .badge-video {
            background: rgba(217, 70, 239, 0.15);
            color: var(--accent);
            border-color: rgba(217, 70, 239, 0.3);
        }

        /* Controllers Grid */
        .controls-grid {
            display: grid;
            grid-template-columns: repeat(3, 1fr);
            gap: 1rem;
        }

        .btn {
            background: var(--card-bg);
            border: 1px solid var(--card-border);
            backdrop-filter: var(--glass-blur);
            -webkit-backdrop-filter: var(--glass-blur);
            border-radius: 20px;
            padding: 1.25rem;
            color: var(--text-main);
            font-family: inherit;
            font-size: 1rem;
            font-weight: 600;
            cursor: pointer;
            display: flex;
            flex-direction: column;
            align-items: center;
            justify-content: center;
            gap: 0.5rem;
            transition: all 0.2s cubic-bezier(0.4, 0, 0.2, 1);
            box-shadow: 0 4px 16px rgba(0,0,0,0.2);
        }

        .btn:hover {
            background: rgba(255, 255, 255, 0.08);
            border-color: rgba(255, 255, 255, 0.2);
            transform: translateY(-2px);
        }

        .btn:active {
            transform: translateY(1px);
            background: rgba(255, 255, 255, 0.04);
        }

        .btn-accent {
            background: rgba(217, 70, 239, 0.05);
            border-color: rgba(217, 70, 239, 0.2);
        }
        .btn-accent:hover {
            background: rgba(217, 70, 239, 0.15);
            border-color: var(--accent);
            box-shadow: 0 0 15px var(--accent-glow);
        }

        .btn-blue {
            background: rgba(6, 182, 212, 0.05);
            border-color: rgba(6, 182, 212, 0.2);
        }
        .btn-blue:hover {
            background: rgba(6, 182, 212, 0.15);
            border-color: var(--neon-blue);
            box-shadow: 0 0 15px var(--neon-blue-glow);
        }

        .btn-icon {
            font-size: 1.5rem;
        }

        .toggle-row {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 1rem;
        }

        .btn-toggle {
            padding: 1rem;
            flex-direction: row;
            gap: 0.75rem;
        }

        .btn-active {
            background: rgba(255, 255, 255, 0.15);
            border-color: var(--text-main);
        }

        .btn-danger {
            background: rgba(239, 68, 68, 0.05);
            border-color: rgba(239, 68, 68, 0.2);
        }
        .btn-danger:hover {
            background: rgba(239, 68, 68, 0.15);
            border-color: #ef4444;
            box-shadow: 0 0 15px rgba(239, 68, 68, 0.4);
        }

        /* Diagnostics Telemetry Panel */
        .telemetry-card {
            background: var(--card-bg);
            border: 1px solid var(--card-border);
            backdrop-filter: var(--glass-blur);
            -webkit-backdrop-filter: var(--glass-blur);
            border-radius: 24px;
            padding: 1.25rem;
            box-shadow: 0 8px 32px rgba(0,0,0,0.3);
        }

        .telemetry-title {
            font-size: 0.8rem;
            text-transform: uppercase;
            letter-spacing: 1.5px;
            color: var(--text-muted);
            margin-bottom: 0.75rem;
            font-weight: 600;
            display: flex;
            align-items: center;
            gap: 0.5rem;
        }

        .telemetry-title::before {
            content: '';
            display: inline-block;
            width: 8px; height: 8px;
            background: var(--neon-blue);
            border-radius: 50%;
            box-shadow: 0 0 8px var(--neon-blue);
        }

        .telemetry-grid {
            display: grid;
            grid-template-columns: repeat(3, 1fr);
            gap: 1rem;
        }

        .telemetry-item {
            display: flex;
            flex-direction: column;
            gap: 0.25rem;
        }

        .telemetry-label {
            font-size: 0.75rem;
            color: var(--text-muted);
        }

        .telemetry-value {
            font-family: 'JetBrains Mono', monospace;
            font-size: 1.1rem;
            font-weight: 600;
            color: var(--text-main);
        }

        /* Responsive */
        @media (max-width: 480px) {
            h1 { font-size: 1.5rem; }
            .btn { padding: 1rem; font-size: 0.9rem; }
            .btn-icon { font-size: 1.25rem; }
        }
    </style>
</head>
<body>
    <div class="container">
        <!-- Header -->
        <header>
            <h1>piTrove controller</h1>
            <div class="subtitle">v11.0.0 glassmorphic system</div>
        </header>

        <!-- Live Preview -->
        <div class="preview-card">
            <div class="preview-container">
                <img id="preview" class="preview-image" src="/api/preview" alt="Slideshow Preview" onload="onPreviewLoaded()" onerror="onPreviewError()">
                <div id="loading" class="preview-loading">Syncing...</div>
            </div>
            <div class="media-info">
                <div id="media-title" class="media-title">Fetching media...</div>
                <div class="media-meta">
                    <span id="media-type" class="badge">IMAGE</span>
                    <span id="media-progress">0 / 0</span>
                </div>
            </div>
        </div>

        <!-- Controls Grid -->
        <div class="controls-grid">
            <button class="btn btn-blue" onclick="sendCommand('/api/prev')">
                <span class="btn-icon">⏮</span>
                <span>Previous</span>
            </button>
            <button id="btn-pause" class="btn btn-accent" onclick="sendCommand('/api/pause')">
                <span id="icon-pause" class="btn-icon">⏸</span>
                <span id="txt-pause">Pause</span>
            </button>
            <button class="btn btn-blue" onclick="sendCommand('/api/next')">
                <span class="btn-icon">⏭</span>
                <span>Next</span>
            </button>
        </div>

        <!-- Toggle Row -->
        <div class="toggle-row">
            <button id="btn-shuffle" class="btn btn-toggle" onclick="sendCommand('/api/toggle_shuffle')">
                <span class="btn-icon">🔀</span>
                <span>Shuffle: <strong id="lbl-shuffle">ON</strong></span>
            </button>
            <button class="btn btn-danger btn-toggle" onclick="confirmRestart()">
                <span class="btn-icon">🔄</span>
                <span>Soft Restart</span>
            </button>
        </div>

        <!-- Telemetry Panel -->
        <div class="telemetry-card">
            <div class="telemetry-title">Diagnostics Telemetry</div>
            <div class="telemetry-grid">
                <div class="telemetry-item">
                    <span class="telemetry-label">CPU Temp</span>
                    <span id="stat-temp" class="telemetry-value">--°C</span>
                </div>
                <div class="telemetry-item">
                    <span class="telemetry-label">Cache DB</span>
                    <span id="stat-db" class="telemetry-value">-- MB</span>
                </div>
                <div class="telemetry-item">
                    <span class="telemetry-label">Queue Size</span>
                    <span id="stat-queue" class="telemetry-value">--</span>
                </div>
            </div>
        </div>

        <!-- MQTT Integration Panel -->
        <div class="telemetry-card" style="margin-top: 1rem;">
            <div class="telemetry-title">MQTT & Home Assistant Integration</div>
            <div class="telemetry-grid" style="grid-template-columns: repeat(2, 1fr);">
                <div class="telemetry-item">
                    <span class="telemetry-label">Broker IP</span>
                    <span id="stat-mqtt-broker" class="telemetry-value">--</span>
                </div>
                <div class="telemetry-item">
                    <span class="telemetry-label">Status</span>
                    <span id="stat-mqtt-status" class="telemetry-value">Disabled</span>
                </div>
            </div>
            <div class="toggle-row" style="margin-top: 1rem; gap: 0.75rem; display: flex; width: 100%;">
                <button id="btn-screen" class="btn btn-toggle" onclick="sendCommand('/api/toggle_screen')" style="flex: 1; min-height: 48px; border-radius: 14px;">
                    <span class="btn-icon">📺</span>
                    <span>Screen: <strong id="lbl-screen">ON</strong></span>
                </button>
                <button class="btn btn-blue" onclick="sendCommand('/api/trigger_motion')" style="flex: 1; min-height: 48px; border-radius: 14px; display: flex; align-items: center; justify-content: center; gap: 0.5rem; border: 1px solid var(--card-border); background: var(--card-bg); color: var(--text-main); font-weight: 600;">
                    <span class="btn-icon">🏃</span>
                    <span>Trigger Motion</span>
                </button>
            </div>
        </div>
    </div>

    <script>
        let currentFilename = "";
        let isPolling = false;

        async function fetchStatus() {
            if (isPolling) return;
            isPolling = true;
            try {
                const res = await fetch('/api/status');
                if (res.ok) {
                    const status = await res.json();
                    
                    // Update Media Info
                    document.getElementById('media-title').innerText = status.filename;
                    document.getElementById('media-progress').innerText = `${status.index + 1} / ${status.total}`;
                    
                    // Update Badges
                    const typeBadge = document.getElementById('media-type');
                    if (status.is_video) {
                        typeBadge.innerText = "Video";
                        typeBadge.className = "badge badge-video";
                    } else {
                        typeBadge.innerText = "Photo";
                        typeBadge.className = "badge badge-photo";
                    }

                    // Update Shuffle Button
                    const shuffleLbl = document.getElementById('lbl-shuffle');
                    const shuffleBtn = document.getElementById('btn-shuffle');
                    if (status.shuffle) {
                        shuffleLbl.innerText = "ON";
                        shuffleBtn.classList.add('btn-active');
                    } else {
                        shuffleLbl.innerText = "OFF";
                        shuffleBtn.classList.remove('btn-active');
                    }

                    // Update Pause Button
                    const pauseTxt = document.getElementById('txt-pause');
                    const pauseIcon = document.getElementById('icon-pause');
                    const pauseBtn = document.getElementById('btn-pause');
                    if (status.paused) {
                        pauseTxt.innerText = "Resume";
                        pauseIcon.innerText = "▶";
                        pauseBtn.classList.add('btn-active');
                    } else {
                        pauseTxt.innerText = "Pause";
                        pauseIcon.innerText = "⏸";
                        pauseBtn.classList.remove('btn-active');
                    }

                    // Update Telemetry Panel
                    document.getElementById('stat-temp').innerText = status.temp;
                    document.getElementById('stat-db').innerText = status.db_size;
                    document.getElementById('stat-queue').innerText = status.total;

                    // Update MQTT Status
                    const mqttBroker = document.getElementById('stat-mqtt-broker');
                    const mqttStatus = document.getElementById('stat-mqtt-status');
                    if (status.mqtt_enabled) {
                        mqttBroker.innerText = `${status.mqtt_broker}:${status.mqtt_port}`;
                        mqttStatus.innerText = "Active";
                        mqttStatus.style.color = "#10b981"; // Emerald green
                    } else {
                        mqttBroker.innerText = "N/A";
                        mqttStatus.innerText = "Disabled";
                        mqttStatus.style.color = "#ef4444"; // Red
                    }

                    // Update Screen Blanked Button
                    const screenLbl = document.getElementById('lbl-screen');
                    const screenBtn = document.getElementById('btn-screen');
                    if (!status.screen_blanked) {
                        screenLbl.innerText = "ON";
                        screenBtn.classList.add('btn-active');
                    } else {
                        screenLbl.innerText = "OFF";
                        screenBtn.classList.remove('btn-active');
                    }

                    // Trigger Preview Image reload if filename changes
                    if (status.filename !== currentFilename) {
                        currentFilename = status.filename;
                        reloadPreview();
                    }
                }
            } catch (err) {
                console.error("Status polling failed:", err);
            } finally {
                isPolling = false;
            }
        }

        function reloadPreview() {
            const preview = document.getElementById('preview');
            const loading = document.getElementById('loading');
            loading.style.opacity = "1";
            preview.style.opacity = "0.4";
            // Bust browser image caching using timestamp
            preview.src = `/api/preview?t=${Date.now()}`;
        }

        function onPreviewLoaded() {
            document.getElementById('loading').style.opacity = "0";
            document.getElementById('preview').style.opacity = "1";
        }

        function onPreviewError() {
            document.getElementById('loading').innerText = "Load error";
            document.getElementById('loading').style.opacity = "1";
        }

        async function sendCommand(url) {
            try {
                const res = await fetch(url);
                if (res.ok) {
                    // Update status immediately following command execution
                    await fetchStatus();
                }
            } catch (err) {
                console.error(`Command failed [${url}]:`, err);
            }
        }

        function confirmRestart() {
            if (confirm("Are you sure you want to soft restart the piTrove application?")) {
                sendCommand('/api/restart');
                alert("Soft restart signal dispatched. Application will reboot in 2-3 seconds.");
            }
        }

        // Start status loop
        fetchStatus();
        setInterval(fetchStatus, 1000);
    </script>
</body>
</html>
)HTML";

static std::string escape_json(const std::string& s) {
    std::ostringstream oss;
    for (char c : s) {
        switch (c) {
            case '"':  oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) <= 0x1f) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << (int)c;
                } else {
                    oss << c;
                }
        }
    }
    return oss.str();
}

static std::string get_api_status() {
    int idx = 0;
    int total = 0;
    std::string filename = "None";
    std::string path = "";
    std::string type = "image";
    bool shuffle = false;
    bool paused = false;

    {
        std::lock_guard<std::mutex> lk(g_playlist_mtx);
        total = (int)g_eligible.size();
        if (total > 0 && current_idx >= 0 && current_idx < total) {
            idx = current_idx;
            filename = g_eligible[current_idx].filename;
            path = g_eligible[current_idx].path;
            type = g_eligible[current_idx].type;
        }
    }
    
    bool mqtt_enabled = false;
    std::string mqtt_broker = "";
    int mqtt_port = 1883;
    bool screen_blanked = g_screen_blanked.load();

    {
        std::lock_guard<std::mutex> lk(g_config_mtx);
        shuffle = g_cfg.shuffle;
        mqtt_enabled = g_cfg.mqtt_enabled;
        mqtt_broker = g_cfg.mqtt_broker;
        mqtt_port = g_cfg.mqtt_port;
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
        std::string db_path = g_cfg.cache_dir + "/cache.db";
        if (std::filesystem::exists(db_path)) {
            db_mb = std::filesystem::file_size(db_path) / (1024.0 * 1024.0);
        }
    } catch (...) {}

    std::ostringstream oss;
    oss << "{\n"
        << "  \"index\": " << idx << ",\n"
        << "  \"total\": " << total << ",\n"
        << "  \"filename\": \"" << escape_json(filename) << "\",\n"
        << "  \"is_video\": " << (type == "video" ? "true" : "false") << ",\n"
        << "  \"shuffle\": " << (shuffle ? "true" : "false") << ",\n"
        << "  \"paused\": " << (paused ? "true" : "false") << ",\n"
        << "  \"temp\": \"" << (temp_c > 0.0 ? std::to_string(temp_c).substr(0, 4) + "°C" : "N/A") << "\",\n"
        << "  \"db_size\": \"" << (db_mb > 0.0 ? std::to_string(db_mb).substr(0, 4) + " MB" : "0.0 MB") << "\",\n"
        << "  \"mqtt_enabled\": " << (mqtt_enabled ? "true" : "false") << ",\n"
        << "  \"mqtt_broker\": \"" << escape_json(mqtt_broker) << "\",\n"
        << "  \"mqtt_port\": " << mqtt_port << ",\n"
        << "  \"screen_blanked\": " << (screen_blanked ? "true" : "false") << "\n"
        << "}";
    return oss.str();
}

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
    std::ostringstream oss;
    oss << status_line << "\r\n"
        << "Content-Type: " << mime << "\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    std::string response = oss.str();
    ssize_t remain = response.size();
    const char* ptr = response.data();
    while (remain > 0) {
        ssize_t w = write(fd, ptr, remain);
        if (w <= 0) break;
        ptr += w;
        remain -= w;
    }
}

static void handle_preview(int fd) {
    std::string path = "";
    std::string type = "image";

    {
        std::lock_guard<std::mutex> lk(g_playlist_mtx);
        if (!g_eligible.empty() && current_idx >= 0 && current_idx < (int)g_eligible.size()) {
            path = g_eligible[current_idx].path;
            type = g_eligible[current_idx].type;
        }
    }

    // Video Fallback Card
    if (type == "video" || path.empty()) {
        send_response(fd, "HTTP/1.1 200 OK", "image/svg+xml", VIDEO_FALLBACK_SVG);
        return;
    }

    // Dynamic Image File Streaming
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        send_response(fd, "HTTP/1.1 404 Not Found", "text/plain", "File not found on disk");
        return;
    }

    // Determine MIME type
    std::string mime = "image/jpeg";
    std::string lower_path = path;
    std::transform(lower_path.begin(), lower_path.end(), lower_path.begin(), ::tolower);
    if (lower_path.find(".png") != std::string::npos) mime = "image/png";
    else if (lower_path.find(".webp") != std::string::npos) mime = "image/webp";
    else if (lower_path.find(".gif") != std::string::npos) mime = "image/gif";

    // Read file size
    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    // Send HTTP Headers
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: " << mime << "\r\n"
        << "Content-Length: " << file_size << "\r\n"
        << "Cache-Control: no-cache, no-store, must-revalidate\r\n"
        << "Connection: close\r\n\r\n";
    std::string headers = oss.str();
    write(fd, headers.data(), headers.size());

    // Stream body in chunks
    char buffer[8192];
    while (file.good()) {
        file.read(buffer, sizeof(buffer));
        std::streamsize bytes = file.gcount();
        if (bytes > 0) {
            ssize_t w = write(fd, buffer, bytes);
            if (w <= 0) break; // client disconnected
        }
    }
}

static void server_loop(int port) {
    struct sockaddr_in server_addr;
    int current_port = port;
    int max_attempts = 10;
    bool bound = false;

    for (int attempt = 0; attempt < max_attempts; attempt++) {
        g_listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (g_listen_fd < 0) {
            g_logger.error("HTTP: Failed to open stream socket.");
            return;
        }

        // Set SO_REUSEADDR to avoid address in use crashes on quick restart
        int optval = 1;
        setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

        std::memset(&server_addr, 0, sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = INADDR_ANY;
        server_addr.sin_port = htons(current_port);

        if (bind(g_listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) >= 0) {
            bound = true;
            break;
        }

        g_logger.warn("HTTP: Port %d was in use, trying next port...", current_port);
        close(g_listen_fd);
        g_listen_fd = -1;
        current_port++;
    }

    if (!bound) {
        g_logger.error("HTTP: Failed to bind to any port after %d attempts starting from %d.", max_attempts, port);
        return;
    }

    if (current_port != port) {
        g_logger.warn("HTTP: Port %d was in use. Dynamic fallback bound to port %d", port, current_port);
        std::lock_guard<std::mutex> lock(g_config_mtx);
        g_cfg.http_port = current_port;
    }

    if (listen(g_listen_fd, 10) < 0) {
        g_logger.error("HTTP: Listen failed on socket.");
        close(g_listen_fd);
        g_listen_fd = -1;
        return;
    }

    g_logger.info("HTTP: Background Web Remote server active on port %d", current_port);

    char buffer[2048];
    while (g_server_running.load()) {
        // Set a timeout on accept so it can periodically check if g_server_running is false
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(g_listen_fd, &rfds);

        int select_rc = select(g_listen_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (select_rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (select_rc == 0) continue; // select timed out, loop around to check running status

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept4(g_listen_fd, (struct sockaddr*)&client_addr, &client_len, SOCK_CLOEXEC);
        if (client_fd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED || errno == EMFILE) {
                if (errno == EMFILE) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                continue;
            }
            g_logger.error("HTTP server accept failed: %s", strerror(errno));
            continue;
        }

        // Set client socket timeout to prevent slowloris hangs
        struct timeval client_tv;
        client_tv.tv_sec = 2;
        client_tv.tv_usec = 0;
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &client_tv, sizeof(client_tv));

        std::memset(buffer, 0, sizeof(buffer));
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            std::string request(buffer);

            // Very simple router
            if (request.rfind("GET / ", 0) == 0 || request.rfind("GET /dashboard", 0) == 0) {
                send_response(client_fd, "HTTP/1.1 200 OK", "text/html", DASHBOARD_HTML);
            } 
            else if (request.rfind("GET /api/status", 0) == 0) {
                send_response(client_fd, "HTTP/1.1 200 OK", "application/json", get_api_status());
            } 
            else if (request.rfind("GET /api/next", 0) == 0) {
                g_remote_command.store(1);
                send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
            } 
            else if (request.rfind("GET /api/prev", 0) == 0) {
                g_remote_command.store(2);
                send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
            } 
            else if (request.rfind("GET /api/pause", 0) == 0) {
                g_remote_command.store(3);
                send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
            } 
            else if (request.rfind("GET /api/toggle_shuffle", 0) == 0) {
                {
                    std::lock_guard<std::mutex> lock(g_config_mtx);
                    g_cfg.shuffle = !g_cfg.shuffle;
                }
                g_config_changed.store(true);
                send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
            } 
            else if (request.rfind("GET /api/restart", 0) == 0) {
                g_running.store(false);
                send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
            } 
            else if (request.rfind("GET /api/toggle_screen", 0) == 0) {
                g_screen_blanked = !g_screen_blanked.load();
                int res = ::system(g_screen_blanked.load() ? "vcgencmd display_power 0" : "vcgencmd display_power 1");
                (void)res;
                mqtt_publish(g_cfg.mqtt_topic_prefix + "/status/screen", g_screen_blanked.load() ? "OFF" : "ON", true);
                send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
            }
            else if (request.rfind("GET /api/trigger_motion", 0) == 0) {
                g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
                if (g_screen_blanked.load()) {
                    g_screen_blanked = false;
                    int res = ::system("vcgencmd display_power 1");
                    (void)res;
                    mqtt_publish(g_cfg.mqtt_topic_prefix + "/status/screen", "ON", true);
                }
                mqtt_publish(g_cfg.mqtt_motionsensor_topic, "ON", false);
                std::thread([]() {
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                    mqtt_publish(g_cfg.mqtt_motionsensor_topic, "OFF", false);
                }).detach();
                send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
            }
            else if (request.rfind("GET /api/preview", 0) == 0) {
                handle_preview(client_fd);
            } 
            else {
                send_response(client_fd, "HTTP/1.1 404 Not Found", "text/plain", "Not Found");
            }
        }
        close(client_fd);
    }

    close(g_listen_fd);
    g_listen_fd = -1;
    g_logger.info("HTTP: Background remote controller server stopped.");
}

void start_http_server(int port) {
    if (g_server_running.load()) return;
    g_server_running.store(true);
    g_server_thread = std::thread(server_loop, port);
}

void stop_http_server() {
    if (!g_server_running.load()) return;
    g_server_running.store(false);
    
    // shutdown socket to interrupt select/accept
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
    }
    
    if (g_server_thread.joinable()) {
        g_server_thread.join();
    }
}

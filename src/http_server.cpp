#include "http_server.h"
#include "media_item.h"
#include "util.h"
#include "config.h"
#include "mqtt.h"
#include "google_photos.h"
#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>
#include <functional>
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
static std::atomic<int> g_listen_fd{-1};
static std::atomic<int> g_active_connections{0};

struct TrackedThreadInfo {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished;
};
static std::vector<TrackedThreadInfo> g_http_client_threads;
static std::mutex g_http_threads_mtx;

static void spawn_tracked_thread(std::function<void()> func) {
    auto finished = std::make_shared<std::atomic<bool>>(false);
    std::thread t([func, finished]() {
        try { func(); } catch (...) {}
        finished->store(true);
    });
    
    std::lock_guard<std::mutex> lk(g_http_threads_mtx);
    for (auto it = g_http_client_threads.begin(); it != g_http_client_threads.end(); ) {
        if (it->finished->load()) {
            if (it->thread.joinable()) {
                it->thread.join();
            }
            it = g_http_client_threads.erase(it);
        } else {
            ++it;
        }
    }
    g_http_client_threads.push_back({std::move(t), finished});
}

static std::string execute_curl(const std::string& cmd) {
    std::shared_ptr<FILE> pipe(popen((cmd + " 2>/dev/null").c_str(), "r"), pclose);
    if (!pipe) return "";
    char buffer[4096];
    std::string result = "";
    while (!feof(pipe.get())) {
        if (fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
            result += buffer;
        }
    }
    return result;
}

static std::string parse_json_value(const std::string& json, const std::string& key) {
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

static std::string get_query_param(const std::string& request, const std::string& key) {
    size_t pos = request.find(key + "=");
    if (pos == std::string::npos) return "";
    pos += key.length() + 1;
    size_t end = request.find_first_of(" &\r\n", pos);
    if (end == std::string::npos) return request.substr(pos);
    
    // Simple URL decoding
    std::string val = request.substr(pos, end - pos);
    std::string dec = "";
    for (size_t i = 0; i < val.length(); i++) {
        if (val[i] == '%' && i + 2 < val.length()) {
            char hex[3] = { val[i+1], val[i+2], '\0' };
            dec += (char)std::strtol(hex, nullptr, 16);
            i += 2;
        } else if (val[i] == '+') {
            dec += ' ';
        } else {
            dec += val[i];
        }
    }
    return dec;
}

static std::string get_host_header(const std::string& request) {
    size_t pos = request.find("Host: ");
    if (pos == std::string::npos) return "192.168.4.110:8080";
    pos += 6;
    size_t end = request.find_first_of("\r\n", pos);
    if (end == std::string::npos) return "192.168.4.110:8080";
    return request.substr(pos, end - pos);
}

static std::string get_setup_html(const std::string& redirect_uri) {
    std::string html = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>piTrove - Google Photos Setup</title>
    <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600;800&family=JetBrains+Mono&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg-grad: linear-gradient(135deg, #09090b 0%, #1e0b36 50%, #03001e 100%);
            --accent: #d946ef;
            --accent-glow: rgba(217, 70, 239, 0.4);
            --neon-blue: #06b6d4;
            --card-bg: rgba(255, 255, 255, 0.03);
            --card-border: rgba(255, 255, 255, 0.08);
            --glass-blur: blur(20px);
            --text-main: #f8fafc;
            --text-muted: #94a3b8;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: 'Outfit', sans-serif;
            background: var(--bg-grad);
            background-attachment: fixed;
            color: var(--text-main);
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 2rem 1rem;
        }
        .card {
            width: 100%;
            max-width: 550px;
            background: var(--card-bg);
            border: 1px solid var(--card-border);
            backdrop-filter: var(--glass-blur);
            -webkit-backdrop-filter: var(--glass-blur);
            border-radius: 24px;
            padding: 2.5rem;
            box-shadow: 0 16px 40px 0 rgba(0, 0, 0, 0.45);
            position: relative;
            overflow: hidden;
        }
        .card::before {
            content: '';
            position: absolute;
            top: 0; left: 0; right: 0; height: 4px;
            background: linear-gradient(90deg, var(--accent), var(--neon-blue));
        }
        h2 { font-weight: 800; font-size: 2rem; margin-bottom: 0.5rem; text-align: center; }
        .subtitle { font-size: 0.9rem; color: var(--text-muted); text-align: center; margin-bottom: 2rem; }
        .step {
            background: rgba(255, 255, 255, 0.02);
            border: 1px solid rgba(255, 255, 255, 0.05);
            border-radius: 12px;
            padding: 1rem;
            margin-bottom: 1.5rem;
            font-size: 0.9rem;
            line-height: 1.4;
        }
        .step code {
            font-family: 'JetBrains Mono', monospace;
            background: rgba(0,0,0,0.3);
            padding: 0.2rem 0.4rem;
            border-radius: 4px;
            color: var(--neon-blue);
            font-size: 0.85rem;
        }
        .form-group {
            margin-bottom: 1.25rem;
            display: flex;
            flex-direction: column;
            gap: 0.5rem;
        }
        label { font-weight: 600; font-size: 0.85rem; text-transform: uppercase; letter-spacing: 1px; color: var(--text-muted); }
        input[type="text"] {
            background: rgba(0, 0, 0, 0.25);
            border: 1px solid var(--card-border);
            border-radius: 12px;
            padding: 0.8rem 1rem;
            color: var(--text-main);
            font-family: inherit;
            font-size: 0.95rem;
            transition: all 0.3s ease;
        }
        input[type="text"]:focus {
            outline: none;
            border-color: var(--accent);
            box-shadow: 0 0 10px var(--accent-glow);
        }
        button {
            width: 100%;
            background: linear-gradient(90deg, var(--accent), var(--neon-blue));
            border: none;
            border-radius: 12px;
            padding: 1rem;
            color: white;
            font-family: inherit;
            font-size: 1rem;
            font-weight: 800;
            text-transform: uppercase;
            letter-spacing: 1px;
            cursor: pointer;
            transition: all 0.3s ease;
            box-shadow: 0 4px 15px rgba(217, 70, 239, 0.3);
            margin-top: 1rem;
        }
        button:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 20px rgba(217, 70, 239, 0.5);
        }
    </style>
</head>
<body>
    <div class="card">
        <h2>Google Photos Link</h2>
        <p class="subtitle">Step 1: Obtain Google Photos OAuth 2.0 Credentials</p>
        
        <div class="step">
            1. Go to the <a href="https://console.cloud.google.com/" target="_blank" style="color:var(--accent); text-decoration:none; font-weight:600;">Google Cloud Console</a>.<br>
            2. Create a Project, enable the <strong>Google Photos Library API</strong>.<br>
            3. Set up OAuth Consent Screen, and create <strong>OAuth Client ID</strong> credentials for Web Application.<br>
            4. In "Authorized redirect URIs", add exactly:<br>
            <code style="display:block; margin-top:0.4rem; padding: 0.5rem; text-align:center;">redirect_uri</code>
        </div>

        <form action="/google_photos_setup" method="GET">
            <div class="form-group" style="display:none;">
                <input type="text" name="action" value="submit">
            </div>
            <div class="form-group">
                <label for="client_id">OAuth Client ID</label>
                <input type="text" id="client_id" name="client_id" placeholder="Paste Client ID here" required>
            </div>
            
            <div class="form-group">
                <label for="client_secret">OAuth Client Secret</label>
                <input type="text" id="client_secret" name="client_secret" placeholder="Paste Client Secret here" required>
            </div>
            
            <button type="submit">Configure & Authenticate</button>
        </form>
    </div>
</body>
</html>
)HTML";
    
    // Replace redirect_uri
    size_t r_pos = html.find("redirect_uri");
    if (r_pos != std::string::npos) {
        html.replace(r_pos, 12, redirect_uri);
    }
    return html;
}

static std::string get_success_html() {
    std::string html = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>piTrove - Integration Successful</title>
    <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600;800&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg-grad: linear-gradient(135deg, #09090b 0%, #1e0b36 50%, #03001e 100%);
            --accent: #22c55e;
            --accent-glow: rgba(34, 197, 94, 0.4);
            --neon-blue: #06b6d4;
            --card-bg: rgba(255, 255, 255, 0.03);
            --card-border: rgba(255, 255, 255, 0.08);
            --glass-blur: blur(20px);
            --text-main: #f8fafc;
            --text-muted: #94a3b8;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: 'Outfit', sans-serif;
            background: var(--bg-grad);
            background-attachment: fixed;
            color: var(--text-main);
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 2rem 1rem;
        }
        .card {
            width: 100%;
            max-width: 500px;
            background: var(--card-bg);
            border: 1px solid var(--card-border);
            backdrop-filter: var(--glass-blur);
            -webkit-backdrop-filter: var(--glass-blur);
            border-radius: 24px;
            padding: 2.5rem;
            box-shadow: 0 16px 40px 0 rgba(0, 0, 0, 0.45);
            text-align: center;
            position: relative;
            overflow: hidden;
        }
        .card::before {
            content: '';
            position: absolute;
            top: 0; left: 0; right: 0; height: 4px;
            background: linear-gradient(90deg, var(--accent), var(--neon-blue));
        }
        .success-icon {
            width: 80px; height: 80px;
            background: rgba(34, 197, 94, 0.1);
            border: 2px solid var(--accent);
            border-radius: 50%;
            display: flex;
            justify-content: center;
            align-items: center;
            margin: 0 auto 1.5rem;
            color: var(--accent);
            font-size: 2.5rem;
            box-shadow: 0 0 20px var(--accent-glow);
        }
        h2 { font-weight: 800; font-size: 1.8rem; margin-bottom: 0.5rem; }
        p { font-size: 1rem; color: var(--text-muted); line-height: 1.5; margin-bottom: 2rem; }
        .btn {
            display: inline-block;
            background: linear-gradient(90deg, var(--accent), var(--neon-blue));
            border: none;
            border-radius: 12px;
            padding: 0.9rem 2rem;
            color: white;
            font-family: inherit;
            font-size: 0.95rem;
            font-weight: 800;
            text-transform: uppercase;
            text-decoration: none;
            letter-spacing: 1px;
            cursor: pointer;
            transition: all 0.3s ease;
            box-shadow: 0 4px 15px rgba(34, 197, 94, 0.3);
        }
        .btn:hover {
            transform: translateY(-2px);
            box-shadow: 0 6px 20px rgba(34, 197, 94, 0.5);
        }
    </style>
</head>
<body>
    <div class="card">
        <div class="success-icon">✓</div>
        <h2>Integration Successful!</h2>
        <p>piTrove has successfully authenticated with your Google Photos account. Cloud media synchronization is now active and will download images in the background.</p>
        <a href="/" class="btn">Go to Dashboard</a>
    </div>
</body>
</html>
)HTML";
    return html;
}

static std::string get_error_html(const std::string& message) {
    std::string html = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>piTrove - Integration Error</title>
    <link href="https://fonts.googleapis.com/css2?family=Outfit:wght@300;400;600;800&display=swap" rel="stylesheet">
    <style>
        :root {
            --bg-grad: linear-gradient(135deg, #09090b 0%, #1e0b36 50%, #03001e 100%);
            --accent: #ef4444;
            --accent-glow: rgba(239, 68, 68, 0.4);
            --neon-blue: #06b6d4;
            --card-bg: rgba(255, 255, 255, 0.03);
            --card-border: rgba(255, 255, 255, 0.08);
            --glass-blur: blur(20px);
            --text-main: #f8fafc;
            --text-muted: #94a3b8;
        }
        * { box-sizing: border-box; margin: 0; padding: 0; }
        body {
            font-family: 'Outfit', sans-serif;
            background: var(--bg-grad);
            background-attachment: fixed;
            color: var(--text-main);
            min-height: 100vh;
            display: flex;
            justify-content: center;
            align-items: center;
            padding: 2rem 1rem;
        }
        .card {
            width: 100%;
            max-width: 500px;
            background: var(--card-bg);
            border: 1px solid var(--card-border);
            backdrop-filter: var(--glass-blur);
            -webkit-backdrop-filter: var(--glass-blur);
            border-radius: 24px;
            padding: 2.5rem;
            box-shadow: 0 16px 40px 0 rgba(0, 0, 0, 0.45);
            text-align: center;
            position: relative;
            overflow: hidden;
        }
        .card::before {
            content: '';
            position: absolute;
            top: 0; left: 0; right: 0; height: 4px;
            background: linear-gradient(90deg, var(--accent), var(--neon-blue));
        }
        .error-icon {
            width: 80px; height: 80px;
            background: rgba(239, 68, 68, 0.1);
            border: 2px solid var(--accent);
            border-radius: 50%;
            display: flex;
            justify-content: center;
            align-items: center;
            margin: 0 auto 1.5rem;
            color: var(--accent);
            font-size: 2.5rem;
            box-shadow: 0 0 20px var(--accent-glow);
        }
        h2 { font-weight: 800; font-size: 1.8rem; margin-bottom: 0.5rem; }
        p { font-size: 1rem; color: var(--text-muted); line-height: 1.5; margin-bottom: 2rem; }
        .btn {
            display: inline-block;
            background: linear-gradient(90deg, var(--accent), var(--neon-blue));
            border: none;
            border-radius: 12px;
            padding: 0.9rem 2rem;
            color: white;
            font-family: inherit;
            font-size: 0.95rem;
            font-weight: 800;
            text-transform: uppercase;
            text-decoration: none;
            letter-spacing: 1px;
            cursor: pointer;
            transition: all 0.3s ease;
        }
    </style>
</head>
<body>
    <div class="card">
        <div class="error-icon">✗</div>
        <h2>Integration Failed</h2>
        <p>errorMessage</p>
        <a href="/google_photos_setup" class="btn">Try Again</a>
    </div>
</body>
</html>
)HTML";
    
    // Replace error message
    size_t m_pos = html.find("errorMessage");
    if (m_pos != std::string::npos) {
        html.replace(m_pos, 12, message);
    }
    return html;
}

// Premium Glassmorphic Dashboard HTML
static std::string get_dashboard_html() {
    std::string html = R"HTML(
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
    // Replace hardcoded version placeholder with actual build version
    static const std::string placeholder = "v11.0.0 glassmorphic system";
    size_t pos = html.find(placeholder);
    if (pos != std::string::npos) {
        html.replace(pos, placeholder.size(), std::string("v") + VERSION + " glassmorphic system");
    }
    return html;
}

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
    std::string path;
    std::string type;
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
        std::string cache_dir;
        { std::lock_guard<std::mutex> lk(g_config_mtx); cache_dir = g_cfg.cache_dir; }
        std::string db_path = cache_dir + "/cache.db";
        if (std::filesystem::exists(db_path)) {
            db_mb = std::filesystem::file_size(db_path) / (1024.0 * 1024.0);
        }
    } catch (...) {}

    std::ostringstream temp_stream;
    if (temp_c > 0.0) {
        temp_stream << std::fixed << std::setprecision(1) << temp_c << "°C";
    } else {
        temp_stream << "N/A";
    }

    std::ostringstream db_stream;
    if (db_mb > 0.0) {
        db_stream << std::fixed << std::setprecision(2) << db_mb << " MB";
    } else {
        db_stream << "0.00 MB";
    }

    std::ostringstream oss;
    oss << "{\n"
        << "  \"index\": " << idx << ",\n"
        << "  \"total\": " << total << ",\n"
        << "  \"filename\": \"" << escape_json(filename) << "\",\n"
        << "  \"is_video\": " << (type == "video" ? "true" : "false") << ",\n"
        << "  \"shuffle\": " << (shuffle ? "true" : "false") << ",\n"
        << "  \"paused\": " << (paused ? "true" : "false") << ",\n"
        << "  \"temp\": \"" << temp_stream.str() << "\",\n"
        << "  \"db_size\": \"" << db_stream.str() << "\",\n"
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
    (void)write(fd, headers.data(), headers.size());

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

static void handle_client(int client_fd) {
    // Set client socket timeout to prevent slowloris hangs
    struct timeval client_tv;
    { std::lock_guard<std::mutex> lk(g_config_mtx); client_tv.tv_sec = g_cfg.http_socket_timeout; }
    client_tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &client_tv, sizeof(client_tv));
    setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &client_tv, sizeof(client_tv));

    char buffer[8192];
    std::memset(buffer, 0, sizeof(buffer));
    ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
    if (bytes_read > 0) {
        buffer[bytes_read] = '\0';
        std::string request(buffer);

        // Very simple router
        if (request.rfind("GET / ", 0) == 0 || request.rfind("GET /dashboard", 0) == 0) {
            send_response(client_fd, "HTTP/1.1 200 OK", "text/html", get_dashboard_html());
        } 
        else if (request.rfind("GET /google_photos_setup", 0) == 0) {
            std::string action = get_query_param(request, "action");
            if (action == "submit") {
                std::string client_id = get_query_param(request, "client_id");
                std::string client_secret = get_query_param(request, "client_secret");
                
                {
                    std::lock_guard<std::mutex> lk(g_config_mtx);
                    g_cfg.google_photos_client_id = client_id;
                    g_cfg.google_photos_client_secret = client_secret;
                }
                
                std::string host = get_host_header(request);
                std::string redirect_uri = "http://" + host + "/google_photos_callback";
                
                // Build Google OAuth authorization URL
                std::string auth_url = "https://accounts.google.com/o/oauth2/v2/auth?"
                                       "client_id=" + client_id + "&"
                                       "redirect_uri=" + redirect_uri + "&"
                                       "response_type=code&"
                                       "scope=https://www.googleapis.com/auth/photoslibrary.readonly&"
                                       "access_type=offline&"
                                       "prompt=consent";
                                       
                std::ostringstream redirect_headers;
                redirect_headers << "HTTP/1.1 302 Found\r\n"
                                 << "Location: " << auth_url << "\r\n"
                                 << "Connection: close\r\n\r\n";
                (void)write(client_fd, redirect_headers.str().data(), redirect_headers.str().size());
            } else {
                std::string host = get_host_header(request);
                std::string redirect_uri = "http://" + host + "/google_photos_callback";
                send_response(client_fd, "HTTP/1.1 200 OK", "text/html", get_setup_html(redirect_uri));
            }
        }
        else if (request.rfind("GET /google_photos_callback", 0) == 0) {
            std::string code = get_query_param(request, "code");
            std::string error = get_query_param(request, "error");
            
            if (!error.empty()) {
                send_response(client_fd, "HTTP/1.1 200 OK", "text/html", get_error_html("Access denied or authentication cancelled: " + error));
            } else if (code.empty()) {
                send_response(client_fd, "HTTP/1.1 200 OK", "text/html", get_error_html("OAuth code parameter is missing."));
            } else {
                // Exchange code for token
                std::string client_id, client_secret;
                {
                    std::lock_guard<std::mutex> lk(g_config_mtx);
                    client_id = g_cfg.google_photos_client_id;
                    client_secret = g_cfg.google_photos_client_secret;
                }
                
                std::string host = get_host_header(request);
                std::string redirect_uri = "http://" + host + "/google_photos_callback";
                
                std::string cmd = "curl -s -X POST https://oauth2.googleapis.com/token "
                                  "-d client_id=\"" + client_id + "\" "
                                  "-d client_secret=\"" + client_secret + "\" "
                                  "-d code=\"" + code + "\" "
                                  "-d redirect_uri=\"" + redirect_uri + "\" "
                                  "-d grant_type=authorization_code";
                                  
                std::string json = execute_curl(cmd);
                std::string refresh_token = parse_json_value(json, "refresh_token");
                
                if (refresh_token.empty()) {
                    // Try to parse error details
                    std::string err_desc = parse_json_value(json, "error_description");
                    if (err_desc.empty()) err_desc = parse_json_value(json, "error");
                    if (err_desc.empty()) err_desc = "Could not obtain refresh token. Note that Google only sends the refresh_token on the FIRST authorization. If you are re-authorizing, go to your Google Account and remove piTrove's permissions first, then retry.";
                    
                    send_response(client_fd, "HTTP/1.1 200 OK", "text/html", get_error_html(err_desc));
                } else {
                    // Save and reload sync thread
                    {
                        std::lock_guard<std::mutex> lk(g_config_mtx);
                        g_cfg.google_photos_refresh_token = refresh_token;
                        g_cfg.google_photos_enabled = true;
                    }
                    g_cfg.save("/app/config/config.toml"); // active config location in container
                    
                    // Clear error and restart background sync thread safely
                    trigger_error(0);
                    
                    g_google_photos.stop();
                    g_google_photos.start();
                    
                    send_response(client_fd, "HTTP/1.1 200 OK", "text/html", get_success_html());
                }
            }
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
            bool expected = g_screen_blanked.load();
            bool desired = !expected;
            while (!g_screen_blanked.compare_exchange_weak(expected, desired)) {
                desired = !expected;
            }
            set_display_power(expected);
            std::string prefix, topic;
            { std::lock_guard<std::mutex> lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; topic = g_cfg.mqtt_motionsensor_topic; }
            mqtt_publish(prefix + "/status/screen", g_screen_blanked.load() ? "OFF" : "ON", true);
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
        }
        else if (request.rfind("GET /api/trigger_motion", 0) == 0) {
            g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
            std::string prefix, sensor_topic;
            { std::lock_guard<std::mutex> lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; sensor_topic = g_cfg.mqtt_motionsensor_topic; }
            if (g_screen_blanked.exchange(false)) {
                set_display_power(true);
                mqtt_publish(prefix + "/status/screen", "ON", true);
            }
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

static void server_loop(int port) {
    struct sockaddr_in server_addr;
    int current_port = port;
    int max_attempts;
    { std::lock_guard<std::mutex> lk(g_config_mtx); max_attempts = g_cfg.http_bind_attempts; }
    bool bound = false;

    for (int attempt = 0; attempt < max_attempts; attempt++) {
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

        g_logger.warn("HTTP: Port %d was in use, trying next port...", current_port);
        close(socket_fd);
        current_port++;
    }

    if (!bound) {
        trigger_error(104); // E104: PORT_BIND_CONFLICT
        return;
    }

    if (current_port != port) {
        g_logger.warn("HTTP: Port %d was in use. Dynamic fallback bound to port %d", port, current_port);
        {
            std::lock_guard<std::mutex> lock(g_config_mtx);
            g_cfg.http_port = current_port;
        }
        g_config_changed.store(true);
    }

    int active_fd = g_listen_fd.load();
    if (active_fd < 0 || listen(active_fd, 10) < 0) {
        g_logger.error("HTTP: Listen failed on socket.");
        int fd_to_close = g_listen_fd.exchange(-1);
        if (fd_to_close >= 0) {
            close(fd_to_close);
        }
        return;
    }

    g_logger.info("HTTP: Background Web Remote server active on port %d", current_port);

    while (g_server_running.load()) {
        int fd = g_listen_fd.load();
        if (fd < 0) break;

        // Set a timeout on accept so it can periodically check if g_server_running is false
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        int select_rc = select(fd + 1, &rfds, nullptr, nullptr, &tv);
        if (select_rc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (select_rc == 0) continue; // select timed out, loop around to check running status

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int current_fd = g_listen_fd.load();
        if (current_fd < 0) break;
        int client_fd = accept4(current_fd, (struct sockaddr*)&client_addr, &client_len, SOCK_CLOEXEC);
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

        // Set a socket read/write timeout to prevent slow/hanging clients from starving the connection pool
        struct timeval timeout;
        {
            std::lock_guard<std::mutex> lk(g_config_mtx);
            timeout.tv_sec = g_cfg.http_socket_timeout;
        }
        timeout.tv_usec = 0;
        if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
            g_logger.warn("HTTP: Failed to set SO_RCVTIMEO on client socket.");
        }
        if (setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
            g_logger.warn("HTTP: Failed to set SO_SNDTIMEO on client socket.");
        }

        int prev = g_active_connections.fetch_add(1);
        if (prev >= 10) {
            g_active_connections.fetch_sub(1);
            send_response(client_fd, "HTTP/1.1 503 Service Unavailable", "text/plain", "Too Many Connections");
            close(client_fd);
            continue;
        }
        spawn_tracked_thread([client_fd]() {
            handle_client(client_fd);
            g_active_connections.fetch_sub(1);
        });
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
    g_server_thread = std::thread(server_loop, port);
}

void stop_http_server() {
    if (!g_server_running.load()) return;
    g_server_running.store(false);
    
    // shutdown socket to interrupt select/accept
    int fd = g_listen_fd.load();
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }
    
    if (g_server_thread.joinable()) {
        g_server_thread.join();
    }

    // Join all tracked client and delay threads cleanly
    {
        std::lock_guard<std::mutex> lk(g_http_threads_mtx);
        for (auto& info : g_http_client_threads) {
            if (info.thread.joinable()) {
                info.thread.join();
            }
        }
        g_http_client_threads.clear();
    }
}

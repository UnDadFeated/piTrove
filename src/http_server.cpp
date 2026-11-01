#include "http_server.h"
#include "http_protocol.h"
#include "http_transfer.h"
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
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
static std::string get_host_header(const std::string& request) {
    http_protocol::Request parsed;
    size_t required;
    if (http_protocol::parse(request, parsed, required) == http_protocol::Result::complete) {
        if (auto it = parsed.headers.find("host"); it != parsed.headers.end()) return it->second;
    }
    std::shared_lock lock(g_config_mtx);
    return std::format("192.168.4.110:{}", g_cfg.http_port);
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

        <form action="/google_photos_setup" method="POST">
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
        html.replace(r_pos, 12, html_escape(redirect_uri));
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
        html.replace(m_pos, 12, html_escape(message));
    }
    return html;
}

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
            --bg-primary: #0f0f11;
            --bg-secondary: #161619;
            --card-bg: rgba(22, 22, 25, 0.75);
            --card-border: rgba(228, 228, 231, 0.15);
            --border-color: rgba(228, 228, 231, 0.15);
            --text-main: #f4f4f5;
            --text-muted: #a1a1aa;
            --glass-blur: blur(20px);
            --shadow-premium: 0 10px 30px rgba(0, 0, 0, 0.5), inset 0 1px 0 rgba(255, 255, 255, 0.05);

            --accent: #e4e4e7;
            --accent-glow: rgba(228, 228, 231, 0.15);
            --accent-hover: #ffffff;
        }

        body.light-theme {
            --bg-primary: #f4f4f5;
            --bg-secondary: #ffffff;
            --card-bg: rgba(255, 255, 255, 0.8);
            --card-border: rgba(39, 39, 42, 0.12);
            --border-color: rgba(39, 39, 42, 0.12);
            --text-main: #18181b;
            --text-muted: #71717a;
            --shadow-premium: 0 10px 30px rgba(0, 0, 0, 0.03), inset 0 1px 0 rgba(255, 255, 255, 0.8);
            
            --accent: #27272a;
            --accent-glow: rgba(39, 39, 42, 0.15);
            --accent-hover: #09090b;
        }

        body.palette-emerald {
            --accent: #10b981;
            --accent-glow: rgba(16, 185, 129, 0.25);
            --accent-hover: #34d399;
        }
        body.palette-sapphire {
            --accent: #3b82f6;
            --accent-glow: rgba(59, 130, 246, 0.25);
            --accent-hover: #60a5fa;
        }
        body.palette-amber {
            --accent: #f59e0b;
            --accent-glow: rgba(245, 158, 11, 0.25);
            --accent-hover: #fbbf24;
        }

        body.light-theme.palette-emerald {
            --accent: #059669;
            --accent-glow: rgba(5, 150, 105, 0.2);
            --accent-hover: #047857;
        }
        body.light-theme.palette-sapphire {
            --accent: #2563eb;
            --accent-glow: rgba(37, 99, 235, 0.2);
            --accent-hover: #1d4ed8;
        }
        body.light-theme.palette-amber {
            --accent: #d97706;
            --accent-glow: rgba(217, 119, 6, 0.2);
            --accent-hover: #b45309;
        }

        * {
            box-sizing: border-box;
            margin: 0;
            padding: 0;
            user-select: none;
        }

        body {
            font-family: 'Outfit', sans-serif;
            background-color: var(--bg-primary);
            color: var(--text-main);
            min-height: 100vh;
            display: flex;
            flex-direction: column;
            align-items: center;
            overflow-x: hidden;
            padding: 2rem 1rem;
            transition: background-color 0.3s ease, color 0.3s ease;
        }

        .container {
            width: 100%;
            max-width: 600px;
            display: flex;
            flex-direction: column;
            gap: 0.8rem;
        }

        header {
            background: var(--card-bg);
            border: 1px solid var(--card-border);
            backdrop-filter: var(--glass-blur);
            -webkit-backdrop-filter: var(--glass-blur);
            border-radius: 24px;
            padding: 1.5rem;
            text-align: center;
            box-shadow: var(--shadow-premium);
            position: relative;
            overflow: hidden;
            transition: all 0.3s ease;
        }

        header::before {
            content: '';
            position: absolute;
            top: 0; left: 0; right: 0; height: 3px;
            background: var(--accent);
        }

        h1 {
            font-weight: 800;
            font-size: 1.8rem;
            letter-spacing: 1.5px;
            color: var(--text-main);
            text-shadow: 0 4px 12px rgba(0,0,0,0.1);
        }

        .subtitle {
            font-size: 0.85rem;
            color: var(--text-muted);
            text-transform: uppercase;
            letter-spacing: 2px;
            margin-top: 0.25rem;
        }

        .tabs {
            display: flex;
            gap: 0.5rem;
            background: var(--bg-secondary);
            border: 1px solid var(--card-border);
            padding: 0.35rem;
            border-radius: 16px;
            transition: all 0.3s ease;
        }
        .tab-btn {
            flex: 1;
            background: transparent;
            border: none;
            padding: 0.75rem;
            color: var(--text-muted);
            font-family: inherit;
            font-weight: 600;
            font-size: 0.95rem;
            border-radius: 12px;
            cursor: pointer;
            transition: all 0.3s ease;
        }
        .tab-btn:hover {
            color: var(--text-main);
            background: rgba(255, 255, 255, 0.05);
        }
        .tab-btn.active {
            color: var(--bg-primary);
            background: var(--accent);
            box-shadow: 0 4px 12px var(--accent-glow);
        }

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
            box-shadow: var(--shadow-premium);
            position: relative;
            transition: all 0.3s ease;
        }

        .preview-container {
            width: 100%;
            aspect-ratio: 4 / 3;
            border-radius: 20px;
            background: #000;
            overflow: hidden;
            position: relative;
            border: 1px solid var(--card-border);
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
            border: 1px solid var(--card-border);
            background: rgba(161, 161, 170, 0.08);
            color: var(--text-main);
        }

        .badge-photo {
            background: rgba(161, 161, 170, 0.12);
        }

        .badge-video {
            background: var(--accent-glow);
            color: var(--accent);
            border-color: var(--accent);
        }

        .btn {
            background: var(--bg-secondary);
            border: 1px solid var(--card-border);
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
            box-shadow: 0 4px 16px rgba(0,0,0,0.15);
        }

        .btn:hover {
            background: rgba(255, 255, 255, 0.05);
            border-color: var(--accent);
            transform: translateY(-2px);
        }

        .btn:active {
            transform: translateY(1px);
        }

        .btn-accent {
            background: var(--bg-secondary);
            border-color: var(--card-border);
        }
        .btn-accent:hover {
            background: var(--accent);
            color: var(--bg-primary);
            box-shadow: 0 0 15px var(--accent-glow);
        }

        .btn-blue {
            background: var(--bg-secondary);
        }
        .btn-blue:hover {
            border-color: var(--accent);
        }

        .btn-icon {
            font-size: 1.5rem;
        }

        .btn-toggle {
            padding: 1rem;
            flex-direction: row;
            gap: 0.75rem;
        }

        .btn-active {
            background: var(--accent);
            color: var(--bg-primary);
            border-color: var(--accent);
        }

        .btn-danger {
            background: var(--bg-secondary);
        }
        .btn-danger:hover {
            background: #ef4444;
            color: #ffffff;
            border-color: #ef4444;
            box-shadow: 0 0 15px rgba(239, 68, 68, 0.4);
        }

        .telemetry-card {
            background: var(--card-bg);
            border: 1px solid var(--card-border);
            backdrop-filter: var(--glass-blur);
            -webkit-backdrop-filter: var(--glass-blur);
            border-radius: 24px;
            padding: 1.25rem;
            box-shadow: var(--shadow-premium);
            transition: all 0.3s ease;
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
            background: var(--accent);
            border-radius: 50%;
            box-shadow: 0 0 8px var(--accent);
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

        .form-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 1rem;
        }
        .form-group {
            display: flex;
            flex-direction: column;
            gap: 0.4rem;
        }
        .form-group label {
            font-weight: 600;
            font-size: 0.75rem;
            text-transform: uppercase;
            letter-spacing: 1px;
            color: var(--text-muted);
        }
        .form-group input[type="text"],
        .form-group input[type="number"],
        .form-group select {
            background: var(--bg-secondary);
            border: 1px solid var(--card-border);
            border-radius: 12px;
            padding: 0.7rem 0.9rem;
            color: var(--text-main);
            font-family: inherit;
            font-size: 0.9rem;
            transition: all 0.3s ease;
        }
        .form-group input:focus,
        .form-group select:focus {
            outline: none;
            border-color: var(--accent);
        }
        .form-group input[type="range"] {
            -webkit-appearance: none;
            width: 100%;
            height: 6px;
            background: var(--bg-primary);
            border-radius: 3px;
        }
        .form-group input[type="range"]::-webkit-slider-thumb {
            -webkit-appearance: none;
            width: 18px;
            height: 18px;
            border-radius: 50%;
            background: var(--accent);
            cursor: pointer;
        }

        .switch-grid {
            display: flex;
            flex-direction: column;
            gap: 0.8rem;
        }
        .switch-item {
            display: flex;
            justify-content: space-between;
            align-items: center;
            padding: 0.5rem 0;
            border-bottom: 1px solid var(--card-border);
        }
        .switch-item:last-child {
            border-bottom: none;
        }
        .switch {
            position: relative;
            display: inline-block;
            width: 46px;
            height: 26px;
        }
        .switch input { opacity: 0; width: 0; height: 0; }
        .slider {
            position: absolute;
            cursor: pointer;
            top: 0; left: 0; right: 0; bottom: 0;
            background-color: var(--bg-primary);
            transition: .4s;
            border-radius: 34px;
            border: 1px solid var(--card-border);
        }
        .slider:before {
            position: absolute;
            content: "";
            height: 18px;
            width: 18px;
            left: 3px;
            bottom: 3px;
            background-color: var(--text-muted);
            transition: .4s;
            border-radius: 50%;
        }
        input:checked + .slider {
            background-color: var(--accent-glow);
            border-color: var(--accent);
        }
        input:checked + .slider:before {
            transform: translateX(20px);
            background-color: var(--accent);
        }

        .log-console {
            background: var(--bg-primary);
            border: 1px solid var(--card-border);
            border-radius: 12px;
            padding: 1rem;
            font-family: 'JetBrains Mono', monospace;
            font-size: 0.65rem;
            color: #4ade80;
            height: 55vh;
            overflow-y: auto;
            white-space: pre-wrap;
            line-height: 1.4;
        }

        .toast {
            position: fixed;
            bottom: 2rem;
            left: 50%;
            transform: translateX(-50%) translateY(100px);
            background: var(--bg-secondary);
            border: 1px solid var(--accent);
            color: var(--text-main);
            padding: 1rem 2rem;
            border-radius: 16px;
            backdrop-filter: var(--glass-blur);
            -webkit-backdrop-filter: var(--glass-blur);
            box-shadow: 0 8px 32px rgba(0, 0, 0, 0.5), 0 0 15px var(--accent-glow);
            font-weight: 600;
            transition: transform 0.4s cubic-bezier(0.175, 0.885, 0.32, 1.275);
            z-index: 1000;
            pointer-events: none;
        }
        .toast.show {
            transform: translateX(-50%) translateY(0);
        }

        .btn-save {
            width: 100%;
            border-radius: 14px;
            padding: 1rem;
            font-size: 1rem;
            font-weight: 800;
            cursor: pointer;
            transition: all 0.3s ease;
            background: var(--accent);
            color: var(--bg-primary);
            border: 1px solid var(--card-border);
        }
        .btn-save:hover {
            transform: translateY(-2px);
            box-shadow: 0 0 15px var(--accent-glow);
        }
        @media (max-width: 480px) {
            h1 { font-size: 1.5rem; }
            .form-grid { grid-template-columns: 1fr; }
            .playback-controls-row {
                flex-wrap: wrap;
            }
            .playback-controls-row .btn {
                flex: 1 1 calc(50% - 0.25rem);
                min-width: 0;
                padding: 0.6rem;
                font-size: 0.85rem;
                border-radius: 12px;
            }
            .action-buttons-row {
                flex-wrap: wrap;
            }
            .action-buttons-row .btn {
                flex: 1 1 calc(33.33% - 0.33rem);
                min-width: 0;
                padding: 0.6rem;
                font-size: 0.85rem;
            }
            .preview-container {
                border-radius: 16px;
            }
            .telemetry-grid {
                grid-template-columns: repeat(2, 1fr) !important;
            }
            .telemetry-item[style*="grid-column: span 3"] {
                grid-column: span 2 !important;
            }
            .tabs {
                padding: 0.25rem;
            }
        }
        }
    </style>
</head>
<body>
    <div class="container">
        <header style="position: relative;">
            <div style="position: absolute; right: 1rem; top: 1.25rem; display: flex; gap: 0.5rem; align-items: center; z-index: 10;">
                <select id="palette-select" onchange="changePalette(this.value)" style="background: var(--card-bg); color: var(--text-main); border: 1px solid var(--card-border); border-radius: 8px; padding: 0.25rem 0.5rem; font-size: 0.75rem; cursor: pointer; outline: none; color-scheme:dark;">
                    <option value="grey">Zinc</option>
                    <option value="emerald">Emerald</option>
                    <option value="sapphire">Sapphire</option>
                    <option value="amber">Amber</option>
                </select>
                <button onclick="toggleTheme()" id="theme-btn" style="background: var(--card-bg); color: var(--text-main); border: 1px solid var(--card-border); border-radius: 8px; padding: 0.25rem 0.5rem; font-size: 0.75rem; cursor: pointer; outline: none; color-scheme:dark;">🌙</button>
            </div>
            <h1>piTrove controller</h1>
            <div class="subtitle">v17.7.10 glassmorphic system</div>
        </header>

        <div class="tabs">
            <button class="tab-btn active" onclick="switchTab('remote')">Remote</button>
            <button class="tab-btn" onclick="switchTab('settings')">Settings</button>
            <button class="tab-btn" onclick="switchTab('logs')">Diagnostics</button>
        </div>

        <div id="tab-remote-content" class="tab-content">
            <div style="display: flex; flex-direction: column; gap: 0.8rem;">
                <div class="preview-card">
                    <div class="preview-container">
                        <img id="preview" class="preview-image" src="/api/preview" alt="Preview" onload="onPreviewLoaded()" onerror="onPreviewError()">
                        <div id="loading" class="preview-loading">Syncing...</div>
                    </div>
                    <div class="media-info">
                        <div id="media-title" class="media-title" style="margin-bottom: 0.4rem;">Fetching media...</div>
                        <div class="media-meta" style="margin-bottom: 0.6rem; display: flex; width: 100%; align-items: center; gap: 0.5rem;">
                            <span id="media-type" class="badge">IMAGE</span>
                            <span id="media-progress">0 / 0</span>
                            <span id="media-timer" class="badge" style="background: rgba(161, 161, 170, 0.15); border: 1px solid var(--border-color); color: var(--text-main); margin-left: auto; display: none;">--s</span>
                        </div>
                    </div>
                    <div class="playback-controls-row" style="display: flex; gap: 0.5rem; width: 100%;">
                        <button class="btn btn-blue" onclick="sendCommand('/api/prev')" style="flex: 1; padding: 0.75rem; border-radius: 12px; font-size: 0.9rem; flex-direction: row; gap: 0.2rem;"><span class="btn-icon">⏮</span></button>
                        <button id="btn-pause" class="btn btn-accent" onclick="sendCommand('/api/pause')" style="flex: 1.5; padding: 0.75rem; border-radius: 12px; font-size: 0.9rem; flex-direction: row; gap: 0.2rem;"><span id="icon-pause" class="btn-icon">⏸</span><span id="txt-pause">Pause</span></button>
                        <button class="btn btn-blue" onclick="sendCommand('/api/next')" style="flex: 1; padding: 0.75rem; border-radius: 12px; font-size: 0.9rem; flex-direction: row; gap: 0.2rem;"><span class="btn-icon">⏭</span></button>
                        <button id="btn-shuffle" class="btn btn-toggle" onclick="sendCommand('/api/toggle_shuffle')" style="flex: 1; padding: 0.75rem; border-radius: 12px; font-size: 0.9rem; flex-direction: row; gap: 0.2rem;"><span class="btn-icon">🔀</span><span><strong id="lbl-shuffle">ON</strong></span></button>
                    </div>
                </div>

                <div class="telemetry-card">
                    <div class="telemetry-title">System & Automation</div>
                    <div class="telemetry-grid" style="grid-template-columns: repeat(3, 1fr); gap: 0.5rem; margin-bottom: 0.75rem;">
                        <div class="telemetry-item">
                            <span class="telemetry-label">Uptime</span>
                            <span id="stat-uptime" class="telemetry-value">--</span>
                        </div>
                        <div class="telemetry-item">
                            <span class="telemetry-label">CPU Temp</span>
                            <span id="stat-temp" class="telemetry-value">--°C</span>
                        </div>
                        <div class="telemetry-item">
                            <span class="telemetry-label">Memory</span>
                            <span id="stat-mem" class="telemetry-value">--</span>
                        </div>
                        <div class="telemetry-item">
                            <span class="telemetry-label">Disk</span>
                            <span id="stat-disk" class="telemetry-value">--</span>
                        </div>
                        <div class="telemetry-item">
                            <span class="telemetry-label">Cache DB</span>
                            <span id="stat-db" class="telemetry-value">-- MB</span>
                        </div>
                        <div class="telemetry-item">
                            <span class="telemetry-label">Queue</span>
                            <span id="stat-queue" class="telemetry-value">--</span>
                        </div>
                        <div class="telemetry-item" style="grid-column: span 3; border-top: 1px solid var(--border-color); padding-top: 0.4rem; margin-top: 0.2rem;">
                            <span class="telemetry-label">MQTT</span>
                            <span id="stat-mqtt-status" class="telemetry-value" style="font-size: 0.95rem; font-family: inherit;">Disabled</span>
                            <span id="stat-mqtt-broker" style="font-size: 0.75rem; color: var(--text-muted); display: block; margin-top: 0.1rem;">--</span>
                        </div>
                    </div>
                    <div class="action-buttons-row" style="display: flex; gap: 0.5rem; width: 100%;">
                        <button id="btn-screen" class="btn btn-toggle" onclick="sendCommand('/api/toggle_screen')" style="flex: 1.2; padding: 0.6rem; border-radius: 12px; font-size: 0.85rem; flex-direction: row; gap: 0.4rem;"><span class="btn-icon" style="font-size: 1rem;">📺</span><span>Screen: <strong id="lbl-screen">ON</strong></span></button>
                        <button class="btn btn-danger" onclick="confirmRestart()" style="flex: 1; padding: 0.6rem; border-radius: 12px; font-size: 0.85rem; flex-direction: row; gap: 0.4rem;"><span class="btn-icon" style="font-size: 1rem;">🔄</span><span>Restart</span></button>
                    </div>
                </div>
            </div>
        </div>

        <div id="tab-settings-content" class="tab-content" style="display: none;">
            <div class="telemetry-card">
                <div class="telemetry-title">App Settings</div>
                <form id="settings-form" onsubmit="event.preventDefault(); saveSettings();" style="display: flex; flex-direction: column; gap: 1.2rem;">
                    <div class="form-group" style="grid-column: 1 / -1;"><label for="set-api-key">API Key for /api/settings/update</label><input type="password" id="set-api-key" placeholder="Set API key (blank = no auth)"><span id="api-key-status" style="font-size:0.75rem;color:var(--text-muted);margin-left:0.5rem;"></span></div>
                    <div>
                        <h3 style="font-size: 0.85rem; text-transform: uppercase; color: var(--accent); letter-spacing: 1px; margin-bottom: 0.6rem; border-bottom: 1px solid var(--border-color); padding-bottom: 0.2rem; font-weight: 800;">Slideshow & Playback</h3>
                        <div class="form-grid" style="display: grid; grid-template-columns: repeat(2, 1fr); gap: 0.8rem; margin-bottom: 0.6rem;">
                            <div class="form-group"><label for="set-transition-delay">Interval</label><select id="set-transition-delay" style="width:100%; padding:0.4rem; border-radius:8px; border:1px solid var(--card-border); background:var(--card-bg); color:var(--text-main); color-scheme:dark;"><option value="30">30s</option>
<option value="60">60s</option>
<option value="90">90s</option>
<option value="120" selected>120s (2m)</option>
<option value="150">150s (2m)</option>
<option value="180">180s (3m)</option>
<option value="210">210s (3m)</option>
<option value="240">240s (4m)</option>
<option value="270">270s (4m)</option>
<option value="300">300s (5m)</option></select></div>
                            <div class="form-group"><label for="set-video-volume">Video Volume</label><div style="display:flex; align-items:center; gap:0.5rem;"><input type="range" id="set-video-volume" min="0" max="150" style="flex:1;" oninput="document.getElementById('lbl-video-volume-val').innerText = this.value + '%'"><span id="lbl-video-volume-val">--%</span></div></div>
                        </div>
                        <div style="display: grid; grid-template-columns: repeat(2, 1fr); gap: 0.8rem;">
                            <div style="display:flex; align-items:center; gap:0.4rem;"><input type="checkbox" id="set-shuffle" style="cursor:pointer;"><label for="set-shuffle" style="font-size:0.85rem; color:var(--text-muted); cursor:pointer;">Shuffle Playlist</label></div>
                            <div style="display:flex; align-items:center; gap:0.4rem;"><input type="checkbox" id="set-ken-burns" style="cursor:pointer;"><label for="set-ken-burns" style="font-size:0.85rem; color:var(--text-muted); cursor:pointer;">Ken Burns Effect</label></div>
                        </div>
                    </div>
                    
                    <div>
                        <h3 style="font-size: 0.85rem; text-transform: uppercase; color: var(--accent); letter-spacing: 1px; margin-bottom: 0.6rem; border-bottom: 1px solid var(--border-color); padding-bottom: 0.2rem; font-weight: 800;">Display Options</h3>
                        <div style="display: grid; grid-template-columns: repeat(2, 1fr); gap: 0.8rem;">
                            <div style="display:flex; align-items:center; gap:0.4rem;"><input type="checkbox" id="set-blurred-background" style="cursor:pointer;"><label for="set-blurred-background" style="font-size:0.85rem; color:var(--text-muted); cursor:pointer;">Blurred Background</label></div>
                            <div style="display:flex; align-items:center; gap:0.4rem;"><input type="checkbox" id="set-color-matched-matte" style="cursor:pointer;"><label for="set-color-matched-matte" style="font-size:0.85rem; color:var(--text-muted); cursor:pointer;">Color-Matched Matte</label></div>
                            <div style="display:flex; align-items:center; gap:0.4rem;"><input type="checkbox" id="set-matting" style="cursor:pointer;"><label for="set-matting" style="font-size:0.85rem; color:var(--text-muted); cursor:pointer;">Matte Border (1")</label></div>
                            <div style="display:flex; align-items:center; gap:0.4rem;"><label for="set-matting-size" style="font-size:0.85rem; color:var(--text-muted);">Matte Size:</label><input type="number" id="set-matting-size" min="0" max="500" style="width:65px; background:rgba(0,0,0,0.3); border:1px solid var(--border-color); color:var(--text); border-radius:4px; padding:2px 5px; font-size:0.85rem;"><span style="font-size:0.75rem; color:var(--text-muted);">px</span></div>
                            <div style="display:flex; align-items:center; gap:0.4rem;"><input type="checkbox" id="set-geotag-enabled" style="cursor:pointer;"><label for="set-geotag-enabled" style="font-size:0.85rem; color:var(--text-muted); cursor:pointer;">GPS Geotag Overlay</label></div>
                        </div>
                    </div>

                    <div>
                        <h3 style="font-size: 0.85rem; text-transform: uppercase; color: var(--accent); letter-spacing: 1px; margin-bottom: 0.6rem; border-bottom: 1px solid var(--border-color); padding-bottom: 0.2rem; font-weight: 800;">Input & Touch</h3>
                        <div style="display: grid; grid-template-columns: repeat(2, 1fr); gap: 0.8rem;">
                            <div style="display:flex; align-items:center; gap:0.4rem;"><input type="checkbox" id="set-touch-enabled" style="cursor:pointer;"><label for="set-touch-enabled" style="font-size:0.85rem; color:var(--text-muted); cursor:pointer;">Enable Touchscreen Mode</label></div>
                        </div>
                    </div>
                    
                                        <div>
                        <h3 style="font-size: 0.85rem; text-transform: uppercase; color: var(--accent); letter-spacing: 1px; margin-bottom: 0.6rem; border-bottom: 1px solid var(--border-color); padding-bottom: 0.2rem; font-weight: 800;">Infopanels & Widgets (News & Calendar)</h3>
                        <div class="form-grid" style="display: grid; grid-template-columns: repeat(2, 1fr); gap: 0.8rem; margin-bottom: 0.6rem;">
                            <div class="form-group">
                                <label for="set-timezone">Display Timezone</label>
                                <select id="set-timezone" style="width:100%; padding:0.4rem; border-radius:8px; border:1px solid var(--card-border); background:var(--card-bg); color:var(--text-main); color-scheme:dark;">
                                    <option value="UTC">UTC</option>
                                    <option value="America/Los_Angeles">America/Los_Angeles (PST/PDT)</option>
                                    <option value="America/Denver">America/Denver (MST/MDT)</option>
                                    <option value="America/Chicago">America/Chicago (CST/CDT)</option>
                                    <option value="America/New_York">America/New_York (EST/EDT)</option>
                                    <option value="Europe/London">Europe/London (GMT/BST)</option>
                                    <option value="Europe/Paris">Europe/Paris (CET/CEST)</option>
                                    <option value="Europe/Berlin">Europe/Berlin (CET/CEST)</option>
                                    <option value="Asia/Tokyo">Asia/Tokyo (JST)</option>
                                    <option value="Asia/Shanghai">Asia/Shanghai (CST)</option>
                                    <option value="Asia/Kolkata">Asia/Kolkata (IST)</option>
                                    <option value="Australia/Sydney">Australia/Sydney (AEST/AEDT)</option>
                                </select>
                            </div>
                            <div class="form-group">
                                <label for="set-news-query">News Zipcode / Location</label>
                                <input type="text" id="set-news-query" placeholder="e.g. 10001 or 95624" style="width:100%; padding:0.4rem; border-radius:8px; border:1px solid var(--card-border); background:var(--card-bg); color:var(--text-main);">
                            </div>
                            <div class="form-group">
                                <label for="set-news-blacklist">Blacklist News Domains</label>
                                <input type="text" id="set-news-blacklist" placeholder="e.g. realtor.com, legacy.com, obituary, echovita.com" style="width:100%; padding:0.4rem; border-radius:8px; border:1px solid var(--card-border); background:var(--card-bg); color:var(--text-main);">
                            </div>
                        </div>
                        <div style="display: grid; grid-template-columns: repeat(2, 1fr); gap: 0.8rem; margin-bottom: 0.6rem;">
                            <div style="display:flex; align-items:center; gap:0.4rem;"><input type="checkbox" id="set-infopanels-enabled" style="cursor:pointer;"><label for="set-infopanels-enabled" style="font-size:0.85rem; color:var(--text-muted); cursor:pointer;">Enable Infopanels</label></div>
                            <div style="display:flex; align-items:center; gap:0.4rem;"><input type="checkbox" id="set-news-enabled" style="cursor:pointer;"><label for="set-news-enabled" style="font-size:0.85rem; color:var(--text-muted); cursor:pointer;">Live News Ticker</label></div>
                            <div style="display:flex; align-items:center; gap:0.4rem;"><input type="checkbox" id="set-gcalendar-enabled" style="cursor:pointer;"><label for="set-gcalendar-enabled" style="font-size:0.85rem; color:var(--text-muted); cursor:pointer;">Google Calendar Sidebar</label></div>
                            <div style="display:flex; align-items:center; gap:0.4rem;"><label for="set-gcalendar-name" style="font-size:0.85rem; color:var(--text-muted);">Calendar Name:</label><input type="text" id="set-gcalendar-name" placeholder="Family" style="width:90px; background:rgba(0,0,0,0.3); border:1px solid var(--border-color); color:var(--text); border-radius:4px; padding:2px 5px; font-size:0.85rem;"></div>
                        </div>
                        <div class="form-group" style="margin-top:0.4rem;">
                            <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:0.2rem;">
                                <label for="set-gcalendar-url" style="margin-bottom:0;">Google Calendar iCal Secret URL</label>
                                <button type="button" onclick="syncCalendarNow(this)" style="background:rgba(0,200,255,0.15); border:1px solid rgba(0,200,255,0.4); color:var(--accent); font-size:0.75rem; padding:2px 8px; border-radius:4px; cursor:pointer; font-weight:500;">↻ Sync Now</button>
                            </div>
                            <input type="text" id="set-gcalendar-url" placeholder="https://calendar.google.com/calendar/ical/.../basic.ics" style="width:100%; background:rgba(0,0,0,0.3); border:1px solid var(--border-color); color:var(--text); border-radius:4px; padding:4px 8px; font-size:0.8rem;">
                        </div>
                    </div>

                    <button type="submit" class="btn btn-save" style="margin-top:0.4rem;">Save Configuration</button>
                </form>
                <div style="margin-top: 1.2rem; font-size: 0.75rem; color: var(--text-muted); text-align: center; border-top: 1px solid var(--card-border); padding-top: 0.8rem; line-height: 1.4;">
                    More configuration options are available via the <code style="background: rgba(0,0,0,0.25); padding: 0.15rem 0.35rem; border-radius: 6px; font-family: 'JetBrains Mono', monospace; font-size: 0.7rem; color: var(--accent); border: 1px solid var(--card-border);">ssh pitrove config</code> command.
                </div>
            </div>
        </div>

        <div id="tab-logs-content" class="tab-content" style="display: none;">
            <div class="telemetry-card">
                <div class="telemetry-title">Diagnostics Logs</div>
                <div class="log-console" id="log-console">Fetching logs...</div>
            </div>
        </div>
    </div>

    <div id="toast" class="toast">Configuration Saved Successfully!</div>

    <script>
        const savedTheme = localStorage.getItem('pitrove-theme') || 'dark-theme';
        const savedPalette = localStorage.getItem('pitrove-palette') || 'grey';
        document.body.className = savedTheme + ' palette-' + savedPalette;
        
        function toggleTheme() {
            const isLight = document.body.classList.contains('light-theme');
            const newTheme = isLight ? 'dark-theme' : 'light-theme';
            document.body.className = newTheme + ' palette-' + savedPalette;
            localStorage.setItem('pitrove-theme', newTheme);
        }

        function changePalette(palette) {
            document.body.className = savedTheme + ' palette-' + palette;
            localStorage.setItem('pitrove-palette', palette);
        }

        let logInterval = null;
        let lastFilename = "";
        let lastPaused = null;
        function switchTab(tabId) {
            document.querySelectorAll('.tab-content').forEach(el => el.style.display = 'none');
            document.querySelectorAll('.tab-btn').forEach(el => el.classList.remove('active'));
            document.getElementById(`tab-${tabId}-content`).style.display = 'block';
            event.target.classList.add('active');

            if (tabId === 'settings') {
                loadSettings();
                stopLogStreaming();
            } else if (tabId === 'logs') {
                startLogStreaming();
            } else {
                stopLogStreaming();
            }
        }

        function showToast(message) {
            const toast = document.getElementById('toast');
            toast.innerText = message;
            toast.classList.add('show');
            setTimeout(() => {
                toast.classList.remove('show');
            }, 3000);
        }

        async function syncCalendarNow(btn) {
            if (btn) { btn.disabled = true; btn.innerText = "Syncing..."; }
            try {
                const apiKey = localStorage.getItem('api_key') || '';
                const headers = {};
                if (apiKey) headers['Authorization'] = 'Bearer ' + apiKey;
                const res = await fetch('/api/calendar/sync', { method: 'POST', headers });
                if (res.ok) {
                    showToast("Calendar Sync Triggered!");
                } else {
                    showToast("Failed to trigger calendar sync.");
                }
            } catch (err) {
                showToast("Error triggering calendar sync.");
            } finally {
                if (btn) {
                    setTimeout(() => { btn.disabled = false; btn.innerText = "↻ Sync Now"; }, 2000);
                }
            }
        }

        async function loadSettings() {
            try {
                const apiKey = localStorage.getItem('api_key') || '';
                const headers = {};
                if (apiKey) headers['Authorization'] = 'Bearer ' + apiKey;
                const res = await fetch('/api/settings', { headers });
                if (res.status === 401) {
                    const entered = prompt("Enter API Key to load settings:");
                    if (entered) {
                        localStorage.setItem('api_key', entered);
                        loadSettings();
                    }
                    return;
                }
                if (res.ok) {
                    const settings = await res.json();
                    document.getElementById('set-transition-delay').value = settings.transition_delay;
                    document.getElementById('set-video-volume').value = settings.video_volume;
                    document.getElementById('lbl-video-volume-val').innerText = settings.video_volume + '%';
                    
                    document.getElementById('set-shuffle').checked = settings.shuffle;
                    document.getElementById('set-ken-burns').checked = settings.ken_burns;
                    document.getElementById('set-blurred-background').checked = settings.blurred_background;
                    document.getElementById('set-color-matched-matte').checked = settings.color_matched_matte;
                    if (document.getElementById('set-matting')) {
                        document.getElementById('set-matting').checked = !!settings.matting;
                    }
                    if (document.getElementById('set-matting-size')) {
                        document.getElementById('set-matting-size').value = settings.matting_size !== undefined ? settings.matting_size : 96;
                    }
                    document.getElementById('set-touch-enabled').checked = settings.touch_enabled;
                    if (document.getElementById('set-timezone') && settings.timezone) {
                        document.getElementById('set-timezone').value = settings.timezone;
                    }
                    if (document.getElementById('set-infopanels-enabled')) {
                        document.getElementById('set-infopanels-enabled').checked = !!settings.infopanels_enabled;
                    }
                    if (document.getElementById('set-news-enabled')) {
                        document.getElementById('set-news-enabled').checked = !!settings.news_enabled;
                    }
                    if (document.getElementById('set-news-source') && settings.news_source) {
                        document.getElementById('set-news-source').value = settings.news_source;
                    }
                    if (document.getElementById('set-news-query') && settings.news_local_query) {
                        document.getElementById('set-news-query').value = settings.news_local_query;
                    }
                    if (document.getElementById('set-news-blacklist') && settings.news_blacklist) {
                        document.getElementById('set-news-blacklist').value = settings.news_blacklist;
                    }
                    if (document.getElementById('set-gcalendar-enabled')) {
                        document.getElementById('set-gcalendar-enabled').checked = !!settings.gcalendar_enabled;
                    }
                    if (document.getElementById('set-gcalendar-name') && settings.gcalendar_name) {
                        document.getElementById('set-gcalendar-name').value = settings.gcalendar_name;
                    }
                    if (document.getElementById('set-gcalendar-url') && settings.gcalendar_ical_url) {
                        document.getElementById('set-gcalendar-url').value = settings.gcalendar_ical_url;
                    }
                    if (document.getElementById('set-geotag-enabled')) {
                        document.getElementById('set-geotag-enabled').checked = !!settings.geotag_enabled;
                    }
                    document.getElementById('api-key-status').textContent = settings.api_key_set ? '(configured)' : '(not set)';
                    if (settings.api_key_set && apiKey) {
                        document.getElementById('set-api-key').value = apiKey;
                    }
                }
            } catch (err) {
                console.error("Failed to load settings:", err);
            }
        }

        async function saveSettings() {
            const delay = document.getElementById('set-transition-delay').value;
            const volume = document.getElementById('set-video-volume').value;
            const shuffle = document.getElementById('set-shuffle').checked ? "1" : "0";
            const kenBurns = document.getElementById('set-ken-burns').checked ? "1" : "0";
            const blurredBg = document.getElementById('set-blurred-background').checked ? "1" : "0";
            const matte = document.getElementById('set-color-matched-matte').checked ? "1" : "0";
            const matting = (document.getElementById('set-matting') && document.getElementById('set-matting').checked) ? "1" : "0";
            const mattingSize = document.getElementById('set-matting-size') ? document.getElementById('set-matting-size').value : "96";
            const touch = document.getElementById('set-touch-enabled').checked ? "1" : "0";
            const tz = document.getElementById('set-timezone') ? encodeURIComponent(document.getElementById('set-timezone').value) : "UTC";
            const info = (document.getElementById('set-infopanels-enabled') && document.getElementById('set-infopanels-enabled').checked) ? "1" : "0";
            const news = (document.getElementById('set-news-enabled') && document.getElementById('set-news-enabled').checked) ? "1" : "0";
            const newsSrc = document.getElementById('set-news-source') ? encodeURIComponent(document.getElementById('set-news-source').value) : "global";
            const newsQuery = document.getElementById('set-news-query') ? encodeURIComponent(document.getElementById('set-news-query').value) : "10001";
            const newsBl = document.getElementById('set-news-blacklist') ? encodeURIComponent(document.getElementById('set-news-blacklist').value) : "realtor.com, legacy.com, legacy obituary, obituary, echovita.com, tributes.com, dignitymemorial.com, everloved.com";
            const gcal = (document.getElementById('set-gcalendar-enabled') && document.getElementById('set-gcalendar-enabled').checked) ? "1" : "0";
            const gcalName = document.getElementById('set-gcalendar-name') ? encodeURIComponent(document.getElementById('set-gcalendar-name').value) : "Family";
            const gcalUrl = document.getElementById('set-gcalendar-url') ? encodeURIComponent(document.getElementById('set-gcalendar-url').value) : "";
            const geotag = (document.getElementById('set-geotag-enabled') && document.getElementById('set-geotag-enabled').checked) ? "1" : "0";
            
            try {
                const apiKey = document.getElementById('set-api-key').value;
                const url = `/api/settings/update?timezone=${tz}&infopanels_enabled=${info}&news_enabled=${news}&news_source=${newsSrc}&news_local_query=${newsQuery}&news_blacklist=${newsBl}&gcalendar_enabled=${gcal}&gcalendar_name=${gcalName}&gcalendar_ical_url=${gcalUrl}&transition_delay=${delay}&video_volume=${volume}&shuffle=${shuffle}&ken_burns=${kenBurns}&blurred_background=${blurredBg}&color_matched_matte=${matte}&matting=${matting}&matting_size=${mattingSize}&touch_enabled=${touch}&geotag_enabled=${geotag}`;
                const headers = {};
                if (apiKey) {
                    headers['Authorization'] = 'Bearer ' + apiKey;
                }
                const res = await fetch(url, { headers });
                if (res.ok) {
                    if (apiKey) {
                        localStorage.setItem('api_key', apiKey);
                    } else {
                        localStorage.removeItem('api_key');
                    }
                    showToast("Configuration Saved Successfully!");
                } else {
                    showToast("Failed to save configuration.");
                }
            } catch (err) {
                showToast("Error saving configuration.");
            }
        }

        async function fetchLogs() {
            try {
                const apiKey = localStorage.getItem('api_key') || '';
                const headers = {};
                if (apiKey) headers['Authorization'] = 'Bearer ' + apiKey;
                const res = await fetch('/api/logs', { headers });
                if (res.ok) {
                    const data = await res.json();
                    const consoleEl = document.getElementById('log-console');
                    const isScrolledToBottom = consoleEl.scrollHeight - consoleEl.clientHeight - consoleEl.scrollTop < 50;
                    consoleEl.innerText = data.logs;
                    if (isScrolledToBottom) {
                        consoleEl.scrollTop = consoleEl.scrollHeight;
                    }
                }
            } catch (err) {
                console.error("Failed to fetch logs:", err);
            }
        }

        function startLogStreaming() {
            fetchLogs();
            if (!logInterval) {
                logInterval = setInterval(fetchLogs, 2000);
            }
        }

        function stopLogStreaming() {
            if (logInterval) {
                clearInterval(logInterval);
                logInterval = null;
            }
        }

        async function fetchStatus() {
            try {
                const res = await fetch('/api/status');
                if (res.ok) {
                    const status = await res.json();
                    
                    if (status.filename !== lastFilename) {
                        lastFilename = status.filename;
                        const loadingEl = document.getElementById('loading');
                        loadingEl.innerText = "Syncing...";
                        loadingEl.style.opacity = '1';
                        document.getElementById('preview').src = "/api/preview?t=" + new Date().getTime();
                    } else if (status.paused !== lastPaused) {
                        lastPaused = status.paused;
                        document.getElementById('preview').src = "/api/preview?t=" + new Date().getTime();
                    }
                    
                    document.getElementById('media-title').innerText = status.filename;
                    document.getElementById('media-progress').innerText = `${status.index + 1} / ${status.total}`;
                    const typeBadge = document.getElementById('media-type');
                    typeBadge.innerText = status.is_video ? "Video" : "Photo";
                    typeBadge.className = status.is_video ? "badge badge-video" : "badge badge-photo";
                    
                    if (!status.is_video) {
                        document.getElementById('media-timer').style.display = "inline-block";
                        document.getElementById('media-timer').innerText = `${Math.max(0, Math.round(status.transition_delay - (status.item_timer || 0)))}s`;
                    } else {
                        document.getElementById('media-timer').style.display = "none";
                    }

                    // Update control buttons active states & labels
                    document.getElementById('lbl-shuffle').innerText = status.shuffle ? "ON" : "OFF";
                    document.getElementById('lbl-screen').innerText = status.screen_blanked ? "OFF" : "ON";
                    document.getElementById('icon-pause').innerText = status.paused ? "▶" : "⏸";
                    document.getElementById('txt-pause').innerText = status.paused ? "Resume" : "Pause";

                    if (status.shuffle) {
                        document.getElementById('btn-shuffle').classList.add('active');
                    } else {
                        document.getElementById('btn-shuffle').classList.remove('active');
                    }
                    if (!status.screen_blanked) {
                        document.getElementById('btn-screen').classList.add('active');
                    } else {
                        document.getElementById('btn-screen').classList.remove('active');
                    }

                    // Update stats telemetry
                    document.getElementById('stat-uptime').innerText = status.uptime;
                    document.getElementById('stat-temp').innerText = status.temp;
                    document.getElementById('stat-mem').innerText = status.mem;
                    document.getElementById('stat-disk').innerText = status.disk;
                    document.getElementById('stat-db').innerText = status.db_size;
                    document.getElementById('stat-queue').innerText = status.total;

                    // Update MQTT details
                    const mqttStatusEl = document.getElementById('stat-mqtt-status');
                    const mqttBrokerEl = document.getElementById('stat-mqtt-broker');
                    if (status.mqtt_status === "enabled") {
                        mqttStatusEl.innerText = "Connected";
                        mqttStatusEl.style.color = "#22c55e"; // green
                        mqttBrokerEl.innerText = `${status.mqtt_broker}:${status.mqtt_port}`;
                    } else {
                        mqttStatusEl.innerText = "Disabled";
                        mqttStatusEl.style.color = "var(--text-muted)";
                        mqttBrokerEl.innerText = "--";
                    }
                }
            } catch (err) {}
        }

        function onPreviewLoaded() {
            document.getElementById('loading').style.opacity = '0';
        }
        function onPreviewError() {
            const loadingEl = document.getElementById('loading');
            loadingEl.innerText = "Load failed";
            loadingEl.style.opacity = '1';
        }

        async function sendCommand(url) { await fetch(url); await fetchStatus(); }
        function confirmRestart() { if(confirm("Restart?")) sendCommand('/api/restart'); }
        setInterval(fetchStatus, 1000);
    </script>
</body>
</html>
)HTML";
    static const std::string placeholder = "v17.4.3 glassmorphic system";
    size_t pos = html.find(placeholder);
    if (pos != std::string::npos) {
        html.replace(pos, placeholder.size(), std::string("v") + VERSION + " glassmorphic system");
    }
    return html;
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
        total = std::ssize(g_eligible);
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
    double transition_delay = 15.0;

    {
        std::lock_guard lk(g_config_mtx);
        shuffle = g_cfg.shuffle;
        mqtt_enabled = g_cfg.mqtt_enabled;
        mqtt_broker = g_cfg.mqtt_broker;
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
        { std::lock_guard lk(g_config_mtx); cache_dir = g_cfg.cache_dir; }
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
                if (line.find("MemTotal:") == 0) {
                    mem_total = std::stoull(line.substr(9)) / 1024;
                } else if (line.find("MemAvailable:") == 0) {
                    mem_available = std::stoull(line.substr(13)) / 1024;
                }
            }
            meminfo.close();
            if (mem_total > 0) {
                int used_mb = static_cast<int>(mem_total - mem_available);
                int total_mb = static_cast<int>(mem_total);
                int pct = static_cast<int>(100.0 * used_mb / total_mb);
                mem_str = std::format("{}MB/{}MB ({}%)", used_mb, total_mb, pct);
            }
        }
    }

    // Query disk usage, cached. The dashboard polls /api/status about once a second and
    // each call previously forked a three-stage shell pipeline (df | tail | tr).
    std::string disk_str = "N/A";
    {
        static std::mutex disk_mtx;
        static std::string cached_disk = "N/A";
        static int64_t cached_at = 0;
        std::lock_guard lk(disk_mtx);
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (cached_at == 0 || now - cached_at > 30000) {
            std::string disk_out;
            auto read_cmd = popen("df / --output=pcent 2>/dev/null | tail -1 | tr -d ' %'", "r");
            if (read_cmd) {
                char buf[256];
                while (fgets(buf, sizeof(buf), read_cmd)) {
                    disk_out += buf;
                }
                pclose(read_cmd);
                if (!disk_out.empty() && disk_out.back() == '\n') disk_out.pop_back();
                cached_disk = disk_out.empty() ? "N/A" : disk_out + "%";
            }
            cached_at = now;
        }
        disk_str = cached_disk;
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
    std::lock_guard lk(g_config_mtx);
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
  "news_enabled": {},
  "news_source": "{}",
  "news_local_query": "{}",
  "news_blacklist": "{}",
  "news_refresh_minutes": {},
  "news_scroll_speed": {},
  "news_font_size": {},
  "gcalendar_enabled": {},
  "gcalendar_source_type": "{}",
  "gcalendar_ical_url": "{}",
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
        g_cfg.news_enabled,
        escape_json(g_cfg.news_source),
        escape_json(g_cfg.news_local_query),
        escape_json([&](){ std::string r; for (size_t k=0; k<g_cfg.news_blacklist.size(); ++k) { if(k>0) r += ", "; r += g_cfg.news_blacklist[k]; } return r; }()),
        g_cfg.news_refresh_minutes,
        g_cfg.news_scroll_speed,
        g_cfg.news_font_size,
        g_cfg.gcalendar_enabled,
        escape_json(g_cfg.gcalendar_source_type),
        escape_json(g_cfg.gcalendar_ical_url),
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
        "Cache-Control: no-cache, no-store, must-validate\r\nConnection: close\r\n\r\n",
        status_line, mime, body.size());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(response_timeout_ms);
    if (http_protocol::send_until(fd, headers, deadline)) http_protocol::send_until(fd, body, deadline);
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
        g_screenshot_ready.store(false);
        g_screenshot_out_path = shot_path;
    }
    g_screenshot_requested.store(true, std::memory_order_release);

    std::unique_lock lk(g_screenshot_mtx);
    bool ready = g_screenshot_cv.wait_for(lk, std::chrono::milliseconds(5000), []{
        return g_screenshot_ready.load(std::memory_order_acquire);
    });

    if (!ready) {
        send_response(fd, "HTTP/1.1 504 Gateway Timeout", "text/plain", "Screenshot capture timed out");
        return;
    }

    lk.unlock(); // File I/O and socket writes must not hold the renderer's state mutex.

    // Auto-prune older screenshots keeping the latest 50
    try {
        std::vector<std::filesystem::directory_entry> entries;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".png") {
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

    std::ifstream file(shot_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        send_response(fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Failed to open screenshot file");
        return;
    }

    std::streamsize size = file.tellg();
    if (size < 0) { // tellg() returns -1 on failure; std::string(-1) would request a huge allocation
        send_response(fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Failed to determine screenshot size");
        return;
    }
    file.seekg(0, std::ios::beg);

    std::string buffer(static_cast<size_t>(size), '\0');
    if (file.read(&buffer[0], size)) {
        send_response(fd, "HTTP/1.1 200 OK", "image/png", buffer);
    } else {
        send_response(fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Failed to read screenshot data");
    }
}

static void handle_screenshots_list(int fd) {
    std::string dir = "/app/screenshots";
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> entries;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".png") {
                entries.push_back(entry);
            }
        }
        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.last_write_time() > b.last_write_time(); // newest first
        });
    } catch (...) {}

    std::ostringstream ss;
    ss << "[";
    for (size_t i = 0; i < entries.size(); ++i) {
        if (i > 0) ss << ",";
        std::string fname = entries[i].path().filename().string();
        uintmax_t fsz = entries[i].file_size(ec);
        ss << "{\"filename\":\"" << fname << "\",\"size_bytes\":" << fsz << ",\"url\":\"/api/screenshots/" << fname << "\"}";
    }
    ss << "]";
    send_response(fd, "HTTP/1.1 200 OK", "application/json", ss.str());
}

static void handle_screenshot_file(int fd, const std::string& filename) {
    // Sanitization: disallow path traversal
    if (filename.find("..") != std::string::npos || filename.find('/') != std::string::npos) {
        send_response(fd, "HTTP/1.1 400 Bad Request", "text/plain", "Invalid filename");
        return;
    }
    std::string path = "/app/screenshots/" + filename;
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        send_response(fd, "HTTP/1.1 404 Not Found", "text/plain", "Screenshot not found");
        return;
    }
    std::streamsize size = file.tellg();
    if (size < 0) {
        send_response(fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Failed to determine screenshot size");
        return;
    }
    file.seekg(0, std::ios::beg);
    std::string buffer(static_cast<size_t>(size), '\0');
    if (file.read(&buffer[0], size)) {
        send_response(fd, "HTTP/1.1 200 OK", "image/png", buffer);
    } else {
        send_response(fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Failed to read screenshot");
    }
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
    std::string ext = std::filesystem::path(lower_path).extension().string();
    if (ext == ".png") mime = "image/png";
    else if (ext == ".webp") mime = "image/webp";
    else if (ext == ".gif") mime = "image/gif";

    // Read file size
    file.seekg(0, std::ios::end);
    const auto file_end = file.tellg();
    if (file_end < 0) {
        send_response(fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Failed to determine preview size");
        return;
    }
    const size_t file_size = static_cast<size_t>(file_end);
    file.seekg(0, std::ios::beg);

    // Send HTTP Headers
    std::string headers = std::format(
        "HTTP/1.1 200 OK\r\n" 
        "Content-Type: {}\r\n" 
        "Content-Length: {}\r\n" 
        "Cache-Control: no-cache, no-store, must-validate\r\n" 
        "Connection: close\r\n\r\n",
        mime, file_size);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(response_timeout_ms);
    if (!http_protocol::send_until(fd, headers, deadline)) return;

    // Stream body in chunks
    char buffer[8192];
    while (file.good()) {
        file.read(buffer, sizeof(buffer));
        std::streamsize bytes = file.gcount();
        if (bytes > 0) {
            if (!http_protocol::send_until(fd, std::string_view(buffer, size_t(bytes)), deadline)) break;
        }
    }
}

static void handle_client(int client_fd) {
    struct ClientFdGuard {
        int fd;
        ClientFdGuard(int f) : fd(f) { try { register_client_fd(fd); } catch (...) { close(fd); throw; } }
        ~ClientFdGuard() { unregister_client_fd(fd); close(fd); }
    };
    ClientFdGuard guard(client_fd);

    // Set client socket timeout to prevent slowloris hangs
    struct timeval client_tv;
    { std::lock_guard lk(g_config_mtx); client_tv.tv_sec = g_cfg.http_socket_timeout; }
    client_tv.tv_usec = 0;
    response_timeout_ms = std::clamp<int>(client_tv.tv_sec, 1, 60) * 1000;
    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &client_tv, sizeof(client_tv)) < 0) {
        g_logger.warn("HTTP: Failed to set SO_RCVTIMEO on client socket.");
    }
    if (setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &client_tv, sizeof(client_tv)) < 0) {
        g_logger.warn("HTTP: Failed to set SO_SNDTIMEO on client socket.");
    }

    std::string request;
    http_protocol::Request parsed_request;
    auto received = http_protocol::receive(client_fd, request, parsed_request,
                                          std::max(1L, client_tv.tv_sec) * 1000);
    if (received != http_protocol::Result::complete) {
        const char* status = received == http_protocol::Result::too_large ? "HTTP/1.1 413 Payload Too Large" :
                             received == http_protocol::Result::timeout ? "HTTP/1.1 408 Request Timeout" :
                             "HTTP/1.1 400 Bad Request";
        send_response(client_fd, status, "text/plain", "Incomplete or invalid HTTP request");
        return;
    }
    auto matches_route = [&](std::string_view method, std::string_view path) {
        return parsed_request.method == method && parsed_request.path == path;
    };

    if (!request.empty()) {
        // Failed-attempt throttling. The accept loop discards the peer address, so this is
        // a global lockout rather than per-IP; enough to make online brute force impractical.
        static std::atomic<int64_t> g_auth_failures{0};
        static std::atomic<int64_t> g_auth_lockout_until{0};
        static constexpr int64_t AUTH_MAX_FAILURES = 5;
        static constexpr int64_t AUTH_LOCKOUT_MS  = 60000;
        auto auth_now_ms = []() -> int64_t {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
        };
        // Constant-time equality: std::string::operator!= short-circuits on the first
        // differing byte, which leaks the length of the matching prefix.
        auto secure_equals = [](const std::string& a, const std::string& b) -> bool {
            if (a.size() != b.size()) return false;
            return sodium_memcmp(a.data(), b.data(), a.size()) == 0;
        };

        auto is_authorized = [&](const std::string& req, int fd) -> bool {
            std::string api_key;
            {
                std::shared_lock<std::shared_mutex> lk(g_config_mtx);
                api_key = g_cfg.http_api_key;
            }
            // Opt-in: with no key configured the control API stays open. Reported at
            // startup rather than enforced, so an absent key is a deliberate choice.
            if (api_key.empty()) return true;

            if (auth_now_ms() < g_auth_lockout_until.load()) {
                std::string body = "HTTP/1.1 429 Too Many Requests\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\nToo many failed attempts";
                http_protocol::send_all(fd, body);
                return false;
            }
            auto deny = [&](const char* msg) {
                std::string body = std::string("HTTP/1.1 401 Unauthorized\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n") + msg;
                http_protocol::send_all(fd, body);
                if (g_auth_failures.fetch_add(1) + 1 >= AUTH_MAX_FAILURES) {
                    g_auth_lockout_until.store(auth_now_ms() + AUTH_LOCKOUT_MS);
                    g_auth_failures.store(0);
                    g_logger.warn("HTTP: auth lockout engaged for {}s after {} failed attempts",
                                  AUTH_LOCKOUT_MS / 1000, AUTH_MAX_FAILURES);
                }
                return false;
            };

            auto header = parsed_request.headers.find("authorization");
            if (header == parsed_request.headers.end()) return deny("API key required");
            std::string auth_value = header->second;
            if (auth_value.size() >= 7) {
                std::string scheme = auth_value.substr(0, 7);
                std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char c) { return std::tolower(c); });
                if (scheme == "bearer ") auth_value.erase(0, 7);
            }
            if (!secure_equals(auth_value, api_key)) {
                g_logger.warn("HTTP: Unauthorized request - invalid key");
                return deny("Invalid API key");
            }
            g_auth_failures.store(0);
            return true;
        };

        // Query parameters can contain OAuth codes or credentials. Log only the route.
        g_logger.info("HTTP: Request: {} {}", parsed_request.method, parsed_request.path);

        // Very simple router
        if (matches_route("GET", "/") || matches_route("GET", "/dashboard")) {
            send_response(client_fd, "HTTP/1.1 200 OK", "text/html", get_dashboard_html());
        } 
        else if (matches_route("GET", "/google_photos_setup") || matches_route("POST", "/google_photos_setup")) {
            // Starts a state-bound flow; credentials become live only after a successful exchange.
            // /google_photos_callback is left open on purpose: it is a Google-initiated
            // redirect that cannot carry an Authorization header, and it only persists
            // config after a successful token exchange. Opt-in via [remote] api_key.
            if (!is_authorized(request, client_fd)) return;
            std::string action = get_query_param(request, "action");
            if (action == "submit") {
                std::string client_id = get_query_param(request, "client_id");
                std::string client_secret = get_query_param(request, "client_secret");
                
                std::string host = get_host_header(request);
                std::string redirect_uri = "http://" + host + "/google_photos_callback";
                
                std::string state = g_oauth_state.begin(client_id, client_secret, redirect_uri);
                if (state.empty()) {
                    send_response(client_fd, "HTTP/1.1 500 Internal Server Error", "text/plain", "Failed to initialize authorization");
                    return;
                }
                std::string auth_url = "https://accounts.google.com/o/oauth2/v2/auth?client_id=" + http_protocol::url_encode(client_id) +
                    "&redirect_uri=" + http_protocol::url_encode(redirect_uri) +
                    "&response_type=code&scope=https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fphotoslibrary.readonly&access_type=offline&prompt=consent&state=" + state;

                std::string redirect_headers = std::format(
                    "HTTP/1.1 302 Found\r\n" 
                    "Location: {}\r\n" 
                    "Connection: close\r\n\r\n", auth_url);
                http_protocol::send_all(client_fd, redirect_headers);
            } else {
                std::string host = get_host_header(request);
                std::string redirect_uri = "http://" + host + "/google_photos_callback";
                send_response(client_fd, "HTTP/1.1 200 OK", "text/html", get_setup_html(redirect_uri));
            }
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
                    trigger_error(0);
                    
                    g_google_photos.stop();
                    g_google_photos.start();
                    
                    send_response(client_fd, "HTTP/1.1 200 OK", "text/html", get_success_html());
                }
            }
        } 
        else if (matches_route("GET", "/api/settings/update")) {
            if (!is_authorized(request, client_fd)) return;
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

                // 1. Validation Checks
                if (has_query_param(request, "transition_delay")) {
                    double val = safe_stod(get_query_param(request, "transition_delay"), -999.0);
                    if (val < 1.0) {
                        validation_failed = true;
                        err_msg = "transition_delay must be >= 1.0";
                    }
                }
                if (!validation_failed && has_query_param(request, "transition_duration")) {
                    double val = safe_stod(get_query_param(request, "transition_duration"), -999.0);
                    if (val < 0.1 || val > 10.0) {
                        validation_failed = true;
                        err_msg = "transition_duration must be between 0.1 and 10.0";
                    }
                }
                if (!validation_failed && has_query_param(request, "ken_burns_speed")) {
                    double val = safe_stod(get_query_param(request, "ken_burns_speed"), -999.0);
                    if (val < 0.001 || val > 5.0) {
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
                    if (has_query_param(request, "gcalendar_ical_url")) {
                        std::string val = get_query_param(request, "gcalendar_ical_url");
                        if (g_cfg.gcalendar_ical_url != val) { g_cfg.gcalendar_ical_url = val; changed = true; }
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
            if (is_authorized(request, client_fd)) {
                send_response(client_fd, "HTTP/1.1 200 OK", "application/json", get_api_settings());
            }
        }
        else if (matches_route("GET", "/api/logs")) {
            if (is_authorized(request, client_fd)) {
            std::string log_content = "";
            std::string path = g_logger.log_file_path;
            bool read_success = false;
            
            if (!path.empty() && std::filesystem::exists(path)) {
                std::ifstream log_file(path);
                if (log_file.is_open()) {
                    log_file.seekg(0, std::ios::end);
                    size_t size = log_file.tellg();
                    size_t offset = (size > 30720) ? (size - 30720) : 0;
                    log_file.seekg(offset, std::ios::beg);
                    
                    std::string line;
                    if (offset > 0) {
                        std::getline(log_file, line);
                    }
                    while (std::getline(log_file, line)) {
                        log_content += line + "\n";
                    }
                    read_success = true;
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
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", g_news_ticker.get_status_json());
        }
        else if (matches_route("GET", "/api/calendar/sync") || matches_route("POST", "/api/calendar/sync")) {
            if (is_authorized(request, client_fd)) {
                spawn_tracked_thread([]() {
                    g_calendar.sync();
                });
                send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\",\"message\":\"Calendar sync triggered\"}");
            }
        }
        else if (matches_route("GET", "/api/calendar")) {
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", g_calendar.get_status_json());
        }
        else if (matches_route("GET", "/api/status")) {
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", get_api_status());
        } 
        else if (matches_route("GET", "/api/next")) {
            if (!is_authorized(request, client_fd)) return;
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
            std::string sub = parsed_request.path.substr(std::string_view("/api/screenshots/").size());
            handle_screenshot_file(client_fd, sub);
        }
        else if (matches_route("GET", "/api/screenshots")) {
            handle_screenshots_list(client_fd);
        }
        else if (matches_route("GET", "/api/screenshot")) {
            // Was the only state-changing route with neither is_authorized nor a cooldown:
            // each call blocks its worker up to 5s and forces a full framebuffer readback
            // plus PNG encode on the render thread.
            if (!is_authorized(request, client_fd)) return;
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
            handle_preview(client_fd);
        } 
        else if (matches_route("GET", "/api/prev")) {
            if (!is_authorized(request, client_fd)) return;
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
            if (!is_authorized(request, client_fd)) return;
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
            if (!is_authorized(request, client_fd)) return;
            static std::atomic<int64_t> last_shuffle_ms{0};
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - last_shuffle_ms.load() < 500) {
                send_response(client_fd, "HTTP/1.1 429 Too Many Requests", "text/plain", "Rate limited");
                return;
            }
            last_shuffle_ms.store(now_ms);
            {
                std::lock_guard lock(g_config_mtx);
                g_cfg.shuffle = !g_cfg.shuffle;
            }
            g_config_changed.store(true);
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
        } 
        else if (matches_route("GET", "/api/stocks")) {
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", g_stock_streamer.get_status_json());
        }
        else if (matches_route("GET", "/api/news/refresh") || matches_route("POST", "/api/news/refresh")) {
            if (!is_authorized(request, client_fd)) return;
            g_news_ticker.sync();
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", g_news_ticker.get_status_json());
        }
        else if (matches_route("GET", "/api/news")) {
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", g_news_ticker.get_status_json());
        }
        else if (matches_route("GET", "/api/restart")) {
            if (!is_authorized(request, client_fd)) return;
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
            if (!is_authorized(request, client_fd)) return;
            static std::atomic<int64_t> last_toggle_screen_ms{0};
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - last_toggle_screen_ms.load() < 500) {
                send_response(client_fd, "HTTP/1.1 429 Too Many Requests", "text/plain", "Rate limited");
                return;
            }
            last_toggle_screen_ms.store(now_ms);
            bool expected = g_screen_blanked.load();
            bool desired = !expected;
            while (!g_screen_blanked.compare_exchange_weak(expected, desired)) {
                desired = !expected;
            }
            set_display_power(expected);
            std::string prefix, topic;
            { std::lock_guard lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; topic = g_cfg.mqtt_motionsensor_topic; }
            mqtt_publish(prefix + "/status/screen", g_screen_blanked.load() ? "OFF" : "ON", true);
            send_response(client_fd, "HTTP/1.1 200 OK", "application/json", "{\"status\":\"ok\"}");
        }
        else if (matches_route("GET", "/api/trigger_motion")) {
            if (!is_authorized(request, client_fd)) return;
            static std::atomic<int64_t> last_motion_ms{0};
            int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now_ms - last_motion_ms.load() < 500) {
                send_response(client_fd, "HTTP/1.1 429 Too Many Requests", "text/plain", "Rate limited");
                return;
            }
            last_motion_ms.store(now_ms);
            g_last_motion_time.store(static_cast<int64_t>(std::time(nullptr)));
            std::string prefix, sensor_topic;
            { std::lock_guard lk(g_config_mtx); prefix = g_cfg.mqtt_topic_prefix; sensor_topic = g_cfg.mqtt_motionsensor_topic; }
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
        else if (matches_route("GET", "/api/play_video")) {
            if (!is_authorized(request, client_fd)) return;
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
    { std::lock_guard lk(g_config_mtx); max_attempts = g_cfg.http_bind_attempts; }
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
    if (active_fd < 0 || listen(active_fd, 10) < 0) {
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
            g_logger.error("HTTP server accept failed: {}", strerror(errno));
            continue;
        }

        // Set a socket read/write timeout to prevent slow/hanging clients from starving the connection pool
        struct timeval timeout;
        {
            std::lock_guard lk(g_config_mtx);
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
            send_response(client_fd, "HTTP/1.1 503 Service Unavailable", "text/plain", "Too Many Connections");
            close(client_fd);
            continue;
        }
        if (!spawn_tracked_thread([client_fd]() {
            struct ConnectionCountGuard { ~ConnectionCountGuard() { g_active_connections.fetch_sub(1); } } count_guard;
            handle_client(client_fd);
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

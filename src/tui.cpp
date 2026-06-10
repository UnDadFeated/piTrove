#include "tui.h"
#include "config.h"
#include "util.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <sys/ioctl.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <termios.h>
#include <cstring>
#include <atomic>
#include <mutex>
#include <poll.h>
#include <fcntl.h>

// ── Message flash buffer (replaces blocking usleep) ──
static struct {
    const char* text;
    uint32_t color;
    uint32_t elapsed_ms;
    bool active;
    std::chrono::steady_clock::time_point msg_start;
    int duration_ms;
} msg_buf = {nullptr, 0, 0, false, {}, 2000};

static void flash_msg(const char* text, uint32_t color, int duration_ms) {
    msg_buf.text = text;
    msg_buf.color = color;
    msg_buf.elapsed_ms = 0;
    msg_buf.active = true;
    msg_buf.msg_start = std::chrono::steady_clock::now();
    msg_buf.duration_ms = duration_ms;
}

// ── Save original terminal settings once ──
static struct termios g_orig_termios;
static bool g_termios_saved = false;

static void save_termios() {
    if (!g_termios_saved) {
        tcgetattr(STDIN_FILENO, &g_orig_termios);
        g_termios_saved = true;
    }
}

static void set_termios_raw() {
    save_termios();
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void restore_termios() {
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    }
}

// Unused static helpers removed to prevent Wunused-function compiler warnings

void config_wizard(const std::string& config_path) {
    auto save_cfg = [&]() -> bool {
        return g_cfg.save(config_path);
    };

    // ── TERMINAL SIZING ──
    struct winsize w;
    std::memset(&w, 0, sizeof(w));
    int term_cols = 100;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &w) >= 0 && w.ws_col > 0) {
        term_cols = w.ws_col;
    }
    if (term_cols < 100) {
        printf("\033[8;40;155t");
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::microseconds(100000));
        std::memset(&w, 0, sizeof(w));
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &w) >= 0 && w.ws_col > 0) {
            term_cols = w.ws_col;
        } else {
            term_cols = 100;
        }
    }
    int tui_width = std::max(100, std::min(155, term_cols));

    set_termios_raw();
    printf("\033[?1049h\033[40m\033[37m\033[H\033[J");

    // ── Dirty flag system: only redraw what changed ──
    // Row indices (0-based, matching printf output lines):
    //   0: version header
    //   1: separator line
    //   2: blank
    //   3: category bar
    //   4: blank
    //   5: column headers
    //   6+: data rows
    enum { ROW_CAT_BAR=3, ROW_COLHDR=5, ROW_ROW0=7 };
    int dirty_from = 0, dirty_to = 0;
    int last_sel = -1, last_sel_sub = -1, last_edit = -1;
    bool dirty_full = true;

    // ── DEFINITIONS WITH DESCRIPTIONS ──
    enum IT { STR, INT, FLT, TGL, ENM, LST };
    struct CI { const char* n; IT t; const char* desc; };

    static const CI CA[] = {
        {"Rotation", INT, "Screen rotation in degrees (0, 90, 180, 270)"},
        {"Ken Burns Zoom", FLT, "Zoom intensity for Ken Burns effect (0.0 to 1.0)"},
        {"Auto Display Rotation", TGL, "Rotate images based on EXIF orientation"},
        {"Brightness Auto", TGL, "Auto backlight dimming based on time of day"},
        {"3D Border", TGL, "Enable 3D miter border around photos"},
        {"3D Border Width", INT, "Thickness of the 3D border in pixels"},
        {"Background Style", ENM, "Background style option (photo, plain, pattern)"},
        {"Pattern Brightness", INT, "Contrast offset for animated pattern style (0 to 150)"},
        {"Pattern Style", ENM, "Design pattern type (combined, grid, waves, dots)"},
        {"Pattern Blend Count", INT, "Number of design patterns to blend (1 to 3)"}
    };
    static const CI CB[] = {
        {"Media Directory", STR, "Root folder containing photos and videos"},
        {"Cache Directory", STR, "Folder for SQLite metadata cache"},
        {"Log Directory", STR, "Folder to store runtime logs"},
        {"Sleep Time", STR, "Time to turn off HDMI port (HH:MM, e.g. 23:00)"},
        {"Wake Time", STR, "Time to turn on HDMI port (HH:MM, e.g. 07:30)"},
        {"HTTP Remote", TGL, "Enable local web server to skip/pause"},
        {"Web Dashboard", TGL, "Enable glassmorphic HTTP web remote control dashboard"},
        {"HTTP Port", INT, "Port for local web server and dashboard (default: 9000)"},
        {"Splash Overlay Y", FLT, "Vertical position of splash UI (0.0 to 1.0)"},
        {"Auto Update", TGL, "Enable automatic background system updates"},
        {"Auto Update Branch", ENM, "Git branch to pull updates from (main/develop)"}
    };
    static const CI CC[] = {
        {"Timer Enabled", TGL, "Show remaining photo/video duration overlay"},
        {"Timer X Pos", FLT, "Horizontal position of timer (0.0 to 1.0)"},
        {"Timer Y Pos", FLT, "Vertical position of timer (0.0 to 1.0)"},
        {"Timer Size", INT, "Font size of timer text in pixels"},
        {"Timer Color", ENM, "Color of timer text"},
        {"Clock Enabled", TGL, "Show time overlay on screen"},
        {"Clock X Pos", FLT, "Horizontal position of clock (0.0 to 1.0)"},
        {"Clock Y Pos", FLT, "Vertical position of clock (0.0 to 1.0)"},
        {"Clock Size", INT, "Font size of clock text in pixels"},
        {"Clock Color", ENM, "Color of clock text"},
        {"Clock 24h", TGL, "Use 24-hour format for clock"},
        {"Count Enabled", TGL, "Show playlist progress (e.g., '14 / 2054')"},
        {"Diagnostics HUD", TGL, "Monospace OSD overlay showing FPS, SoC temp, SQLite size, tags"},
        {"Adaptive Text", TGL, "Color text outline dynamically based on background brightness"}
    };
    static const CI CD[] = {
        {"Video Volume", INT, "Volume level for video playback (0=muted)"},
        {"Videos per Photos", INT, "Interleave ratio. E.g., '2' plays 2 vids per 10 pics"},
        {"Probe Timeout", INT, "Max seconds for ffprobe duration extraction (0=disabled)"},
        {"Play Just Photos", TGL, "Completely exclude videos from playback"},
        {"Play Just Videos", TGL, "Completely exclude photos from playback"},
        {"Closed Captions", TGL, "Enable video closed captions/subtitles by default"},
        {"Subtitles Dir", STR, "Path to folder containing .srt files (matching video basename)"},
        {"OSD Offset X", INT, "Horizontal offset for mpv OSD overlay (pixels, negative=left)"},
        {"OSD Offset Y", INT, "Vertical offset for mpv OSD overlay (pixels, negative=down)"},
        {"Max Tex Dim", INT, "Max texture dimension in pixels (256-8192)"},
        {"HTTP Timeout", INT, "HTTP client socket timeout in seconds (1-60)"},
        {"HTTP Bind Atmpt", INT, "Max HTTP port binding attempts (1-100)"}
    };
    static const CI CE[] = {
        {"Transition Delay", FLT, "Seconds to display photo before transitioning"},
        {"Transition Duration", FLT, "Seconds the transition animation takes"},
        {"Transition Effect", ENM, "Visual style when swapping (crossfade/wipe/pixelate)"},
        {"Ken Burns", TGL, "Smoothly pan and zoom across static photos"},
        {"Ken Burns Speed", FLT, "Speed multiplier for pan/zoom movement"},
        {"Bias Lighting", TGL, "Enable ambient background glow derived from photo"},
        {"Bias Anim Speed", FLT, "Speed of background glow animation (0.0 to stop)"},
        {"Bias Anim Style", ENM, "Visual style of glow (pulsing/edge_glow/aura)"},
        {"Bias Color Mode", ENM, "Color source for glow (auto/rainbow)"},
        {"Matting Enabled", TGL, "Draw 3D matte border around photos"},
        {"Matting Size", INT, "Thickness of the matte border in pixels"},
        {"Cooldown Days", INT, "Days to wait before showing a photo again (0=off)"},
        {"Shuffle", TGL, "Randomize photo/video order"},
        {"Twin Portrait Split", TGL, "Render consecutive portrait images side-by-side"},
        {"Preload Capacity", INT, "Max images to load in the background (default: 4)"},
        {"Preload Workers", INT, "Number of background loading threads (default: 2)"},
        {"Reset Cooldown", TGL, "Reset all shown history on app restart"},
        {"Edge Glow Shadow", TGL, "Apply edge glow to right/bottom only for a 3D shadow look"}
    };
    static const CI CG[] = {
        {"Recursive Scan", TGL, "Recursively scan subdirectories"},
        {"Scan Depth", INT, "Max subdirectory depth to scan"},
        {"Temporal Window", INT, "Show media from =/- X days of today, any year. 0=all"},
        {"Ignore Folders", LST, "Comma-separated folder names to skip"},
        {"Max Concurrent", INT, "Max threads during loading (match CPU cores)"},
        {"Keep People", TGL, "Only show photos containing people (family, faces, friends)"},
        {"Keep Animals", TGL, "Only show photos containing animals (pets, wildlife)"},
        {"On This Day", TGL, "Filter playlist to pictures matching current month and day"}
    };
    static const CI CH[] = {
        {"Weather Enabled", TGL, "Fetch local weather via Open-Meteo API"},
        {"Latitude", FLT, "Location latitude for weather API"},
        {"Longitude", FLT, "Location longitude for weather API"}
    };
    static const CI CF[] = {
        {"DRM Card", STR, "Parent GPU modesetting card path (e.g. '/dev/dri/card1' or 'auto')"},
        {"DRM Connector", STR, "Active connected display connector port (e.g. 'HDMI-A-1' or 'auto')"},
        {"Font Path", STR, "Custom path to TTF/OTF font file (or 'auto' for default search)"},
        {"Audio Device", STR, "Custom audio device identifier for mpv video player (or 'auto')"}
    };
    static const CI CI2[] = {
        {"Log Level", ENM, "Console verbosity (debug, info, warn, error)"},
        {"Min Brightness", INT, "Floor for auto-brightness (0-100)"},
        {"Max Brightness", INT, "Ceiling for auto-brightness (0-100)"},
        {"SQLite mmap Size", INT, "Bytes to allocate for DB memory mapping"},
        {"Log Keep Count", INT, "Number of old log files to retain (default: 5)"}
    };
    static const CI CMQ[] = {
        {"MQTT Enabled", TGL, "Enable MQTT features (0/1)"},
        {"MQTT Broker", STR, "IP or domain of the MQTT broker"},
        {"MQTT Port", INT, "Port of the MQTT broker (default 1883)"},
        {"MQTT Username", STR, "Broker username (leave blank if none)"},
        {"MQTT Password", STR, "Broker password (leave blank if none)"},
        {"MQTT Topic Prefix", STR, "Base topic for state/command messages"},
        {"Motion Topic", STR, "MQTT topic that transmits motion events"},
        {"Motion Cooldown", INT, "Screen off delay in seconds when no motion is detected"}
    };
    static const CI CGP[] = {
        {"GPhotos Enabled", TGL, "Enable Google Photos synchronization (0/1)"},
        {"Client ID", STR, "OAuth 2.0 Client ID for Google Photos API"},
        {"Client Secret", STR, "OAuth 2.0 Client Secret for Google Photos API"},
        {"Refresh Token", STR, "OAuth 2.0 Refresh Token"},
        {"Album ID", STR, "Specific Google Photos Album ID (blank for all)"},
        {"Sync Interval", INT, "Sync interval in minutes (default 60)"},
        {"Cache Folder", STR, "Storage path for synced cloud media"}
    };

    struct CAT { const char* n; const CI* i; int c; };
    static const CAT CATS[] = {
        {"Display", CA, sizeof(CA)/sizeof(CA[0])},
        {"System", CB, sizeof(CB)/sizeof(CB[0])},
        {"Overlays", CC, sizeof(CC)/sizeof(CC[0])},
        {"Videos", CD, sizeof(CD)/sizeof(CD[0])},
        {"Slideshow", CE, sizeof(CE)/sizeof(CE[0])},
        {"Scanning", CG, sizeof(CG)/sizeof(CG[0])},
        {"Weather", CH, sizeof(CH)/sizeof(CH[0])},
        {"Hardware", CF, sizeof(CF)/sizeof(CF[0])},
        {"Advanced", CI2, sizeof(CI2)/sizeof(CI2[0])},
        {"MQTT", CMQ, sizeof(CMQ)/sizeof(CMQ[0])},
        {"GPhotos", CGP, sizeof(CGP)/sizeof(CGP[0])}
    };

    // ── DATA ACCESSORS ──
    auto gv = [&](int c, int i) -> std::string {
        if (c == 0) switch(i) {
            case 0: return std::to_string(g_cfg.rotation);
            case 1: return std::to_string(g_cfg.ken_burns_zoom);
            case 2: return g_cfg.auto_display_rotation?"[ON]":"[OFF]";
            case 3: return g_cfg.brightness_auto?"[ON]":"[OFF]";
            case 4: return g_cfg.border_enabled?"[ON]":"[OFF]";
            case 5: return std::to_string(g_cfg.border_width);
            case 6: return g_cfg.bg_style;
            case 7: return std::to_string(g_cfg.pattern_offset);
            case 8: return g_cfg.pattern_style;
            case 9: return std::to_string(g_cfg.pattern_blend_count);
        }
        if (c == 1) switch(i) {
            case 0: return g_cfg.media_dir; case 1: return g_cfg.cache_dir; case 2: return g_cfg.log_dir;
            case 3: return g_cfg.sleep_time; case 4: return g_cfg.wake_time;
            case 5: return g_cfg.http_enabled?"[ON]":"[OFF]";
            case 6: return g_cfg.web_dashboard_enabled?"[ON]":"[OFF]";
            case 7: return std::to_string(g_cfg.http_port);
            case 8: return std::to_string(g_cfg.splash_overlay_y);
            case 9: return g_cfg.auto_update?"[ON]":"[OFF]";
            case 10: return g_cfg.auto_update_branch;
        }
        if (c == 2) switch(i) {
            case 0: return g_cfg.timer_enabled?"[ON]":"[OFF]";
            case 1: return std::to_string(g_cfg.timer_x);
            case 2: return std::to_string(g_cfg.timer_y);
            case 3: return std::to_string(g_cfg.timer_font_size);
            case 4: return g_cfg.timer_color;
            case 5: return g_cfg.clock_enabled?"[ON]":"[OFF]";
            case 6: return std::to_string(g_cfg.clock_x);
            case 7: return std::to_string(g_cfg.clock_y);
            case 8: return std::to_string(g_cfg.clock_font_size);
            case 9: return g_cfg.clock_color;
            case 10: return g_cfg.clock_24h?"[ON]":"[OFF]";
            case 11: return g_cfg.count_enabled?"[ON]":"[OFF]";
            case 12: return g_cfg.diagnostics_hud_enabled?"[ON]":"[OFF]";
            case 13: return g_cfg.adaptive_text_enabled?"[ON]":"[OFF]";
        }
        if (c == 3) switch(i) {
            case 0: return std::to_string(g_cfg.video_volume);
            case 1: return std::to_string(g_cfg.videos_per_photos);
            case 2: return std::to_string(g_cfg.video_probe_timeout);
            case 3: return g_cfg.play_just_photos?"[ON]":"[OFF]";
            case 4: return g_cfg.play_just_videos?"[ON]":"[OFF]";
            case 5: return g_cfg.closed_captions_enabled?"[ON]":"[OFF]";
            case 6: return g_cfg.video_subtitles_dir;
            case 7: return std::to_string(g_cfg.osd_offset_x);
            case 8: return std::to_string(g_cfg.osd_offset_y);
            case 9: return std::to_string(g_cfg.max_texture_dim);
            case 10: return std::to_string(g_cfg.http_socket_timeout);
            case 11: return std::to_string(g_cfg.http_bind_attempts);
        }
        if (c == 4) switch(i) {
            case 0: return std::to_string(g_cfg.transition_delay);
            case 1: return std::to_string(g_cfg.transition_duration);
            case 2: return g_cfg.transition_effect;
            case 3: return g_cfg.ken_burns?"[ON]":"[OFF]";
            case 4: return std::to_string(g_cfg.ken_burns_speed);
            case 5: return g_cfg.bias_lighting?"[ON]":"[OFF]";
            case 6: return std::to_string(g_cfg.bias_anim_speed);
            case 7: return g_cfg.bias_anim_style; case 8: return g_cfg.bias_color_mode;
            case 9: return g_cfg.matting?"[ON]":"[OFF]";
            case 10: return std::to_string(g_cfg.matting_size);
            case 11: return std::to_string(g_cfg.cooldown_days);
            case 12: return g_cfg.shuffle?"[ON]":"[OFF]";
            case 13: return g_cfg.twin_portrait_enabled?"[ON]":"[OFF]";
            case 14: return std::to_string(g_cfg.preload_capacity);
            case 15: return std::to_string(g_cfg.preload_workers);
            case 16: return g_cfg.reset_cooldown_on_restart?"[ON]":"[OFF]";
            case 17: return g_cfg.edge_glow_shadow?"[ON]":"[OFF]";
        }
        if (c == 5) switch(i) {
            case 0: return g_cfg.recursive?"[ON]":"[OFF]";
            case 1: return std::to_string(g_cfg.scan_depth);
            case 2: return std::to_string(g_cfg.scan_window_days);
            case 3: { std::string s; for(size_t x=0;x<g_cfg.ignore_folders.size();x++) s+=g_cfg.ignore_folders[x]+(x<g_cfg.ignore_folders.size()-1?",":""); return s; }
            case 4: return std::to_string(g_cfg.max_concurrent);
            case 5: return g_cfg.show_people_faces?"[ON]":"[OFF]";
            case 6: return g_cfg.keep_animals?"[ON]":"[OFF]";
            case 7: return g_cfg.on_this_day_enabled?"[ON]":"[OFF]";
        }
        if (c == 6) switch(i) {
            case 0: return g_cfg.weather_enabled?"[ON]":"[OFF]";
            case 1: return std::to_string(g_cfg.weather_lat);
            case 2: return std::to_string(g_cfg.weather_lon);
        }
        if (c == 7) switch(i) {
            case 0: return g_cfg.drm_card;
            case 1: return g_cfg.drm_connector;
            case 2: return g_cfg.font_path;
            case 3: return g_cfg.video_audio_device;
        }
        if (c == 8) switch(i) {
            case 0: return g_cfg.verbose?"debug":"info";
            case 1: return std::to_string(g_cfg.brightness_auto_min);
            case 2: return std::to_string(g_cfg.brightness_auto_max);
            case 3: return std::to_string(g_cfg.cache_mmap_size);
            case 4: return std::to_string(g_cfg.log_keep_count);
        }
        if (c == 9) switch(i) {
            case 0: return g_cfg.mqtt_enabled ? "[ON]" : "[OFF]";
            case 1: return g_cfg.mqtt_broker;
            case 2: return std::to_string(g_cfg.mqtt_port);
            case 3: return g_cfg.mqtt_user;
            case 4: return g_cfg.mqtt_pass;
            case 5: return g_cfg.mqtt_topic_prefix;
            case 6: return g_cfg.mqtt_motionsensor_topic;
            case 7: return std::to_string(g_cfg.mqtt_motionsensor_cooldown);
        }
        if (c == 10) switch(i) {
            case 0: return g_cfg.google_photos_enabled ? "[ON]" : "[OFF]";
            case 1: return g_cfg.google_photos_client_id;
            case 2: return g_cfg.google_photos_client_secret;
            case 3: return g_cfg.google_photos_refresh_token;
            case 4: return g_cfg.google_photos_album_id;
            case 5: return std::to_string(g_cfg.google_photos_sync_interval);
            case 6: return g_cfg.google_photos_cache_dir;
        }
        return "";
    };

    auto sv = [&](int c, int i, const std::string& v) {
        if(v.empty()) return;
        std::lock_guard<std::mutex> lk(g_config_mtx);
        try {
            if(c==0) switch(i){
                case 0:{ try { int rot=std::stoi(v); if(rot==0||rot==90||rot==180||rot==270) g_cfg.rotation=rot; else g_cfg.rotation=0; } catch(...) {} break; }
                case 1:{ try { float val=std::stof(v); g_cfg.ken_burns_zoom=std::max(0.01f, std::min(5.0f, val)); } catch(...) {} break; }
                case 2:g_cfg.auto_display_rotation=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 3:g_cfg.brightness_auto=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 4:g_cfg.border_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 5:{ try { int val = std::stoi(v); g_cfg.border_width=std::max(0, std::min(250, val)); } catch(...) {} break; }
                case 6:g_cfg.bg_style=v;break;
                case 7:{ try { int val = std::stoi(v); g_cfg.pattern_offset=std::max(0, std::min(150, val)); } catch(...) {} break; }
                case 8:g_cfg.pattern_style=v;break;
                case 9:{ try { int val = std::stoi(v); g_cfg.pattern_blend_count=std::max(1, std::min(3, val)); } catch(...) {} break; }
            }
            else if(c==1) switch(i){
                case 0:g_cfg.media_dir=v;break; case 1:g_cfg.cache_dir=v;break; case 2:g_cfg.log_dir=v;break;
                case 3:g_cfg.sleep_time=v;break; case 4:g_cfg.wake_time=v;break;
                case 5:g_cfg.http_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 6:g_cfg.web_dashboard_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 7:{ try { int val = std::stoi(v); if(val>=1&&val<=65535) g_cfg.http_port=val; } catch(...) {} break; }
                case 8:{ try { g_cfg.splash_overlay_y=std::stof(v); } catch(...) {} break; }
                case 9:g_cfg.auto_update=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 10:g_cfg.auto_update_branch=v;break;
            }
            else if(c==2) switch(i){
                case 0:g_cfg.timer_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 1:{ try { g_cfg.timer_x=std::stof(v); } catch(...) {} break; } case 2:{ try { g_cfg.timer_y=std::stof(v); } catch(...) {} break; }
                case 3:{ try { g_cfg.timer_font_size=std::stoi(v); } catch(...) {} break; } case 4:g_cfg.timer_color=v;break;
                case 5:g_cfg.clock_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 6:{ try { g_cfg.clock_x=std::stof(v); } catch(...) {} break; } case 7:{ try { g_cfg.clock_y=std::stof(v); } catch(...) {} break; }
                case 8:{ try { g_cfg.clock_font_size=std::stoi(v); } catch(...) {} break; } case 9:g_cfg.clock_color=v;break;
                case 10:g_cfg.clock_24h=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 11:g_cfg.count_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 12:g_cfg.diagnostics_hud_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 13:g_cfg.adaptive_text_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
            }
            else if(c==3) switch(i){
                case 0:{ try { int val = std::stoi(v); g_cfg.video_volume=std::max(0, std::min(150, val)); } catch(...) {} break; }
                case 1:{ try { int val = std::stoi(v); g_cfg.videos_per_photos=std::max(1, std::min(100, val)); } catch(...) {} break; }
                case 2:{ try { int val = std::stoi(v); g_cfg.video_probe_timeout=std::max(1, std::min(30, val)); } catch(...) {} break; }
                case 3:g_cfg.play_just_photos=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 4:g_cfg.play_just_videos=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 5:g_cfg.closed_captions_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 6:g_cfg.video_subtitles_dir=v;break;
                case 7:{ try { g_cfg.osd_offset_x=std::stoi(v); } catch(...) {} break; }
                case 8:{ try { g_cfg.osd_offset_y=std::stoi(v); } catch(...) {} break; }
                case 9:{ try { g_cfg.max_texture_dim=std::max(256, std::min(8192, std::stoi(v))); } catch(...) {} break; }
                case 10:{ try { g_cfg.http_socket_timeout=std::max(1, std::min(60, std::stoi(v))); } catch(...) {} break; }
                case 11:{ try { g_cfg.http_bind_attempts=std::max(1, std::min(100, std::stoi(v))); } catch(...) {} break; }
            }
            else if(c==4) switch(i){
                case 0:{ try { float val = std::stof(v); g_cfg.transition_delay=std::max(1.0f, val); } catch(...) {} break; } case 1:{ try { float val = std::stof(v); g_cfg.transition_duration=std::max(0.1f, std::min(10.0f, val)); } catch(...) {} break; }
                case 2:g_cfg.transition_effect=v;break;
                case 3:g_cfg.ken_burns=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 4:{ try { float val = std::stof(v); g_cfg.ken_burns_speed=std::max(0.001f, std::min(5.0f, val)); } catch(...) {} break; }
                case 5:g_cfg.bias_lighting=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 6:{ try { g_cfg.bias_anim_speed=std::stof(v); } catch(...) {} break; }
                case 7:g_cfg.bias_anim_style=v;break; case 8:g_cfg.bias_color_mode=v;break;
                case 9:g_cfg.matting=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 10:{ try { g_cfg.matting_size=std::stoi(v); } catch(...) {} break; }
                case 11:{ try { g_cfg.cooldown_days=std::stoi(v); } catch(...) {} break; }
                case 12:g_cfg.shuffle=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 13:g_cfg.twin_portrait_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 14:{ try { g_cfg.preload_capacity=std::max(1, std::min(32, std::stoi(v))); } catch(...) {} break; }
                case 15:{ try { g_cfg.preload_workers=std::max(1, std::min(16, std::stoi(v))); } catch(...) {} break; }
                case 16:g_cfg.reset_cooldown_on_restart=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 17:g_cfg.edge_glow_shadow=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
            }
            else if(c==5) switch(i){
                case 0:g_cfg.recursive=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 1:{ try { int val = std::stoi(v); g_cfg.scan_depth=std::max(1, std::min(100, val)); } catch(...) {} break; }
                case 2:{ try { int val = std::stoi(v); g_cfg.scan_window_days=std::max(0, std::min(365, val)); } catch(...) {} break; }
                case 3:{
                    g_cfg.ignore_folders.clear();
                    std::string clean = v;
                    clean.erase(std::remove(clean.begin(), clean.end(), '['), clean.end());
                    clean.erase(std::remove(clean.begin(), clean.end(), ']'), clean.end());
                    clean.erase(std::remove(clean.begin(), clean.end(), '"'), clean.end());
                    clean.erase(std::remove(clean.begin(), clean.end(), '\''), clean.end());
                    
                    size_t pos = 0, f;
                    while((f = clean.find(',', pos)) != std::string::npos) {
                        std::string token = clean.substr(pos, f - pos);
                        token.erase(0, token.find_first_not_of(" \t"));
                        token.erase(token.find_last_not_of(" \t") + 1);
                        if (!token.empty()) g_cfg.ignore_folders.push_back(token);
                        pos = f + 1;
                    }
                    std::string last = clean.substr(pos);
                    last.erase(0, last.find_first_not_of(" \t"));
                    last.erase(last.find_last_not_of(" \t") + 1);
                    if (!last.empty()) g_cfg.ignore_folders.push_back(last);
                }break;
                case 4:{ try { int val = std::stoi(v); g_cfg.max_concurrent=std::max(1, std::min(64, val)); } catch(...) {} break; }
                case 5:g_cfg.show_people_faces=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 6:g_cfg.keep_animals=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 7:g_cfg.on_this_day_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
            }
            else if(c==6) switch(i){
                case 0:g_cfg.weather_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 1:{ try { float vl=std::stof(v); if(vl>=-90.0f&&vl<=90.0f) g_cfg.weather_lat=vl; } catch(...) {} break; }
                case 2:{ try { float vn=std::stof(v); if(vn>=-180.0f&&vn<=180.0f) g_cfg.weather_lon=vn; } catch(...) {} break; }
            }
            else if(c==7) switch(i){
                case 0:g_cfg.drm_card=v;break;
                case 1:g_cfg.drm_connector=v;break;
                case 2:g_cfg.font_path=v;break;
                case 3:g_cfg.video_audio_device=v;break;
            }
            else if(c==8) switch(i){
                case 0:{ if(v=="debug") g_cfg.verbose=true; else g_cfg.verbose=false; }break;
                case 1:{ try { int val = std::stoi(v); g_cfg.brightness_auto_min=std::max(0, std::min(100, val)); } catch(...) {} break; }
                case 2:{ try { int val = std::stoi(v); g_cfg.brightness_auto_max=std::max(0, std::min(100, val)); } catch(...) {} break; }
                case 3:{ try { g_cfg.cache_mmap_size = std::max(0LL, std::min(268435456LL, std::stoll(v))); } catch(...) {} break; }
                case 4:{ try { g_cfg.log_keep_count=std::max(1, std::min(100, std::stoi(v))); } catch(...) {} break; }
            }
            else if(c==9) switch(i){
                case 0:g_cfg.mqtt_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 1:g_cfg.mqtt_broker=v;break;
                case 2:{ try { g_cfg.mqtt_port=std::stoi(v); } catch(...) {} break; }
                case 3:g_cfg.mqtt_user=v;break;
                case 4:g_cfg.mqtt_pass=v;break;
                case 5:g_cfg.mqtt_topic_prefix=v;break;
                case 6:g_cfg.mqtt_motionsensor_topic=v;break;
                case 7:{ try { g_cfg.mqtt_motionsensor_cooldown=std::stoi(v); } catch(...) {} break; }
            }
            else if(c==10) switch(i){
                case 0:g_cfg.google_photos_enabled=(v=="1"||v=="ON"||v=="true"||v=="[ON]"||v=="[  ON  ]");break;
                case 1:g_cfg.google_photos_client_id=v;break;
                case 2:g_cfg.google_photos_client_secret=v;break;
                case 3:g_cfg.google_photos_refresh_token=v;break;
                case 4:g_cfg.google_photos_album_id=v;break;
                case 5:{ try { g_cfg.google_photos_sync_interval=std::stoi(v); } catch(...) {} break; }
                case 6:g_cfg.google_photos_cache_dir=v;break;
            }
        } catch(...) {}
    };

    auto enums = [&](int c, int i) -> std::vector<std::string> {
        if(c==0&&i==6) return {"photo","plain","pattern"};
        if(c==0&&i==8) return {"random_animated", "random_static", "animated_combined", "animated_grid", "animated_waves", "animated_dots", "animated_circles", "animated_crosses", "animated_triangles", "animated_squares", "animated_hexagons", "animated_fractals", "animated_polygons", "animated_rectangles", "animated_mix", "static_grid", "static_waves", "static_dots", "static_circles", "static_crosses", "static_triangles", "static_squares", "static_hexagons", "static_fractals", "static_polygons", "static_rectangles", "static_mix"};
        if(c==1&&i==9) return {"main","develop"};
        if(c==4&&i==2) return {"crossfade","wipe","pixelate","dissolve","ken_burns"};
        if(c==4&&i==7) return {"pulsing","radiating","absorbing","edge_glow","aura"};
        if(c==4&&i==8) return {"auto","rainbow"};
        if(c==2&&i==4) return {"yellow","white","cyan","red"};
        if(c==2&&i==9) return {"yellow","white","cyan","red"};
        if(c==8&&i==0) return {"debug","info","warn","error"};
        return {};
    };

    int sel = 0, sel_sub = 0;
    bool edit_mode = false;
    std::string ed_buf;
    bool run = true;

    // ── MAIN TUI LOOP ──
    auto start_time = std::chrono::steady_clock::now();
    auto last_render_time = start_time;
    int input_buf_len = 0;
    char input_buf[256];

    while(run) {
        // ── Update message timer ──
        if (msg_buf.active) {
            auto now = std::chrono::steady_clock::now();
            msg_buf.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - msg_buf.msg_start).count();
            if (msg_buf.elapsed_ms > (uint32_t)msg_buf.duration_ms) {
                msg_buf.active = false;
            }
        }

        // Sizing checks
        struct winsize w_curr;
        std::memset(&w_curr, 0, sizeof(w_curr));
        int cur_cols = 100;
        int cur_rows = 24;
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &w_curr) >= 0 && w_curr.ws_col > 0 && w_curr.ws_row > 0) {
            cur_cols = w_curr.ws_col;
            cur_rows = w_curr.ws_row;
        }

        tui_width = std::max(100, std::min(155, cur_cols));
        int name_w = 22;
        int val_w = 26;
        int desc_w = tui_width - name_w - val_w - 6;

        if (cur_cols < 100 || cur_rows < 24) {
            int draw_cols = std::max(80, cur_cols);
            printf("\033[40m\033[37m\033[H\033[J");
            printf("\033[1;31m+"); for(int i=0; i<draw_cols-2; i++) printf("-"); printf("+\033[0m\n");
            printf("\033[1;31m|\033[0m  %-*s\033[1;31m|\033[0m\n", draw_cols-6, " [ TERMINAL WINDOW TOO SMALL ]");
            printf("\033[1;31m|\033[0m  %-*s\033[1;31m|\033[0m\n", draw_cols-6, "");
            printf("\033[1;31m|\033[0m  %-*s\033[1;31m|\033[0m\n", draw_cols-6, " Please stretch or expand your window until the TUI");
            printf("\033[1;31m|\033[0m  %-*s\033[1;31m|\033[0m\n", draw_cols-6, " is clearly visible.");
            printf("\033[1;31m|\033[0m  %-*s\033[1;31m|\033[0m\n", draw_cols-6, "");
            char sz_buf[128];
            snprintf(sz_buf, sizeof(sz_buf), " Current Terminal size:  %dx%d", cur_cols, cur_rows);
            printf("\033[1;31m|\033[0m  %-*s\033[1;31m|\033[0m\n", draw_cols-6, sz_buf);
            printf("\033[1;31m|\033[0m  %-*s\033[1;31m|\033[0m\n", draw_cols-6, " Minimum Required size:  100x24");
            printf("\033[1;31m|\033[0m  %-*s\033[1;31m|\033[0m\n", draw_cols-6, "");
            printf("\033[1;31m+"); for(int i=0; i<draw_cols-2; i++) printf("-"); printf("+\033[0m\n");
            fflush(stdout);

            struct pollfd pfd;
            pfd.fd = STDIN_FILENO;
            pfd.events = POLLIN;
            int pret = poll(&pfd, 1, 100);
            if (pret > 0 && (pfd.revents & POLLIN)) {
                char c;
                if (read(STDIN_FILENO, &c, 1) == 1) {
                    if (c == 'q' || c == 'Q') run = false;
                }
            }
            continue;
        }

        // ── Determine if we need to render this frame ──
        auto now = std::chrono::steady_clock::now();
        int elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_render_time).count();
        bool has_input = input_buf_len > 0;
        bool sel_changed = (sel != last_sel || sel_sub != last_sel_sub);
        bool edit_changed = (edit_mode && last_edit != 1) || (!edit_mode && last_edit != 0);
        bool msg_active = msg_buf.active;
        bool config_changed = g_config_changed.load() && !edit_mode;

        bool need_render = dirty_full || has_input || sel_changed || edit_changed || msg_active || config_changed;
        // Also redraw every 200ms to keep time display fresh
        if (!need_render && elapsed_ms > 200) need_render = true;

        if (need_render) {
            last_render_time = now;

            // ── Full redraw ──
            if (dirty_full) {
                printf("\033[40m\033[37m\033[H\033[J");

                // Header
                printf("\033[1;36m  piTrove Configuration Engine v%s\033[0m\n", VERSION);
                printf("  \033[90m"); for(int i=0; i<tui_width-4; i++) printf("="); printf("\033[0m\n\n");

                // Category bar
                printf("  ");
                for(int i=0; i<(int)(sizeof(CATS)/sizeof(CATS[0])); i++) {
                    if(i==sel) printf("\033[30;43m %s \033[0m  ", CATS[i].n);
                    else printf("\033[1;37m%s\033[0m  ", CATS[i].n);
                }
                printf("\n\n");

                // Column headers
                printf("  \033[1;36m%-*s %-*s %-*s\033[0m\n", name_w, "Setting", val_w, "Value", desc_w, "Description");
                printf("  \033[90m"); for(int i=0; i<tui_width-4; i++) printf("-"); printf("\033[0m\n");

                dirty_from = ROW_ROW0;
                dirty_to = ROW_ROW0 + CATS[sel].c - 1;
                dirty_full = false;
            } else {
                // ── Dirty redraw: position cursor and redraw only changed rows ──
                int y = dirty_from + 1;

                printf("\033[%d;1H", y);

                if (dirty_from <= ROW_CAT_BAR && dirty_to >= ROW_CAT_BAR) {
                    printf("\033[1;36m  piTrove Configuration Engine v%s\033[0m\n", VERSION);
                    printf("  \033[90m"); for(int i=0; i<tui_width-4; i++) printf("="); printf("\033[0m\n\n");
                    printf("  ");
                    for(int i=0; i<(int)(sizeof(CATS)/sizeof(CATS[0])); i++) {
                        if(i==sel) printf("\033[30;43m %s \033[0m  ", CATS[i].n);
                        else printf("\033[1;37m%s\033[0m  ", CATS[i].n);
                    }
                    printf("\n\n");
                    y += 2;
                    printf("\033[%d;1H", y);
                }

                if (dirty_from <= ROW_COLHDR && dirty_to >= ROW_COLHDR) {
                    printf("  \033[1;36m%-*s %-*s %-*s\033[0m\n", name_w, "Setting", val_w, "Value", desc_w, "Description");
                    printf("  \033[90m"); for(int i=0; i<tui_width-4; i++) printf("-"); printf("\033[0m\n");
                    y += 2;
                    printf("\033[%d;1H", y);
                }
            }

            // Render row lines
            int row_start = std::max(0, dirty_from - ROW_ROW0);
            int row_end = std::min(CATS[sel].c, dirty_to - ROW_ROW0 + 1);
            if (edit_mode) { row_start = 0; row_end = CATS[sel].c; }

            // Position cursor to start printing row lines
            printf("\033[%d;1H", ROW_ROW0 + row_start + 1);

            for (int i = row_start; i < row_end; i++) {
                const auto& item = CATS[sel].i[i];
                std::string val = gv(sel, i);
                if (item.t == TGL) {
                    val = (val == "1" || val == "[ON]" || val == "[  ON  ]") ? "[  ON  ]" : "[ OFF  ]";
                }
                std::string desc = item.desc;
                if ((int)desc.length() > desc_w) desc = desc.substr(0, desc_w - 3) + "...";

                if(edit_mode && i==sel_sub)
                    printf("  \033[1;32m%-*s \033[30;47m%-*s\033[0m \033[90m%-*s\033[0m\n", name_w, item.n, val_w, ed_buf.c_str(), desc_w, desc.c_str());
                else if(i==sel_sub)
                    printf("  \033[1;32m%-*s \033[1;37m%-*s\033[0m \033[90m%-*s\033[0m\n", name_w, item.n, val_w, val.c_str(), desc_w, desc.c_str());
                else
                    printf("  %-*s \033[37m%-*s\033[0m \033[90m%-*s\033[0m\n", name_w, item.n, val_w, val.c_str(), desc_w, desc.c_str());
            }

            // Clear remaining rows to prevent stale text
            for (int i = CATS[sel].c; i < 15; i++) printf("\033[K\n");

            // Footer
            int footer_row = ROW_ROW0 + std::min(CATS[sel].c, 15) + 2;
            printf("\033[%d;1H\n  \033[90m", footer_row);
            for (int i = 0; i < tui_width - 4; i++) {
                printf("-");
            }
            printf("\033[0m\n");

            if(!edit_mode)
                printf("  \033[1;37m[^/v]\033[0m Select    \033[1;37m[</>]\033[0m Category    \033[1;37m[SPACE/ENTER]\033[0m Toggle/Edit    \033[1;32m[S]\033[0m Save    \033[1;31m[Q]\033[0m Quit\n");
            else
                printf("  \033[1;32m[ENTER]\033[0m Confirm   \033[1;31m[ESC]\033[0m Cancel      \033[1;37m[^/v]\033[0m Cycle Options\n");

            // Message flash (non-blocking)
            if (msg_buf.active) {
                printf("  \033[1;31m%s\033[0m\n", msg_buf.text);
            }

            // Restart notice
            if (!edit_mode && g_config_changed.load()) {
                printf("  \033[1;33m[NOTICE]\033[0m Previous changes detected. Use \033[1;32m[S]\033[0m to save, then \033[1;36mpitrove restart\033[0m to apply.\n");
            }

            fflush(stdout);
        }

        // ── Input handling ──
        // Drain remaining input buffer
        if (input_buf_len > 0) {
            // Process input_buf
            // ... (see below)
        }

        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        int poll_ret = poll(&pfd, 1, 50);
        if (poll_ret <= 0) {
            continue;
        }

        // Read all available bytes
        int n = read(STDIN_FILENO, input_buf + input_buf_len, sizeof(input_buf) - input_buf_len - 1);
        if (n > 0) {
            input_buf_len += n;
            input_buf[input_buf_len] = '\0';
        }

        // Process input buffer
        int pos = 0;
        while (pos < input_buf_len) {
            int consumed = 0;

            // Check for escape sequences
            if (input_buf[pos] == '\033' && (pos + 2) < input_buf_len) {
                char buf3[4] = {'\033', input_buf[pos+1], input_buf[pos+2], '\0'};
                if (buf3[1] == '[') {
                    char third = buf3[2];
                    if (third == 'A' || third == 'B' || third == 'D' || third == 'C') {
                        consumed = 3;
                        int action = 0;
                        if (third == 'A') action = 1;  // UP
                        else if (third == 'B') action = 2;  // DOWN
                        else if (third == 'D') action = 3;  // LEFT
                        else if (third == 'C') action = 4;  // RIGHT

                        if(!edit_mode) {
                            if (action == 1) { if(sel_sub>0) sel_sub--; else if(sel>0){ sel--; sel_sub=CATS[sel].c-1; } }
                            else if (action == 2) { if(sel_sub<CATS[sel].c-1) sel_sub++; else if(sel<(int)(sizeof(CATS)/sizeof(CATS[0]))-1){ sel++; sel_sub=0; } }
                            else if (action == 3) { if(sel>0) { sel--; sel_sub=0; } }
                            else if (action == 4) { if(sel<(int)(sizeof(CATS)/sizeof(CATS[0]))-1) { sel++; sel_sub=0; } }
                            // Mark dirty for selection change
                            dirty_full = true;
                            last_sel = sel; last_sel_sub = sel_sub; last_edit = edit_mode ? 1 : 0;
                        } else {
                            const auto& item = CATS[sel].i[sel_sub];
                            if(item.t == ENM) {
                                auto opts = enums(sel, sel_sub);
                                if(!opts.empty()) {
                                    auto it = std::find(opts.begin(), opts.end(), ed_buf);
                                    int idx = (it != opts.end()) ? std::distance(opts.begin(), it) : 0;
                                    if (action == 2) idx = (idx + 1) % opts.size(); // DOWN
                                    if (action == 1) idx = (idx - 1 + opts.size()) % opts.size(); // UP
                                    ed_buf = opts[idx];
                                }
                            }
                            dirty_from = ROW_ROW0; dirty_to = ROW_ROW0 + CATS[sel].c;
                            last_sel = sel; last_sel_sub = sel_sub; last_edit = 1;
                        }
                        pos += consumed;
                        continue;
                    }
                }
                // Unknown escape: consume one byte
                pos++;
                continue;
            }

            // Regular character
            char c = input_buf[pos];
            consumed = 1;

            if(!edit_mode) {
                if(c == 'q' || c == 'Q') { run = false; break; }
                else if(c == 's' || c == 'S') {
                    if(!save_cfg()) {
                        flash_msg("[ERROR] Failed to save config file.", 1, 2000);
                    } else {
                        flash_msg("[OK] Configuration saved.", 1, 2000);
                    }
                    run = false;
                    dirty_full = true;
                }
                else if(c == '\n' || c == '\r' || c == ' ') {
                    if (CATS[sel].i[sel_sub].t == TGL) {
                        std::string v = gv(sel, sel_sub);
                        sv(sel, sel_sub, (v=="1"||v=="[ON]"||v=="[  ON  ]") ? "0" : "1");
                        dirty_from = ROW_ROW0 + std::min(sel_sub, 14);
                        dirty_to = dirty_from + 1;
                    } else if (CATS[sel].i[sel_sub].t == ENM && c == ' ') {
                        auto opts = enums(sel, sel_sub);
                        if (!opts.empty()) {
                            std::string curr = gv(sel, sel_sub);
                            auto it = std::find(opts.begin(), opts.end(), curr);
                            int idx = (it != opts.end()) ? std::distance(opts.begin(), it) : 0;
                            sv(sel, sel_sub, opts[(idx + 1) % opts.size()]);
                        }
                        dirty_from = ROW_ROW0 + std::min(sel_sub, 14);
                        dirty_to = dirty_from + 1;
                    } else if (c != ' ') {
                        edit_mode = true;
                        ed_buf = gv(sel, sel_sub);
                        dirty_from = ROW_ROW0; dirty_to = ROW_ROW0 + CATS[sel].c;
                    }
                    dirty_full = true;
                    last_sel = sel; last_sel_sub = sel_sub; last_edit = 1;
                }
            } else {
                if(c == '\n' || c == '\r') {
                    sv(sel, sel_sub, ed_buf);
                    edit_mode = false;
                    dirty_full = true;
                    last_sel = sel; last_sel_sub = sel_sub; last_edit = 0;
                }
                else if(c == '\033') {
                    edit_mode = false;
                    dirty_full = true;
                    last_sel = sel; last_sel_sub = sel_sub; last_edit = 0;
                }
                else if(c == 127 || c == 8) {
                    if(!ed_buf.empty()) ed_buf.pop_back();
                    dirty_from = ROW_ROW0; dirty_to = ROW_ROW0 + CATS[sel].c;
                    last_sel = sel; last_sel_sub = sel_sub; last_edit = 1;
                }
                else if(c >= 32 && c <= 126) {
                    const auto& item = CATS[sel].i[sel_sub];
                    if (item.t == FLT && !isdigit(c) && c != '.' && c != '-') continue;
                    if (item.t == INT && !isdigit(c) && c != '-') continue;
                    ed_buf += c;
                    dirty_from = ROW_ROW0; dirty_to = ROW_ROW0 + CATS[sel].c;
                    last_sel = sel; last_sel_sub = sel_sub; last_edit = 1;
                }
            }

            pos += consumed;
        }

        // Drain remaining buffer
        if (pos > 0 && pos < input_buf_len) {
            input_buf_len -= pos;
            memmove(input_buf, input_buf + pos, input_buf_len);
        } else {
            input_buf_len = 0;
        }
    }

    restore_termios();
    printf("\033[0m\033[?1049l");
}
